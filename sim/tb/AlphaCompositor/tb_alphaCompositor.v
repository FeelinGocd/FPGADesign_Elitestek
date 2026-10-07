//==============================================================================
//  tb_alphaCompositor.v —— 合成器级的 alpha 混合 TB
//------------------------------------------------------------------------------
//  被测：factory/Compositor.v（真的那个，里面例化了 process/alphaBlending.v）
//
//  测什么（合成器这一级能出的错都在这里）
//    ① 模式 = ALPHA（两路同时用）时出的是【混合结果】，不是随便选一路
//    ② alpha=255 ⇒ 逐字节等于 REPLACE（前景原样）；alpha=0 ⇒ 背景原样 + 掩码全 0
//    ③ alpha=128 之类 ⇒ 和整数模型逐字节一致
//    ④ 一拍 4 个像素、【一个像素一个 alpha】：掩码是按像素给的（一个像素 3 位同生共死）
//    ⑤ ck 与 alpha 同时开 ⇒ 掩码相与；只开 ck ⇒ 老行为（用源给的掩码）
//    ⑥ 混合模式的两路 valid 要相与
//    ⑦ 老行为回归：00(FILL) / 01(源①) / 10(源②) 的选路和掩码一位不差
//    ⑧ buf_free：两路同时用时【两路都得 free】才能开新行
//
//  模型（和 alphaBlending.v 同一份整数算法）
//      每通道：d = |fg − bg|, p = d*alpha, q = (p + 1 + (p>>8)) >> 8, out = bg ± q
//      ★ 算法本身已由 tb_alphaBlending 对拍 252 组；本 TB 管"接线/选路/掩码/握手"。
//
//  跑法：sim/tb/run_tb.ps1 -TbDir AlphaCompositor -TbTop tb_alphaCompositor
//==============================================================================
`timescale 1ns/100ps

module tb_alphaCompositor;

localparam PIXEL_WIDTH = 96;
localparam ALPHA_WIDTH = 8;

//==============================================================================
// 时钟 / 复位
//==============================================================================
reg     clk, rst_n;
always #5 clk = ~clk;

//==============================================================================
// DUT 信号
//==============================================================================
reg  [6:0]      line_flags;                      // ★ 模式就在这里面（flags[2:0]），不再有 line_src_en
reg             line_done;                       // 写侧完成（单拍脉冲）：源手上的尾巴靠它放开
reg  [23:0]     line_param;
reg             line_start;

reg  [95:0]     src0_data;
reg             src0_valid;
wire            src0_rd_en;
reg  [95:0]     src1_data;
reg  [11:0]     src1_mask;
reg  [31:0]     src1_alpha;
reg             src1_valid;
wire            src1_rd_en;
reg             src0_free, src1_free;
wire            buf_free;

wire [95:0]     pixel_data;
wire [11:0]     pixel_mask;
wire            pixel_valid;
reg             pixel_ready;

Compositor #(
    .PIXEL_WIDTH        (PIXEL_WIDTH),
    .ALPHA_WIDTH        (ALPHA_WIDTH)
) u_dut (
    .clk            (clk),
    .rst_n          (rst_n),
    .line_start     (line_start),
    .line_flags     (line_flags),
    .line_done      (line_done),
    .line_param     (line_param),
    .src0_data      (src0_data),
    .src0_valid     (src0_valid),
    .src0_rd_en     (src0_rd_en),
    .src1_data      (src1_data),
    .src1_mask      (src1_mask),
    .src1_alpha     (src1_alpha),
    .src1_valid     (src1_valid),
    .src1_rd_en     (src1_rd_en),
    .src0_free      (src0_free),
    .src1_free      (src1_free),
    .buf_free       (buf_free),
    .pixel_data     (pixel_data),
    .pixel_mask     (pixel_mask),
    .pixel_valid    (pixel_valid),
    .pixel_ready    (pixel_ready)
);

//==============================================================================
// 账
//==============================================================================
integer     err_cnt;
integer     case_no;

//==============================================================================
// 参考模型
//==============================================================================
//   单通道混合（和 alphaBlending.v 同一份整数算法）
function [7:0] blend_ch;
    input [7:0] fg;
    input [7:0] bg;
    input [7:0] al;
    reg   [15:0] p;
    reg   [15:0] q;
    reg   [8:0]  d;
    begin
        d = (fg > bg) ? (fg - bg) : (bg - fg);
        p = d * al;
        q = (p + 16'd1 + (p >> 8)) >> 8;
        blend_ch = (fg > bg) ? (bg + q[7:0]) : (bg - q[7:0]);
    end
endfunction

//   期望的混合结果 + 掩码（逐像素逐通道）
task expect_mix;
    input  [95:0] bg;
    input  [95:0] fg;
    input  [31:0] al;
    output [95:0] d;
    output [11:0] m;
    integer       c;
    reg    [7:0]  a;
    reg    [7:0]  fgr, fgg, fgb, bgr, bgg, bgb;
    begin
        d = 96'd0;
        m = 12'd0;
        for (c = 0; c < 4; c = c + 1) begin
            a   = al [c*ALPHA_WIDTH +: 8];
            fgr = fg [c*24 + 16 +: 8];
            fgg = fg [c*24 +  8 +: 8];
            fgb = fg [c*24       +: 8];
            bgr = bg [c*24 + 16 +: 8];
            bgg = bg [c*24 +  8 +: 8];
            bgb = bg [c*24       +: 8];
            d[c*24       +: 8] = blend_ch(fgb, bgb, a);
            d[c*24 +  8  +: 8] = blend_ch(fgg, bgg, a);
            d[c*24 + 16  +: 8] = blend_ch(fgr, bgr, a);
            if (a != 8'd0)
                m[c*3 +: 3] = 3'b111;           // 一个像素的 3 个字节同生共死
        end
    end
endtask

//   期望的 FILL 字：常数色原样铺（★ 不再按目的相位轮转，见 Compositor.v 文件头）
//     "流的第 0 个字节落在目的块的第 0 个字节上"是 DDRPacker 行首预置 cnt_byte 保证的，
//     所以这里和 doff 无关。绝对颜色由 Factory_ck_tb 的 chk_fill_abs 逐字节扫过。
function [95:0] expect_fill;
    input [23:0] color;
    input [3:0]  doff;
    begin
        expect_fill = {4{color}};
    end
endfunction

//==============================================================================
// 驱动任务
//==============================================================================
//   发一行任务（line_start 单拍）：
//     mode   = flags[2:0]（0=FILL / 1=REPLACE / 2=ALPHA / 3=CKEY）
//     pal    = flags[3]（1 = 源是 PRAM 精灵）
//     ck     = flags[4]（颜色键控使能）
task do_line;
    input [2:0] mode;
    input       pal;
    input       ck;
    input [3:0] doff;
    begin
        @(negedge clk);
        line_flags    = {2'b00, ck, pal, mode};     // [6:5]=镜像（本 TB 不开）, [4]=ck, [3]=src, [2:0]=模式
        line_done     = 1'b0;
        //   ★ 目的地址这根线本模块不要了（拍内相位归 DDRPacker），doff 只是保留参数
        line_param    = 24'h336699;
        line_start    = 1'b1;
        @(negedge clk);
        line_start    = 1'b0;
        #1;
    end
endtask

//   摆一拍数据（组合逻辑，稳定后就能查输出）
task beat;
    input [95:0] bg;
    input [95:0] fg;
    input [31:0] al;
    input [11:0] fm;
    input        v0;
    input        v1;
    begin
        @(negedge clk);
        src0_data = bg;   src0_valid = v0;
        src1_data = fg;   src1_alpha = al;   src1_mask = fm;   src1_valid = v1;
        #1;
    end
endtask

//   只查 valid（数据/掩码是组合输出，valid=0 时下游根本不看它们）
task chk_v;
    input         e_valid;
    input [8*40-1:0] tag;
    begin
        if (pixel_valid !== e_valid) begin
            err_cnt = err_cnt + 1;
            $display("  [FAIL] %0s : valid got %b exp %b", tag, pixel_valid, e_valid);
        end
    end
endtask

//   检查
task chk;
    input [95:0]  e_data;
    input [11:0]  e_mask;
    input         e_valid;
    input [8*40-1:0] tag;
    begin
        if (pixel_data !== e_data) begin
            err_cnt = err_cnt + 1;
            $display("  [FAIL] %0s : data got %024x exp %024x", tag, pixel_data, e_data);
        end
        if (pixel_mask !== e_mask) begin
            err_cnt = err_cnt + 1;
            $display("  [FAIL] %0s : mask got %03x exp %03x", tag, pixel_mask, e_mask);
        end
        if (pixel_valid !== e_valid) begin
            err_cnt = err_cnt + 1;
            $display("  [FAIL] %0s : valid got %b exp %b", tag, pixel_valid, e_valid);
        end
    end
endtask

//==============================================================================
// 测试向量
//==============================================================================
//   背景：4 个像素 (R,G,B) = (10,20,30) / (40,50,60) / (70,80,90) / (100,110,120)
//   前景：4 个像素 (R,G,B) = (200,210,220) / (0,0,0) / (255,255,255) / (130,140,150)
//   ★ 字节序：像素 i 占 [i*24 +: 24]，【低字节 = B】
`define PX(r,g,b) {r, g, b}

