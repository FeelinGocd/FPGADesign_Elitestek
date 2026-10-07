//==============================================================================
//  CmdChannel.v —— 命令通道的顶层：CI 进来，行任务出去
//------------------------------------------------------------------------------
//  它是三个东西的容器
//      CIUnpack （在 CmdBuffer 里面）：两条 CI 拼成一条命令
//      CmdBuffer（子模块）           ：CI 应答握手 + 命令队列（跨时钟域）
//      CmdExec  （子模块）           ：一条命令 → h 个行任务
//
//  对外只有三组接口（外加一组只读观测，给 APB 寄存器用）
//      左：CI 的 8 根线（接 SoC 的 cpu0_customInstruction_*）
//      右：行任务口（广播给各源 / 合成器 / 写侧）
//          外加一根 line_buffer_free：各源的 src_free + 写侧的 wr_free 汇总后回传
//          （只算源不够 —— 源有两块 ping-pong 是为预载下一行，见 GPU_2D 顶部注释）
//      下：配置口（只有 mode —— 源和目的跨距都由它派生，见 CmdExec）
//
//  ★ 它自己没有任何逻辑 —— 只有例化和连线
//    所以它不需要"行为 TB"：通路测试（tb_CmdChannel）从 CI 推进去、
//    从行任务口看出来，验的就是三个子模块接起来对不对
//
//  两个时钟域（和 CmdBuffer 一致）
//      clk_ci  = CPU 时钟（CI 挂在 CPU 流水线上，不能跨域）
//      clk_acc = 加速器时钟（v1 与 clk_ci 同源）
//      ★ 跨域点只有一处：CmdBuffer 里的异步 FIFO
//
//  参考：docs/spec/设计总纲.md §2（四个域）/ docs/spec/硬件_加速器.md §3
//==============================================================================
`timescale 1ns/100ps

module CmdChannel #(
    parameter FIFO_AW    = 9,                   // 命令队列深度 = 2^9 = 512 条（≈9 KB）
    parameter LINE_BYTES = 5760                 // 单段行宽（全屏 1920×3）
)(
    //==================== 时钟 / 复位（两个域）====================
    input  wire             clk_ci,             //(I) CI 侧时钟（= CPU 时钟）
    input  wire             rst_ci_n,           //(I) CI 侧复位，异步低有效
    input  wire             clk_acc,            //(I) 命令侧时钟（v1 与 clk_ci 同源）
    input  wire             rst_acc_n,          //(I) 命令侧复位，异步低有效

    //==================== CI 端口（接 SoC 的 cpu0_customInstruction_*）====================
    input  wire             ci_cmd_valid,       //(I) CPU 有命令要发
    output wire             ci_cmd_ready,       //(O) 我们收下了
    input  wire [9:0]       ci_function_id,     //(I) {funct7[6:0], funct3[2:0]}
    input  wire [31:0]      ci_inputs_0,        //(I) 第一参数
    input  wire [31:0]      ci_inputs_1,        //(I) 第二参数
    output wire             ci_rsp_valid,       //(O) 结果好了
    input  wire             ci_rsp_ready,       //(I) CPU 能收结果
    output wire [31:0]      ci_outputs_0,       //(O) 结果（v1 回固定标识）

    //==================== 配置口（APB）====================
    input  wire             lineBuffer_mode,    //(I) 0=全屏(5760) 1=分屏(2880)

    //==================== 行任务口（广播给各源 / 合成器 / 写侧）====================
    output wire             line_start,         //(O) 单拍脉冲：这一行的任务在这拍有效
    output wire [31:0]      line_dst_addr,      //(O) 这一行的目的起始地址
    output wire [31:0]      line_src_addr,      //(O) 这一行的源起始地址；不要源时恒 0
    output wire [15:0]      line_w,             //(O) 这一行多少像素
    output wire [6:0]       line_flags,         //(O) 合成模式，原样转发
    output wire [23:0]      line_param,         //(O) 填充色，原样转发
    input  wire             line_done,          //(I) 单拍脉冲：这一行已经写进 DDR
    input  wire             line_buffer_free,   //(I) 下游（各源 + 写侧）都还能接下一行（合成器汇总后回传）

    //==================== 观测口（原样透传 CmdExec 的，给 APB 寄存器）====================
    output wire             acc_busy,           //(O) 有命令在手上（没做完）
    output wire             cmd_done,           //(O) 单拍：这一条命令真做完了
    output wire             frame_done          //(O) 单拍：FENCE 完成 = 本帧画完（→ 显示侧交帧）
);

//==============================================================================
// wire：队列出口（本层内部连线，不再跨模块边界）
//==============================================================================
wire            cmd_valid;
wire            cmd_ready;
wire [2:0]      cmd_type;
wire [31:0]     cmd_src;
wire [31:0]     cmd_dst;
wire [15:0]     cmd_w;
wire [15:0]     cmd_h;
wire [6:0]      cmd_flags;
wire [23:0]     cmd_color;

//==============================================================================
// 例化
//==============================================================================
CmdBuffer #(
    .FIFO_AW            (FIFO_AW)
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
    .cmd_color          (cmd_color)
);

CmdExec #(
    .LINE_BYTES         (LINE_BYTES)
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
    .cmd_color          (cmd_color),

    .lineBuffer_mode    (lineBuffer_mode),

    .line_start         (line_start),
    .line_dst_addr      (line_dst_addr),
    .line_src_addr      (line_src_addr),
    .line_w             (line_w),
    .line_flags         (line_flags),
    .line_param         (line_param),
    .line_done          (line_done),
    .line_buffer_free   (line_buffer_free),

    .acc_busy           (acc_busy),
    .cmd_done           (cmd_done),
    .frame_done         (frame_done)
);

endmodule
