//==============================================================================
//  CmdBuffer.v —— 命令入口：CI 握手应答 + 两条 CI 拼一条命令 + 命令队列
//------------------------------------------------------------------------------
//  它是三个东西的容器
//      CI 应答握手（本文件的时序逻辑）：收下一条 CI 就欠一个响应，直到 CPU 收走
//      CIUnpack（子模块）             ：两条 CI 拼成一条命令
//      async_fifo（子模块）           ：存「待执行的命令」，内部含 BlockRAM

//  对外只有两组接口
//      左：CI 的 8 根线（接 SoC 的 cpu0_customInstruction_*）
//      右：命令队列出口（接 accCmdExec）

//  两个时钟域
//      CI 侧   clk_ci  = CPU 时钟（CI 挂在 CPU 流水线上，不能跨域）
//      命令侧  clk_acc = 加速器时钟（v1 与 clk_ci 同源，将来要独立就只改顶层接线）
//      ★ 跨域点就在这里：命令是 144 位数据，只能靠异步 FIFO 跨，
//        不能拿逐位同步器去同步（各 bit 延迟不同会拼出错数据）

//  命令在 FIFO 里的打包顺序（144 位，有效 130 位）
//      { 保留(14) , 颜色(24) , flags(7) , 高(16) , 宽(16) , 目的地址(32) , 源地址(32) , 类型(3) }
//      ★ BlockRAM 宽度必须是 8 的整数倍 → 130 位补到 144 位（18 字节）
//      ★ 读写两侧用同一串拼接顺序，不用记位段偏移

//  CI 应答为什么用计数器（而不是「等上一次响应握完才收下一条」）
//      CI 是阻塞式握手：每条指令都要一个响应，不给响应 CPU 会卡在写回级。
//      我们无法确定 CPU 会不会流水式连发（响应还没被收走就发下一条），
//      所以用 rsp_cnt 如实记账：收一条 +1，被收走一条 -1，欠着就把 rsp_valid 拉住。
//      ★ buffer 的职责就是吸收命令 burst，入口不该人为设卡：
//        进料只受「队列满」和「计数防溢出」两个约束

//  接口约定
//      1) 出口是【标准 valid/ready】：cmd_valid=1 时七个字段【已经有效】，
//         拉一拍 cmd_ready 即取走一条；队列里还有下一条的话，下一拍 cmd_valid 立刻保持 1（无气泡）
//      2) 内部用一级 FWFT 预读把 BlockRAM 的「读延迟 1 拍」藏起来 ——
//         下游完全不需要知道这个队列是 BRAM 做的
//         ★ 预读规则：手上没货（或货正被取走）且队列非空 → 就去读一条备着
//      3) 队列出口的【内容契约】（由 CIUnpack 保证，下游可直接依赖）：
//         类型 ∈ {1,2,3}；FILL/BLIT 的 宽≥1 且 高≥1；FENCE 的其余字段恒为 0
//      4) ci_outputs_0 回一个固定标识：v1 全用 push，返回值用不到，
//         但必须给确定值（悬空会警告，将来若用 cfu_type_R 读会是垃圾）；
//         顺带上板烧完读一次就知道 CI 通路通不通

