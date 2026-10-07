//==============================================================================
//  tb_DDRUnpacker.v —— 128bit 拍流 → 4 像素/拍，含丢头/补头
//------------------------------------------------------------------------------
//  喂进去的字节流是已知的: 读流第 p 字节 = p & 0xFF
//  期望输出 = 该字节流前移 shift 个字节（shift<0 时前面补零）
//  只比"完整吐出的输出字"，尾部兜里的余数不参与比较（那本来就还没吐）
//==============================================================================
`timescale 1ns/100ps
module tb_DDRUnpacker;

localparam IN_W = 128, OUT_W = 96;
localparam NB   = 24;                       // 喂多少拍
localparam CLK_HALF = 5;

reg                 clk, rst_n;
reg  [IN_W-1:0]     in_data;
reg                 in_valid;
wire                in_ready;
reg                 line_start;
reg  [4:0]          shift;
wire [OUT_W-1:0]    out_data;
wire                out_valid;
reg                 out_ready;

integer             err_cnt;
integer             i, k, p;
reg  [7:0]          got [0:8191];
integer             got_n;

DDRUnpacker #(
    .IN_WIDTH    (IN_W),
    .PIXEL_BYTES   (3),
    .PIXELS_PER_OUT (4),
    .THRESH      (12)
) dut (
    .clk        (clk),
    .rst_n      (rst_n),
    .in_data    (in_data),
    .in_valid   (in_valid),
    .in_ready   (in_ready),
    .line_start (line_start),
    .shift      (shift),
    .out_data   (out_data),
    .out_valid  (out_valid),
    .out_ready  (out_ready)
);

initial begin clk = 1'b0; forever #(CLK_HALF) clk = ~clk; end

always @(posedge clk) begin
    if (out_valid & out_ready) begin
        for (i = 0; i < 12; i = i + 1)
            got[got_n + i] = out_data[8*i +: 8];
        got_n = got_n + 12;
    end
end

function [IN_W-1:0] mkbeat;
    input integer kk;
    integer ii;
    begin
        mkbeat = {IN_W{1'b0}};
        for (ii = 0; ii < 16; ii = ii + 1)
            mkbeat[8*ii +: 8] = (16*kk + ii) & 8'hFF;
    end
endfunction

// 输出流第 p 字节期望值（读流第 q 字节 = q & 0xFF；q<0 处补零）
function [7:0] exp_byte;
    input integer pp;
    integer qq;
    begin
        qq = pp + $signed({{27{shift[4]}}, shift});
        exp_byte = (qq < 0) ? 8'h00 : (qq & 8'hFF);
    end
endfunction

task run_case;
    input [4:0]      sh;
    input [8*20-1:0] tag;
    integer          bad;
    begin
        bad = 0;
        rst_n = 1'b0; in_valid = 1'b0; line_start = 1'b0;
        shift = sh; out_ready = 1'b1; got_n = 0;
        repeat (4) @(posedge clk);
        rst_n = 1'b1; @(posedge clk); #1;

        line_start = 1'b1; @(posedge clk); #1; line_start = 1'b0;

        for (k = 0; k < NB; k = k + 1) begin
            @(posedge clk); #1;
            while (!in_ready) begin @(posedge clk); #1; end
            in_valid = 1'b1; in_data = mkbeat(k);
            @(posedge clk); #1;
            in_valid = 1'b0;
        end
        repeat (80) @(posedge clk);

        $display("---- %0s: shift=%0d, 吐出 %0d 字节 ----", tag, $signed({{27{sh[4]}}, sh}), got_n);
        for (p = 0; p < got_n; p = p + 1) begin
            if (got[p] !== exp_byte(p)) begin
                if (bad < 3)
                    $display("  FAIL 第 %0d 字节 = %02h，期望 %02h", p, got[p], exp_byte(p));
                bad = bad + 1;
            end
        end
        if (bad == 0) $display("  PASS  %0d 字节全部正确", got_n);
        else begin
            $display("  FAIL  共 %0d 个字节错", bad);
            err_cnt = err_cnt + 1;
        end
    end
endtask

initial begin
    err_cnt = 0;
    run_case(5'd0,  "shift=0");
    run_case(5'd6,  "shift=+6(丢2像素)");
    run_case(5'd30, "shift=-2(补2像素)");   // 5'd30 即 -2 的补码
    run_case(5'd3,  "shift=+3(丢1像素)");
    run_case(5'd31, "shift=-1(补1像素)");
    $display("==== err_cnt = %0d ====", err_cnt);
    $finish;
end

initial begin #2_000_000; $display("FAIL 全局超时"); $finish; end
endmodule
