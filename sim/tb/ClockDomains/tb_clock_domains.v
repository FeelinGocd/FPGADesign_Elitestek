`timescale 1ns/1ps
module tb_clock_domains #(parameter real CORE_HALF = 2.5);
reg fast = 0, slow = 0, rst_n = 0;
always #(CORE_HALF) fast = ~fast;
initial begin #1.3; forever #5 slow = ~slow; end
integer sent = 0, received = 0, fast_cycle = 0, slow_cycle = 0;
wire src_ready, dst_valid;
wire [31:0] dst_data;
wire src_valid, dst_ready;
assign src_valid = rst_n && (sent < 1000) && (fast_cycle % 7 != 3);
assign dst_ready = rst_n && (slow_cycle % 13 < 8);
CdcStream #(.WIDTH(32), .ADDR_WIDTH(3)) u_stream (
    .src_clk(fast), .src_rst_n(rst_n), .src_data(sent ^ 32'h5a53c719),
    .src_valid(src_valid), .src_ready(src_ready),
    .dst_clk(slow), .dst_rst_n(rst_n), .dst_data(dst_data),
    .dst_valid(dst_valid), .dst_ready(dst_ready)
);
always @(posedge fast) if (rst_n) begin
    fast_cycle <= fast_cycle + 1;
    if (src_valid && src_ready) sent <= sent + 1;
end
reg stalled = 0;
reg [31:0] held;
always @(posedge slow) if (rst_n) begin
    slow_cycle <= slow_cycle + 1;
    if (stalled && (!dst_valid || dst_data !== held)) $fatal(1,"CDC payload changed during backpressure");
    stalled <= dst_valid && !dst_ready;
    held <= dst_data;
    if (dst_valid && dst_ready) begin
        if (dst_data !== (received ^ 32'h5a53c719)) $fatal(1,"FIFO order/data mismatch at %0d",received);
        received <= received + 1;
    end
end

reg frame_request = 0, enable = 0, mode_pending = 0;
reg [63:0] base_a = 64'h00100000_00200000;
reg [63:0] base_b = 64'h00300000_00400000;
reg [63:0] base_full = 64'h00500000_00600000;
reg done_acc = 0, done_cpu = 0;
wire frame_fast, mem_enable, mem_mode, frame_commit;
wire [63:0] mem_addr;
wire acc_enable, acc_mode;
wire [63:0] acc_addr;
wire [1:0] done_clr;
integer flips = 0, commits = 0;
reg [64:0] expected;
reg capture_expected = 0;
GPU2D_APBHandler u_handler (
    .clk(fast), .rst_n(rst_n), .rst_n_disp(rst_n),
    .enable(enable), .split_pending(mode_pending),
    .framea_base_addr(base_a), .frameb_base_addr(base_b), .frame_full_base_addr(base_full),
    .done_gpu2d(done_acc), .done_cpu(done_cpu), .done_clr(done_clr),
    .lineBuffer_frame_start(frame_fast), .lineBuffer_enable(acc_enable),
    .lineBuffer_mode(acc_mode), .lineBuffer_addr(acc_addr),
    .acc_busy(1'b0), .cmd_done(1'b0), .frame_done(1'b0),
    .wr_beat_hs(1'b0), .rd_beat_hs(1'b0)
);
GPU2D_DisplayCDC u_display (
    .clk_acc(fast), .rst_acc_n(rst_n), .clk_mem(slow), .rst_mem_n(rst_n),
    .mem_frame_start(frame_request), .acc_frame_start(frame_fast),
    .acc_enable(acc_enable), .acc_mode(acc_mode), .acc_addr(acc_addr),
    .mem_enable(mem_enable), .mem_mode(mem_mode), .mem_addr(mem_addr),
    .mem_frame_commit(frame_commit)
);
always @(posedge fast) if (rst_n) begin
    capture_expected <= frame_fast;
    if (capture_expected) expected <= {acc_mode,acc_addr};
    if (frame_fast) flips <= flips + 1;
    if (done_clr[0]) done_acc <= 0;
    if (done_clr[1]) done_cpu <= 0;
end
always @(posedge slow) if (rst_n && frame_commit) begin
    if ({mem_mode,mem_addr} !== expected) $fatal(1,"Frame committed with stale mode/base");
    if (commits >= flips) $fatal(1,"Unsolicited frame commit");
    commits <= commits + 1;
end
integer frame;
initial begin
    #41 rst_n = 1;
    enable = 1;
    repeat (5) @(negedge slow);
    if (!mem_enable) $fatal(1,"Enable CDC failed");
    for (frame=0; frame<32; frame=frame+1) begin
        @(negedge fast);
        mode_pending = frame[0];
        base_a = base_a + 64'h1000000010;
        base_b = base_b + 64'h1000000010;
        base_full = base_full + 64'h1000000010;
        done_acc = 1;
        done_cpu = frame[1];
        @(negedge slow); frame_request = 1;
        @(negedge slow); frame_request = 0;
        repeat (15) @(negedge slow);
        if (commits != frame+1 || flips != frame+1) $fatal(1,"Lost/duplicated frame handshake");
        if (done_acc || done_cpu) $fatal(1,"DONE was not consumed");
    end
    enable = 0;
    repeat (5) @(negedge slow);
    if (mem_enable) $fatal(1,"Disable did not cross between frames");
    wait (received == 1000);
    repeat (10) @(negedge slow);
    $display("PASS: 1000 CDC words with backpressure; 32 atomic frame flips; immediate disable");
    $finish;
end
initial begin #100000; $fatal(1,"Clock-domain test timed out"); end
endmodule