localparam [95:0] BG_A = { `PX(100,110,120), `PX(70,80,90), `PX(40,50,60), `PX(10,20,30) };
localparam [95:0] FG_A = { `PX(130,140,150), `PX(255,255,255), `PX(0,0,0), `PX(200,210,220) };

//==============================================================================
// 主流程
//==============================================================================
reg [95:0] e_d;
reg [11:0] e_m;
reg [11:0] ck_win;

initial begin
    clk = 1'b0; rst_n = 1'b0; err_cnt = 0; case_no = 0;
    line_start = 1'b0; line_flags = 7'd0; line_done = 1'b0;
    line_param = 24'd0;
    src0_data = 96'd0; src0_valid = 1'b0;
    src1_data = 96'd0; src1_alpha = 32'd0; src1_mask = 12'hfff; src1_valid = 1'b0;
    src0_free = 1'b1; src1_free = 1'b1; pixel_ready = 1'b1;

    repeat (4) @(negedge clk);
    rst_n = 1'b1;
    @(negedge clk);

    //------------------------------------------------------------------
    $display("---- 1) alpha=255 : 逐字节等于 REPLACE ----");
    case_no = 1;
    do_line(3'd2, 1'b1, 1'b0, 4'd0);
    beat(BG_A, FG_A, 32'hFFFFFFFF, 12'hfff, 1'b1, 1'b1);
    expect_mix(BG_A, FG_A, 32'hFFFFFFFF, e_d, e_m);
    chk(e_d, e_m, 1'b1, "a255");
    if (pixel_data !== FG_A) begin
        err_cnt = err_cnt + 1;
        $display("  [FAIL] a255 : 和 REPLACE 不一致（应该是前景原样）");
    end

    //------------------------------------------------------------------
    $display("---- 2) alpha=0 : 背景原样 + 掩码全 0（一个字节都不写）----");
    case_no = 2;
    do_line(3'd2, 1'b1, 1'b0, 4'd0);
    beat(BG_A, FG_A, 32'h00000000, 12'hfff, 1'b1, 1'b1);
    expect_mix(BG_A, FG_A, 32'h00000000, e_d, e_m);
    chk(e_d, e_m, 1'b1, "a0");

    //------------------------------------------------------------------
    $display("---- 3) alpha 逐像素不同（0/64/128/255）----");
    case_no = 3;
    do_line(3'd2, 1'b1, 1'b0, 4'd0);
    beat(BG_A, FG_A, 32'hFF80_4000, 12'hfff, 1'b1, 1'b1);   // 像素3=FF 2=80 1=40 0=00
    expect_mix(BG_A, FG_A, 32'hFF80_4000, e_d, e_m);
    chk(e_d, e_m, 1'b1, "a-mix");
    //   掩码按像素：像素 0 的 alpha=0 ⇒ 【那个像素的 3 个字节】一起关掉 ⇒ 低 3 位为 0
    if (pixel_mask !== 12'hff8) begin
        err_cnt = err_cnt + 1;
        $display("  [FAIL] a-mix : 掩码应该是 ff8（像素0 的 3 字节全关），实际 %03x", pixel_mask);
    end

    //------------------------------------------------------------------
    $display("---- 4) ck 与 alpha 同时开 : 掩码相与 ----");
    case_no = 4;
    do_line(3'd2, 1'b1, 1'b1, 4'd0);                              // ck 开
    ck_win = 12'b0000_1111_1111;                             // 源只给前 8 个字节写
    beat(BG_A, FG_A, 32'hFF80_4000, ck_win, 1'b1, 1'b1);
    //   期望掩码 = 源掩码 & alpha 掩码(ff8)
    if (pixel_mask !== (ck_win & 12'hff8)) begin
        err_cnt = err_cnt + 1;
        $display("  [FAIL] ck&alpha : 掩码 got %03x exp %03x", pixel_mask, ck_win & 12'hff8);
    end
    expect_mix(BG_A, FG_A, 32'hFF80_4000, e_d, e_m);
    chk(e_d, e_m & ck_win, 1'b1, "ck&alpha");

    //------------------------------------------------------------------
    $display("---- 5) 混合模式两路 valid 相与 ----");
    case_no = 5;
    do_line(3'd2, 1'b1, 1'b0, 4'd0);
    beat(BG_A, FG_A, 32'hFFFFFFFF, 12'hfff, 1'b0, 1'b1);
    chk_v(1'b0, "v0=0");                                     // 背景没货 ⇒ 不算一拍
    beat(BG_A, FG_A, 32'hFFFFFFFF, 12'hfff, 1'b1, 1'b0);
    chk_v(1'b0, "v1=0");                                     // 前景没货 ⇒ 同样不算
    beat(BG_A, FG_A, 32'hFFFFFFFF, 12'hfff, 1'b1, 1'b1);
    chk(FG_A, 12'hfff, 1'b1, "v=11");

    //------------------------------------------------------------------
    $display("---- 6) 取数节奏 : rd_en = 本行用到那一路 & pixel_ready ----");
    case_no = 6;
    do_line(3'd2, 1'b1, 1'b0, 4'd0);
    beat(BG_A, FG_A, 32'hFFFFFFFF, 12'hfff, 1'b1, 1'b1);
    pixel_ready = 1'b1; #1;
    if (src0_rd_en !== 1'b1 || src1_rd_en !== 1'b1) begin
        err_cnt = err_cnt + 1;
        $display("  [FAIL] rd_en : 两路都该取 (got %b/%b)", src0_rd_en, src1_rd_en);
    end
    pixel_ready = 1'b0; #1;
    if (src0_rd_en !== 1'b0 || src1_rd_en !== 1'b0) begin
        err_cnt = err_cnt + 1;
        $display("  [FAIL] rd_en : 下游不要就不该取 (got %b/%b)", src0_rd_en, src1_rd_en);
    end
    pixel_ready = 1'b1;
    do_line(3'd1, 1'b1, 1'b0, 4'd0);                              // 只用源② ⇒ 源① 一根都不取
    pixel_ready = 1'b1; #1;
    if (src0_rd_en !== 1'b0 || src1_rd_en !== 1'b1) begin
        err_cnt = err_cnt + 1;
        $display("  [FAIL] rd_en(10) : got %b/%b exp 0/1", src0_rd_en, src1_rd_en);
    end

    //------------------------------------------------------------------
    $display("---- 7) buf_free : 两路同时用时两路都得 free ----");
    case_no = 7;
    do_line(3'd2, 1'b1, 1'b0, 4'd0);
    src0_free = 1'b1; src1_free = 1'b1; #1;
    if (buf_free !== 1'b1) begin
        err_cnt = err_cnt + 1;
        $display("  [FAIL] buf_free : 两路都 free 应该是 1");
    end
    src0_free = 1'b0; src1_free = 1'b1; #1;
    if (buf_free !== 1'b0) begin
        err_cnt = err_cnt + 1;
        $display("  [FAIL] buf_free : 源① 没 free 时不该开新行");
    end
    src0_free = 1'b1; src1_free = 1'b0; #1;
    if (buf_free !== 1'b0) begin
        err_cnt = err_cnt + 1;
        $display("  [FAIL] buf_free : 源② 没 free 时不该开新行");
    end
    src0_free = 1'b1; src1_free = 1'b1;
    //   只用一路时，只看那一路
    do_line(3'd1, 1'b0, 1'b0, 4'd0);
    src0_free = 1'b1; src1_free = 1'b0; #1;
    if (buf_free !== 1'b1) begin
        err_cnt = err_cnt + 1;
        $display("  [FAIL] buf_free(01) : 没用源② 就不该看它");
    end
    do_line(3'd0, 1'b0, 1'b0, 4'd0);
    src0_free = 1'b0; src1_free = 1'b0; #1;
    if (buf_free !== 1'b1) begin
        err_cnt = err_cnt + 1;
        $display("  [FAIL] buf_free(00) : FILL 不用源，永远能开新行");
    end
    src0_free = 1'b1; src1_free = 1'b1;

    //------------------------------------------------------------------
    $display("---- 8) 老行为回归 : 00/01/10 三个模式的选路与掩码 ----");
    case_no = 8;
    //   00 = FILL：两路都不看，出常数色（原样铺，不轮转）
    do_line(3'd0, 1'b0, 1'b0, 4'd0);
    beat(BG_A, FG_A, 32'hFFFFFFFF, 12'hfff, 1'b1, 1'b1);
    chk(expect_fill(24'h336699, 4'd0), 12'hfff, 1'b1, "fill ph0");
    do_line(3'd0, 1'b0, 1'b0, 4'd4);                              // dst_off=4：FILL 不受它影响
    beat(BG_A, FG_A, 32'hFFFFFFFF, 12'hfff, 1'b1, 1'b1);
    chk(expect_fill(24'h336699, 4'd4), 12'hfff, 1'b1, "fill ph1");
    do_line(3'd0, 1'b0, 1'b1, 4'd0);                              // FILL 时开 ck 也不该改掩码
    beat(BG_A, FG_A, 32'hFFFFFFFF, 12'h000, 1'b1, 1'b1);
    chk(expect_fill(24'h336699, 4'd0), 12'hfff, 1'b1, "fill+ck");
    //   01 = 源①（DDR）
    do_line(3'd1, 1'b0, 1'b0, 4'd0);
    beat(BG_A, FG_A, 32'hFFFFFFFF, 12'hfff, 1'b1, 1'b1);
    chk(BG_A, 12'hfff, 1'b1, "src0");
    do_line(3'd1, 1'b0, 1'b1, 4'd0);                              // 源① 的 ck 还没做 ⇒ 掩码仍全 1
    beat(BG_A, FG_A, 32'hFFFFFFFF, 12'h000, 1'b1, 1'b1);
    chk(BG_A, 12'hfff, 1'b1, "src0+ck");
    //   10 = 源②（调色板）：开 ck 才用源给的掩码
    do_line(3'd1, 1'b1, 1'b0, 4'd0);
    beat(BG_A, FG_A, 32'h00000000, 12'h123, 1'b1, 1'b1);
    chk(FG_A, 12'hfff, 1'b1, "src1");
    do_line(3'd1, 1'b1, 1'b1, 4'd0);
    beat(BG_A, FG_A, 32'h00000000, 12'h123, 1'b1, 1'b1);
    chk(FG_A, 12'h123, 1'b1, "src1+ck");

    //------------------------------------------------------------------
    $display("---- 9) 取数门控：写完(line_done)之后，源手上的尾巴要能各取各的丢掉 ----");
    //   ★ 这条是上板卡死那个 bug 的回归哨兵：
    //     ① 正常情况下"取走一拍"必须用【和下游同一个条件】(pixel_valid & pixel_ready)，
    //        否则先到的那一路会被取走但没人要地丢掉（PRAM 先到、DDR 后到）。
    //     ② line_done 之后，两路各自把剩下的尾巴丢掉，src_free 才回得来。
    case_no = 9;
    do_line(3'd2, 1'b1, 1'b0, 4'd0);                         // alpha
    //   ① 只有一路有货、下游想要：两路都不该 pop（配不上）
    beat(BG_A, 96'd0, 32'hFFFFFFFF, 12'hfff, 1'b1, 1'b0);    // 源① 有货, 源② 没有
    if (src0_rd_en !== 1'b0) begin
        err_cnt = err_cnt + 1;
        $display("  [FAIL] 配对不齐时源① 被取走了（数据会被白丢）");
    end
    if (pixel_valid !== 1'b0) begin
        err_cnt = err_cnt + 1;
        $display("  [FAIL] 只有一路有货时 pixel_valid 应为 0");
    end
    //   ② 两路都有货、下游也想要 ⇒ 两路一起 pop（这才叫"确实收下"）
    beat(BG_A, FG_A, 32'hFFFFFFFF, 12'hfff, 1'b1, 1'b1);
    if ((src0_rd_en !== 1'b1) || (src1_rd_en !== 1'b1)) begin
        err_cnt = err_cnt + 1;
        $display("  [FAIL] 配对齐了应该两路一起 pop (got %b/%b)", src0_rd_en, src1_rd_en);
    end
    //   ③ 写侧完成后：只剩一路有尾巴，也要能取走（否则 src_free 永远不回）
    @(negedge clk); line_done = 1'b1; @(negedge clk); line_done = 1'b0;
    @(negedge clk);
    src0_data = BG_A; src0_valid = 1'b0;      // 源① 空了（尾巴都在源② 那边）
    src1_data = FG_A; src1_valid = 1'b1; src1_alpha = 32'hFFFFFFFF;
    #1;
    if (src1_rd_en !== 1'b1) begin
        err_cnt = err_cnt + 1;
        $display("  [FAIL] line_done 之后源② 的尾巴应该能丢掉 (rd_en=%b)", src1_rd_en);
    end

    //------------------------------------------------------------------
    repeat (4) @(negedge clk);
    if (err_cnt == 0)
        $display("SUMMARY: alphaCompositor err_cnt=0 RESULT=ALL-PASS");
    else
        $display("SUMMARY: alphaCompositor err_cnt=%0d RESULT=FAIL", err_cnt);
    $finish;
end

endmodule