//  参考：docs/spec/硬件_加速器.md §1.1 / §7.1
//==============================================================================
`timescale 1ns/100ps

module CmdBuffer #(
    parameter FIFO_AW = 9                   // 命令队列深度 = 2^9 = 512 条（≈9.2 KB）
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

    //==================== 命令队列出口（接 accCmdExec）====================
    output wire             cmd_valid,          //(O) 队列非空：有命令可取
    input  wire             cmd_ready,          //(I) 下游取走一条
    output wire [2:0]       cmd_type,           //(O) 3'd1=FILL / 3'd2=BLIT / 3'd3=FENCE
    output wire [31:0]      cmd_src,            //(O) 源地址
    output wire [31:0]      cmd_dst,            //(O) 目的地址
    output wire [15:0]      cmd_w,              //(O) 宽度（像素）
    output wire [15:0]      cmd_h,              //(O) 高度（行）
    output wire [6:0]       cmd_flags,          //(O) 源格式 / 透明使能
    output wire [23:0]      cmd_color           //(O) 填充色（RGB888）
);

//==============================================================================
// localparam
//==============================================================================
localparam  FIFO_W      = 144;              // 一条命令的存储宽度（130 位有效 + 14 位填充）
localparam  RSP_CNT_MAX = 3'd7;             // 欠响应计数上限（防溢出；实际到不了）
localparam  CI_ID       = 32'h434D4431;     // "CMD1"：v1 的固定返回标识

//==============================================================================
// wire / reg
//==============================================================================
// 子模块互连（CI 侧）
wire                dec_in_ready;
wire                dec_cmd_wr;
wire [2:0]          dec_type;
wire [31:0]         dec_src;
wire [31:0]         dec_dst;
wire [15:0]         dec_w;
wire [15:0]         dec_h;
wire [6:0]          dec_flags;
wire [23:0]         dec_color;

// FIFO 接口
wire [FIFO_W-1:0]   fifo_wr_data;
wire [FIFO_W-1:0]   fifo_rd_data;
wire                fifo_wr_full;
wire                fifo_rd_empty;

// 出口预读（FWFT）
reg                 hold_valid;             // 手上有一条已经就绪的命令
wire                pre_rd;                 // 内部预读（下游看不见）

// CI 应答记账
reg  [2:0]          rsp_cnt;                    // 欠 CPU 几个响应
wire                rsp_cnt_full;
wire                cmd_fire;                   // CI 的 cmd 握手成功
wire                rsp_fire;                   // CI 的 rsp 握手成功

//==============================================================================
// assign 区：打包 / 拆包（两侧同一串拼接顺序，不用记位段偏移）
//==============================================================================
assign fifo_wr_data = {14'd0, dec_color, dec_flags, dec_h, dec_w, dec_dst, dec_src, dec_type};

assign {cmd_color, cmd_flags, cmd_h, cmd_w, cmd_dst, cmd_src, cmd_type} = fifo_rd_data[129:0];

//==============================================================================
// 判断组合条件
//==============================================================================
// CI 侧：收一条就欠一个响应；欠着就一直拉 rsp_valid
assign cmd_fire     = ci_cmd_valid & ci_cmd_ready;
assign rsp_fire     = ci_rsp_valid & ci_rsp_ready;
assign rsp_cnt_full = (rsp_cnt == RSP_CNT_MAX);

// 收料的两个约束：队列没满 + 计数没顶
assign ci_cmd_ready = dec_in_ready & ~rsp_cnt_full;

// 队列出口（FWFT 预读）
//   手上没货、或货正被取走，且队列非空 → 预读一条备着（数据下一拍到）
assign pre_rd       = ~fifo_rd_empty & (~hold_valid | cmd_ready);
assign cmd_valid    = hold_valid;                    // 数据有效才说有效

//==============================================================================
// 组合输出
//==============================================================================
assign ci_rsp_valid = |rsp_cnt;             // 还欠着 → 一直拉高，直到 CPU 收走
assign ci_outputs_0 = CI_ID;                // v1 固定标识（用 push 时 CPU 不会读它）

//==============================================================================
// 例化
//==============================================================================
CIUnpack u_CIUnpack (
    .clk                (clk_ci),
    .rst_n              (rst_ci_n),
    .in_valid           (ci_cmd_valid),
    .in_function_id     (ci_function_id),
    .in_inputs_0        (ci_inputs_0),
    .in_inputs_1        (ci_inputs_1),
    .in_ready           (dec_in_ready),
    .cmd_wr             (dec_cmd_wr),
    .fifo_full          (fifo_wr_full),
    .cmd_type           (dec_type),
    .cmd_src            (dec_src),
    .cmd_dst            (dec_dst),
    .cmd_w              (dec_w),
    .cmd_h              (dec_h),
    .cmd_flags          (dec_flags),
    .cmd_color          (dec_color)
);

async_fifo #(
    .WR_WIDTH           (FIFO_W),
    .ADDR_WIDTH         (FIFO_AW),
    .RD_WIDTH           (FIFO_W)        // 该参数在 async_fifo 里未参与拼接，占位即可
) u_cmd_fifo (
    // 写侧：CI 时钟域
    .clk_wr             (clk_ci),
    .rst_wr_n           (rst_ci_n),
    .data_in            (fifo_wr_data),
    .en_wr              (dec_cmd_wr),
    .wr_full            (fifo_wr_full),
    // 读侧：命令时钟域
    .clk_rd             (clk_acc),
    .rst_rd_n           (rst_acc_n),
    .data_out           (fifo_rd_data),
    .en_rd              (pre_rd),               // 内部预读，不是 cmd_ready
    .rd_empty           (fifo_rd_empty)
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