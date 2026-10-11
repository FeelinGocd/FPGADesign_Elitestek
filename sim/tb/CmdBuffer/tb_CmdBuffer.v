////////////////////////////////////////////////////////////////////////////////
// tb_CmdBuffer.v —— 命令入口验证（CI 应答 + 两条 CI 拼命令 + 命令队列）
//
//  时序约定（重要）
//      所有驱动与采样都在 posedge 之后 #1 进行 —— 这样 DUT 的 NBA 已经落地，
//      避免和 DUT 抢同一个 delta；同时不会把「BRAM 读延迟 1 拍」读错成上一条
//
//  验什么
//    1) FILL + ARGS  → 一条命令，七个字段逐位正确
//    2) BLIT + ARGS  → 源/目的落对位置
//    3) FENCE        → 一条命令，类型=3，其余字段为 0
//    4) 孤儿 ARGS    → 不产生命令
//    5) 截断命令     → 连续两个 op 只出后一条（新的顶掉旧的）
//    6) NOP / 未知功能号 / 组号非 0 → 不产生命令
//    7) 队列写满     → ci_cmd_ready 拉低
//    8) 腾出空间后   → 原来被挡的那条能进去
//    9) CI 应答记账  → 收料数 == 回应答数，最后 rsp_valid 回低
//   10) 零尺寸(w=0 / h=0)        → 不产生命令
//   11) 零尺寸之后紧跟正常命令   → 状态清干净，正常命令照出
//   12) 零尺寸之后跟一条孤儿 ARGS → 不产生「幽灵命令」（组装状态必须已复位）
//   13) FENCE 打断没拼完的命令   → 只出 FENCE
//
//  编译运行(ModelSim)
//    vlog BlockRAM.v async_fifo.v CIUnpack.v CmdBuffer.v ; vlog tb_CmdBuffer.v
//    vsim -c tb_CmdBuffer -do "run -all; quit -f"
////////////////////////////////////////////////////////////////////////////////
`timescale 1ns/100ps

module tb_CmdBuffer;

localparam CLK_HALF = 5.0;
localparam FIFO_AW  = 3;                    // 标称深度 8（wr_full 会提前 2 拍拉高，实测可用略少）

reg  clk, rst_n;

// ---- CI 侧 ----
reg  [9:0]  ci_function_id;
reg  [31:0] ci_inputs_0;
reg  [31:0] ci_inputs_1;
reg         ci_cmd_valid;
wire        ci_cmd_ready;
wire        ci_rsp_valid;
reg         ci_rsp_ready;
wire [31:0] ci_outputs_0;

// ---- 命令队列出口 ----
//   ★ N = 一条命令几张图（这些线里一张图一格）
//     （localparam 必须写在用到它的线之前，不然 Verilog 会当成隐式线网）
localparam IMAGE_NUM = 2;
wire        cmd_valid;
reg         cmd_ready;
wire [2:0]              cmd_type;             // 共用
wire [31:0]             cmd_dst;              // 共用
wire [15:0]             cmd_w;                // 共用
wire [15:0]             cmd_h;                // 共用
wire [32*IMAGE_NUM-1:0] cmd_src_full;         // 每张图一格
wire [7*IMAGE_NUM-1:0]  cmd_flags_full;       // 每张图一格
wire [32*IMAGE_NUM-1:0] cmd_args_full;        // 每张图一格

//   下面的断言只看第 1 张图（原图那格）；第 2 张图用 check_slot 单独查
wire [31:0] cmd_src   = cmd_src_full[31:0];
wire [6:0]  cmd_flags = cmd_flags_full[6:0];
wire [23:0] cmd_args  = cmd_args_full[23:0];

integer err_cnt, i, k, ncmd;
integer ready_seen;
reg  full_hit, ok;
reg  [9:0]  rty_fid;
reg  [31:0] rty_in0, rty_in1;

CmdBuffer #(.FIFO_AW(FIFO_AW), .IMAGE_NUM(IMAGE_NUM)) u_dut (
    .clk_ci         (clk),
    .rst_ci_n       (rst_n),
    .clk_acc        (clk),
    .rst_acc_n      (rst_n),
    .ci_cmd_valid   (ci_cmd_valid),
    .ci_cmd_ready   (ci_cmd_ready),
    .ci_function_id (ci_function_id),
    .ci_inputs_0    (ci_inputs_0),
    .ci_inputs_1    (ci_inputs_1),
    .ci_rsp_valid   (ci_rsp_valid),
    .ci_rsp_ready   (ci_rsp_ready),
    .ci_outputs_0   (ci_outputs_0),
    .cmd_valid      (cmd_valid),
    .cmd_ready      (cmd_ready),
    .cmd_type       (cmd_type),
    .cmd_dst        (cmd_dst),
    .cmd_w          (cmd_w),
    .cmd_h          (cmd_h),
    .cmd_src        (cmd_src_full),
    .cmd_flags      (cmd_flags_full),
    .cmd_args       (cmd_args_full)
);

localparam [9:0] FID_NOP   = 10'h000;
localparam [9:0] FID_FILL  = 10'h001;
localparam [9:0] FID_BLIT  = 10'h002;
localparam [9:0] FID_FENCE = 10'h003;
localparam [9:0] FID_ARGS  = 10'h007;

////////////////////////////////////////////////////////////////////////////////
// 节拍: posedge 之后再 #1 —— 本 TB 所有驱动/采样都走它
////////////////////////////////////////////////////////////////////////////////
task tick; begin @(posedge clk); #1; end endtask

////////////////////////////////////////////////////////////////////////////////
// 应答服务: 每 3 拍收一个响应（故意不立刻收，逼出「欠响应」的情况）
////////////////////////////////////////////////////////////////////////////////
reg  rsp_srv_en;
reg  [1:0] srv_cnt;
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        srv_cnt      <= 2'd0;
        ci_rsp_ready <= 1'b0;
    end
    else begin
        srv_cnt      <= srv_cnt + 2'd1;
        ci_rsp_ready <= rsp_srv_en & ci_rsp_valid & (srv_cnt == 2'd2);
    end
end

integer ci_fire_cnt, rsp_fire_cnt;
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin ci_fire_cnt <= 0; rsp_fire_cnt <= 0; end
    else begin
        if (ci_cmd_valid & ci_cmd_ready) ci_fire_cnt  <= ci_fire_cnt + 1;
        if (ci_rsp_valid & ci_rsp_ready) rsp_fire_cnt <= rsp_fire_cnt + 1;
    end
end

////////////////////////////////////////////////////////////////////////////////
// 任务
////////////////////////////////////////////////////////////////////////////////
// 推一条 CI；ready 在 maxwait 拍内没来 → ok=0（说明队列满了）
task push_ci_chk;
    input [9:0]   fid;
    input [31:0]  in0;
    input [31:0]  in1;
    input integer maxwait;
    output reg    o;
    integer wd;
    begin
        tick;
        ci_function_id = fid; ci_inputs_0 = in0; ci_inputs_1 = in1; ci_cmd_valid = 1'b1;
        o = 1'b0; wd = 0;
        while (!o && wd < maxwait) begin
            tick;
            if (ci_cmd_ready) o = 1'b1;
            wd = wd + 1;
        end
        ci_cmd_valid = 1'b0;
        tick;
    end
endtask

task push_ci;
    input [9:0]  fid;
    input [31:0] in0;
    input [31:0] in1;
    begin
        push_ci_chk(fid, in0, in1, 300, ok);
        if (!ok) begin
            $display("FAIL: 收料超时 (功能号 %h)", fid);
            err_cnt = err_cnt + 1;
        end
    end
endtask

task push_fill;
    input [31:0] dst; input [15:0] w; input [15:0] h;
    input [23:0] col; input [6:0]  fl;
    begin
        push_ci(FID_FILL, {w, h}, dst);
        push_ci(FID_ARGS, 32'd0, {1'b0, fl, col});   // 纯色图：地址填 0，纯色放参数格
    end
endtask

task push_blit;
    input [31:0] src; input [31:0] dst; input [15:0] w; input [15:0] h; input [6:0] fl;
    begin
        push_ci(FID_BLIT, {w, h}, dst);
        push_ci(FID_ARGS, src, {1'b0, fl, 24'd0});   // 原图：地址在它自己那条 CI 里
    end
endtask

// 弹一条: 拉一拍 cmd_ready, 下一拍（tick 之后）数据有效
task pop_cmd;
    output [2:0]  t;
    output [31:0] s;
    output [31:0] d;
    output [15:0] w;
    output [15:0] h;
    output [6:0]  f;
    output [23:0] c;
    integer wd;
    begin
        wd = 0;
        while (!cmd_valid && wd < 300) begin tick; wd = wd + 1; end
        if (wd >= 300) begin
            $display("FAIL: 等不到命令");
            err_cnt = err_cnt + 1;
            t = 3'd0; s = 32'd0; d = 32'd0; w = 16'd0; h = 16'd0; f = 7'd0; c = 24'd0;
        end
        else begin
            // 标准握手：cmd_valid=1 时数据【已经有效】→ 先采样，再拉 cmd_ready 把它弹走
            t = cmd_type; s = cmd_src; d = cmd_dst;
            w = cmd_w; h = cmd_h; f = cmd_flags; c = cmd_args;
            cmd_ready = 1'b1;
            tick;
            cmd_ready = 1'b0;
        end
    end
endtask

task pop_discard;
    integer wd;
    begin
        wd = 0;
        while (!cmd_valid && wd < 300) begin tick; wd = wd + 1; end
        if (!cmd_valid) begin
            $display("FAIL: 等不到命令(丢弃)");
            err_cnt = err_cnt + 1;
        end
        else begin
            cmd_ready = 1'b1;
            tick;
            cmd_ready = 1'b0;
        end
    end
endtask

task check_cmd;
    input [2:0]   et;  input [31:0] es; input [31:0] ed;
    input [15:0]  ew;  input [15:0] eh; input [6:0]  ef; input [23:0] ec;
    input [8*16-1:0] tag;
    reg [2:0]  t;  reg [31:0] s, d;
    reg [15:0] w, h; reg [6:0] f;   reg [23:0] c;
    begin
        pop_cmd(t, s, d, w, h, f, c);
        if (t !== et || s !== es || d !== ed || w !== ew || h !== eh || f !== ef || c !== ec) begin
            $display("FAIL [%0s]: 类型=%0d 源=%h 目的=%h 宽=%0d 高=%0d flags=%h 颜色=%h",
                     tag, t, s, d, w, h, f, c);
            $display("      期望    : 类型=%0d 源=%h 目的=%h 宽=%0d 高=%0d flags=%h 颜色=%h",
                     et, es, ed, ew, eh, ef, ec);
            err_cnt = err_cnt + 1;
        end
        else $display("PASS [%0s]", tag);
    end
endtask

// 查第 k 张图（k 从 0 起）：多图的组装对不对，就看每根线的第 k 格里是不是那张图的资料
//   （宽高是几张图共用的，不在里面）
task check_slot;
    input integer k;
    input [31:0] es; input [6:0] ef; input [23:0] ec;
    input [8*16-1:0] tag;
    reg [31:0] s; reg [6:0] f; reg [23:0] c;
    begin
        s = cmd_src_full  [32*k +: 32];
        f = cmd_flags_full[ 7*k +:  7];
        c = cmd_args_full [32*k +: 24];
        if (s !== es || f !== ef || c !== ec) begin
            $display("FAIL [%0s]: 第%0d张图 地址=%h flags=%h 参数=%h", tag, k+1, s, f, c);
            $display("      期望      : 地址=%h flags=%h 参数=%h", es, ef, ec);
            err_cnt = err_cnt + 1;
        end
        else $display("PASS [%0s]", tag);
    end
endtask

task expect_no_cmd;
    input integer ncyc;
    input [8*16-1:0] tag;
    integer kk;
    reg bad;
    begin
        bad = 1'b0;
        for (kk = 0; kk < ncyc; kk = kk + 1) begin
            tick;
            if (cmd_valid) bad = 1'b1;
        end
        if (bad) begin
            $display("FAIL [%0s]: 不该有命令，却出现了 cmd_valid", tag);
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
    err_cnt = 0; rsp_srv_en = 1'b1;
    rst_n = 1'b0;
    ci_cmd_valid = 1'b0; ci_function_id = 10'd0; ci_inputs_0 = 32'd0; ci_inputs_1 = 32'd0;
    cmd_ready = 1'b0;
    repeat (10) @(posedge clk);
    rst_n = 1'b1;
    repeat (5) @(posedge clk);

    $display("---- 用例1: FILL + ARGS ----");
    push_fill(32'h0800_1000, 16'd64, 16'd32, 24'h00FF00, 7'h01);
    check_cmd(3'd1, 32'd0, 32'h0800_1000, 16'd64, 16'd32, 7'h01, 24'h00FF00, "FILL");

    $display("---- 用例2: BLIT + ARGS ----");
    push_blit(32'h0900_0000, 32'h0800_2000, 16'd32, 16'd16, 7'h00);
    check_cmd(3'd2, 32'h0900_0000, 32'h0800_2000, 16'd32, 16'd16, 7'h00, 24'd0, "BLIT");

    $display("---- 用例3: FENCE ----");
    push_ci(FID_FENCE, 32'hDEAD_BEEF, 32'h1234_5678);
    check_cmd(3'd3, 32'd0, 32'd0, 16'd0, 16'd0, 7'd0, 24'd0, "FENCE其余字段0");

    $display("---- 用例4: 连弹两条（检查下一条会按时就绪）----");
    push_fill(32'h0800_3000, 16'd16, 16'd16, 24'h0000FF, 7'h00);
    push_fill(32'h0800_4000, 16'd16, 16'd16, 24'h00FF00, 7'h00);
    check_cmd(3'd1, 32'd0, 32'h0800_3000, 16'd16, 16'd16, 7'h00, 24'h0000FF, "连弹-第一条");
    // ★ 不要求「当拍就绪」：异步 FIFO 的读侧要等写指针同步过来（约 2 拍），
    //   刚写完就弹，下一条可能晚几拍才可见 —— 这是 CDC 的正常代价，不是缺陷。
    //   真正要保证的是「几拍内就绪」，所以这里给 8 拍窗口。
    k = 0;
    while (!cmd_valid && k < 8) begin tick; k = k + 1; end
    if (cmd_valid !== 1'b1) begin
        $display("FAIL [下一条就绪]: 弹走后 8 拍内下一条还没就绪");
        err_cnt = err_cnt + 1;
    end
    else $display("PASS [下一条就绪]（延迟 %0d 拍）", k);
    check_cmd(3'd1, 32'd0, 32'h0800_4000, 16'd16, 16'd16, 7'h00, 24'h00FF00, "连弹-第二条");

    $display("---- 用例5: 孤儿 ARGS ----");
    push_ci(FID_ARGS, 32'h0900_0000, {1'b0, 7'h00, 24'hFF0000});   // 没在收图，白给
    expect_no_cmd(20, "孤儿ARGS不出命令");

    $display("---- 用例6: 截断（FILL 没等 ARGS 就来 BLIT）----");
    push_ci(FID_FILL, {16'd16, 16'd16}, 32'h1111_0000);   // 只有 op，没等图那几条
    push_blit(32'h0900_0000, 32'h0800_2000, 16'd32, 16'd16, 7'h00);
    check_cmd(3'd2, 32'h0900_0000, 32'h0800_2000, 16'd32, 16'd16, 7'h00, 24'd0, "截断后出BLIT");

    $display("---- 用例7: NOP / 未知功能号 / 组号非 0 ----");
    push_ci(FID_NOP, 32'hAAAA_AAAA, 32'h5555_5555);
    push_ci(10'h005, 32'hAAAA_AAAA, 32'h5555_5555);
    push_ci(10'h080, 32'hAAAA_AAAA, 32'h5555_5555);
    expect_no_cmd(20, "NOP未知组号不出命令");

    $display("---- 用例8: 一直推到写满 ----");
    full_hit = 1'b0; ncmd = 0;
    while (!full_hit && ncmd < 12) begin
        rty_fid = FID_FILL; rty_in0 = {16'd64, 16'd32}; rty_in1 = 32'h0A00_0000 + ncmd*32'h1000;
        push_ci_chk(rty_fid, rty_in0, rty_in1, 40, ok);
        if (!ok) full_hit = 1'b1;
        else begin
            rty_fid = FID_ARGS; rty_in0 = 32'd0; rty_in1 = {1'b0, 7'h00, 24'h00FF00};
            push_ci_chk(rty_fid, rty_in0, rty_in1, 40, ok);
            if (!ok) full_hit = 1'b1; else ncmd = ncmd + 1;
        end
    end
    if (full_hit) $display("PASS [队列写满反压]  写进去 %0d 条命令", ncmd);
    else begin $display("FAIL [队列写满反压]: 推了 %0d 条还没满", ncmd); err_cnt = err_cnt + 1; end

    $display("---- 用例9: 腾出空间后被挡的那条能进去 ----");
    for (k = 0; k < 3; k = k + 1) pop_discard;
    push_ci_chk(rty_fid, rty_in0, rty_in1, 100, ok);
    if (ok) $display("PASS [腾空间后恢复]");
    else begin $display("FAIL [腾空间后恢复]: 一直没能进去"); err_cnt = err_cnt + 1; end

    // 排空队列
    for (k = 0; k < 20; k = k + 1) if (cmd_valid) pop_discard;
    repeat (40) @(posedge clk); #1;

    $display("---- 用例10: 零尺寸 FILL(w=0) ----");
    push_ci(FID_FILL, {16'd0, 16'd32}, 32'h0800_5000);
    push_ci(FID_ARGS, 32'd0, {1'b0, 7'h00, 24'h00FF00});
    expect_no_cmd(20, "w=0不出命令");

    $display("---- 用例11: 零尺寸 FILL(h=0) ----");
    push_ci(FID_FILL, {16'd64, 16'd0}, 32'h0800_6000);
    push_ci(FID_ARGS, 32'd0, {1'b0, 7'h00, 24'h00FF00});
    expect_no_cmd(20, "h=0不出命令");

    $display("---- 用例12: 零尺寸之后紧跟一条正常命令 ----");
    push_ci(FID_FILL, {16'd0, 16'd0}, 32'h0800_7000);
    push_ci(FID_ARGS, 32'd0, {1'b0, 7'h00, 24'h00FF00});
    push_fill(32'h0800_8000, 16'd8, 16'd8, 24'h0000FF, 7'h00);
    check_cmd(3'd1, 32'd0, 32'h0800_8000, 16'd8, 16'd8, 7'h00, 24'h0000FF, "零尺寸后正常命令");

    $display("---- 用例13: 零尺寸之后跟一条孤儿 ARGS（幽灵命令陷阱）----");
    push_ci(FID_FILL, {16'd0, 16'd16}, 32'h0800_9000);
    push_ci(FID_ARGS, 32'h0900_0000, {1'b0, 7'h00, 24'h11_2233});   // 结束词（零尺寸 ⇒ 不发）
    push_ci(FID_ARGS, 32'h0900_1000, {1'b0, 7'h00, 24'h44_5566});   // 孤儿
    expect_no_cmd(20, "零尺寸后孤儿ARGS不出幽灵命令");

    $display("---- 用例14: FENCE 打断没拼完的命令 ----");
    push_ci(FID_FILL, {16'd16, 16'd16}, 32'h0800_A000);   // 只有 op，没有图
    push_ci(FID_FENCE, 32'hDEAD_BEEF, 32'h1234_5678);
    check_cmd(3'd3, 32'd0, 32'd0, 16'd0, 16'd0, 7'd0, 24'd0, "FENCE打断截断命令");

    //==========================================================================
    //  ★ 用例15~17：一条命令能画几张图（N = IMAGE_NUM，合成就是 2 张）
    //     几张图 = 几条参数 CI；几条 CI 只是"一条装不下"，命令还是只有一条
    //==========================================================================
    $display("---- 用例15: 两张图（op 给宽高+目的，两条 ARGS 各给一张图）----");
    push_ci(FID_BLIT, {16'd16, 16'd8}, 32'h0800_D000);             // 共用：宽高 + 目的
    push_ci(FID_ARGS, 32'h0000_1000, {1'b1, 7'h09, 24'h000008});   // 图1（原图）  more=1
    //   图2 = 特效图：地址 + { 跨距 12 | 强度 8 | 特效模式 4 } + flags（和图1同一套）+ more=0
    //   args[23:20]=4'b0011（色调模式=1(红) + 垫在底图下面）, [19:12]=8'h80（强度 128）,
    //   [11:0]=12'h168（360 字行跨距）; flags=7'h0A（ALPHA + PRAM源 + MX）
    push_ci(FID_ARGS, 32'h0A00_0000, {1'b0, 7'h0A, 24'h38_0168});   // 图2（特效图）more=0
    check_cmd(3'd2, 32'h0000_1000, 32'h0800_D000, 16'd16, 16'd8, 7'h09, 24'h000008,
              "case15-image1");
    check_slot(1, 32'h0A00_0000, 7'h0A, 24'h38_0168, "case15-image2-fx");
    expect_no_cmd(20, "case15-no-extra");

    $display("---- 用例16: 三张图（超过 N=2）⇒ 整条丢掉，不许截断 ----");
    push_ci(FID_BLIT, {16'd8, 16'd4}, 32'h0800_D800);
    push_ci(FID_ARGS, 32'h0000_3000, {1'b1, 7'h09, 24'h000008});   // 图1 more=1
    push_ci(FID_ARGS, 32'h0000_4000, {1'b1, 7'h2A, 24'hABCDEF});   // 图2 more=1
    push_ci(FID_ARGS, 32'h0000_5000, {1'b0, 7'h15, 24'h123456});   // 图3 ⇒ 没格子了
    expect_no_cmd(30, "case16-over-limit-dropped");

    $display("---- 用例17: 没给结束词（图2 还是 more=1）⇒ 先不发；下一条 op 重同步 ----");
    push_ci(FID_BLIT, {16'd16, 16'd16}, 32'h0800_E000);
    push_ci(FID_ARGS, 32'h0000_6000, {1'b1, 7'h09, 24'h000008});   // 图1 more=1
    push_ci(FID_ARGS, 32'h0000_7000, {1'b1, 7'h2A, 24'hABCDEF});   // 图2 more=1（还没完）
    expect_no_cmd(20, "case17-unfinished-no-cmd");
    push_blit(32'h0000_8000, 32'h0800_E800, 16'd8, 16'd8, 7'h00);  // 重同步：新命令照常出
    check_cmd(3'd2, 32'h0000_8000, 32'h0800_E800, 16'd8, 16'd8, 7'h00, 24'd0, "case17-resync");

    $display("---- 结果 ----");
    if (ci_fire_cnt !== rsp_fire_cnt) begin
        $display("FAIL: 收料 %0d 条, 回应答 %0d 个 —— 必须一一对应", ci_fire_cnt, rsp_fire_cnt);
        err_cnt = err_cnt + 1;
    end
    else $display("PASS: 收料 %0d 条, 回应答 %0d 个", ci_fire_cnt, rsp_fire_cnt);
    if (ci_rsp_valid !== 1'b0) begin
        $display("FAIL: 应答全部收完之后 rsp_valid 还高着");
        err_cnt = err_cnt + 1;
    end
    else $display("PASS: 应答收完后 rsp_valid 回低");
    if (ci_outputs_0 !== 32'h434D4431) begin
        $display("FAIL: ci_outputs_0 = %h, 期望 434D4431", ci_outputs_0);
        err_cnt = err_cnt + 1;
    end
    else $display("PASS: ci_outputs_0 固定标识正确");

    if (err_cnt == 0) $display("\n*** 全部通过 ***");
    else              $display("\n*** 失败: err_cnt = %0d ***", err_cnt);
    $finish;
end

initial begin
    #800_000;
    $display("FAIL: 全局超时");
    $finish;
end

endmodule