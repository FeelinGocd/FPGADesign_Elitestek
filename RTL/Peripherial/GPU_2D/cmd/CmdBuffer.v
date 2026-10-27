//==============================================================================
//  CmdBuffer.v —— 命令入口：CI 握手应答 + 命令队列
//------------------------------------------------------------------------------
//  它是两个东西的容器
//      CI 应答握手（本文件的时序逻辑）：收下一条 CI 就欠一个响应，直到 CPU 收走
//      async_fifo（子模块）           ：存「待执行的命令」，内部含 BlockRAM
//      （"几条 CI 拼成一条命令"在 CIUnpack 里，挂在 CIUnpack 那一级）

//  对外只有两组接口（★ 接口就是那几根命令线，每张图一格的线本来就宽）
//      左：CI 的 8 根线（接 SoC 的 cpu0_customInstruction_*）
//      右：命令队列出口（接 accCmdExec）

//  命令在 FIFO 里怎么摆
//      进队列就是把几根线首尾接起来存（BlockRAM 只能存一根宽字），出来再拆回原样。
//      ★ 这只是【存储】的摆法，接口上仍然是分开的几根线 ——
//        地址那根还是地址，数据那几根还是数据，谁也看不见这个拼接。
//      ★ 宽度 = 3 + 32 + 16 + 16 + (32+8+32)*IMAGE_NUM
//               = 67 + 72*IMAGE_NUM（IMAGE_NUM=2 时 = 211 位）
//        再补到整字节 = 216 位（27 字节），BlockRAM 的宽度要求。
//        3 = cmd_type，32 = dst，16 = w，16 = h；每图一格 = 32 源 + 8 flags + 32 参数

//  两个时钟域
//      CI 侧   clk_ci  = CPU 时钟（CI 挂在 CPU 流水线上，不能跨域）
//      命令侧  clk_acc = 加速器时钟（v1 与 clk_ci 同源，将来要独立就只改顶层接线）
//      ★ 跨域点就在这里：整条命令是一百多位的数据，只能靠异步 FIFO 跨，
//        不能拿逐位同步器去同步（各 bit 延迟不同会拼出错数据）

//  CI 应答为什么用计数器（而不是「等上一次响应握完才收下一条」）
//      CI 是阻塞式握手：每条指令都要一个响应，不给响应 CPU 会卡在写回级。
//      我们无法确定 CPU 会不会流水式连发（响应还没被收走就发下一条），
//      所以用 rsp_cnt 如实记账：收一条 +1，被收走一条 -1，欠着就把 rsp_valid 拉住。
//      ★ buffer 的职责就是吸收命令 burst，入口不该人为设卡：
//        进料只受「队列满」和「计数防溢出」两个约束

//  接口约定
//      1) 出口是【标准 valid/ready】：cmd_valid=1 时各根命令线【已经有效】，
//         拉一拍 cmd_ready 即取走一条；队列里还有下一条的话，下一拍 cmd_valid 立刻保持 1（无气泡）
//      2) 内部用一级 FWFT 预读把 BlockRAM 的「读延迟 1 拍」藏起来 ——
//         下游完全不需要知道这个队列是 BRAM 做的
//         ★ 预读规则：手上没货（或货正被取走）且队列非空 → 就去读一条备着
//      3) 命令的【内容契约】（由 CIUnpack 保证，下游可直接依赖）：
//         类型 ∈ {1,2,3}；FILL/BLIT 的 宽≥1 且 高≥1；FENCE 的其余线恒为 0
//      4) ci_outputs_0 回一个固定标识：v1 全用 push，返回值用不到，
//         但必须给确定值（悬空会警告，将来若用 cfu_type_R 读会是垃圾）；
//         顺带上板烧完读一次就知道 CI 通路通不通

