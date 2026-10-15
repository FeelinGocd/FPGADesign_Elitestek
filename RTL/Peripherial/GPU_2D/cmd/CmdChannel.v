//==============================================================================
//  CmdChannel.v —— 命令通道的顶层：CI 进来，行任务出去
//------------------------------------------------------------------------------
//  它是两个东西的容器
//      CmdBuffer（子模块）：CI 应答握手 + CIUnpack（拼命令）+ 命令队列（跨时钟域）
//      CmdExec  （子模块）：一条命令 → h 个行任务
//      ★ 命令在这条通道上还是【分开的几根线】：类型 / 源 / 目的 + 4 根带格子的参数线，
//        CmdBuffer 出来什么样，进 CmdExec 还是什么样（FIFO 里怎么存是存储的事）

//  对外只有三组接口（外加一组只读观测，给 APB 寄存器用）
//      左：CI 的 8 根线（接 SoC 的 cpu0_customInstruction_*）
//      右：行任务口（广播给各源 / 合成器 / 写侧）
//          外加一根 line_buffer_free：各源的 src_free + 写侧的 wr_free 汇总后回传
//          （只算源不够 —— 源有两块 ping-pong 是为预载下一行，见 GPU_2D 顶部注释）
//      下：配置口（只有 mode —— 源和目的跨距都由它派生，见 CmdExec）

//  ★ 它自己没有任何逻辑 —— 只有例化和连线
//    所以它不需要"行为 TB"：通路测试（tb_CmdChannel）从 CI 推进去、
//    从行任务口看出来，验的就是子模块接起来对不对

//  两个时钟域（和 CmdBuffer 一致）
//      clk_ci  = CPU 时钟（CI 挂在 CPU 流水线上，不能跨域）
//      clk_acc = 加速器时钟（v1 与 clk_ci 同源）
//      ★ 跨域点只有一处：CmdBuffer 里的异步 FIFO

