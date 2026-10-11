////////////////////////////////////////////////////////////////////////////////
// tb_CmdChannel.v —— 命令通路的端到端验证（CI 推进去 → 行任务看出来）
//
//  这条通路串了三个模块：
//      CI 8 根线 → CIUnpack → CmdBuffer(FIFO) → CmdExec → 行任务
//  所以它验的是"接起来对不对"，不是某一个模块的内部行为
//  （CIUnpack / CmdBuffer 的行为 TB 是 tb_CmdBuffer，CmdExec 的是 tb_CmdExec）
//
//  改名说明
//      待测模块原来是 CmdHandler，现在叫 CmdChannel（它自己没有任何逻辑，
//      只是 CIUnpack + CmdBuffer + CmdExec 的容器），TB 跟着改名。
//
//  时序约定
//      所有驱动与采样都在 posedge 之后 #1 进行
//
//  ★ 接口对齐（RTL 现状；旧 TB 按老接口写的地方）
//    1) 合成模式（flags[2:0]）【不再】在 CmdExec 里解成"要哪几路源"：
//       CmdExec 只把 flags 原样广播，谁要不要动由各源和合成器自己看 flags 判。
//       （旧 TB 给 FILL 塞了 flags=7'h01、给 BLIT 塞了 7'h05 —— 前者会变成"要源①"，
//         后者是未定义模式被当成不要源，两个都和新解码对不上）
//       line_flags / line_param 现在【无条件原样转发】
//    2) 没有 src_stride 了 —— 源和目的【同跨距】，由 lineBuffer_mode 内部派生：
//         全屏 5760 / 分屏 2880
//    3) 新增 line_buffer_free —— 发送节奏由它说了算（有空就继续发，不再等 line_done）。
//       本 TB 是通路验证，直接恒 1（节奏本身在 tb_CmdExec 里单独验）
//
//  验什么
//    1) FILL 多行 → 行数对、目的地址按 5760 累加、模式=0、src 填 0、flags/param 透传
//    2) BLIT    → 模式=1、源地址【也按 5760 累加】、flags/param 透传
//    3) FENCE   → 不发任何行任务，且后面的命令照常走（不卡住通路）
//    4) 零尺寸  → 在 CIUnpack 被挡掉，不发任务，且不影响后面的命令
//    5) 分屏    → 目的地址按 2880 累加
//    6) 命令按先进先出顺序执行，跨命令边界不串行任务
//    7) CI 记账 → 收料数 == 回应答数，最后 rsp_valid 回低
//
//  编译运行(ModelSim)
//    vlog -quiet -work work ../../../project/verified/06_SoC_2DGPU/RTL/Peripherial/LineBuffer/PingPongBuffer/BlockRAM.v
//    vlog -quiet -work work ../../../project/verified/06_SoC_2DGPU/RTL/Peripherial/LineBuffer/PingPongBuffer/async_fifo.v
//    vlog -quiet -work work ../../../project/verified/06_SoC_2DGPU/RTL/Peripherial/GPU_2D/cmd/CIUnpack.v
//    vlog -quiet -work work ../../../project/verified/06_SoC_2DGPU/RTL/Peripherial/GPU_2D/cmd/CmdBuffer.v
//    vlog -quiet -work work ../../../project/verified/06_SoC_2DGPU/RTL/Peripherial/GPU_2D/cmd/CmdExec.v
//    vlog -quiet -work work ../../../project/verified/06_SoC_2DGPU/RTL/Peripherial/GPU_2D/cmd/CmdChannel.v
//    vlog -quiet -work work tb_CmdChannel.v
//    vsim -c -quiet -do "run -all; quit -f" work.tb_CmdChannel
////////////////////////////////////////////////////////////////////////////////
`timescale 1ns/100ps

module tb_CmdChannel;

localparam CLK_HALF = 5.0;
localparam FIFO_AW  = 3;                    // 队列做小一点，够用即可

reg          clk, rst_n;

// ---- CI 侧 ----
reg  [9:0]   ci_function_id;
reg  [31:0]  ci_inputs_0, ci_inputs_1;
reg          ci_cmd_valid;
wire         ci_cmd_ready;
wire         ci_rsp_valid;
reg          ci_rsp_ready;
wire  [31:0] ci_outputs_0;

// ---- 配置口 ----
//   ★ 只有 lineBuffer_mode：源和目的同跨距，5760/2880 由 CmdExec 内部派生
reg          lineBuffer_mode;

// ---- 行任务口 ----
//   ★ 模式不再单独出一根线，就在 line_flags[2:0] 里；line_buffer_free 只是发送节奏，通路验证恒 1
wire         line_start;
wire  [31:0] line_dst_addr, line_src_addr;
wire  [15:0] line_w;
wire  [6:0]  line_flags;
wire  [23:0] line_param;
//   ★ "最后一格"那三根（背景源 / 合成器 / 写侧用）；本 TB 只验透传，接出来免得悬空告警
wire  [31:0] line_src_addr_last;
wire  [6:0]  line_flags_last;
wire  [23:0] line_param_last;
reg          line_done;
reg          line_buffer_free;

integer err_cnt, line_cnt, ci_fire_cnt, rsp_fire_cnt, k;

localparam [9:0] FID_FILL  = 10'h001;
localparam [9:0] FID_BLIT  = 10'h002;
localparam [9:0] FID_FENCE = 10'h003;
localparam [9:0] FID_ARGS  = 10'h007;

//   合成模式（cmd_flags[2:0]）
localparam [2:0] MD_FILL    = 3'd0;
localparam [2:0] MD_REPLACE = 3'd1;

//   行跨距：全屏 5760 / 分屏 2880
localparam [31:0] STRIDE_FULL = 32'd5760;
localparam [31:0] STRIDE_HALF = 32'd2880;

CmdChannel #(.FIFO_AW(FIFO_AW), .LINE_BYTES(5760)) u_dut (
    .clk_ci             (clk),
    .rst_ci_n           (rst_n),
    .clk_acc            (clk),
    .rst_acc_n          (rst_n),

    .ci_cmd_valid       (ci_cmd_valid),
    .ci_cmd_ready       (ci_cmd_ready),
    .ci_function_id     (ci_function_id),
    .ci_inputs_0        (ci_inputs_0),
    .ci_inputs_1        (ci_inputs_1),
    .ci_rsp_valid       (ci_rsp_valid),
    .ci_rsp_ready       (ci_rsp_ready),
    .ci_outputs_0       (ci_outputs_0),

    .lineBuffer_mode    (lineBuffer_mode),

    .line_start         (line_start),
    .line_dst_addr      (line_dst_addr),
    .line_src_addr      (line_src_addr),
    .line_w             (line_w),
    .line_flags         (line_flags),
    .line_param         (line_param),
    .line_src_addr_last (line_src_addr_last),
    .line_flags_last    (line_flags_last),
    .line_param_last    (line_param_last),
    .line_done          (line_done),
    .line_buffer_free   (line_buffer_free)
);

////////////////////////////////////////////////////////////////////////////////
// 节拍 / 记账
////////////////////////////////////////////////////////////////////////////////
task tick; begin @(posedge clk); #1; end endtask

always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        line_cnt <= 0; ci_fire_cnt <= 0; rsp_fire_cnt <= 0;
    end
    else begin
        if (line_start) line_cnt <= line_cnt + 1;
        if (ci_cmd_valid & ci_cmd_ready) ci_fire_cnt  <= ci_fire_cnt + 1;
        if (ci_rsp_valid & ci_rsp_ready) rsp_fire_cnt <= rsp_fire_cnt + 1;
    end
end

////////////////////////////////////////////////////////////////////////////////
// 写控制器桩: 每收到一个 line_start 就登记一行，line_delay 拍后回一个单拍 line_done
//   ★ 只要 line_buffer_free 高，CmdExec 就一路发下去，一屏多行同时在飞 ——
//     桩必须【每行各回一个 done】，否则 line_done_cnt 追不上 h，命令卡在 S_RUN。
//     done_pipe 就是一根移位延迟线：line_start 从右进，line_delay 位处出。
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

////////////////////////////////////////////////////////////////////////////////
// 任务
////////////////////////////////////////////////////////////////////////////////
// 推一条 CI；等不到 ready 就报错
task push_ci;
    input [9:0]  fid;
    input [31:0] in0, in1;
    integer wd;
    begin
        tick;
        ci_function_id = fid; ci_inputs_0 = in0; ci_inputs_1 = in1; ci_cmd_valid = 1'b1;
        wd = 0;
        while (!ci_cmd_ready && wd < 300) begin tick; wd = wd + 1; end
        if (!ci_cmd_ready) begin
            $display("FAIL: CI 收不进去（功能号 %h）", fid);
            err_cnt = err_cnt + 1;
        end
        tick;
        ci_cmd_valid = 1'b0;
    end
endtask

// 一次 Fill = 两条 CI
task push_fill;
    input [31:0] dst; input [15:0] w, h;
    input [23:0] col; input [6:0]  fl;
    begin
        push_ci(FID_FILL, {w, h}, dst);              // 命令开始：宽高 + 目的
        push_ci(FID_ARGS, 32'd0, {1'b0, fl, col});   // 第 1 张图（纯色图）：地址填 0
    end
endtask

// 一次 Blit = 两条 CI（color 现在会被原样转发到行任务口，所以照样给值）
task push_blit;
    input [31:0] src, dst; input [15:0] w, h; input [6:0] fl; input [23:0] col;
    begin
        push_ci(FID_BLIT, {w, h}, dst);              // 命令开始：宽高 + 目的
        push_ci(FID_ARGS, src, {1'b0, fl, col});     // 第 1 张图（原图）：地址在它那条 CI 里
    end
endtask

// 等一个行任务并逐字段检查；采完把那拍消费掉（line_start 只持续一拍）
//   ★ emode: 这一行的合成模式 = flags[2:0]（CmdExec 只广播 flags，不再解“要哪几路源”）
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
        tick;
    end
endtask

// 观察 ncyc 拍，确认一个行任务都没有
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
    ci_cmd_valid = 1'b0; ci_function_id = 10'd0; ci_inputs_0 = 32'd0; ci_inputs_1 = 32'd0;
    ci_rsp_ready = 1'b1;                    // 快收应答：应答记账由 tb_CmdBuffer 单独验
    lineBuffer_mode = 1'b0;
    line_buffer_free = 1'b1;                // 通路验证：下游恒有空
    repeat (10) @(posedge clk);
    rst_n = 1'b1;
    repeat (5) @(posedge clk);

    $display("---- 用例1: FILL 100x3（CI 两条 → 3 个行任务，src_en=00）----");
    push_fill(32'h0800_1000, 16'd100, 16'd3, 24'h00FF00, {4'd0, MD_FILL});
    expect_line(3'd0, 32'h0800_1000, 32'd0, 16'd100, 7'h00, 24'h00FF00, "FILL-第0行");
    expect_line(3'd0, 32'h0800_1000 + STRIDE_FULL, 32'd0, 16'd100, 7'h00, 24'h00FF00, "FILL-第1行");
    expect_line(3'd0, 32'h0800_1000 + 2*STRIDE_FULL, 32'd0, 16'd100, 7'h00, 24'h00FF00, "FILL-第2行");

    $display("---- 用例2: BLIT 64x2 REPLACE（src_en=01，源也按 5760 累加）----");
    push_blit(32'h0900_0000, 32'h0800_2000, 16'd64, 16'd2, {4'd0, MD_REPLACE}, 24'h0A0B0C);
    expect_line(3'd1, 32'h0800_2000, 32'h0900_0000, 16'd64, 7'h01, 24'h0A0B0C, "BLIT-第0行");
    expect_line(3'd1, 32'h0800_2000 + STRIDE_FULL, 32'h0900_0000 + STRIDE_FULL,
                16'd64, 7'h01, 24'h0A0B0C, "BLIT-第1行");

    $display("---- 用例3: FENCE 不发行任务，后面的命令照走 ----");
    push_ci(FID_FENCE, 32'hDEAD_BEEF, 32'h1234_5678);
    expect_no_line(20, "FENCE不发任务");
    push_fill(32'h0800_5000, 16'd16, 16'd1, 24'h00AA55, {4'd0, MD_FILL});
    expect_line(3'd0, 32'h0800_5000, 32'd0, 16'd16, 7'h00, 24'h00AA55, "FENCE后照走");

    $display("---- 用例4: 零尺寸被入口挡掉，且不影响后面的命令 ----");
    push_ci(FID_FILL, {16'd0, 16'd32}, 32'h0800_5800);
    push_ci(FID_ARGS, 32'd0, {1'b0, 7'h00, 24'hFF0000});
    expect_no_line(20, "零尺寸不发任务");
    push_fill(32'h0800_6000, 16'd8, 16'd1, 24'h0000FF, {4'd0, MD_FILL});
    expect_line(3'd0, 32'h0800_6000, 32'd0, 16'd8, 7'h00, 24'h0000FF, "零尺寸后照走");

    $display("---- 用例5: 分屏（目的地址按 2880 累加）+ 连发两条 ----");
    lineBuffer_mode = 1'b1;
    push_fill(32'h0800_7000, 16'd20, 16'd2, 24'h112233, {4'd0, MD_FILL});
    push_fill(32'h0800_8000, 16'd20, 16'd1, 24'h445566, {4'd0, MD_FILL});
    expect_line(3'd0, 32'h0800_7000, 32'd0, 16'd20, 7'h00, 24'h112233, "分屏A-第0行");
    expect_line(3'd0, 32'h0800_7000 + STRIDE_HALF, 32'd0, 16'd20, 7'h00, 24'h112233, "分屏A-第1行(+2880)");
    expect_line(3'd0, 32'h0800_8000, 32'd0, 16'd20, 7'h00, 24'h445566, "分屏B-第0行");
    lineBuffer_mode = 1'b0;

    $display("---- 用例6: 两张图的命令（合成形状）—— 第 2 格先躺着，执行只看第 1 张图 ----");
    //   第 1 条 = 命令开始（宽高 + 目的）；第 2 条 = 图1（原图）；第 3 条 = 图2（特效图）
    //   ★ 像素侧还没做合成，所以这一版就该【只按第 1 张图】发 h 个行任务；
    //     第 2 格（特效的地址/参数）躺在命令里，谁也不看它 —— 这就是要钉的行为
    push_ci(FID_BLIT, {16'd32, 16'd2}, 32'h0800_9000);
    push_ci(FID_ARGS, 32'h0900_0000, {1'b1, 7'h01, 24'd0});          // 图1（原图）  more=1
    push_ci(FID_ARGS, 32'h0A00_0000, {1'b0, 7'h0A, 24'h38_0168});    // 图2（特效图）more=0
    expect_line(3'd1, 32'h0800_9000, 32'h0900_0000, 16'd32, 7'h01, 24'd0, "两图-第0行(按图1)");
    expect_line(3'd1, 32'h0800_9000 + STRIDE_FULL, 32'h0900_0000 + STRIDE_FULL,
                16'd32, 7'h01, 24'd0, "两图-第1行(按图1)");

    repeat (20) tick;

    $display("---- 结果 ----");
    if (line_cnt !== 12) begin
        $display("FAIL: 行任务总数 %0d，期望 12（3+2+0+1+0+1+2+1+2）", line_cnt);
        err_cnt = err_cnt + 1;
    end
    else $display("PASS: 行任务总数 %0d 正确（3+2+0+1+0+1+2+1+2）", line_cnt);

    if (ci_fire_cnt !== rsp_fire_cnt) begin
        $display("FAIL: 收料 %0d 条，回应答 %0d 个 —— 必须一一对应", ci_fire_cnt, rsp_fire_cnt);
        err_cnt = err_cnt + 1;
    end
    else $display("PASS: CI 收料 %0d 条 == 回应答 %0d 个", ci_fire_cnt, rsp_fire_cnt);

    if (ci_rsp_valid !== 1'b0) begin
        $display("FAIL: 应答收完之后 rsp_valid 还高着");
        err_cnt = err_cnt + 1;
    end
    else $display("PASS: 应答收完后 rsp_valid 回低");

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
