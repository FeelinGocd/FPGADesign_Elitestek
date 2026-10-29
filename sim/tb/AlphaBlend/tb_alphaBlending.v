//==============================================================================
//  tb_alphaBlending.v —— alphaBlending 自检：逐字节和【软件整数模型】对拍
//------------------------------------------------------------------------------
//  模型（和 RTL 用不同的写法，避免"自己跟自己对"）：
//      d = fg − bg
//      q = |d| × α / 255        ← 整数除法，向零取整
//      out = bg + (d<0 ? −q : +q)
//
//  另外两条【与公式无关】的硬性质，专门抓"参数边界"：
//      α = 255 ⇒ out 必须逐位等于 fg   （开 α 且全不透明 == REPLACE）
//      α = 0   ⇒ 掩码全 0（一个字节都不写），且 out == bg
//
//  组合：fg/bg 取 {0,1,127,128,254,255}，α 取 {0,1,64,128,192,254,255}
//        ⇒ 6×6×7 = 252 组 × 12 字节 = 3024 次比对
//==============================================================================
`timescale 1ns/1ps

module tb_alphaBlending;

localparam integer PIXELS = 4;
localparam integer BPP    = 3;
localparam integer AW     = 8;

reg  [95:0]  fg_data;
reg  [31:0]  fg_alpha;
reg  [95:0]  bg_data;
wire [95:0]  blend_data;
wire [11:0]  blend_mask;
reg clk = 1'b0;
always #1.667 clk = ~clk;
reg rst_n = 1'b0;
reg in_valid = 1'b0;
reg out_ready = 1'b0;
wire in_ready, out_valid, pipeline_free;

alphaBlending #(
    .PIXEL_WIDTH        (96     ),
    .PIXELS             (PIXELS ),
    .BYTES_PER_PIXEL    (BPP    ),
    .ALPHA_WIDTH        (AW     )
) dut (
    .clk                (clk),
    .rst_n              (rst_n),
    .line_start         (1'b0),
    .line_done          (1'b0),
    .fg_data            (fg_data   ),
    .fg_alpha           (fg_alpha  ),
    .bg_data            (bg_data   ),
    .fg_mask            (12'hfff),
    .in_valid           (in_valid),
    .in_ready           (in_ready),
    .blend_data         (blend_data),
    .blend_mask         (blend_mask),
    .out_valid          (out_valid),
    .out_ready          (out_ready),
    .pipeline_free      (pipeline_free)
);

integer err = 0, nchk = 0;
integer p, c, m;                        /* ★ 别用 i/k：task 里也用它们会打架 */
reg [7:0] f, b, a, exp_b, got_b;
integer   d, q;
reg [7:0] fset [0:5];
reg [7:0] aset [0:6];

//------------------------------------------------------------------------------
// 软件模型
//------------------------------------------------------------------------------
task model;
    input  [7:0] fv, bv, av;
    output [7:0] ov;
    integer dd, qq;
    begin
        dd = fv - bv;
        if (dd < 0) qq = ((-dd) * av) / 255;
        else        qq = ( dd  * av) / 255;
        ov = (dd < 0) ? (bv - qq) : (bv + qq);
    end
endtask

//------------------------------------------------------------------------------
// 把 4 个像素、3 个通道全填成同一组 (f, b, a)，然后逐字节比
//------------------------------------------------------------------------------
task one_case;
    input [7:0] fv, bv, av;
    //   ★ 循环变量必须和外层【不同名】：Verilog 的 task 共享模块命名空间，
    //     用同名的 i/k 会把调用处的循环变量改掉 ⇒ 死循环（这个 TB 踩过）
    integer ii, kk, timeout;
    begin
        @(negedge clk);
        for (ii = 0; ii < PIXELS; ii = ii + 1) begin
            fg_alpha[ii*AW +: AW] = av;
            for (kk = 0; kk < BPP; kk = kk + 1) begin
                fg_data[(ii*BPP + kk)*8 +: 8] = fv;
                bg_data[(ii*BPP + kk)*8 +: 8] = bv;
            end
        end
        in_valid = 1'b1;
        @(posedge clk);
        timeout = 0;
        while (!in_ready && timeout < 100) begin
            @(posedge clk);
            timeout = timeout + 1;
        end
        if (!in_ready) begin
            err = err + 1;
            $display("FAIL: alpha input handshake timeout");
        end
        @(negedge clk); in_valid = 1'b0;
        timeout = 0;
        while (!out_valid && timeout < 100) begin
            @(negedge clk);
            timeout = timeout + 1;
        end
        if (!out_valid) begin
            err = err + 1;
            $display("FAIL: alpha output handshake timeout");
        end
        #0.010;
        for (ii = 0; ii < PIXELS; ii = ii + 1)
            for (kk = 0; kk < BPP; kk = kk + 1) begin
                model(fv, bv, av, exp_b);
                got_b = blend_data[(ii*BPP + kk)*8 +: 8];
                nchk = nchk + 1;
                if (got_b !== exp_b) begin
                    err = err + 1;
                    if (err <= 8)
                        $display("  [ERR] fg=%02x bg=%02x a=%02x  pix%0d ch%0d: got %02x exp %02x",
                                 fv, bv, av, ii, kk, got_b, exp_b);
                end
                if ((av == 8'd0) && (blend_mask[ii*BPP + kk] !== 1'b0)) begin
                    err = err + 1;
                    if (err <= 8) $display("  [ERR] a=0 但掩码不是 0 (pix%0d ch%0d)", ii, kk);
                end
                if ((av == 8'd255) && (got_b !== fv)) begin
                    err = err + 1;
                    if (err <= 8) $display("  [ERR] a=255 但结果 != fg (got %02x fg %02x)", got_b, fv);
                end
            end
        @(negedge clk); out_ready = 1'b1;
        @(posedge clk);
        @(negedge clk); out_ready = 1'b0;
        #0.010;
        if (pipeline_free !== 1'b1) begin
            err = err + 1;
            $display("FAIL: alpha did not empty after its single result");
        end
    end
endtask

initial begin
    fset[0]=8'd0; fset[1]=8'd1; fset[2]=8'd127; fset[3]=8'd128; fset[4]=8'd254; fset[5]=8'd255;
    aset[0]=8'd0; aset[1]=8'd1; aset[2]=8'd64;  aset[3]=8'd128; aset[4]=8'd192; aset[5]=8'd254; aset[6]=8'd255;

    fg_data = 96'd0; bg_data = 96'd0; fg_alpha = 32'd0;
    repeat (3) @(negedge clk);
    rst_n = 1'b1;
    $display("---- alphaBlending: 软件模型对拍 ----");
    for (p = 0; p < 6; p = p + 1)
        for (c = 0; c < 6; c = c + 1)
            for (m = 0; m < 7; m = m + 1)
                one_case(fset[p], fset[c], aset[m]);

    $display("  采样 %0d 字节, 不一致 %0d", nchk, err);
    if (err == 0) $display("SUMMARY: ALPHA-BLEND ALL-PASS");
    else          $display("SUMMARY: err_cnt=%0d RESULT=FAIL", err);
    $finish;
end

endmodule
