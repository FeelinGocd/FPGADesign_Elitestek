////////////////////////////////////////////////////////////////////////////////
// tb_CmdExec.v —— 命令展开验证（命令 → 行任务）
//
//  时序约定（重要）
//      所有驱动与采样都在 posedge 之后 #1 进行 —— DUT 的 NBA 已经落地，
//      不会和 DUT 抢同一个 delta
//
//  为什么不用例化 CmdBuffer
//      CmdExec 的左边是【标准 valid/ready】，所以这里直接手搓命令；
//      行引擎还没写，右边用桩：每收到一个 line_start 就【登记一行】，
//      line_delay 拍之后回一个单拍 line_done。
//      → 两个方向都能单独驱动，出问题不用怀疑上游
//
//  ★ 接口对齐（RTL 现状；旧 TB 有三处按老接口写的地方）
//    1) 合成模式（flags[2:0]）【不再】在 CmdExec 里解码成"要哪几路源"：
//       CmdExec 只把 flags 原样广播，谁要不要动由各源和合成器自己看 flags 判。
//       所以本 TB 查的就是 flags（expect_line 的 "mode" 参数 = flags[2:0]）。
//       所以 FILL 的命令必须给 flags[2:0]=0；旧 TB 给 FILL 塞了 flags=7'h01，
//       在新解码下会变成"要源①"，语义正好反了。BLIT 要 REPLACE 就给 flags[2:0]=1。
//       line_flags 现在是【无条件原样转发】—— FILL 也是一种模式，flags 对它同样有效。
//    2) 没有 src_stride 了 —— 源和目的【同跨距】，由 lineBuffer_mode 在内部派生：
//         全屏 5760 / 分屏 2880（dst 和 src 两个地址都按这同一个值累加）
//    3) 新增 line_buffer_free（下游还有没有空块）—— 发送节奏改由它说了算：
//         发一行 → 只要下游有空就继续发下一行，【不再等 line_done】
//       line_done 仍然只用来判"这条命令真的做完了没"（收够 h 个才回 S_IDLE）
//
//  验什么
//    1) FILL 多行全屏 → 行数对、目的地址按 5760 累加、模式=0、src 填 0、flags/color 原样转发
//    2) 分屏(mode=1)   → 目的地址按 2880 累加
//    3) BLIT(REPLACE)  → 模式=1、源地址【也按 5760 累加】、color 原样转发
//    3b) ALPHA         → 模式=2；★ 源使能已经不在这里解了，本 TB 只钉 flags 本身
//    4) FENCE          → 一个行任务都不发，且后面的命令照收（不卡住）
//    5) 未定义类型     → 同上
//    6) 高为 0         → 同上（不能挂死）
//    7) 单行(h=1)      → 只发一个行任务
//    8) 发送节奏       → line_buffer_free=0 时有空行也不发，拉高后立刻续上（且不等 line_done）
//    9) 连续两条命令   → 上一条落盘完成后下一条立刻被收下（无气泡）
//
//  编译运行(ModelSim)
//    vlog -quiet -work work CmdExec.v tb_CmdExec.v
//    vsim -c -quiet -do "run -all; quit -f" work.tb_CmdExec
////////////////////////////////////////////////////////////////////////////////
`timescale 1ns/100ps

module tb_CmdExec;

localparam CLK_HALF = 5.0;

reg          clk, rst_n;

// ---- 命令口 ----
reg          cmd_valid;
wire         cmd_ready;
reg  [2:0]   cmd_type;
reg  [31:0]  cmd_src, cmd_dst;
reg  [15:0]  cmd_w, cmd_h;
reg  [6:0]   cmd_flags;
reg  [23:0]  cmd_color;

// ---- 配置口 ----
//   ★ 只有 lineBuffer_mode：源和目的同跨距，5760/2880 由它在内部派生
//     （旧 TB 的 src_stride 端口已经不存在了）
reg          lineBuffer_mode;

// ---- 行任务口 ----
//   ★ 行任务口：CmdExec 只广播 flags，不再广播"要哪几路源"
wire         line_start;
wire  [31:0] line_dst_addr, line_src_addr;
wire  [15:0] line_w;
wire  [6:0]  line_flags;
wire  [23:0] line_param;
reg          line_done;
reg          line_buffer_free;              // 下游还有没有空块（TB 自己驱动，测发送节奏）

integer err_cnt;
integer line_cnt;
integer wait_cnt;
integer k;

localparam [2:0] CT_FILL  = 3'd1;
localparam [2:0] CT_BLIT  = 3'd2;
localparam [2:0] CT_FENCE = 3'd3;

//   合成模式（cmd_flags[2:0]）
localparam [2:0] MD_FILL    = 3'd0;
localparam [2:0] MD_REPLACE = 3'd1;
localparam [2:0] MD_ALPHA   = 3'd2;

//   行跨距：全屏 5760 / 分屏 2880（和 RTL 的 LINE_BYTES/LINE_BYTES/2 对上）
localparam [31:0] STRIDE_FULL = 32'd5760;
localparam [31:0] STRIDE_HALF = 32'd2880;

CmdExec #(.LINE_BYTES(5760)) u_dut (
    .clk_acc            (clk),
    .rst_acc_n          (rst_n),
    .cmd_valid          (cmd_valid),
    .cmd_ready          (cmd_ready),
    .cmd_type           (cmd_type),
    .cmd_src            (cmd_src),
    .cmd_dst            (cmd_dst),
    .cmd_w              (cmd_w),
    .cmd_h              (cmd_h),
    .cmd_flags          (cmd_flags),
    .cmd_color          (cmd_color),
    .lineBuffer_mode    (lineBuffer_mode),
    .line_start         (line_start),
    .line_dst_addr      (line_dst_addr),
    .line_src_addr      (line_src_addr),
    .line_w             (line_w),
    .line_flags         (line_flags),
    .line_param         (line_param),
    .line_done          (line_done),
    .line_buffer_free   (line_buffer_free)
);

////////////////////////////////////////////////////////////////////////////////
// 节拍: posedge 之后再 #1 —— 本 TB 所有驱动/采样都走它
////////////////////////////////////////////////////////////////////////////////
task tick; begin @(posedge clk); #1; end endtask

////////////////////////////////////////////////////////////////////////////////
// 行任务计数器
////////////////////////////////////////////////////////////////////////////////
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) line_cnt <= 0;
    else if (line_start) line_cnt <= line_cnt + 1;
end

////////////////////////////////////////////////////////////////////////////////
// 写控制器桩: 每收到一个 line_start 就登记一行，line_delay 拍后回一个单拍 line_done
//   ★ 旧 TB 只有一个 srv_busy：那时"发一行等一行"，同时在飞的行最多一个。
//     现在只要 line_buffer_free 高就一路发下去，一屏多行会同时在飞 ——
//     桩必须【每行各回一个 done】，否则 line_done_cnt 永远追不上 h，命令卡在 S_RUN。
//   ★ done_pipe 是一根移位延迟线：line_start 从右边进，line_delay 位处出。
//     换 line_delay 必须等管子里排空（否则在飞的脉冲会被错位成假 done）——
//     用 set_line_delay 统一改。
////////////////////////////////////////////////////////////////////////////////
integer    line_delay;
reg [63:0] done_pipe;

always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        done_pipe <= 64'd0;
        line_done <= 1'b0;
    end
    else begin
        done_pipe <= {done_pipe[62:0], line_start};
        line_done <= done_pipe[line_delay-1];
    end
end

//   改行延迟：先把在飞的 done 全部放出来再改
task set_line_delay;
    input integer d;
    begin
        while (done_pipe !== 64'd0) tick;
        repeat (2) tick;                    // line_done 是寄存器输出，多等两拍
        line_delay = d;
    end
endtask

////////////////////////////////////////////////////////////////////////////////
// 任务
////////////////////////////////////////////////////////////////////////////////
// 推一条命令（标准 valid/ready）；wait_cnt 记录等了几拍
task push_cmd;
    input [2:0]  t;
    input [31:0] s, d;
    input [15:0] w, h;
    input [6:0]  fl;
    input [23:0] col;
    begin
        tick;
        cmd_type = t; cmd_src = s; cmd_dst = d;
        cmd_w = w; cmd_h = h; cmd_flags = fl; cmd_color = col;
        cmd_valid = 1'b1;
        wait_cnt = 0;
        while (!cmd_ready && wait_cnt < 200) begin tick; wait_cnt = wait_cnt + 1; end
        if (!cmd_ready) begin
            $display("FAIL: 命令进不去（等不到 cmd_ready）");
            err_cnt = err_cnt + 1;
        end
        tick;                       // 握手成功这一拍
        cmd_valid = 1'b0;
    end
endtask

// 等一个行任务，逐字段检查
//   ★ emode: 这一行的【合成模式】= flags[2:0]。
//     ★ 注意：CmdExec 现在【不再】把模式解成"要哪几路源"了 —— 那是各源和
//       合成器自己看 flags 的事（见 硬件_加速器.md §1.3）。所以这里查的就是 flags。
task expect_line;
    input [2:0]      emode;
    input [31:0]     edst, esrc;
    input [15:0]     ew;
    input [6:0]      efl;
    input [23:0]     ecol;
    input [8*24-1:0] tag;
    integer wd;
    reg     bad;
    begin
        wd = 0;
        while (!line_start && wd < 400) begin tick; wd = wd + 1; end
        if (!line_start) begin
            $display("FAIL [%0s]: 等不到 line_start", tag);
            err_cnt = err_cnt + 1;
        end
        else begin
            bad = 1'b0;
            if (line_flags[2:0] !== emode) bad = 1'b1;
            if (line_dst_addr !== edst) bad = 1'b1;
            if (line_src_addr !== esrc) bad = 1'b1;
            if (line_w        !== ew)   bad = 1'b1;
            if (line_flags    !== efl)  bad = 1'b1;
            if (line_param    !== ecol) bad = 1'b1;
            if (bad) begin
                $display("FAIL [%0s]: mode=%b dst=%h src=%h w=%0d flags=%h col=%h",
                         tag, line_flags[2:0], line_dst_addr, line_src_addr, line_w, line_flags, line_param);
                $display("      期望: mode=%b dst=%h src=%h w=%0d flags=%h col=%h",
                         emode, edst, esrc, ew, efl, ecol);
                err_cnt = err_cnt + 1;
            end
            else $display("PASS [%0s]", tag);
        end
        // ★ line_start 只持续一拍：采完必须往前走一拍把它消费掉，
        //   否则下一次调用会在同一拍上再采一遍（看到的是同一行的地址）
        tick;
    end
endtask

// 观察 ncyc 拍，确认期间一个行任务都没有
task expect_no_line;
    input integer    ncyc;
    input [8*24-1:0] tag;
    integer kk;
    reg     bad;
    begin
        bad = 1'b0;
        for (kk = 0; kk < ncyc; kk = kk + 1) begin
            tick;
            if (line_start) bad = 1'b1;
        end
        if (bad) begin
            $display("FAIL [%0s]: 不该发行任务，却出现了 line_start", tag);
            err_cnt = err_cnt + 1;
        end
        else $display("PASS [%0s]", tag);
    end
endtask

////////////////////////////////////////////////////////////////////////////////
// 主流程
////////////////////////////////////////////////////////////////////////////////
initial begin
    clk = 1'b0;
    forever #(CLK_HALF) clk = ~clk;
end

initial begin
    err_cnt = 0; line_delay = 2;
    rst_n = 1'b0;
    cmd_valid = 1'b0; cmd_type = 3'd0; cmd_src = 32'd0; cmd_dst = 32'd0;
    cmd_w = 16'd0; cmd_h = 16'd0; cmd_flags = 7'd0; cmd_color = 24'd0;
    lineBuffer_mode = 1'b0;
    line_buffer_free = 1'b1;            // 默认下游有空（节奏用例里再拉低）
    repeat (10) @(posedge clk);
    rst_n = 1'b1;
    repeat (5) @(posedge clk);

    $display("---- 用例1: FILL 100x3 全屏（目的地址按 5760 累加，模式=0）----");
    push_cmd(CT_FILL, 32'd0, 32'h0800_1000, 16'd100, 16'd3, {4'd0, MD_FILL}, 24'h00FF00);
    expect_line(3'd0, 32'h0800_1000, 32'd0, 16'd100, 7'h00, 24'h00FF00, "FILL-第0行");
    expect_line(3'd0, 32'h0800_1000 + STRIDE_FULL, 32'd0, 16'd100, 7'h00, 24'h00FF00, "FILL-第1行(+5760)");
    expect_line(3'd0, 32'h0800_1000 + 2*STRIDE_FULL, 32'd0, 16'd100, 7'h00, 24'h00FF00, "FILL-第2行(+5760)");

    $display("---- 用例2: FILL 50x2 分屏（目的地址按 2880 累加）----");
    lineBuffer_mode = 1'b1;
    push_cmd(CT_FILL, 32'd0, 32'h0800_1000, 16'd50, 16'd2, {4'd0, MD_FILL}, 24'h0000FF);
    expect_line(3'd0, 32'h0800_1000, 32'd0, 16'd50, 7'h00, 24'h0000FF, "分屏-第0行");
    expect_line(3'd0, 32'h0800_1000 + STRIDE_HALF, 32'd0, 16'd50, 7'h00, 24'h0000FF, "分屏-第1行(+2880)");
    lineBuffer_mode = 1'b0;

    $display("---- 用例3: BLIT 64x2 REPLACE（模式=1、SRC=0，源也按 5760 累加）----");
    push_cmd(CT_BLIT, 32'h0900_0000, 32'h0800_2000, 16'd64, 16'd2, {4'd0, MD_REPLACE}, 24'h123456);
    expect_line(3'd1, 32'h0800_2000, 32'h0900_0000, 16'd64, 7'h01, 24'h123456, "BLIT-第0行");
    expect_line(3'd1, 32'h0800_2000 + STRIDE_FULL, 32'h0900_0000 + STRIDE_FULL,
                16'd64, 7'h01, 24'h123456, "BLIT-第1行(+5760/+5760)");

    $display("---- 用例3-B: 素材源（flags[3]=1，源按命令 color 跨距走）----");
    //   ★ 这是新链路唯一没验过的一步：源地址每行加【命令 color 字段】(32 字),
    //     而目的每行仍加画布跨距(5760) —— 两者独立, 裁剪才能成立。
    push_cmd(CT_BLIT, 32'h0000_0000, 32'h0880_0000, 16'd127, 16'd3, 7'h09, 24'd32);
    expect_line(3'd1, 32'h0880_0000,                     32'h0000_0000,
                16'd127, 7'h09, 24'd32, "素材-第0行");
    expect_line(3'd1, 32'h0880_0000 + STRIDE_FULL,       32'h0000_0020,
                16'd127, 7'h09, 24'd32, "素材-第1行(源+32字, 目的+5760)");
    expect_line(3'd1, 32'h0880_0000 + 2*STRIDE_FULL,     32'h0000_0040,
                16'd127, 7'h09, 24'd32, "素材-第2行(源+64字)");

    $display("---- 用例3b: ALPHA（flags[2:0]=2，flags 原样广播）----");
    push_cmd(CT_BLIT, 32'h0A00_0000, 32'h0800_9000, 16'd8, 16'd1, {4'd0, MD_ALPHA}, 24'h334455);
    expect_line(3'd2, 32'h0800_9000, 32'h0A00_0000, 16'd8, 7'h02, 24'h334455, "ALPHA-单行");

    //   ---- 用例3b-2: ALPHA 多行 —— 背景跟着【目的】逐行走 ----
    //        源①(=背景) 的行地址不从这里出去：CmdExec 广播的是 line_dst_addr，
    //        谁读目的由【源自己】看 flags 判断（DDRSource 里那一行 bg_rd）。这里钉三件事：
    //          · flags 原样广播（模式=2 alpha、flags[3]=1 精灵）—— 源能不能自己判就靠它
    //          · 目的地址逐行 +5760 ⇒ 背景的行跨距 = 画布跨距（天然成立）
    //          · 源②(精灵) 的地址仍按命令 color 的字跨距走，一点不受影响
    $display("---- 用例3b-2: ALPHA 多行（背景=目的，精灵地址不受影响）----");
    push_cmd(CT_BLIT, 32'h0A00_0000, 32'h0800_9000, 16'd64, 16'd2, {4'd1, MD_ALPHA}, 24'h0010);
    expect_line(3'd2, 32'h0800_9000,               32'h0A00_0000,
                16'd64, 7'h0A, 24'h0010, "ALPHA-第0行");
    expect_line(3'd2, 32'h0800_9000 + STRIDE_FULL, 32'h0A00_0000 + 32'd16,
                16'd64, 7'h0A, 24'h0010, "ALPHA-第1行(目的+5760, 精灵+16字)");
    //   ---- 反向用例：非 alpha 的命令【不许】让源去读目的（否则 BLIT 会把自己当源）----
    //        源那边按 flags[2:0]!=2 判，所以这里只要钉住模式位本身就行
    push_cmd(CT_BLIT, 32'h0900_0000, 32'h0800_0000, 16'd64, 16'd1, 7'h01, 24'd0);
    expect_line(3'd1, 32'h0800_0000, 32'h0900_0000, 16'd64, 7'h01, 24'd0, "REPLACE-单行");

    //  ★★ 用例3c/3d/3e：镜像（flags[5] = MX 水平, flags[6] = MY 垂直）
    //     MX：广播的源地址不再是行首，而是【行尾那个字】= 行首 + ceil(w/4) − 1
    //         （取出器只负责"每读一个字 −1"，它不需要知道 w）
     $display("---- 用例3c: 素材源 + MX（w=127 → ceil(127/4)=32 字，行尾 = 行首+31）----");
    push_cmd(CT_BLIT, 32'h0000_0000, 32'h0880_0000, 16'd127, 16'd2, 7'h29, 24'd32);
    expect_line(3'd1, 32'h0880_0000,               32'h0000_001F, 16'd127, 7'h29, 24'd32, "MX-第0行(源=行尾字)");
    expect_line(3'd1, 32'h0880_0000 + STRIDE_FULL, 32'h0000_003F, 16'd127, 7'h29, 24'd32, "MX-第1行(源+32+31)");

     $display("---- 用例3d: MY 垂直镜像（源地址从最后一行往回走）----");
    //   ★ flags 别忘了 MODE 位：REPLACE(1) | MY(0x40) = 7'h41（只写 0x40 就成 FILL 了，模式=0）
    push_cmd(CT_BLIT, 32'h0900_0000, 32'h0800_A000, 16'd64, 16'd3, 7'h41, 24'h123456);
    expect_line(3'd1, 32'h0800_A000,                 32'h0900_0000 + 2*STRIDE_FULL, 16'd64, 7'h41, 24'h123456, "MY-第0行(源=末行)");
    expect_line(3'd1, 32'h0800_A000 + STRIDE_FULL,   32'h0900_0000 + STRIDE_FULL,   16'd64, 7'h41, 24'h123456, "MY-第1行(源-5760)");
    expect_line(3'd1, 32'h0800_A000 + 2*STRIDE_FULL, 32'h0900_0000,                 16'd64, 7'h41, 24'h123456, "MY-第2行(源=首行)");

     $display("---- 用例3e: MX+MY（180°：末行 + 行尾字）----");
    push_cmd(CT_BLIT, 32'h0000_0000, 32'h0880_2000, 16'd127, 16'd2, 7'h69, 24'd32);
    expect_line(3'd1, 32'h0880_2000,               32'h0000_003F, 16'd127, 7'h69, 24'd32, "MXMY-第0行");
    expect_line(3'd1, 32'h0880_2000 + STRIDE_FULL, 32'h0000_001F, 16'd127, 7'h69, 24'd32, "MXMY-第1行");

    $display("---- 用例4: FENCE 什么都不画 ----");
    push_cmd(CT_FENCE, 32'hDEAD_BEEF, 32'h1234_5678, 16'd99, 16'd99, 7'h7F, 24'hFFFFFF);
    expect_no_line(20, "FENCE不发任务");

    $display("---- 用例5: 未定义类型 ----");
    push_cmd(3'd4, 32'h0, 32'h0800_3000, 16'd8, 16'd8, 7'h00, 24'h000000);
    expect_no_line(20, "未定义类型不发任务");

    $display("---- 用例6: 高为 0（不能挂死）----");
    push_cmd(CT_FILL, 32'd0, 32'h0800_4000, 16'd8, 16'd0, 7'h00, 24'h000000);
    expect_no_line(20, "h=0不发任务也不挂死");

    $display("---- 用例7: 单行 h=1 ----");
    push_cmd(CT_FILL, 32'd0, 32'h0800_4800, 16'd16, 16'd1, 7'h00, 24'h00AA55);
    expect_line(3'd0, 32'h0800_4800, 32'd0, 16'd16, 7'h00, 24'h00AA55, "单行");

    //  ★ 用例8：发送节奏 line_buffer_free
    //     旧 TB 这里验的是"等 line_done 期间不许发下一个"。现在不再等 line_done 了，
    //     换成了 line_buffer_free 说了算 —— 所以用例意图照旧保留，判据换成这根线：
    //       free=0 → 还有行也不发（等下游腾地方）
    //       free=1 → 立刻续上，而且 done 还在路上（line_delay=8）就已经发完了
    $display("---- 用例8: 发送节奏 line_buffer_free=0 就等、拉高就续（不等 line_done）----");
    set_line_delay(8);
    line_buffer_free = 1'b0;
    push_cmd(CT_FILL, 32'd0, 32'h0800_5000, 16'd10, 16'd3, {4'd0, MD_FILL}, 24'hFF0000);
    expect_line(3'd0, 32'h0800_5000, 32'd0, 16'd10, 7'h00, 24'hFF0000, "节奏-第0行");
    expect_no_line(10, "下游没空时不发第2行");
    line_buffer_free = 1'b1;
    expect_line(3'd0, 32'h0800_5000 + STRIDE_FULL, 32'd0, 16'd10, 7'h00, 24'hFF0000, "节奏-第1行");
    expect_line(3'd0, 32'h0800_5000 + 2*STRIDE_FULL, 32'd0, 16'd10, 7'h00, 24'hFF0000, "节奏-第2行");

    $display("---- 用例9: 连续两条命令（无气泡）----");
    set_line_delay(2);
    push_cmd(CT_FILL, 32'd0, 32'h0800_6000, 16'd32, 16'd2, {4'd0, MD_FILL}, 24'h001122);
    expect_line(3'd0, 32'h0800_6000, 32'd0, 16'd32, 7'h00, 24'h001122, "连弹A-第0行");
    expect_line(3'd0, 32'h0800_6000 + STRIDE_FULL, 32'd0, 16'd32, 7'h00, 24'h001122, "连弹A-第1行");
    push_cmd(CT_FILL, 32'd0, 32'h0800_8000, 16'd32, 16'd1, {4'd0, MD_FILL}, 24'h003344);
    k = 0;
    while (!line_start && k < 8) begin tick; k = k + 1; end
    if (!line_start) begin
        $display("FAIL [无气泡]: B 的行任务没出来");
        err_cnt = err_cnt + 1;
    end
    else begin
        $display("PASS [无气泡]: B 的行任务在 %0d 拍内出现（等 cmd_ready 用了 %0d 拍）", k, wait_cnt);
        if (line_dst_addr !== 32'h0800_8000 || line_flags[2:0] !== 3'd0) begin
            $display("FAIL [无气泡]: B 的字段不对 dst=%h mode=%b", line_dst_addr, line_flags[2:0]);
            err_cnt = err_cnt + 1;
        end
    end

    // 收尾：等最后一行跑完
    repeat (20) tick;

    $display("---- 结果 ----");
    //   ★ 25 = 原来的用例；+2 = ALPHA 多行；+1 = 反向的 REPLACE 单行
    if (line_cnt !== 28) begin
        $display("FAIL: 一共发了 %0d 个行任务，期望 28", line_cnt);
        err_cnt = err_cnt + 1;
    end
    else $display("PASS: 行任务总数 %0d 正确（原 25 + alpha 多行 2 + 反向 1）", line_cnt);

    if (err_cnt == 0) $display("\n*** 全部通过 ***");
    else              $display("\n*** 失败: err_cnt = %0d ***", err_cnt);
    //  ★ 最后打一行纯 ASCII 的结论：中文串在非 UTF-8 locale 的 ModelSim 里会被打成乱码
    //     （vlog 按系统 ANSI 读源文件，注释不受影响，字符串字面量会），
    //     这一行保证任何终端/日志编码下都能一眼看出过没过
    $display("SUMMARY: err_cnt=%0d  RESULT=%0s", err_cnt, (err_cnt == 0) ? "ALL-PASS" : "FAIL");
    $finish;
end

initial begin
    #500_000;
    $display("FAIL: 全局超时（很可能挂死了）");
    $finish;
end

endmodule
