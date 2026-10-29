//==============================================================================
//  tb_imageCombine.v —— 图片合成级的行为验证
//------------------------------------------------------------------------------
//  验三件事（对照 imageCombine.v 文件头）：
//    1) 旁路：line_fx_en = 0 时 in 直通 out（data / mask / α 三样都原样）
//    2) 合成：line_image_next 之前写缓冲、之后逐拍合；颜色按"上面那张(图二)这一字节要不要写"选 ——
//             要写就用图二，被抠掉就用缓冲里的图一（★ ck 判在合成【之前】，每张图各判各的）、
//             mask = 两张相或、α 跟着"这个像素最终取了谁"
//    3) line_imgRow_free：填缓冲 / 合成期间是 0，上游两趟排空（line_src_free）之后才回 1
//  ★ 素材源给图二第一拍至少比 line_image_next 晚 2 拍（它自己 RAM 读 + Decoder 的固有延迟），
//    本 TB 按这个时序给（先空 2 拍再给图二）
//==============================================================================
`timescale 1ns/100ps

module tb_imageCombine;

localparam CLK_HALF = 5.0;

reg          clk, rst_n;

// ---- 行任务 / 源侧 ----
reg          line_start;
reg          line_fx_en;
reg          line_colorKey_en;                       // 命令级 ck 开关（不开 ⇒ 上面那张整张盖住下面那张）
reg          line_fx_over;                           // 1 = 上面那张在上（默认，= 老行为）
reg          line_image_next;
reg          line_src_free;

// ---- 入口（素材源 → 本模块）----
reg  [95:0]  source_data;
reg  [11:0]  source_mask;
reg  [31:0]  source_alpha;
reg          source_valid;
wire         source_ready;

// ---- 出口（→ Compositor）----
wire [95:0]  imgCombine_data;
wire [11:0]  imgCombine_mask;
wire [31:0]  imgCombine_alpha;
wire         imgCombine_valid;
reg          imgCombine_ready;
wire         line_imgRow_free;

integer      err_cnt;

imageCombine #(
    .PIXEL_WIDTH    (96),
    .PIXEL_BYTES    (12),
    .PIXELS         (4),
    .ALPHA_WIDTH    (8),
    .ROW_PIXELS     (1920)
) u_dut (
    .clk            (clk),
    .rst_n          (rst_n),
    .line_start     (line_start),
    .line_fx_en     (line_fx_en),
    .line_colorKey_en          (line_colorKey_en),
    .line_fx_over              (line_fx_over),
    .line_solid_en            (1'b0),
    .line_solid_data          (96'd0),
    .line_solid_alpha         (32'd0),
    .line_tint                (3'd0),
    .line_image_next     (line_image_next),
    .line_src_free       (line_src_free),
    .source_data        (source_data),
    .source_mask        (source_mask),
    .source_alpha       (source_alpha),
    .source_valid       (source_valid),
    .source_ready       (source_ready),
    .imgCombine_data       (imgCombine_data),
    .imgCombine_mask       (imgCombine_mask),
    .imgCombine_alpha      (imgCombine_alpha),
    .imgCombine_valid      (imgCombine_valid),
    .imgCombine_ready      (imgCombine_ready),
    .line_imgRow_free       (line_imgRow_free)
);

task tick; begin @(posedge clk); #1; end endtask

// 给一拍入口数据（保持到下一个 tick）
task drive_in;
    input [95:0] d; input [11:0] m; input [31:0] a;
    //   ★ 末尾这个 #1 不能省：组合输出是连续赋值驱动的，赋值后要等它传播完再比，
    //     否则读到的是上一拍的旧值（delta 竞争）
    begin source_data = d; source_mask = m; source_alpha = a; source_valid = 1'b1; #1; end
endtask

task end_in; begin source_valid = 1'b0; source_data = 96'd0; source_mask = 12'd0; source_alpha = 32'd0; end endtask

// 比对出口
task check_out;
    input [95:0] ed; input [11:0] em; input [31:0] ea;
    input [8*24-1:0] tag;
    begin
        if (imgCombine_data !== ed || imgCombine_mask !== em || imgCombine_alpha !== ea) begin
            $display("FAIL [%0s]: data=%h mask=%h alpha=%h", tag, imgCombine_data, imgCombine_mask, imgCombine_alpha);
            $display("      期望    : data=%h mask=%h alpha=%h", ed, em, ea);
            err_cnt = err_cnt + 1;
        end
        else $display("PASS [%0s]", tag);
    end
endtask

task check_bits;
    input cond;
    input [8*24-1:0] tag;
    begin
        if (!cond) begin $display("FAIL [%0s]", tag); err_cnt = err_cnt + 1; end
        else       $display("PASS [%0s]", tag);
    end
endtask

// 图一第 n 拍：每字节 = n*16 + i
function [95:0] img_a; input integer n; integer i;
    begin img_a = 96'd0; for (i = 0; i < 12; i = i + 1) img_a[i*8 +: 8] = n*16 + i; end
endfunction
// 图二第 n 拍：每字节 = 0x80 + n*16 + i
function [95:0] img_b; input integer n; integer i;
    begin img_b = 96'd0; for (i = 0; i < 12; i = i + 1) img_b[i*8 +: 8] = 8'h80 + n*16 + i; end
endfunction
// 合成期望：图二 mask=0x0FF（字节 0..7 不透明）⇒ 字节 0..7 用图二，其余用图一
function [95:0] exp_mix; input integer n; integer i;
    begin exp_mix = 96'd0;
          for (i = 0; i < 12; i = i + 1) exp_mix[i*8 +: 8] = (i < 8) ? (8'h80 + n*16 + i) : (n*16 + i);
    end
endfunction

integer n;
initial begin
    clk = 0; forever #(CLK_HALF) clk = ~clk;
end

initial begin
    err_cnt = 0;
    rst_n = 1'b0;
    line_start = 0; line_fx_en = 0; line_colorKey_en = 1'b1; line_fx_over = 1'b1;
    line_image_next = 0; line_src_free = 0;
    source_data = 96'd0; source_mask = 12'd0; source_alpha = 32'd0; source_valid = 1'b0;
    imgCombine_ready = 1'b1;
    repeat (5) tick;
    rst_n = 1'b1;
    repeat (3) tick;

    //==========================================================================
    $display("---- 用例1: 旁路（line_fx_en = 0）----");
    //==========================================================================
    line_fx_en = 1'b0;
    line_start = 1'b1; tick; line_start = 1'b0; tick;
    check_bits(line_imgRow_free === 1'b1, "case1-row-free-1");
    for (n = 0; n < 3; n = n + 1) begin
        drive_in(img_a(n), 12'hFFF, 32'h1111_1111);
        check_bits(imgCombine_valid === 1'b1, "case1-out-valid");
        check_out(img_a(n), 12'hFFF, 32'h1111_1111, "case1-bypass");
        tick;
    end
    end_in; tick;

    //--------------------------------------------------------------------------
    $display("---- 用例1b: 旁路 + ck 关 ⇒ 掩码恒全 1（每个字节都写）----");
    //--------------------------------------------------------------------------
    line_colorKey_en = 1'b0;
    for (n = 0; n < 2; n = n + 1) begin
        drive_in(img_a(n), 12'h0FF, 32'h1111_1111);
        check_out(img_a(n), 12'hFFF, 32'h1111_1111, "case1b-bypass-ck-off");
        tick;
    end
    end_in; tick;
    line_colorKey_en = 1'b1;

    //==========================================================================
    $display("---- 用例2: 合成（图一 3 拍 → line_image_next → 图二 3 拍）----");
    //==========================================================================
    line_fx_en = 1'b1;
    line_start = 1'b1; tick; line_start = 1'b0; tick;
    check_bits(line_imgRow_free === 1'b0, "case2-row-free-during-fill");
    check_bits(imgCombine_valid === 1'b0, "case2-no-out-during-fill");

    // 图一那一趟：3 拍，mask 全不透明
    for (n = 0; n < 3; n = n + 1) begin
        drive_in(img_a(n), 12'hFFF, 32'h1111_1111);
        tick;
    end
    end_in; tick;

    // 上游说：图一到此为止（同时上游开始读图二）
    line_image_next = 1'b1; tick; line_image_next = 1'b0;
    check_bits(line_imgRow_free === 1'b0, "case2-row-free-during-combine");
    tick; tick;                                  // 素材源的固有延迟（RAM 读 + Decoder）

    // 图二那一趟：3 拍，只有字节 0..7 不透明
    for (n = 0; n < 3; n = n + 1) begin
        drive_in(img_b(n), 12'h0FF, 32'h2222_2222);
        while (!source_ready) tick;
        tick; // the elastic input slot accepts this foreground/background pair
        end_in;
        while (!imgCombine_valid) tick;
        check_bits(imgCombine_valid === 1'b1, "case2-out-valid");
        //   颜色：图二 mask=1 的字节(0..7) 用图二；被抠掉的字节(8..11) 露出图一
        //   mask = 两张相或 = FFF（两张都写满）
        //   α：像素 3（字节 9..11）整块由图一给 ⇒ 用图一的 α，其余用图二的
        check_out(exp_mix(n), 12'hFFF, 32'h1122_2222, "case2-mix");
        if (n == 1) begin
            imgCombine_ready = 0;
            end_in;
            repeat (3) begin
                tick;
                check_bits(imgCombine_valid === 1'b1, "case2-stall-valid");
                check_out(exp_mix(n), 12'hFFF, 32'h1122_2222, "case2-stall-payload");
            end
            imgCombine_ready = 1;
        end
    end
    end_in; tick;

    // 上游两趟都排空 ⇒ 这一行的活干完了
    line_src_free = 1'b1; tick; line_src_free = 1'b0; tick;
    check_bits(line_imgRow_free === 1'b1, "case2-row-free-after-done");

    //==========================================================================
    $display("---- 用例3: 图二整拍被 ck 抠掉(mask=0) —— 合成结果 = 图一 ----");
    //==========================================================================
    line_start = 1'b1; tick; line_start = 1'b0; tick;
    for (n = 0; n < 2; n = n + 1) begin drive_in(img_a(n), 12'hFFF, 32'h1111_1111); tick; end
    end_in; tick;
    line_image_next = 1'b1; tick; line_image_next = 1'b0;
    tick; tick;
    for (n = 0; n < 2; n = n + 1) begin
        drive_in(img_b(n), 12'h000, 32'h2222_2222);
        while (!source_ready) tick;
        tick;
        end_in;
        while (!imgCombine_valid) tick;
        //   ★ 图二整拍被 ck 抠掉 ⇒ 这一拍完全由图一给（颜色/掩码/α 三样都跟图一）
        check_out(img_a(n), 12'hFFF, 32'h1111_1111, "case3-upper-transparent");
    end
    end_in; tick;
    line_src_free = 1'b1; tick; line_src_free = 1'b0; tick;

    //==========================================================================
    $display("---- 用例4: 合成 + ck 关（图二掩码 0x0FF 不生效）⇒ 上面那张整张赢 ----");
    //==========================================================================
    line_colorKey_en = 1'b0;
    line_start = 1'b1; tick; line_start = 1'b0; tick;
    for (n = 0; n < 2; n = n + 1) begin drive_in(img_a(n), 12'hFFF, 32'h1111_1111); tick; end
    end_in; tick;
    line_image_next = 1'b1; tick; line_image_next = 1'b0;
    tick; tick;
    for (n = 0; n < 2; n = n + 1) begin
        drive_in(img_b(n), 12'h0FF, 32'h2222_2222);
        while (!source_ready) tick;
        tick;
        end_in;
        while (!imgCombine_valid) tick;
        //   ★ ck 关 ⇒ 掩码不当判据：颜色/掩码/α 全跟图二（= 老版本"上面那张全赢"）
        check_out(img_b(n), 12'hFFF, 32'h2222_2222, "case4-ck-off-hard-cover");
    end
    end_in; tick;
    line_src_free = 1'b1; tick; line_src_free = 1'b0; tick;
    line_colorKey_en = 1'b1;

    //==========================================================================
    $display("---- 结果 ----");
    if (err_cnt == 0) $display("*** 全部通过 ***");
    else              $display("*** 失败: err_cnt = %0d ***", err_cnt);
    $display("SUMMARY: err_cnt=%0d  RESULT=%0s", err_cnt, (err_cnt == 0) ? "ALL-PASS" : "FAIL");
    $finish;
end

initial begin
    #200000;
    $display("FAIL: 超时（仿真跑了 200 us 还没结束）");
    $finish;
end

endmodule