//  参考：docs/spec/设计总纲.md §2（四个域）/ docs/spec/硬件_加速器.md §3
//  上一级是谁
//      GPU_2D.v —— 顶层只把本模块的线接到各源 / 合成器 / 写侧（本层不含逻辑）。
//==============================================================================
`timescale 1ns/100ps

module CmdChannel #(
    parameter FIFO_AW    = 9,                   // 命令队列深度 = 2^9 = 512 条（= 16 KB）
    parameter LINE_BYTES = 5760,                // 单段行宽（全屏 1920×3）
    parameter IMAGE_NUM  = 2                    // 一条命令几张图（★ 必须与 CIUnpack 一致）
)(
    //==================== 时钟 / 复位（两个域）====================
    input  wire                     clk_ci,             //(I) CI 侧时钟（= CPU 时钟）
    input  wire                     rst_ci_n,           //(I) CI 侧复位，异步低有效
    input  wire                     clk_acc,            //(I) 命令侧时钟（v1 与 clk_ci 同源）
    input  wire                     rst_acc_n,          //(I) 命令侧复位，异步低有效

    //==================== CI 端口（接 SoC 的 cpu0_customInstruction_*）  ====================
    input  wire                     ci_cmd_valid,       //(I) CPU 有命令要发
    output wire                     ci_cmd_ready,       //(O) 我们收下了
    input  wire [9:0]               ci_function_id,     //(I) {funct7[6:0], funct3[2:0]}
    input  wire [31:0]              ci_inputs_0,        //(I) 第一参数
    input  wire [31:0]              ci_inputs_1,        //(I) 第二参数
    output wire                     ci_rsp_valid,       //(O) 结果好了
    input  wire                     ci_rsp_ready,       //(I) CPU 能收结果
    output wire [31:0]              ci_outputs_0,       //(O) 结果（v1 回固定标识）

    //==================== 配置口（APB）  ↔ APB0_GPU_2D（总线层寄存器格子）  ====================
    input  wire                     lineBuffer_mode,    //(I) 0=全屏(5760) 1=分屏(2880)

    //==================== 行任务口 → 各源 / 合成器 / 写侧 ====================
    output wire                     line_start,         //(O) 单拍脉冲：这一行的任务在这拍有效
    output wire [31:0]              line_dst_addr,      //(O) 本行的目的起始地址（几张图共用）
    output wire [15:0]              line_w,             //(O) 本行多少像素（共用）
    output wire                     line_fx_en,         //(O) 本命令有没有第二张图（合成）
    //   ★ 下面三根【每张图一格】（第 1 格 = 图一，第 2 格 = 图二…）
    output wire [32*IMAGE_NUM-1:0]  line_src_addr,  //(O) 每张图那一行的源起始地址
    output wire [8*IMAGE_NUM-1:0]   line_flags,     //(O) 每张图那格的模式位
    output wire [24*IMAGE_NUM-1:0]  line_param,     //(O) 每张图那格的参数
    //   ★ "看最后一格"的三根（背景源 / 合成器 / 写侧用的都是最终结果那一步的模式）
    output wire [31:0]              line_src_addr_last,    //(O) 最后一格那张图的源起始地址
    output wire [7:0]               line_flags_last,       //(O) 最后一格那格的模式位
    output wire [23:0]              line_param_last,       //(O) 最后一格那格的参数
    input  wire                     line_done,          //(I) 这一行已经写进 DDR   ← Factory
    input  wire                     line_buffer_free,   //(I) 各源 + 写侧都还能接下一行 ← Factory

    //==================== 观测口 → APB0_GPU_2D（原样透传 CmdExec 的）====================
    output wire                     acc_busy,           //(O) 有命令在手上（没做完）
    output wire                     cmd_done,           //(O) 单拍：这一条命令真做完了
    output wire                     frame_done          //(O) 单拍：FENCE 完成 = 本帧画完（→ 显示侧交帧）
);

//==============================================================================
// wire：队列出口（本层内部连线，不再跨模块边界）
//==============================================================================
wire                                cmd_valid;
wire                                cmd_ready;
wire [2:0]                          cmd_type;
wire [31:0]                         cmd_dst;
wire [15:0]                         cmd_w;
wire [15:0]                         cmd_h;
wire [32*IMAGE_NUM-1:0]             cmd_src;
wire [8*IMAGE_NUM-1:0]              cmd_flags;
wire [32*IMAGE_NUM-1:0]             cmd_args;

//==============================================================================
// 例化
//==============================================================================
CmdBuffer #(
    .FIFO_AW            (FIFO_AW),
    .IMAGE_NUM          (IMAGE_NUM)
) u_CmdBuffer (
    .clk_ci             (clk_ci),
    .rst_ci_n           (rst_ci_n),
    .clk_acc            (clk_acc),
    .rst_acc_n          (rst_acc_n),

    .ci_cmd_valid       (ci_cmd_valid),
    .ci_cmd_ready       (ci_cmd_ready),
    .ci_function_id     (ci_function_id),
    .ci_inputs_0        (ci_inputs_0),
    .ci_inputs_1        (ci_inputs_1),
    .ci_rsp_valid       (ci_rsp_valid),
    .ci_rsp_ready       (ci_rsp_ready),
    .ci_outputs_0       (ci_outputs_0),

    .cmd_valid          (cmd_valid),
    .cmd_ready          (cmd_ready),
    .cmd_type           (cmd_type),
    .cmd_src            (cmd_src),
    .cmd_dst            (cmd_dst),
    .cmd_w              (cmd_w),
    .cmd_h              (cmd_h),
    .cmd_flags          (cmd_flags),
    .cmd_args          (cmd_args)
);

CmdExec #(
    .LINE_BYTES         (LINE_BYTES),
    .IMAGE_NUM          (IMAGE_NUM)
) u_CmdExec (
    .clk_acc            (clk_acc),
    .rst_acc_n          (rst_acc_n),

    .cmd_valid          (cmd_valid),
    .cmd_ready          (cmd_ready),
    .cmd_type           (cmd_type),
    .cmd_src            (cmd_src),
    .cmd_dst            (cmd_dst),
    .cmd_w              (cmd_w),
    .cmd_h              (cmd_h),
    .cmd_flags          (cmd_flags),
    .cmd_args          (cmd_args),

    .lineBuffer_mode    (lineBuffer_mode),

    .line_start         (line_start),
    .line_dst_addr      (line_dst_addr),
    .line_w             (line_w),
    .line_fx_en         (line_fx_en),
    .line_src_addr      (line_src_addr),
    .line_flags         (line_flags),
    .line_param         (line_param),
    .line_src_addr_last (line_src_addr_last),
    .line_flags_last    (line_flags_last),
    .line_param_last    (line_param_last),
    .line_done          (line_done),
    .line_buffer_free   (line_buffer_free),

    .acc_busy           (acc_busy),
    .cmd_done           (cmd_done),
    .frame_done         (frame_done)
);

endmodule