//  参考：docs/spec/硬件_加速器.md §1.1 / §7.1
//  上一级是谁
//      cmd/CmdChannel.v —— 它把本模块和 CmdExec 装成"命令通道"。
//==============================================================================
`timescale 1ns/100ps

module CmdBuffer #(
    parameter FIFO_AW   = 9,                // 命令队列深度 = 2^9 = 512 条
    parameter IMAGE_NUM = 2                 // 一条命令几张图（★ 与 CIUnpack 一致）
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

    //==================== 命令队列出口（接 accCmdExec）  ====================
    output wire                     cmd_valid,          //(O) 队列非空：有命令可取
    input  wire                     cmd_ready,          //(I) 下游取走一条
    output wire [2:0]               cmd_type,   //(O) 3'd1=FILL / 3'd2=BLIT / 3'd3=FENCE
    output wire [31:0]              cmd_dst,    //(O) 目的地址（几张图共用）
    output wire [15:0]              cmd_w,      //(O) 宽（目标矩形，共用）
    output wire [15:0]              cmd_h,      //(O) 高（目标矩形，共用）
    output wire [32*IMAGE_NUM-1:0]  cmd_src,    //(O) 每张图的源地址：一格 = 一张图
    output wire [8*IMAGE_NUM-1:0]   cmd_flags,  //(O) 每张图的模式位：一格 = 一张图
    output wire [32*IMAGE_NUM-1:0]  cmd_args   //(O) 每张图自己的参数：一格 = 一张图（32 位）
);

//==============================================================================
// localparam
//==============================================================================
//   存进 BlockRAM 的那一根宽字（只是存法，接口上还是分开的几根线）
localparam  CMD_W       = 3 + 32 + 16 + 16 + (32 + 8 + 32)*IMAGE_NUM;   // 有效位
localparam  FIFO_W      = ((CMD_W + 7) / 8) * 8;                        // 补到整字节
localparam  RSP_CNT_MAX = 3'd7;             // 欠响应计数上限（防溢出；实际到不了）
localparam  CI_ID       = 32'h434D4431;     // "CMD1"：v1 的固定返回标识

//==============================================================================
// wire / reg
//==============================================================================
// 子模块互连（CI 侧）
wire                                dec_in_ready;
wire                                dec_cmd_wr;
wire [2:0]                          dec_type;
wire [31:0]                         dec_dst;
wire [15:0]                         dec_w;
wire [15:0]                         dec_h;
wire [32*IMAGE_NUM-1:0]             dec_src;
wire [8*IMAGE_NUM-1:0]              dec_flags;
wire [32*IMAGE_NUM-1:0]             dec_args;

// FIFO 接口
wire [FIFO_W-1:0]                   fifo_wr_data;
wire [FIFO_W-1:0]                   fifo_rd_data;
wire                                fifo_wr_full;
wire                                fifo_rd_empty;

// 出口预读（FWFT）
reg                                 hold_valid;             // 手上有一条已经就绪的命令
wire                                pre_rd;                 // 内部预读（下游看不见）

// CI 应答记账
reg  [2:0]                          rsp_cnt;                    // 欠 CPU 几个响应
wire                                rsp_cnt_full;
wire                                cmd_fire;                   // CI 的 cmd 握手成功
wire                                rsp_fire;                   // CI 的 rsp 握手成功

//==============================================================================
// assign 区：打包 / 拆包（进出 FIFO 共用同一串顺序，不用记位段偏移）
//==============================================================================
assign fifo_wr_data = {{(FIFO_W-CMD_W){1'b0}},
                       dec_args, dec_flags, dec_src, dec_h, dec_w, dec_dst, dec_type};

assign {cmd_args, cmd_flags, cmd_src, cmd_h, cmd_w, cmd_dst, cmd_type} = fifo_rd_data[CMD_W-1:0];

//==============================================================================
// 判断组合条件
//==============================================================================
// CI 侧：收一条就欠一个响应；欠着就一直拉 rsp_valid
assign cmd_fire         = ci_cmd_valid & ci_cmd_ready;
assign rsp_fire         = ci_rsp_valid & ci_rsp_ready;
assign rsp_cnt_full     = (rsp_cnt == RSP_CNT_MAX);

// 收料的两个约束：队列没满 + 计数没顶
assign ci_cmd_ready     = dec_in_ready & ~rsp_cnt_full;

// 队列出口（FWFT 预读）
//   手上没货、或货正被取走，且队列非空 → 预读一条备着（数据下一拍到）
assign pre_rd           = ~fifo_rd_empty & (~hold_valid | cmd_ready);
assign cmd_valid        = hold_valid;                    // 数据有效才说有效

//==============================================================================
// 组合输出
//==============================================================================
assign ci_rsp_valid     = |rsp_cnt;             // 还欠着 → 一直拉高，直到 CPU 收走
assign ci_outputs_0     = CI_ID;                // v1 固定标识（用 push 时 CPU 不会读它）

//==============================================================================
// 例化
//==============================================================================
CIUnpack #(
    .IMAGE_NUM                      (IMAGE_NUM)
) u_CIUnpack (
    .clk                            (clk_ci),
    .rst_n                          (rst_ci_n),
    .in_valid                       (ci_cmd_valid),
    .in_function_id                 (ci_function_id),
    .in_inputs_0                    (ci_inputs_0),
    .in_inputs_1                    (ci_inputs_1),
    .in_ready                       (dec_in_ready),
    .cmd_wr                         (dec_cmd_wr),
    .fifo_full                      (fifo_wr_full),
    .cmd_type                       (dec_type),
    .cmd_dst                        (dec_dst),
    .cmd_w                          (dec_w),
    .cmd_h                          (dec_h),
    .cmd_src                        (dec_src),
    .cmd_flags                      (dec_flags),
    .cmd_args                       (dec_args)
);

async_fifo #(
    .WR_WIDTH                       (FIFO_W),
    .ADDR_WIDTH                     (FIFO_AW),
    .RD_WIDTH                       (FIFO_W)        // 该参数在 async_fifo 里未参与拼接，占位即可
) u_cmd_fifo (
    // 写侧：CI 时钟域
    .clk_wr                         (clk_ci),
    .rst_wr_n                       (rst_ci_n),
    .data_in                        (fifo_wr_data),
    .en_wr                          (dec_cmd_wr),
    .wr_full                        (fifo_wr_full),
    // 读侧：命令时钟域
    .clk_rd                         (clk_acc),
    .rst_rd_n                       (rst_acc_n),
    .data_out                       (fifo_rd_data),
    .en_rd                          (pre_rd),               // 内部预读，不是 cmd_ready
    .rd_empty                       (fifo_rd_empty)
);

//==============================================================================
// 时序逻辑：出口预读的状态位（FWFT）
//   预读发出 → 下一拍数据到位 → 置 1
//   被取走   → 清 0；若同拍又预读了新的，则保持 1 —— 于是没有气泡
//==============================================================================
always @(posedge clk_acc or negedge rst_acc_n) begin
    if (!rst_acc_n) begin
        hold_valid <= 1'b0;
    end
    else if (pre_rd) begin
        hold_valid <= 1'b1;
    end
    else if (cmd_ready) begin
        hold_valid <= 1'b0;
    end
end

//==============================================================================
// 时序逻辑：欠响应计数（收一条 +1，被收走一条 -1，两者同拍抵消）
//   ★ 计数到顶就不再收料（防止把响应记丢 —— 丢一个响应，CPU 会永久卡死）
//==============================================================================
always @(posedge clk_ci or negedge rst_ci_n) begin
    if (!rst_ci_n) begin
        rsp_cnt <= 3'd0;
    end
    else begin
        case ({cmd_fire, rsp_fire})
            2'b10: if (!rsp_cnt_full) rsp_cnt <= rsp_cnt + 3'd1;
            2'b01: rsp_cnt <= rsp_cnt - 3'd1;
            default: ;                  // 都没有 / 同拍一进一出：不动
        endcase
    end
end

endmodule
