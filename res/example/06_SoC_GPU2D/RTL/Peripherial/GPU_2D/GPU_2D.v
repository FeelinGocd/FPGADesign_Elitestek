//==============================================================================
//  GPU_2D.v —— 2D 加速器 + 显示通道的功能层顶层
//------------------------------------------------------------------------------
//  功能
//      CPU 用 CI 发一条命令，本模块把它变成对 DDR 的读/写搬运；
//      同时带着显示通道，把 DDR 里的帧缓存扫成 HDMI 时序送出去。
//
//  结构（按数据流排；每个模块的功能写在它自己的文件头）
//      ① cmd/CmdChannel               CI → 命令 → 行任务
//      ② pixel/DDR/DDRSource          源①：DDR 行 → 像素流
//      ②′pixel/palette/PaletteSource  源②：片上素材 → 调色板 → 像素流
//      ③ factory/Factory              合成 → 补齐 AXI 一拍 → 写回 DDR
//      ④ interface/AXIMastersHandler  AXI 主口（读 + 写）
//      ⑤ interface/GPU2D_APBHandler   寄存器 <-> 有效信号 / 事件计数
//      ⑥ lineBuffer/lineBuffer        显示通道：取数 + 跨域双 bank + 拆包 + HDMI
//      ★ ⑦ 总线层 APB0_gpu2d【不在本模块里】，在 APB0_Interconnect.v 里例化
//
//  连接
//      行任务：CmdChannel ─▶ DDRSource / PaletteSource / Factory（广播，不接力）
//      数据  ：DDRSource、PaletteSource ─▶ Factory ─▶ AXIMastersHandler ─▶ DDR
//      完成  ：WriteController ─line_done─▶ CmdChannel ─▶ CmdExec
//      寄存器：APB0_gpu2d ─端口─▶ ⑤ Handler ─▶ ⑥ 显示通道 / 加速器
//              ◀─端口─ acc_frame_done / done_clr / fps_*_val / 六个观测值
//
//  时钟
//      clk_ci / clk_acc（v1 同源）  ；  clk_HDMI_x1 = 148.5MHz 像素域（只有 ⑥ 用）
//
//  参考：docs/spec/硬件_加速器.md、docs/spec/硬件_显示通路.md
//==============================================================================
`timescale 1ns/100ps

module GPU_2D #(
    parameter FIFO_AW       = 9,                    // 命令队列深度 = 2^9 = 512 条
    parameter LINE_BYTES    = 5760,                 // 单段行宽（全屏 1920×3）
    parameter AXI_ADDR_WIDTH= 32,
    parameter AXI_DATA_WIDTH= 128,
    parameter AXI_STRB_WIDTH= AXI_DATA_WIDTH/8,
    parameter AXI_ID_WIDTH  = 8,                    // 与 Axi_Mux 的 SLAVE_ID_WIDTH 一致
    parameter AXI_LEN_WIDTH = 8,
    parameter AXI_ID        = {AXI_ID_WIDTH{1'b0}}
)(
    //==================== 时钟 / 复位 ====================
    input  wire                     clk_ci,         //(I) CI 侧时钟（= CPU 时钟）
    input  wire                     rst_ci_n,       //(I) CI 侧复位，异步低有效
    input  wire                     clk_acc,        //(I) 命令侧时钟（v1 与 clk_ci 同源）
    input  wire                     rst_acc_n,      //(I) 命令侧复位，异步低有效
    input  wire                     clk_HDMI_x1,    //(I) 显示像素时钟 @148.5MHz（显示通道用）
    input  wire                     rst_hdmi_n,     //(I) 显示通道复位（顶层算好：axi_rst_n & pll_HDMI_locked）

    //==================== CI 端口（接 SoC 的 cpu0_customInstruction_*）====================
    input  wire                     ci_cmd_valid,   //(I) CPU 有命令要发
    output wire                     ci_cmd_ready,   //(O) 我们收下了
    input  wire [9:0]               ci_function_id, //(I) {funct7[6:0], funct3[2:0]}
    input  wire [31:0]              ci_inputs_0,    //(I) 第一参数
    input  wire [31:0]              ci_inputs_1,    //(I) 第二参数
    output wire                     ci_rsp_valid,   //(O) 结果好了
    input  wire                     ci_rsp_ready,   //(I) CPU 能收结果
    output wire [31:0]              ci_outputs_0,   //(O) 结果（v1 回固定标识）

    //==================== 配置口 ====================
    //   行模式（0=全屏 5760 / 1=分屏 2880）跟着显示走，锁存逻辑在本模块内部（见下面
    //   lineBuffer_mode_r）。显示通道也在本模块里 ⇒ 两边用同一个值、同一拍切换，
    //   不存在"接错线导致加速器和显示不同模式"的可能。
    //   源和目的同跨距，两个都在 CmdExec 内部由它派生 —— 没有单独的 src_stride

    //==================== AXI4 主口 → Axi_Mux S3（写）====================
    // ---- AW ----
    output wire [AXI_ID_WIDTH-1:0]  m_AXI_AWID,
    output wire [AXI_ADDR_WIDTH-1:0]m_AXI_AWADDR,
    output wire [AXI_LEN_WIDTH-1:0] m_AXI_AWLEN,
    output wire [2:0]               m_AXI_AWSIZE,
    output wire [1:0]               m_AXI_AWBURST,
    output wire                     m_AXI_AWLOCK,
    output wire [3:0]               m_AXI_AWCACHE,
    output wire [2:0]               m_AXI_AWPROT,
    output wire [3:0]               m_AXI_AWQOS,
    output wire [3:0]               m_AXI_AWREGION,
    output wire                     m_AXI_AWVALID,
    input  wire                     m_AXI_AWREADY,
    // ---- W ----
    output wire [AXI_DATA_WIDTH-1:0]m_AXI_WDATA,
    output wire [AXI_STRB_WIDTH-1:0]m_AXI_WSTRB,
    output wire                     m_AXI_WLAST,
    output wire                     m_AXI_WVALID,
    input  wire                     m_AXI_WREADY,
    // ---- B ----
    input  wire [AXI_ID_WIDTH-1:0]  m_AXI_BID,
    input  wire [1:0]               m_AXI_BRESP,
    input  wire                     m_AXI_BVALID,
    output wire                     m_AXI_BREADY,

    //==================== AXI4 主口 → Axi_Mux S2（读）====================
    // ---- AR ----
    output wire [AXI_ID_WIDTH-1:0]  m_AXI_ARID,
    output wire [AXI_ADDR_WIDTH-1:0]m_AXI_ARADDR,
    output wire [AXI_LEN_WIDTH-1:0] m_AXI_ARLEN,
    output wire [2:0]               m_AXI_ARSIZE,
    output wire [1:0]               m_AXI_ARBURST,
    output wire                     m_AXI_ARLOCK,
    output wire [3:0]               m_AXI_ARCACHE,
    output wire [2:0]               m_AXI_ARPROT,
    output wire [3:0]               m_AXI_ARQOS,
    output wire [3:0]               m_AXI_ARREGION,
    output wire                     m_AXI_ARVALID,
    input  wire                     m_AXI_ARREADY,
    // ---- R ----
    input  wire [AXI_ID_WIDTH-1:0]  m_AXI_RID,
    input  wire [AXI_DATA_WIDTH-1:0]m_AXI_RDATA,
    input  wire [1:0]               m_AXI_RRESP,
    input  wire                     m_AXI_RLAST,
    input  wire                     m_AXI_RVALID,
    output wire                     m_AXI_RREADY,

    //==================== APB 寄存器侧（槽 2；总线层在 APB0_Interconnect 里）====================
    //   ★ APB0_gpu2d（总线层）例化在 APB0_Interconnect 里 ⇒ 本模块【没有 APB 从口】，
    //     只留"已经翻译好的寄存器值"这组功能侧端口（名字和 APB0_gpu2d 的端口一致）。
    //   ★ 地址（绝对 = 0xf8102000 + 槽内偏移，子块位 = PADDR[6]）：
    //        0x2000 - 0x201F  加速器观测寄存器（GPU2D_ID 在 0x2000）
    //        0x2040 - 0x206F  显示通道寄存器（LINEBUFFER_ID 在 0x2040）
    //   ★ 进 = 寄存器格子给的值；出 = 回寄存器格子的事件/观测值
    //     进：显示子块的静态配置
    input  wire                     enable,                 //(I) STATUS.ENABLE
    input  wire                     split_pending,          //(I) STATUS.SPLIT(待生效值)
    input  wire [63:0]              framea_base_addr,       //(I) {A左半(acc), A右半(cpu)}
    input  wire [63:0]              frameb_base_addr,       //(I) {B左半(acc), B右半(cpu)}
    input  wire [63:0]              frame_full_base_addr,   //(I) {全屏 C, 全屏 D}
    //   颜色键控(ck)的键色(显示子块 0x2078, 复位默认白): 源侧拿它比像素, 相等 = 不写
    input  wire [23:0]              ckey_color,             //(I) CKEY_COLOR
    input  wire                     done_gpu2d,               //(I) 加速器那半有新帧
    input  wire                     done_cpu,               //(I) CPU 那半有新帧
    //     出：交帧 / 帧率 / 观测
    output wire                     acc_frame_done,         //(O) FENCE 完成 -> 置 DONE_ACC【单拍】
    output wire [1:0]               done_clr,               //(O) {cpu,acc} 翻页消费后清位
    output wire [31:0]              fps_acc_val,            //(O) 算好的帧率
    output wire [31:0]              fps_cpu_val,
    output wire [31:0]              busy_val,               //(O) STATUS.BUSY
    output wire [31:0]              cmd_done_val,           //(O) 已完成命令数
    output wire [31:0]              frame_done_val,         //(O) 已完成帧数
    output wire [31:0]              wr_beats_val,           //(O) 已写出的 AXI 拍数
    output wire [31:0]              rd_beats_val,           //(O) 已读回的 AXI 拍数
    output wire [31:0]              busy_cyc_val,           //(O) 忙的周期数

    //==================== 显示通道 AXI 主口 → Axi_Mux S1（取数）====================
    output wire [AXI_ID_WIDTH-1:0]  lineBuffer_m_AXI_AWID,
    output wire [AXI_ADDR_WIDTH-1:0]lineBuffer_m_AXI_AWADDR,
    output wire [AXI_LEN_WIDTH-1:0] lineBuffer_m_AXI_AWLEN,
    output wire [2:0]               lineBuffer_m_AXI_AWSIZE,
    output wire [1:0]               lineBuffer_m_AXI_AWBURST,
    output wire                     lineBuffer_m_AXI_AWLOCK,
    output wire                     lineBuffer_m_AXI_AWVALID,
    input  wire                     lineBuffer_m_AXI_AWREADY,

    output wire [AXI_DATA_WIDTH-1:0]lineBuffer_m_AXI_WDATA,
    output wire [AXI_STRB_WIDTH-1:0]lineBuffer_m_AXI_WSTRB,
    output wire                     lineBuffer_m_AXI_WLAST,
    output wire                     lineBuffer_m_AXI_WVALID,
    input  wire                     lineBuffer_m_AXI_WREADY,

    input  wire [AXI_ID_WIDTH-1:0]  lineBuffer_m_AXI_BID,
    input  wire [1:0]               lineBuffer_m_AXI_BRESP,
    input  wire                     lineBuffer_m_AXI_BVALID,
    output wire                     lineBuffer_m_AXI_BREADY,

    output wire [AXI_ID_WIDTH-1:0]  lineBuffer_m_AXI_ARID,
    output wire [AXI_ADDR_WIDTH-1:0]lineBuffer_m_AXI_ARADDR,
    output wire [AXI_LEN_WIDTH-1:0] lineBuffer_m_AXI_ARLEN,
    output wire [2:0]               lineBuffer_m_AXI_ARSIZE,
    output wire [1:0]               lineBuffer_m_AXI_ARBURST,
    output wire                     lineBuffer_m_AXI_ARLOCK,
    output wire                     lineBuffer_m_AXI_ARVALID,
    input  wire                     lineBuffer_m_AXI_ARREADY,

    input  wire [AXI_ID_WIDTH-1:0]  lineBuffer_m_AXI_RID,
    input  wire [AXI_DATA_WIDTH-1:0]lineBuffer_m_AXI_RDATA,
    input  wire [1:0]               lineBuffer_m_AXI_RRESP,
    input  wire                     lineBuffer_m_AXI_RLAST,
    input  wire                     lineBuffer_m_AXI_RVALID,
    output wire                     lineBuffer_m_AXI_RREADY,

    //==================== HDMI 输出（接硬核 LVDS TX）====================
    output wire [9:0]               tmds_blue_o,
    output wire [9:0]               tmds_green_o,
    output wire [9:0]               tmds_red_o,
    output wire [9:0]               tmds_clk_o,
    output wire                     tmds_data0_TX_OE,
    output wire                     tmds_data1_TX_OE,
    output wire                     tmds_data2_TX_OE,
    output wire                     tmds_clk_TX_OE,
    output wire                     tmds_data0_TX_RST,
    output wire                     tmds_data1_TX_RST,
    output wire                     tmds_data2_TX_RST,
    output wire                     tmds_clk_TX_RST

    //★ acc_frame_done 不再是端口：FENCE 完成 = 本帧所有绘制都落盘了 -> 直接接本模块内的
    //  APB0_gpu2d 的显示子块（置 DONE_ACC -> 帧边界翻页 + 帧率统计），不再出模块绕一圈。
);

//==============================================================================
// wire：子模块之间的内部连线
//==============================================================================
// ---- 行任务（CmdChannel 广播给三个下游）----
wire                        line_start;
wire [31:0]                 line_dst_addr;
wire [31:0]                 line_src_addr;
wire [15:0]                 line_w;
wire [6:0]                  line_flags;
wire [23:0]                 line_param;
wire                        line_done;
wire                        line_buffer_free;               // 下游还能不能接下一行（Factory 汇总后回给 CmdExec）

// ---- 源① 的像素流（DDRSource → Compositor）----
//   一拍 = 4 个 RGB888 像素 = 96 位；★ 存的是【像素】不是字节拍（见 DDRSource 的文件头）
wire [95:0]                 src0_data;
wire                        src0_valid;
wire                        src0_rd_en;
// ---- 源②：调色板素材源的像素流（→ Factory 的 src1_*，选路在合成器里）----
wire [95:0]                 src1_data;
wire [11:0]                 src1_mask;           // ★ ck 掩码：和 src1_data 同拍（1 位/字节）
wire [31:0]                 src1_alpha;          // ★ α：每像素 8 位，和 src1_data 同拍（旁路，不进像素流）
wire                        src1_valid;
wire                        src1_rd_en;
wire                        src1_free;
wire                        src_free;

//   合成器 → 打包器 → 写控制器 的内部连线（像素流 / AXI 写拍 / 两路 free 相与）
//   现在都封在 factory/Factory.v 里面，顶层不再出现 ——
//   这一层对外只有"行任务 + 像素流入口 + AXI 写口 + line_done + buf_free"

// ---- 读命令 / 读数据（DDRSource ↔ AXIMastersHandler）----
wire                        rd_cmd_valid;
wire                        rd_cmd_ready;
wire [AXI_ADDR_WIDTH-1:0]   rd_cmd_addr;
wire [AXI_LEN_WIDTH-1:0]    rd_cmd_len;
wire                        rd_cmd_done;
wire [AXI_DATA_WIDTH-1:0]   rd_data;
wire                        rd_valid;
wire                        rd_ready;

// ---- 地址任务 + 数据（WriteController → AXIMastersHandler）----
wire                        cmd_wr_valid;
wire                        cmd_wr_ready;
wire [AXI_ADDR_WIDTH-1:0]   cmd_wr_addr;
wire [AXI_LEN_WIDTH-1:0]    cmd_wr_len;
wire                        cmd_wr_done;
wire                        cmd_wr_err;
wire [AXI_DATA_WIDTH-1:0]   wr_data;
wire [AXI_STRB_WIDTH-1:0]   wr_strb;
wire                        wr_valid;
wire                        wr_ready;

// ---- CmdExec 的观测输出（-> GPU2D_APBHandler）----
wire                        acc_busy;                       // 有命令在手上
wire                        cmd_done;                       // 单拍：一条命令做完
wire                        frame_done;                     // 单拍：FENCE 完成 = 本帧画完

// ---- 拍握手：在顶层直接算，不用去改数据通路那几级 ----
wire                        wr_beat_hs;                     // 写出一拍（wr_valid & wr_ready）
wire                        rd_beat_hs;                     // 读回一拍（rd_valid & rd_ready）

assign  wr_beat_hs     = wr_valid & wr_ready;
assign  rd_beat_hs     = rd_valid & rd_ready;
assign  acc_frame_done = frame_done;            // FENCE 完成 -> 置 DONE_ACC（接本模块内的 APB0_gpu2d 显示子块）

// ---- ⑤ Handler 与 显示通道(⑥) 之间的连线（★ 全在模块内）----
//   寄存器格子 ──静态值 + DONE──▶ Handler ──enable/mode/addr──▶ lineBuffer
//   lineBuffer ──frame_start──▶ Handler ──done_clr / 帧率──▶ 寄存器格子
//   寄存器格子(总线层 APB0_gpu2d)在 APB0_Interconnect 里 ⇒ 这些值走模块端口进出。
//   ★ 给【加速器】的行模式不走 Handler: 用下面单独的 lineBuffer_mode_r(复位 rst_acc_n)
wire                        lineBuffer_enable;                  // Handler -> lineBuffer(使能, 不锁存)
wire                        lineBuffer_mode;                    // Handler -> lineBuffer(显示侧模式)
wire [63:0]                 lineBuffer_addr;                    // Handler -> lineBuffer {段0基址, 段1基址}
wire                        lineBuffer_frame_start;             // lineBuffer -> Handler(帧边界单拍)

//==============================================================================
// 行模式锁存（显示侧配置 → 加速器配置口）
//==============================================================================
//   显示侧的模式是两级：待生效(split_pending) → 帧边界生效。
//   本模块跟着【同一个拍】锁存【同一个值】，两边就永远在同一时刻切换 ——
//   不会出现"加速器已按 2880 走、显示还按 5760 扫"那种错位。
//   ★ 值每帧都重采，复位用哪个都无妨，这里用 rst_acc_n
//   ★ 一帧之内模式是稳的，所以 CmdExec / CmdChannel 不用再锁一次
//   ★ 切模式前要确认没有命令在飞：模式一换帧缓冲就整组换(A/B ↔ C/D)，
//     在飞的命令算出来的基址属于旧那组，画进去本来就是废的 —— 这条靠软件约定
reg                         lineBuffer_mode_r;
always @(posedge clk_acc or negedge rst_acc_n) begin
    if (!rst_acc_n)
        lineBuffer_mode_r <= 1'b0;              // 上电默认全屏，和显示侧一致
    else if (lineBuffer_frame_start)
        lineBuffer_mode_r <= split_pending;
end

//==============================================================================
// 例化
//==============================================================================
//------------------------------------------------------------------------------
// ① 命令通道：CI → 命令 → 行任务
//------------------------------------------------------------------------------
CmdChannel #(
    .FIFO_AW                        (FIFO_AW),
    .LINE_BYTES                     (LINE_BYTES)
) u_CmdChannel (
    .clk_ci                         (clk_ci),
    .rst_ci_n                       (rst_ci_n),
    .clk_acc                        (clk_acc),
    .rst_acc_n                      (rst_acc_n),

    .ci_cmd_valid                   (ci_cmd_valid),
    .ci_cmd_ready                   (ci_cmd_ready),
    .ci_function_id                 (ci_function_id),
    .ci_inputs_0                    (ci_inputs_0),
    .ci_inputs_1                    (ci_inputs_1),
    .ci_rsp_valid                   (ci_rsp_valid),
    .ci_rsp_ready                   (ci_rsp_ready),
    .ci_outputs_0                   (ci_outputs_0),

    .lineBuffer_mode                (lineBuffer_mode_r),

    .line_start                     (line_start),
    .line_dst_addr                  (line_dst_addr),
    .line_src_addr                  (line_src_addr),
    .line_w                         (line_w),
    .line_flags                     (line_flags),
    .line_param                     (line_param),
    .line_done                      (line_done),
    .line_buffer_free               (line_buffer_free),

    .acc_busy                       (acc_busy),
    .cmd_done                       (cmd_done),
    .frame_done                     (frame_done)
);

//------------------------------------------------------------------------------
// ② 源①：DDR 读回来 → 拆成像素 → 自己的 ping-pong 存好一行 → 像素流给合成器
//      ★ bank 长在源里，不长在合成器里：谁去读 DDR、数据是什么格式，都是源自己的事
//------------------------------------------------------------------------------
DDRSource #(
    .ADDR_WIDTH                     (AXI_ADDR_WIDTH),
    .AXI_DATA_WIDTH                 (AXI_DATA_WIDTH),
    .LEN_WIDTH                      (AXI_LEN_WIDTH)
) u_DDRSource (
    .clk                            (clk_acc),
    .rst_n                          (rst_acc_n),

    .line_start                     (line_start),
    .line_src_addr                  (line_src_addr),
    .line_dst_addr                  (line_dst_addr),        // alpha(bg_rd) 时它就是读数的行地址；其余场合不用
    .line_flags                     (line_flags),           // ★ 自己看模式：要不要动、读源行还是读目的
    .line_w                         (line_w),

    .pixel_rd_en                    (src0_rd_en),
    .pixel_rd_data                  (src0_data),
    .pixel_rd_valid                 (src0_valid),
    .src_free                       (src_free),

    .rd_cmd_valid                   (rd_cmd_valid),
    .rd_cmd_ready                   (rd_cmd_ready),
    .rd_cmd_addr                    (rd_cmd_addr),
    .rd_cmd_len                     (rd_cmd_len),
    .rd_cmd_done                    (rd_cmd_done),
    .rd_data                        (rd_data),
    .rd_valid                       (rd_valid),
    .rd_ready                       (rd_ready)
);

//------------------------------------------------------------------------------
// ②' 源②：调色板素材源（片上 PRAM 索引 → 查调色板 → 像素流）
//      ★ 出口契约和 DDRSource 一模一样 ⇒ 合成器那组 src1_* 口直接对接
//      ★ 它对 DDR 一个字节都不读：素材在生成 bit 的阶段就烧进片上 RAM 了
//      ★ 本行用哪一路源，由合成器自己按 line_flags 解（和源① 在同一处分选）
//------------------------------------------------------------------------------
PaletteSource #(
    .INDEX_WIDTH                    (8),
    .COLOR_WIDTH                    (24),
    .PIXELS_PER_WORD                (4),
    .RAM_ADDRESS_WIDTH              (15),
    .PALETTE_INIT_FILE              ("palette.hex"),
    .INDEX_INIT_FILE                ("assets.hex")
) u_PaletteSource (
    .clk                            (clk_acc),
    .rst_n                          (rst_acc_n),

    .line_start                     (line_start),

    .line_src_addr                  (line_src_addr),
    .line_dst_addr                  (line_dst_addr),        // 只用低 4 位算"本行要覆盖多少字节"
    .line_w                         (line_w),

    .palette_ready                  (src1_rd_en),
    .key_color                      (ckey_color),       // ★ 颜色键控的键色(APB 0x2078)
    .line_flags                     (line_flags),       // ★ [5] = MX 水平镜像(倒着读字)
    .palette_data                   (src1_data),
    .palette_mask                   (src1_mask),        // ★ 颜色键控掩码，和像素同拍
    .palette_alpha                  (src1_alpha),       // ★ α 旁路：调色板高 8 位，直接给合成器
    .palette_valid                  (src1_valid),
    .src_free                       (src1_free)
);

//------------------------------------------------------------------------------
// ③ 加工面：各路像素流 → 合成 → 补齐成 AXI 一拍 → 写回 DDR
//      ★ 它是"接数据输入 + 接行任务 + 汇总发送节奏"的那一级：
//        内部三级（Compositor / DDRPacker / WriteController）的握手封在里面，
//        顶层只见"像素流入口 / AXI 写口 / line_done / buf_free"
//      ★ 只有它有资格报 line_done（"真的落进 DDR 了"）
//------------------------------------------------------------------------------
Factory #(
    .PIXEL_WIDTH                    (96),                   // 一路像素流一拍 = 4 个 RGB888 像素
    .DATA_WIDTH                     (AXI_DATA_WIDTH),       // AXI 写一拍 16 字节
    .ADDR_WIDTH                     (AXI_ADDR_WIDTH),
    .LEN_WIDTH                      (AXI_LEN_WIDTH)
) u_Factory (
    .clk                            (clk_acc),
    .rst_n                          (rst_acc_n),

    // 行任务口
    .line_start                     (line_start),
    //   ★ flags 是行任务广播里唯一的"模式"载体：ck（[4]）、镜像（[6:5]）、
    //     合成模式（[2:0]）都在里面。合成器自己解，不需要 CmdExec 再解一份喂给它。
    .line_flags                     (line_flags),
    .line_param                     (line_param),
    .line_dst_addr                  (line_dst_addr),
    .line_w                         (line_w),
    .line_done                      (line_done),

    // 像素流入口：源①（DDR 读回的一行）
    .src0_data                      (src0_data),
    .src0_valid                     (src0_valid),
    .src0_rd_en                     (src0_rd_en),
    .src0_free                      (src_free),
    // 像素流入口：源②（调色板素材源）
    .src1_data                      (src1_data),
    .src1_mask                      (src1_mask),
    .src1_alpha                     (src1_alpha),        // ★ α 旁路 → 合成器的混合器
    .src1_valid                     (src1_valid),
    .src1_rd_en                     (src1_rd_en),
    .src1_free                      (src1_free),

    // 发送节奏（各源 + 写侧，在这层汇总）→ CmdExec
    .buf_free                       (line_buffer_free),

    // AXI 写命令口 + 写数据口
    .cmd_wr_valid                   (cmd_wr_valid),
    .cmd_wr_ready                   (cmd_wr_ready),
    .cmd_wr_addr                    (cmd_wr_addr),
    .cmd_wr_len                     (cmd_wr_len),
    .cmd_wr_done                    (cmd_wr_done),
    .cmd_wr_err                     (cmd_wr_err),

    .wr_data                        (wr_data),
    .wr_strb                        (wr_strb),
    .wr_valid                       (wr_valid),
    .wr_ready                       (wr_ready)
);

//------------------------------------------------------------------------------
// ④ AXI 主口封装（读 + 写两个主）
//------------------------------------------------------------------------------
AXIMastersHandler #(
    .ADDR_WIDTH                     (AXI_ADDR_WIDTH),
    .DATA_WIDTH                     (AXI_DATA_WIDTH),
    .STRB_WIDTH                     (AXI_STRB_WIDTH),
    .ID_WIDTH                       (AXI_ID_WIDTH),
    .LEN_WIDTH                      (AXI_LEN_WIDTH),
    .AXI_ID                         (AXI_ID)
) u_AXIMastersHandler (
    .clk                            (clk_acc),
    .rst_n                          (rst_acc_n),

    // ---- 写 ----
    .cmd_wr_valid                   (cmd_wr_valid),
    .cmd_wr_ready                   (cmd_wr_ready),
    .cmd_wr_addr                    (cmd_wr_addr),
    .cmd_wr_len                     (cmd_wr_len),
    .cmd_wr_done                    (cmd_wr_done),
    .cmd_wr_err                     (cmd_wr_err),

    .wr_data                        (wr_data),
    .wr_strb                        (wr_strb),
    .wr_valid                       (wr_valid),
    .wr_ready                       (wr_ready),

    .m_AXI_AWID                     (m_AXI_AWID),
    .m_AXI_AWADDR                   (m_AXI_AWADDR),
    .m_AXI_AWLEN                    (m_AXI_AWLEN),
    .m_AXI_AWSIZE                   (m_AXI_AWSIZE),
    .m_AXI_AWBURST                  (m_AXI_AWBURST),
    .m_AXI_AWLOCK                   (m_AXI_AWLOCK),
    .m_AXI_AWCACHE                  (m_AXI_AWCACHE),
    .m_AXI_AWPROT                   (m_AXI_AWPROT),
    .m_AXI_AWQOS                    (m_AXI_AWQOS),
    .m_AXI_AWREGION                 (m_AXI_AWREGION),
    .m_AXI_AWVALID                  (m_AXI_AWVALID),
    .m_AXI_AWREADY                  (m_AXI_AWREADY),

    .m_AXI_WDATA                    (m_AXI_WDATA),
    .m_AXI_WSTRB                    (m_AXI_WSTRB),
    .m_AXI_WLAST                    (m_AXI_WLAST),
    .m_AXI_WVALID                   (m_AXI_WVALID),
    .m_AXI_WREADY                   (m_AXI_WREADY),

    .m_AXI_BID                      (m_AXI_BID),
    .m_AXI_BRESP                    (m_AXI_BRESP),
    .m_AXI_BVALID                   (m_AXI_BVALID),
    .m_AXI_BREADY                   (m_AXI_BREADY),

    // ---- 读 ----
    .rd_cmd_valid                   (rd_cmd_valid),
    .rd_cmd_ready                   (rd_cmd_ready),
    .rd_cmd_addr                    (rd_cmd_addr),
    .rd_cmd_len                     (rd_cmd_len),
    .rd_cmd_done                    (rd_cmd_done),
    .rd_cmd_err                     (),                     // 恒 0，不接

    .rd_data                        (rd_data),
    .rd_valid                       (rd_valid),
    .rd_last                        (),                     // 单笔的最后一拍用不上：笔长由我们自己算
    .rd_ready                       (rd_ready),

    .m_AXI_ARID                     (m_AXI_ARID),
    .m_AXI_ARADDR                   (m_AXI_ARADDR),
    .m_AXI_ARLEN                    (m_AXI_ARLEN),
    .m_AXI_ARSIZE                   (m_AXI_ARSIZE),
    .m_AXI_ARBURST                  (m_AXI_ARBURST),
    .m_AXI_ARLOCK                   (m_AXI_ARLOCK),
    .m_AXI_ARCACHE                  (m_AXI_ARCACHE),
    .m_AXI_ARPROT                   (m_AXI_ARPROT),
    .m_AXI_ARQOS                    (m_AXI_ARQOS),
    .m_AXI_ARREGION                 (m_AXI_ARREGION),
    .m_AXI_ARVALID                  (m_AXI_ARVALID),
    .m_AXI_ARREADY                  (m_AXI_ARREADY),

    .m_AXI_RID                      (m_AXI_RID),
    .m_AXI_RDATA                    (m_AXI_RDATA),
    .m_AXI_RRESP                    (m_AXI_RRESP),
    .m_AXI_RLAST                    (m_AXI_RLAST),
    .m_AXI_RVALID                   (m_AXI_RVALID),
    .m_AXI_RREADY                   (m_AXI_RREADY)
);

//------------------------------------------------------------------------------
// ⑤ Handler 集中层（GPU2D_APBHandler）：显示侧 + 加速器侧的 Handler 都在这里
//      · 显示侧：静态寄存器值 -> enable / mode / {段0,段1} 地址；
//                帧边界消费 DONE -> 翻索引 -> 回 done_clr 清位；
//                帧率统计 -> 回 fps_*_val。   （原来是 LineBuffer/bufferAPBHandler）
//      · 加速器侧：事件 -> 计数 -> 观测值。   （原来就在这个文件里）
//      ★ 它是"寄存器 <-> 功能"之间唯一的一层；只翻译、只计数，不碰数据通路
//      ★★ 两路复位【故意分开】：
//           rst_n      = rst_acc_n  → 观测计数
//           rst_n_disp = rst_hdmi_n → 显示侧翻页/帧率(要等显示通路活着才动)
//         ★ 显示侧的 lineBuffer_mode 输出【只给显示通道】。
//           加速器的行模式【绝不能】挂 rst_hdmi_n: 那上面还串着 pll_HDMI_locked,
//           一旦被摁住, 加速器就按错误几何算地址 -> 命令卡死 -> CI 是阻塞握手的
//           -> CPU 停住 -> 调试器再也停不住 CPU(vexriscv 断言崩溃) -> 整块板子没法调。
//           所以给加速器的那一份由下面 lineBuffer_mode_r 单独锁存(复位 rst_acc_n)。
//------------------------------------------------------------------------------
GPU2D_APBHandler #(
    .ADDR_WIDTH                         (AXI_ADDR_WIDTH),
    .FPS_WINDOW                         (60)
) u_GPU2D_APBHandler (
    .clk                                (clk_acc),
    .rst_n                              (rst_acc_n),
    .rst_n_disp                         (rst_hdmi_n),

    // 来自 APB 寄存器格子(APB0_gpu2d 的显示子块, 走本模块端口)
    .enable                             (enable),
    .split_pending                      (split_pending),
    .framea_base_addr                   (framea_base_addr),
    .frameb_base_addr                   (frameb_base_addr),
    .frame_full_base_addr               (frame_full_base_addr),
    .done_gpu2d                         (done_gpu2d),
    .done_cpu                           (done_cpu),

    // 回 APB 寄存器格子
    .done_clr                           (done_clr),
    .fps_acc_val                        (fps_acc_val),
    .fps_cpu_val                        (fps_cpu_val),

    // 显示事件 / 给显示通道
    .lineBuffer_frame_start             (lineBuffer_frame_start),
    .lineBuffer_enable                  (lineBuffer_enable),
    .lineBuffer_mode                    (lineBuffer_mode),
    .lineBuffer_addr                    (lineBuffer_addr),

    // 加速器侧：事件 / 状态
    .acc_busy                           (acc_busy),
    .cmd_done                           (cmd_done),
    .frame_done                         (frame_done),
    .wr_beat_hs                         (wr_beat_hs),
    .rd_beat_hs                         (rd_beat_hs),

    // 回 APB 寄存器格子(槽 2 观测, 走本模块端口)
    .busy_val                           (busy_val),
    .cmd_done_val                       (cmd_done_val),
    .frame_done_val                     (frame_done_val),
    .wr_beats_val                       (wr_beats_val),
    .rd_beats_val                       (rd_beats_val),
    .busy_cyc_val                       (busy_cyc_val)
);

//------------------------------------------------------------------------------
// ⑥ 显示通道：行预取 + 跨域双 bank + 拆包 + HDMI 时序/TMDS + AXI 取数
//      ★ 从第 1 步起它例化在本模块里（原来在 Accelerator_Top 顶层）：
//        显示和加速器要"同一拍锁存同一个模式值"，搬进来之后这条约束就是一根内部线。
//      ★ 寄存器侧走 APB0 槽 2 的显示子块(0x2040)：格子(APB0_gpu2d)也在本模块里(见 ⑦)，
//        寄存器值 -> 有效信号的翻译就是上面 ⑤ Handler 干的活
//      ★ 两级时钟：clk_acc(100MHz, AXI/取数) / clk_HDMI_x1(148.5MHz, 像素)
//------------------------------------------------------------------------------
lineBuffer #(
    .AXI_ADDR_WIDTH                     (AXI_ADDR_WIDTH),
    .AXI_DATA_WIDTH                     (AXI_DATA_WIDTH),
    .AXI_STRB_WIDTH                     (AXI_STRB_WIDTH),
    .AXI_ID_WIDTH                       (AXI_ID_WIDTH  ),
    .AXI_LEN_WIDTH                      (AXI_LEN_WIDTH ),
    .AXI_ID                             (8'h00)
) u_lineBuffer (
    .clk                                (clk_acc),
    .clk_HDMI_x1                        (clk_HDMI_x1),
    .rst_n_wr                           (rst_hdmi_n),

    // 来自 ⑤ Handler：只有"已经翻译好的"3 根有效信号
    .lineBuffer_enable                  (lineBuffer_enable),
    .lineBuffer_mode                    (lineBuffer_mode),
    .lineBuffer_addr                    (lineBuffer_addr),

    // 回给 ⑤ Handler：帧边界(翻页 / 帧率统计的时机)
    .lineBuffer_frame_start             (lineBuffer_frame_start),

    // HDMI 输出(硬核 LVDS TX)
    .tmds_blue_o                        (tmds_blue_o),
    .tmds_green_o                       (tmds_green_o),
    .tmds_red_o                         (tmds_red_o),
    .tmds_clk_o                         (tmds_clk_o),
    .tmds_data0_TX_OE                   (tmds_data0_TX_OE),
    .tmds_data1_TX_OE                   (tmds_data1_TX_OE),
    .tmds_data2_TX_OE                   (tmds_data2_TX_OE),
    .tmds_clk_TX_OE                     (tmds_clk_TX_OE),
    .tmds_data0_TX_RST                  (tmds_data0_TX_RST),
    .tmds_data1_TX_RST                  (tmds_data1_TX_RST),
    .tmds_data2_TX_RST                  (tmds_data2_TX_RST),
    .tmds_clk_TX_RST                    (tmds_clk_TX_RST),

    // AXI 主口 → Axi_Mux S1
    .m_AXI_AWID                         (lineBuffer_m_AXI_AWID),
    .m_AXI_AWADDR                       (lineBuffer_m_AXI_AWADDR),
    .m_AXI_AWLEN                        (lineBuffer_m_AXI_AWLEN),
    .m_AXI_AWSIZE                       (lineBuffer_m_AXI_AWSIZE),
    .m_AXI_AWBURST                      (lineBuffer_m_AXI_AWBURST),
    .m_AXI_AWLOCK                       (lineBuffer_m_AXI_AWLOCK),
    .m_AXI_AWCACHE                      (),
    .m_AXI_AWPROT                       (),
    .m_AXI_AWQOS                        (),
    .m_AXI_AWREGION                     (),
    .m_AXI_AWVALID                      (lineBuffer_m_AXI_AWVALID),
    .m_AXI_AWREADY                      (lineBuffer_m_AXI_AWREADY),

    .m_AXI_WDATA                        (lineBuffer_m_AXI_WDATA),
    .m_AXI_WSTRB                        (lineBuffer_m_AXI_WSTRB),
    .m_AXI_WLAST                        (lineBuffer_m_AXI_WLAST),
    .m_AXI_WVALID                       (lineBuffer_m_AXI_WVALID),
    .m_AXI_WREADY                       (lineBuffer_m_AXI_WREADY),

    .m_AXI_BID                          (lineBuffer_m_AXI_BID),
    .m_AXI_BRESP                        (lineBuffer_m_AXI_BRESP),
    .m_AXI_BVALID                       (lineBuffer_m_AXI_BVALID),
    .m_AXI_BREADY                       (lineBuffer_m_AXI_BREADY),

    .m_AXI_ARID                         (lineBuffer_m_AXI_ARID),
    .m_AXI_ARADDR                       (lineBuffer_m_AXI_ARADDR),
    .m_AXI_ARLEN                        (lineBuffer_m_AXI_ARLEN),
    .m_AXI_ARSIZE                       (lineBuffer_m_AXI_ARSIZE),
    .m_AXI_ARBURST                      (lineBuffer_m_AXI_ARBURST),
    .m_AXI_ARLOCK                       (lineBuffer_m_AXI_ARLOCK),
    .m_AXI_ARCACHE                      (),
    .m_AXI_ARPROT                       (),
    .m_AXI_ARQOS                        (),
    .m_AXI_ARREGION                     (),
    .m_AXI_ARVALID                      (lineBuffer_m_AXI_ARVALID),
    .m_AXI_ARREADY                      (lineBuffer_m_AXI_ARREADY),

    .m_AXI_RID                          (lineBuffer_m_AXI_RID),
    .m_AXI_RDATA                        (lineBuffer_m_AXI_RDATA),
    .m_AXI_RRESP                        (lineBuffer_m_AXI_RRESP),
    .m_AXI_RLAST                        (lineBuffer_m_AXI_RLAST),
    .m_AXI_RVALID                       (lineBuffer_m_AXI_RVALID),
    .m_AXI_RREADY                       (lineBuffer_m_AXI_RREADY)
);

endmodule
