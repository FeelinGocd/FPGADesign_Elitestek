`timescale 1ns/1ps

// Sequence-number scoreboard covers both physical storage choices and both
// directions of a 300/100 MHz crossing. Requests may stay asserted at full or
// empty; only requests accepted by the documented flags count as transfers.
module async_fifo_case #(
    parameter CASE_ID = 0,
    parameter USE_REGISTERS = 0,
    parameter FAST_WRITE = 1,
    parameter ADDR_WIDTH = 4,
    parameter TOTAL = 4096
)(
    output reg done = 0
);
localparam DEPTH = 1 << ADDR_WIDTH;
localparam FINAL_COUNT = TOTAL + 1 + DEPTH;
localparam FILL = 0, RANDOM_IO = 1, DRAIN = 2, EMPTY_HOLD = 3;
localparam WAKE_EMPTY = 4, READ_ONE = 5, REFILL = 6, FULL_HOLD = 7;
localparam FREE_ONE = 8, HOLD_FLAGS = 9, FINAL_DRAIN = 10;

reg clk_wr = 0, clk_rd = 0, rst_n = 0;
reg en_wr = 0, en_rd = 0;
wire wr_full, rd_empty;
wire [31:0] data_in, data_out;
integer phase = -1, epoch = 0;
integer sent = 0, received = 0;
integer blocked_full = 0, blocked_empty = 0;
integer total_checked = 0, timeout_count;
reg [31:0] wr_random = 32'h9e3779b9 ^ CASE_ID;
reg [31:0] rd_random = 32'h7f4a7c15 ^ (CASE_ID << 8);
reg [31:0] expected_read;

function [31:0] sequence_word;
    input integer n;
    begin sequence_word = n ^ 32'hc001cafe ^ (CASE_ID << 20) ^ (epoch << 28); end
endfunction

assign data_in = sequence_word(sent);

initial begin
    forever #(FAST_WRITE ? 1.667 : 5.0) clk_wr = ~clk_wr;
end
initial begin
    #(0.371 + CASE_ID * 0.173);
    forever #(FAST_WRITE ? 5.0 : 1.667) clk_rd = ~clk_rd;
end

async_fifo #(
    .WR_WIDTH(32), .RD_WIDTH(32), .ADDR_WIDTH(ADDR_WIDTH),
    .USE_REGISTERS(USE_REGISTERS)
) dut (
    .clk_wr(clk_wr), .rst_wr_n(rst_n), .data_in(data_in),
    .en_wr(en_wr), .wr_full(wr_full),
    .clk_rd(clk_rd), .rst_rd_n(rst_n), .data_out(data_out),
    .en_rd(en_rd), .rd_empty(rd_empty)
);

// Drive at falling edges so the scoreboard and DUT see identical requests.
always @(negedge clk_wr) begin
    if (!rst_n) en_wr = 0;
    else begin
        wr_random = {wr_random[30:0], wr_random[31] ^ wr_random[21]
                                         ^ wr_random[1] ^ wr_random[0]};
        case (phase)
            FILL, FULL_HOLD: en_wr = 1;
            RANDOM_IO: en_wr = (sent < TOTAL) && (wr_random[7:0] < 8'd183);
            WAKE_EMPTY: en_wr = (sent < TOTAL + 1);
            REFILL: en_wr = (sent < FINAL_COUNT);
            default: en_wr = 0;
        endcase
    end
end

always @(negedge clk_rd) begin
    if (!rst_n) en_rd = 0;
    else begin
        rd_random = {rd_random[30:0], rd_random[31] ^ rd_random[21]
                                         ^ rd_random[1] ^ rd_random[0]};
        case (phase)
            RANDOM_IO: en_rd = (rd_random[7:0] < 8'd171);
            DRAIN, EMPTY_HOLD, FINAL_DRAIN: en_rd = 1;
            READ_ONE: en_rd = (received < TOTAL + 1);
            FREE_ONE: en_rd = (received < TOTAL + 2);
            default: en_rd = 0;
        endcase
    end
end

always @(posedge clk_wr) begin
    if (!rst_n) begin
        sent <= 0;
        blocked_full <= 0;
    end
    else if (en_wr) begin
        if (wr_full) blocked_full <= blocked_full + 1;
        else sent <= sent + 1;
    end
end

always @(posedge clk_rd) begin
    if (!rst_n) begin
        received <= 0;
        blocked_empty <= 0;
    end
    else if (en_rd) begin
        if (rd_empty) blocked_empty <= blocked_empty + 1;
        else begin
            expected_read = sequence_word(received);
            // Both memory implementations have a synchronous registered read.
            #0.010;
            if (data_out !== expected_read)
                $fatal(1, "case=%0d epoch=%0d read=%0d got=%08x expected=%08x",
                       CASE_ID, epoch, received, data_out, expected_read);
            received <= received + 1;
            total_checked = total_checked + 1;
        end
    end
end

task wait_for_sent;
    input integer wanted;
    begin
        timeout_count = 0;
        while (sent < wanted && timeout_count < 100000) begin
            @(negedge clk_wr); #0.020;
            timeout_count = timeout_count + 1;
        end
        if (sent != wanted)
            $fatal(1, "case=%0d phase=%0d sent=%0d wanted=%0d", CASE_ID, phase, sent, wanted);
    end
endtask

task wait_for_received;
    input integer wanted;
    begin
        timeout_count = 0;
        while (received < wanted && timeout_count < 100000) begin
            @(negedge clk_rd); #0.020;
            timeout_count = timeout_count + 1;
        end
        if (received != wanted)
            $fatal(1, "case=%0d phase=%0d received=%0d wanted=%0d",
                   CASE_ID, phase, received, wanted);
    end
endtask

initial begin
    for (epoch = 0; epoch < 2; epoch = epoch + 1) begin
        phase = -1;
        rst_n = 0;
        #31.123;
        rst_n = 1;
        phase = FILL;
        wait_for_sent(DEPTH);
        repeat (12) @(negedge clk_wr);
        #0.020;
        if (!wr_full || sent != DEPTH || received != 0)
            $fatal(1, "case=%0d initial fill/full guard failed", CASE_ID);

        phase = RANDOM_IO;
        wait_for_sent(TOTAL);
        phase = DRAIN;
        wait_for_received(TOTAL);
        repeat (8) @(negedge clk_rd);
        #0.020;
        if (!rd_empty) $fatal(1, "case=%0d did not drain to empty", CASE_ID);

        phase = EMPTY_HOLD;
        repeat (12) @(negedge clk_rd);
        #0.020;
        if (!rd_empty || received != TOTAL)
            $fatal(1, "case=%0d empty guard accepted a read", CASE_ID);

        // Empty must clear when the write pointer crosses even with en_rd=0.
        phase = WAKE_EMPTY;
        wait_for_sent(TOTAL + 1);
        repeat (12) @(negedge clk_rd);
        #0.020;
        if (rd_empty || received != TOTAL)
            $fatal(1, "case=%0d empty did not clear while read request was idle", CASE_ID);
        phase = READ_ONE;
        wait_for_received(TOTAL + 1);
        repeat (8) @(negedge clk_rd);

        phase = REFILL;
        wait_for_sent(FINAL_COUNT);
        repeat (8) @(negedge clk_wr);
        phase = FULL_HOLD;
        repeat (12) @(negedge clk_wr);
        #0.020;
        if (!wr_full || sent != FINAL_COUNT)
            $fatal(1, "case=%0d full guard accepted an extra write", CASE_ID);

        // Full must clear after a read crosses even with en_wr=0.
        phase = FREE_ONE;
        wait_for_received(TOTAL + 2);
        phase = HOLD_FLAGS;
        repeat (12) @(negedge clk_wr);
        #0.020;
        if (wr_full || sent != FINAL_COUNT)
            $fatal(1, "case=%0d full did not clear while write request was idle", CASE_ID);

        phase = FINAL_DRAIN;
        wait_for_received(FINAL_COUNT);
        repeat (8) @(negedge clk_rd);
        #0.020;
        if (!rd_empty || blocked_full < 12 || blocked_empty < 12)
            $fatal(1, "case=%0d missing full/empty coverage full=%0d empty=%0d",
                   CASE_ID, blocked_full, blocked_empty);
    end
    phase = -1;
    $display("PASS: async_fifo case=%0d registers=%0d fast_write=%0d checked=%0d depth=%0d",
             CASE_ID, USE_REGISTERS, FAST_WRITE, total_checked, DEPTH);
    done = 1;
end
endmodule

module tb_async_fifo;
wire [3:0] done;
async_fifo_case #(.CASE_ID(0), .USE_REGISTERS(0), .FAST_WRITE(1)) c0(.done(done[0]));
async_fifo_case #(.CASE_ID(1), .USE_REGISTERS(1), .FAST_WRITE(1)) c1(.done(done[1]));
async_fifo_case #(.CASE_ID(2), .USE_REGISTERS(0), .FAST_WRITE(0)) c2(.done(done[2]));
async_fifo_case #(.CASE_ID(3), .USE_REGISTERS(1), .FAST_WRITE(0)) c3(.done(done[3]));
initial begin
    wait (&done);
    $display("SUMMARY: tb_async_fifo ALL-PASS: 4 clock/storage cases, 2 resets each, 32904 ordered words; full/empty wake-up, random stalls and pointer wraps");
    $finish;
end
initial begin
    #20000000;
    $fatal(1, "SUMMARY: tb_async_fifo FAIL: timeout done=%b", done);
end
endmodule
