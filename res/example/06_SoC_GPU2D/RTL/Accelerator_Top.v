`timescale 1ps/1ps
`include "ddr3_controller/ddr3_parameter.vh"

// 不要动 define
// 不要动 define
// 不要动 define
`define Efinity_Debug
// `define SOFT_JTAG

module Accelerator_Top # (
    parameter                       RANK_RATIO          = 1,                 // # of unique CS outputs per rank
    parameter                       CK_RATIO            = `CK_RATIO, 
    parameter                       ASYN_AXI_CLK        = `ASYN_AXI_CLK, 
    parameter                       RANKS               = `RANKS,
    parameter                       CK_WIDTH            = `CK_WIDTH,         // # of CK/CK# outputs to memory   
    parameter                       CKE_WIDTH           = `CKE_WIDTH,        // # of cke outputs
    parameter                       CS_WIDTH            = `CS_WIDTH,         // # of unique CS outputs
    parameter                       BANK_WIDTH          = `BANK_WIDTH,       // # of bank bits
    parameter                       ROW_WIDTH           = `ROW_WIDTH,        // DRAM address bus width
    parameter                       COL_WIDTH           = `COL_WIDTH,        // column address width
    parameter                       DM_WIDTH            = `DM_WIDTH,         // # of DM (data mask)
    parameter                       DQS_WIDTH           = `DQS_WIDTH,        // # of DQS (strobe)
    parameter                       DQ_WIDTH            = `DQ_WIDTH,         // # of DQ (data)
    parameter                       ODT_WIDTH           = `ODT_WIDTH,
    parameter                       DQ_CNT_WIDTH        = `DQ_CNT_WIDTH,     // = ceil(log2(DQ_WIDTH))
    parameter                       DQS_CNT_WIDTH       = `DQS_CNT_WIDTH,    // = ceil(log2(DQS_WIDTH))  
    parameter                       DRAM_WIDTH          = `DRAM_WIDTH,       // # of DQ per DQS   
    parameter                       DATA_WIDTH          = `DATA_WIDTH,
    parameter                       ADDR_WIDTH          = `ADDR_WIDTH,    
    parameter                       AXI_ID_WIDTH        = `AXI_ID_WIDTH,
    parameter                       AXI_ADDR_WIDTH      = `AXI_ADDR_WIDTH,
    parameter                       AXI_DATA_WIDTH      = `AXI_DATA_WIDTH
)(
//------------------------------------------------------------------------------
// Clock and reset ports
//------------------------------------------------------------------------------
    input   wire                    axi_clk,      
    input   wire                    core_clk,           // CORE CLK @ 100MHz
    input   wire                    sdram_clk,          // SDRAM CK @ 400MHz
    input   wire                    rx_cal_clk,         // SDRAM CK @ 400MHz
    input   wire                    tx_cal_clk,         // SDRAM CK @ 400MHz
    input   wire                    tx_cal_clk_90edge,  // SDRAM CK @ 400MHz

    input   wire                    pll_locked,
    input   wire                    user_pll_locked,

    input   wire                    clk_HDMI_x1,        // HDMI 像素时钟 @ 148.5MHz(peri PLL slow)
    input   wire                    pll_HDMI_locked,    // HDMI peri PLL 锁定指示(低 = 未锁 → 显示侧复位)
    //  input                              peripheralClk,

//------------------------------------------------------------------------------
// debug core ports
//------------------------------------------------------------------------------
`ifdef  Efinity_Debug  //&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&& 
    `ifdef SOFT_JTAG
    input   wire                    io_jtag_tms,
    input   wire                    io_jtag_tdi,
    output  wire                    io_jtag_tdo,
    input   wire                    io_jtag_tck,
    `else
    input   wire                    jtag_inst1_CAPTURE,
    input   wire                    jtag_inst1_DRCK,
    input   wire                    jtag_inst1_RESET,
    input   wire                    jtag_inst1_RUNTEST,
    input   wire                    jtag_inst1_SEL,
    input   wire                    jtag_inst1_SHIFT,
    input   wire                    jtag_inst1_TCK,
    input   wire                    jtag_inst1_TDI,
    input   wire                    jtag_inst1_TMS,
    input   wire                    jtag_inst1_UPDATE,
    output  wire                    jtag_inst1_TDO,
    `endif
`endif  //&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&

//------------------------------------------------------------------------------
// PLL status flags 
//------------------------------------------------------------------------------
    output  wire    [2:0]           pll_shift,
    output  wire    [4:0]           pll_shift_sel,
    output  wire                    pll_shift_ena,

//------------------------------------------------------------------------------
// memory interface ports
//------------------------------------------------------------------------------
    output  wire                                    ddr_ck_hi,
    output  wire                                    ddr_ck_lo,
    output  wire                                    ddr_reset_n,
    output  wire    [CKE_WIDTH-1:0]                 ddr_cke,     
    output  wire    [ROW_WIDTH-1:0]                 ddr_addr,
    output  wire    [BANK_WIDTH-1:0]                ddr_ba,
    output  wire                                    ddr_cas_n,

    output  wire    [CS_WIDTH*RANK_RATIO-1:0]       ddr_cs_n,
    output  wire                                    ddr_ras_n,
    output  wire                                    ddr_we_n,

    input   wire    [DQS_WIDTH-1:0]                 ddr_dqs_in_hi,
    input   wire    [DQS_WIDTH-1:0]                 ddr_dqs_in_lo,
    input   wire    [DQ_WIDTH-1:0]                  ddr_dq_in_hi,
    input   wire    [DQ_WIDTH-1:0]                  ddr_dq_in_lo,

    output  wire    [DQS_WIDTH-1:0]                 ddr_dqs_oe,
    output  wire    [DQS_WIDTH-1:0]                 ddr_dqs_oe_n,
    output  wire    [DQ_WIDTH-1:0]                  ddr_dq_oe,  
    output  wire    [DQS_WIDTH-1:0]                 ddr_dqs_out_hi,
    output  wire    [DQS_WIDTH-1:0]                 ddr_dqs_out_lo,
    output  wire    [DQ_WIDTH-1:0]                  ddr_dq_out_hi,
    output  wire    [DQ_WIDTH-1:0]                  ddr_dq_out_lo,
    output  wire    [DM_WIDTH-1:0]                  ddr_dm_hi,
    output  wire    [DM_WIDTH-1:0]                  ddr_dm_lo,
    output  wire    [ODT_WIDTH-1:0]                 ddr_odt,


//------------------------------------------------------------------------------
// SoC IP Setup
//------------------------------------------------------------------------------
    // SoC GPIO0
    input   wire    [3:0]           soc_gpio_IN,
    output  wire    [3:0]           soc_gpio_OUT,
    output  wire    [3:0]           soc_gpio_OE,

    // SoC UART0
    output  wire                    system_uart_0_io_txd,
    input   wire                    system_uart_0_io_rxd,
    
    // SoC SPI0
    input   wire                    system_spi_0_io_data_0_IN,
    input   wire                    system_spi_0_io_data_1_IN,
    output  wire                    system_spi_0_io_ss,
    output  wire                    system_spi_0_io_sclk_write,
    output  wire                    system_spi_0_io_data_0_OUT,
    output  wire                    system_spi_0_io_data_0_OE,
    output  wire                    system_spi_0_io_data_1_OUT,
    output  wire                    system_spi_0_io_data_1_OE,

//------------------------------------------------------------------------------
// APB3 Peripherial ports
//------------------------------------------------------------------------------
// port 0
    input   wire   [3:0]            key_pin,

//------------------------------------------------------------------------------
// HDMI 显示输出(接硬核 LVDS TX)
//   OE/RST 由 HDMI_Top 按复位状态驱动, 顶层直通;引脚在 Interface Designer 里分配
//------------------------------------------------------------------------------
    output  wire    [9:0]           tmds_blue_o,        // 通道0 = 蓝(带 hs/vs 控制码)
    output  wire    [9:0]           tmds_green_o,       // 通道1 = 绿
    output  wire    [9:0]           tmds_red_o,         // 通道2 = 红
    output  wire    [9:0]           tmds_clk_o,         // 时钟通道
    output  wire                    tmds_data0_TX_OE,
    output  wire                    tmds_data1_TX_OE,
    output  wire                    tmds_data2_TX_OE,
    output  wire                    tmds_clk_TX_OE,
    output  wire                    tmds_data0_TX_RST,
    output  wire                    tmds_data1_TX_RST,
    output  wire                    tmds_data2_TX_RST,
    output  wire                    tmds_clk_TX_RST
);

//------------------------------------------------------------------------------
// Parameter Define
//------------------------------------------------------------------------------
parameter   FREQ            = 200;			// default is 100 MHz.  Redefine as needed.

`ifndef SIM
    localparam CNT_INIT = 1.5*FREQ*1000;
`else
    localparam CNT_INIT = 10;
`endif

//------------------------------------------------------------------------------
// Wire declarations
//------------------------------------------------------------------------------

// 全局时钟复位
wire                                user_clk;
wire                                ddr_rstn;
reg  [19:0]                         cnt;
wire                                sys_rst;

// DDR3 inst
// Application interface ports
wire                                app_sr_active;
wire                                app_ref_ack;
wire                                app_zq_ack;
// Slave Interface Write Address Ports
wire [AXI_ID_WIDTH-1:0]             ddr_s_AXI_AWID;
wire [AXI_ADDR_WIDTH-1:0]           ddr_s_AXI_AWADDR;
wire [7:0]                          ddr_s_AXI_AWLEN;
wire [2:0]                          ddr_s_AXI_AWSIZE;
wire [1:0]                          ddr_s_AXI_AWBURST;
wire [0:0]                          ddr_s_AXI_AWLOCK;
wire [3:0]                          ddr_s_AXI_AWCACHE; 
wire [2:0]                          ddr_s_AXI_AWPROT; 
wire                                ddr_s_AXI_AWVALID;
wire                                ddr_s_AXI_AWREADY;
// Slave Interface Write Data Ports
wire [AXI_DATA_WIDTH-1:0]           ddr_s_AXI_WDATA;
wire [(AXI_DATA_WIDTH/8)-1:0]       ddr_s_AXI_WSTRB;
wire                                ddr_s_AXI_WLAST;
wire                                ddr_s_AXI_WVALID;
wire                                ddr_s_AXI_WREADY;
// Slave Interface Write Response Ports
wire [AXI_ID_WIDTH-1:0]             ddr_s_AXI_BID;
wire [1:0]                          ddr_s_AXI_BRESP;
wire                                ddr_s_AXI_BVALID;
wire                                ddr_s_AXI_BREADY;
// Slave Interface Read Address Ports
wire [AXI_ID_WIDTH-1:0]             ddr_s_AXI_ARID;
wire [AXI_ADDR_WIDTH-1:0]           ddr_s_AXI_ARADDR;
wire [7:0]                          ddr_s_AXI_ARLEN;
wire [2:0]                          ddr_s_AXI_ARSIZE;
wire [1:0]                          ddr_s_AXI_ARBURST;
wire [0:0]                          ddr_s_AXI_ARLOCK;
wire [3:0]                          ddr_s_AXI_ARCACHE; 
wire [2:0]                          ddr_s_AXI_ARPROT; 
wire                                ddr_s_AXI_ARVALID;
wire                                ddr_s_AXI_ARREADY;
// Slave Interface Read Data Ports
wire [AXI_ID_WIDTH-1:0]             ddr_s_AXI_RID;
wire [AXI_DATA_WIDTH-1:0]           ddr_s_AXI_RDATA;
wire [1:0]                          ddr_s_AXI_RRESP;
wire                                ddr_s_AXI_RLAST;
wire                                ddr_s_AXI_RVALID;
wire                                ddr_s_AXI_RREADY;

assign  ddr_s_AXI_AWCACHE       =   4'd0;       // Axi_Mux 无 CACHE 输出 → 接常量，防悬空灌入 DDR
assign  ddr_s_AXI_AWPROT        =   3'd0;       // Axi_Mux 无 PROT  输出 → 接常量

assign  ddr_s_AXI_ARCACHE       =   4'd0;       // Axi_Mux 无 CACHE 输出 → 接常量，防悬空灌入 DDR
assign  ddr_s_AXI_ARPROT        =   3'd0;       // Axi_Mux 无 PROT  输出 → 接常量
//------------------------------------------------------------------------------
// AXI 网名规范：<模块>_<m|s>_AXI_<通道><字段>（通道/字段大写）
//     m = 该接口扮演主（发起请求）    s = 该接口扮演从（响应请求）
//     例：soc_m_AXI_AWADDR（SoC 的 AXI 主口） ddr_s_AXI_RDATA（DDR 的 AXI 从口）
//   注：IP 自身的端口名（io_ddrA_* / s_axi_*）保持原样，只规范我们自己的网名
//------------------------------------------------------------------------------
// SoC io_ddrA(AXI Master) → Axi_Mux S0 的中间链路（宽度与 SoC 口一致：数据128 / ID8 / 地址32）
wire [7:0]                          soc_m_AXI_AWID;
wire [31:0]                         soc_m_AXI_AWADDR;
wire [7:0]                          soc_m_AXI_AWLEN;
wire [2:0]                          soc_m_AXI_AWSIZE;
wire [1:0]                          soc_m_AXI_AWBURST;
wire [0:0]                          soc_m_AXI_AWLOCK;
wire                                soc_m_AXI_AWVALID;
wire                                soc_m_AXI_AWREADY;

wire [127:0]                        soc_m_AXI_WDATA;
wire [15:0]                         soc_m_AXI_WSTRB;
wire                                soc_m_AXI_WLAST;
wire                                soc_m_AXI_WVALID;
wire                                soc_m_AXI_WREADY;

wire [7:0]                          soc_m_AXI_BID;
wire                                soc_m_AXI_BVALID;
wire                                soc_m_AXI_BREADY;

wire [7:0]                          soc_m_AXI_ARID;
wire [31:0]                         soc_m_AXI_ARADDR;
wire [7:0]                          soc_m_AXI_ARLEN;
wire [2:0]                          soc_m_AXI_ARSIZE;
wire [1:0]                          soc_m_AXI_ARBURST;
wire [0:0]                          soc_m_AXI_ARLOCK;
wire                                soc_m_AXI_ARVALID;
wire                                soc_m_AXI_ARREADY;

wire [7:0]                          soc_m_AXI_RID;
wire [127:0]                        soc_m_AXI_RDATA;
wire [1:0]                          soc_m_AXI_RRESP;
wire                                soc_m_AXI_RLAST;
wire                                soc_m_AXI_RVALID;
wire                                soc_m_AXI_RREADY;

// Axi_Mux 调试观测：各通道当前激活的通道号（位宽 = clog2(通道数) = 2 bit）
wire [1:0]                          dbg_aw_cnl_num;
wire [1:0]                          dbg_w_cnl_num;
wire [1:0]                          dbg_b_cnl_num;
wire [1:0]                          dbg_ar_cnl_num;
wire [1:0]                          dbg_r_cnl_num;

// DEBUG
wire  [7:0]                         wrlvl_dq_check;
wire  [7:0]                         rd_level_dqs_check;
wire  [2:0]                         rdlvl_shift;
wire  [2:0]                         wrlvl_shift;
wire  [6:0]                         init_cur_state;
wire                                idelay_ld;
wire                                mpr_rdlvl_dly;
wire  [35:0]                        ddr_debug_port; 
wire                                cal_done;

// SoC inst
// APB 0
wire [15:0]                         APB_0_PADDR;
wire                                APB_0_PSEL;
wire                                APB_0_PENABLE;
wire                                APB_0_PREADY;
wire                                APB_0_PWRITE;
wire [31:0]                         APB_0_PWDATA;
wire [31:0]                         APB_0_PRDATA;
wire                                APB_0_PSLVERROR;

// 外设 0:按键 key
//   KEY_NUM      按键个数(APB0_Interconnect 和 key 都用)
//   DEBOUNCE_CNT 消抖阈值,只在下面例化的 key 模块里用
parameter   KEY_NUM                 =   4;
parameter   DEBOUNCE_CNT            =   1_000_000;

// key 的功能层(key)与总线层(APB0_key)之间的连线
wire [3:0]                          key_state;
wire [3:0]                          key_edge;

// cal_done 采样两拍后并进复位 —— 系统级门控在顶层做,功能模块只认 rst_n
reg                                 cal_done_d1;
reg                                 cal_done_d2;
wire                                axi_rst_n;

//------------------------------------------------------------------------------
// 外设 1: GPU_2D(APB0 槽 2 一整块 + 功能层)
//   ★ APB0 总线层(APB0_gpu2d)在 APB0_Interconnect 里例化 ⇒ APB 口不经过顶层。
//     顶层只剩"寄存器 <-> 功能层"这组功能侧线，把互连和 GPU_2D 连起来。
//   ★ 槽 2 一整块：0x2000-0x201F 观测 / 0x2040-0x206F 显示；
//     槽 1 (0x1000) 已空，访问它会走 APB0_Interconnect 兜底 -> PSLVERROR=1。
//   AXI 侧编号不变：显示取数 = S1，加速器读写 = S2/S3
//------------------------------------------------------------------------------
//   寄存器 -> 功能层（显示子块的静态配置）
wire                                GPU2D_enable;               //(O) STATUS.ENABLE
wire                                GPU2D_split_pending;        //(O) STATUS.SPLIT(待生效值)
wire [63:0]                         GPU2D_framea_base_addr;     //(O) {A左半, A右半}
wire [63:0]                         GPU2D_frameb_base_addr;     //(O) {B左半, B右半}
wire [63:0]                         GPU2D_frame_full_base_addr; //(O) {全屏 C, 全屏 D}
wire                                GPU2D_done_gpu2d;           //(O) 加速器那半有新帧
wire                                GPU2D_done_cpu;             //(O) CPU 那半有新帧
//   显示子块 0x2078: 颜色键控(ck)的键色 —— 寄存器 -> 功能层(源侧比像素用)
wire [23:0]                         GPU2D_ckey_color;           //(O) 键色, 复位默认白
//   功能层 -> 寄存器（交帧 / 帧率 / 观测）
wire                                GPU2D_acc_frame_done;       //(I) FENCE 完成【单拍】
wire [1:0]                          GPU2D_done_clr;             //(I) {cpu,acc} 翻页后清位
wire [31:0]                         GPU2D_fps_acc_val;          //(I) 帧率
wire [31:0]                         GPU2D_fps_cpu_val;
wire [31:0]                         GPU2D_busy_val;             //(I) 观测(BUSY)
wire [31:0]                         GPU2D_cmd_done_val;
wire [31:0]                         GPU2D_frame_done_val;
wire [31:0]                         GPU2D_wr_beats_val;
wire [31:0]                         GPU2D_rd_beats_val;
wire [31:0]                         GPU2D_busy_cyc_val;

wire                                rst_hdmi_n;           // 显示通道复位(见下面的门控)

// lineBuffer 的 AXI 主口 → Axi_Mux S1(显示取数)
wire [7:0]                          lineBuffer_m_AXI_AWID;
wire [31:0]                         lineBuffer_m_AXI_AWADDR;
wire [7:0]                          lineBuffer_m_AXI_AWLEN;
wire [2:0]                          lineBuffer_m_AXI_AWSIZE;
wire [1:0]                          lineBuffer_m_AXI_AWBURST;
wire [0:0]                          lineBuffer_m_AXI_AWLOCK;
wire                                lineBuffer_m_AXI_AWVALID;
wire                                lineBuffer_m_AXI_AWREADY;

wire [127:0]                        lineBuffer_m_AXI_WDATA;
wire [15:0]                         lineBuffer_m_AXI_WSTRB;
wire                                lineBuffer_m_AXI_WLAST;
wire                                lineBuffer_m_AXI_WVALID;
wire                                lineBuffer_m_AXI_WREADY;

wire [7:0]                          lineBuffer_m_AXI_BID;
wire [1:0]                          lineBuffer_m_AXI_BRESP;
wire                                lineBuffer_m_AXI_BVALID;
wire                                lineBuffer_m_AXI_BREADY;

wire [7:0]                          lineBuffer_m_AXI_ARID;
wire [31:0]                         lineBuffer_m_AXI_ARADDR;
wire [7:0]                          lineBuffer_m_AXI_ARLEN;
wire [2:0]                          lineBuffer_m_AXI_ARSIZE;
wire [1:0]                          lineBuffer_m_AXI_ARBURST;
wire [0:0]                          lineBuffer_m_AXI_ARLOCK;
wire                                lineBuffer_m_AXI_ARVALID;
wire                                lineBuffer_m_AXI_ARREADY;

wire [7:0]                          lineBuffer_m_AXI_RID;
wire [127:0]                        lineBuffer_m_AXI_RDATA;
wire [1:0]                          lineBuffer_m_AXI_RRESP;
wire                                lineBuffer_m_AXI_RLAST;
wire                                lineBuffer_m_AXI_RVALID;
wire                                lineBuffer_m_AXI_RREADY;

//------------------------------------------------------------------------------
// 外设 3:2D 加速器(GPU_2D)
//   左: CI 8 根线(自定义指令)，接 SoC 的 cpu0_customInstruction_*
//   右: AXI 主口，只接【写】通道 → Axi_Mux S3 (读通道 M2 做 Block Copy 才用)
//   配置: 行模式(全屏/分屏)在 GPU_2D 内部于帧边界锁存，顶层不再有这根线
//   复位: 与命令侧同域，用 axi_rst_n(DDR 校准完成前不许发 AXI)
//------------------------------------------------------------------------------
// CI 侧(CPU0 的自定义指令口)
wire                                cpu0_ci_cmd_valid;
wire                                cpu0_ci_cmd_ready;
wire [9:0]                          cpu0_ci_function_id;
wire [31:0]                         cpu0_ci_inputs_0;
wire [31:0]                         cpu0_ci_inputs_1;
wire                                cpu0_ci_rsp_valid;
wire                                cpu0_ci_rsp_ready;
wire [31:0]                         cpu0_ci_outputs_0;

//   ★ 加速器那 6 个只读观测(gpu2d_*_val) 和 acc_frame_done 不再是顶层线：
//     它们和寄存器格子一起进了 GPU_2D（APB0_gpu2d 一个模块管槽 2 的两个子块）。

// 加速器 AXI 主口：写 → Axi_Mux S3，读 → Axi_Mux S2（块拷贝/BLIT 要读源）
wire [7:0]                          GPU2D_m_AXI_AWID;
wire [31:0]                         GPU2D_m_AXI_AWADDR;
wire [7:0]                          GPU2D_m_AXI_AWLEN;
wire [2:0]                          GPU2D_m_AXI_AWSIZE;
wire [1:0]                          GPU2D_m_AXI_AWBURST;
wire [0:0]                          GPU2D_m_AXI_AWLOCK;
wire                                GPU2D_m_AXI_AWVALID;
wire                                GPU2D_m_AXI_AWREADY;

wire [127:0]                        GPU2D_m_AXI_WDATA;
wire [15:0]                         GPU2D_m_AXI_WSTRB;
wire                                GPU2D_m_AXI_WLAST;
wire                                GPU2D_m_AXI_WVALID;
wire                                GPU2D_m_AXI_WREADY;

wire [7:0]                          GPU2D_m_AXI_BID;
wire [1:0]                          GPU2D_m_AXI_BRESP;
wire                                GPU2D_m_AXI_BVALID;
wire                                GPU2D_m_AXI_BREADY;

// 加速器的【读】通道 → Axi_Mux S2（BLIT/块拷贝读源；FILL 用不到但口要接上）
wire [7:0]                          GPU2D_m_AXI_ARID;
wire [31:0]                         GPU2D_m_AXI_ARADDR;
wire [7:0]                          GPU2D_m_AXI_ARLEN;
wire [2:0]                          GPU2D_m_AXI_ARSIZE;
wire [1:0]                          GPU2D_m_AXI_ARBURST;
wire [0:0]                          GPU2D_m_AXI_ARLOCK;
wire                                GPU2D_m_AXI_ARVALID;
wire                                GPU2D_m_AXI_ARREADY;

wire [7:0]                          GPU2D_m_AXI_RID;
wire [127:0]                        GPU2D_m_AXI_RDATA;
wire [1:0]                          GPU2D_m_AXI_RRESP;
wire                                GPU2D_m_AXI_RLAST;
wire                                GPU2D_m_AXI_RVALID;
wire                                GPU2D_m_AXI_RREADY;

//------------------------------------------------------------------------------
// 上电复位
//------------------------------------------------------------------------------

// 两个 PLL 都锁了才为高
assign sys_rst       = pll_locked & user_pll_locked;

always @(posedge user_clk or negedge sys_rst) begin
	if (!sys_rst)
		cnt <= 0;
    else if (cnt != CNT_INIT) 
        cnt <= cnt + 20'd1;
	else 
        cnt <= cnt;
end

assign ddr_rstn = (cnt == CNT_INIT);

generate
    if (ASYN_AXI_CLK) begin
        assign user_clk = axi_clk;
    end 
    else begin
        assign user_clk = core_clk;
    end
endgenerate

//------------------------------------------------------------------------------
// 系统门控:把 cal_done 并进复位(顶层统一做,功能模块只认 rst_n)
//   - sys_rst     : 异步断言
//   - cal_done_d2 : 时钟域内的寄存器输出 -> 同步撤销
//   合成后的 axi_rst_n 就是"异步断言 / 同步撤销"的标准复位
//------------------------------------------------------------------------------
always @(posedge user_clk or negedge sys_rst) begin
    if (!sys_rst) begin
        cal_done_d1 <= 1'b0;
        cal_done_d2 <= 1'b0;
    end
    else begin
        cal_done_d1 <= cal_done;
        cal_done_d2 <= cal_done_d1;
    end
end

assign axi_rst_n = sys_rst & cal_done_d2;

// 显示通道复位 = 校准完成 + HDMI PLL 锁定
//   DDR 没校准完 → Axi_Mux/DDR 还不能收命令, 不许它发 AXI 读
//   HDMI PLL 没锁   → 像素域时钟不可用, 显示侧也不该放出来
//   像素域复位由 lineBuffer 内部从 rst_n_wr 同步释放(单源复位, 不在顶层再分一个)
//   ★ 这一路引给 GPU_2D 的 rst_hdmi_n；加速器/命令侧仍用 axi_rst_n ——
//     DDR 校准完成前一直处于复位，所以上电后要等 cal_done 再写 CTRL.start
assign rst_hdmi_n = axi_rst_n & pll_HDMI_locked;

// SoC IRQ
wire                                IRQ_frame;                 // 显示帧起始中断(电平) -> SoC userInterruptA

//------------------------------------------------------------------------------
// SoC RISCV
//------------------------------------------------------------------------------

SoC_RISCV32 u_sapphire_soc(
    .io_systemClk                       (user_clk),
    .io_asyncReset                      (!pll_locked),
    // output类型：SoC 输出的系统域同步复位
    .io_systemReset                     (),

    .io_memoryClk                       (user_clk),
    // output 类型：SoC 输出的存储器域同步复位
    .io_memoryReset                     (                                 ),
    
    // External Memory AXI4 Interface
    // 注1：PROT/QOS/CACHE/REGION 是 SoC 的输出口，Axi_Mux 侧无对应信号，不接（合法）
    // 注2：b_payload_resp 是 SoC 的输入口，Axi_Mux 不回 BRESP，故固定接 OKAY 防悬空
    .io_ddrA_aw_payload_prot            (                                 ),//(                                  ),
    .io_ddrA_aw_payload_qos             (                                 ),//(                                  ),
    .io_ddrA_aw_payload_cache           (                                 ),//(                                  ),
    .io_ddrA_aw_payload_lock            (soc_m_AXI_AWLOCK                 ),//(m1_axi_awlock                     ),
    .io_ddrA_aw_payload_burst           (soc_m_AXI_AWBURST                ),//(m1_axi_awburst                    ),
    .io_ddrA_aw_payload_size            (soc_m_AXI_AWSIZE                 ),//(m1_axi_awsize                     ),
    .io_ddrA_aw_payload_len             (soc_m_AXI_AWLEN                  ),//(m1_axi_awlen                      ),
    .io_ddrA_aw_payload_region          (                                 ),//(                                  ),
    .io_ddrA_aw_payload_id              (soc_m_AXI_AWID                   ),//(m1_axi_awid                       ),
    .io_ddrA_aw_payload_addr            (soc_m_AXI_AWADDR                 ),//(m1_axi_awaddr                     ),
    .io_ddrA_aw_ready                   (soc_m_AXI_AWREADY                ),//(m1_axi_awready                    ),
    .io_ddrA_aw_valid                   (soc_m_AXI_AWVALID                ),//(m1_axi_awvalid                    ),
    .io_ddrA_w_payload_last             (soc_m_AXI_WLAST                  ),//(m1_axi_wlast                      ),
    .io_ddrA_w_ready                    (soc_m_AXI_WREADY                 ),//(m1_axi_wready                     ),
    .io_ddrA_w_valid                    (soc_m_AXI_WVALID                 ),  //(m1_axi_wvalid                     ),    
    .io_ddrA_w_payload_strb             (soc_m_AXI_WSTRB                  ),//(m1_axi_wstrb                      ),
    .io_ddrA_w_payload_data             (soc_m_AXI_WDATA                  ),//(m1_axi_wdata                      ),
    .io_ddrA_b_payload_resp             (2'h0                             ),//(                                  ),
    .io_ddrA_b_payload_id               (soc_m_AXI_BID                    ),//(m1_axi_bid                        ),
    .io_ddrA_b_ready                    (soc_m_AXI_BREADY                 ),//(m1_axi_bready                     ),
    .io_ddrA_b_valid                    (soc_m_AXI_BVALID                 ),//(m1_axi_bvalid                     ),
    .io_ddrA_ar_payload_prot            (                                 ),//(                                  ),
    .io_ddrA_ar_payload_qos             (                                 ),//(                                  ),
    .io_ddrA_ar_payload_cache           (                                 ),//(                                  ),
    .io_ddrA_ar_payload_region          (                                 ),//(                                  ),
    .io_ddrA_ar_payload_lock            (soc_m_AXI_ARLOCK                 ),//(m1_axi_arlock                     ),
    .io_ddrA_ar_payload_burst           (soc_m_AXI_ARBURST                ),//(m1_axi_arburst                    ),
    .io_ddrA_ar_payload_size            (soc_m_AXI_ARSIZE                 ),//(m1_axi_arsize                     ),
    .io_ddrA_ar_payload_len             (soc_m_AXI_ARLEN                  ),//(m1_axi_arlen                      ),
    .io_ddrA_ar_payload_id              (soc_m_AXI_ARID                   ),//(m1_axi_arid                       ),
    .io_ddrA_ar_payload_addr            (soc_m_AXI_ARADDR                 ),//(m1_axi_araddr                     ),
    .io_ddrA_ar_ready                   (soc_m_AXI_ARREADY                ),//(m1_axi_arready                    ),
    .io_ddrA_ar_valid                   (soc_m_AXI_ARVALID                ),//(m1_axi_arvalid                    ),
    .io_ddrA_r_payload_last             (soc_m_AXI_RLAST                  ),//(m1_axi_rlast                      ),
    .io_ddrA_r_payload_resp             (soc_m_AXI_RRESP                  ),//(m1_axi_rresp                      ),
    .io_ddrA_r_payload_id               (soc_m_AXI_RID                    ),//(m1_axi_rid                        ),
    .io_ddrA_r_payload_data             (soc_m_AXI_RDATA                  ),//(m1_axi_rdata                      ),
    .io_ddrA_r_ready                    (soc_m_AXI_RREADY                 ),//(m1_axi_rready                     ),
    .io_ddrA_r_valid                    (soc_m_AXI_RVALID                 ),//(m1_axi_rvalid                    ),

    // userInterruptA
    .userInterruptA                     (IRQ_frame),              // 显示帧起始中断(原来接 key_irq, 没人用)
    
    // SPI 0
    .system_spi_0_io_sclk_write         (system_spi_0_io_sclk_write),
    .system_spi_0_io_data_0_writeEnable (system_spi_0_io_data_0_OE),
    .system_spi_0_io_data_0_read        (system_spi_0_io_data_0_IN),
    .system_spi_0_io_data_0_write       (system_spi_0_io_data_0_OUT),
    .system_spi_0_io_data_1_writeEnable (system_spi_0_io_data_1_OE),
    .system_spi_0_io_data_1_read        (system_spi_0_io_data_1_IN),
    .system_spi_0_io_data_1_write       (system_spi_0_io_data_1_OUT),
    .system_spi_0_io_data_2_writeEnable (),
    .system_spi_0_io_data_2_read        (1'b0),
    .system_spi_0_io_data_2_write       (),
    .system_spi_0_io_data_3_writeEnable (),
    .system_spi_0_io_data_3_read        (1'b0),
    .system_spi_0_io_data_3_write       (),
    .system_spi_0_io_ss                 (system_spi_0_io_ss),

    // UART 0
    .system_uart_0_io_txd               (system_uart_0_io_txd),
    .system_uart_0_io_rxd               (system_uart_0_io_rxd),

    // APB 0
    .io_apbSlave_0_PADDR                (APB_0_PADDR),
    .io_apbSlave_0_PSEL                 (APB_0_PSEL),
    .io_apbSlave_0_PENABLE              (APB_0_PENABLE),
    .io_apbSlave_0_PREADY               (APB_0_PREADY),
    .io_apbSlave_0_PWRITE               (APB_0_PWRITE),
    .io_apbSlave_0_PWDATA               (APB_0_PWDATA),
    .io_apbSlave_0_PRDATA               (APB_0_PRDATA),
    .io_apbSlave_0_PSLVERROR            (APB_0_PSLVERROR),

    // GPIO 0
    .system_gpio_0_io_write             (soc_gpio_OUT),
    .system_gpio_0_io_read              (soc_gpio_IN),
    .system_gpio_0_io_writeEnable       (soc_gpio_OE),

    // Hard Jtag Tap
    .jtagCtrl_tck                       (jtag_inst1_TCK),
    .jtagCtrl_tdi                       (jtag_inst1_TDI),
    .jtagCtrl_tdo                       (jtag_inst1_TDO),
    .jtagCtrl_enable                    (jtag_inst1_SEL),
    .jtagCtrl_capture                   (jtag_inst1_CAPTURE),
    .jtagCtrl_shift                     (jtag_inst1_SHIFT),
    .jtagCtrl_update                    (jtag_inst1_UPDATE),
    .jtagCtrl_reset                     (jtag_inst1_RESET),

    //------------------ 自定义指令(CI) → 2D 加速器 ------------------
    //   ★ 必须接：cmd_ready 是 SoC 的【输入】。悬空会被综合成 0，
    //     CPU 执行 cfu_push 时等不到"收下了"，会永久停在写回级。
    .cpu0_customInstruction_cmd_valid   (cpu0_ci_cmd_valid       ),
    .cpu0_customInstruction_cmd_ready   (cpu0_ci_cmd_ready       ),
    .cpu0_customInstruction_function_id (cpu0_ci_function_id     ),
    .cpu0_customInstruction_inputs_0    (cpu0_ci_inputs_0        ),
    .cpu0_customInstruction_inputs_1    (cpu0_ci_inputs_1        ),
    .cpu0_customInstruction_rsp_valid   (cpu0_ci_rsp_valid       ),
    .cpu0_customInstruction_rsp_ready   (cpu0_ci_rsp_ready       ),
    .cpu0_customInstruction_outputs_0   (cpu0_ci_outputs_0       )
);


//------------------------------------------------------------------------------
// DDR3
//------------------------------------------------------------------------------

ddr3_top u_ddr3_top ( 
    .axi_clk                            (user_clk            ),
    .core_clk                           (core_clk            ),
    .sdram_clk                          (sdram_clk           ),  
    .rx_cal_clk                         (rx_cal_clk          ),
    .tx_cal_clk                         (tx_cal_clk          ),
    .tx_cal_clk_90edge                  (tx_cal_clk_90edge   ),
    .rstn                               (ddr_rstn            ),      
    .pll_shift                          (pll_shift           ),
    .pll_shift_sel                      (pll_shift_sel       ),
    .pll_shift_ena                      (pll_shift_ena       ), 
    //DDR BUS
    .ddr_ck_hi                          (ddr_ck_hi           ),
    .ddr_ck_lo                          (ddr_ck_lo           ),
    .ddr_cke                            (ddr_cke             ),    
    .ddr_reset_n                        (ddr_reset_n         ),
    .ddr_cs_n                           (ddr_cs_n            ),
    .ddr_ras_n                          (ddr_ras_n           ),
    .ddr_cas_n                          (ddr_cas_n           ),
    .ddr_we_n                           (ddr_we_n            ),     
    .ddr_addr                           (ddr_addr            ),
    .ddr_ba                             (ddr_ba              ),

    .ddr_dqs_oe                         (ddr_dqs_oe          ),
    .ddr_dqs_oe_n                       (ddr_dqs_oe_n        ),
    .ddr_dq_oe                          (ddr_dq_oe           ),
    .ddr_dqs_in_hi                      (ddr_dqs_in_hi       ),
    .ddr_dqs_in_lo                      (ddr_dqs_in_lo       ),
    .ddr_dq_in_hi                       (ddr_dq_in_hi        ),
    .ddr_dq_in_lo                       (ddr_dq_in_lo        ),

    .ddr_dqs_out_hi                     (ddr_dqs_out_hi      ),
    .ddr_dqs_out_lo                     (ddr_dqs_out_lo      ),
    .ddr_dq_out_hi                      (ddr_dq_out_hi       ),
    .ddr_dq_out_lo                      (ddr_dq_out_lo       ),
    
    .ddr_dm_hi                          (ddr_dm_hi           ),
    .ddr_dm_lo                          (ddr_dm_lo           ),
    .ddr_odt                            (ddr_odt             ),

    // Application interface ports
    .app_sr_req                         (1'b0),
    .app_ref_req                        (1'b0),
    .app_zq_req                         (1'b0),
    .app_sr_active                      (app_sr_active),
    .app_ref_ack                        (app_ref_ack),
    .app_zq_ack                         (app_zq_ack),

    // Slave Interface Write Address Ports
    .s_axi_awid                         (ddr_s_AXI_AWID         ),
    .s_axi_awaddr                       (ddr_s_AXI_AWADDR       ),
    .s_axi_awlen                        (ddr_s_AXI_AWLEN        ),
    .s_axi_awsize                       (ddr_s_AXI_AWSIZE       ),
    .s_axi_awburst                      (ddr_s_AXI_AWBURST      ),
    .s_axi_awlock                       (ddr_s_AXI_AWLOCK       ),
    .s_axi_awcache                      (ddr_s_AXI_AWCACHE      ),
    .s_axi_awprot                       (ddr_s_AXI_AWPROT       ),
    .s_axi_awqos                        (4'h0                   ),
    .s_axi_awvalid                      (ddr_s_AXI_AWVALID      ),
    .s_axi_awready                      (ddr_s_AXI_AWREADY      ),
    // Slave Interface Write Data Ports
    .s_axi_wdata                        (ddr_s_AXI_WDATA        ),
    .s_axi_wstrb                        (ddr_s_AXI_WSTRB        ),
    .s_axi_wlast                        (ddr_s_AXI_WLAST        ),
    .s_axi_wvalid                       (ddr_s_AXI_WVALID       ),
    .s_axi_wready                       (ddr_s_AXI_WREADY       ),
    // Slave Interface Write Response Ports
    .s_axi_bid                          (ddr_s_AXI_BID          ),
    .s_axi_bresp                        (ddr_s_AXI_BRESP        ),
    .s_axi_bvalid                       (ddr_s_AXI_BVALID       ),
    .s_axi_bready                       (ddr_s_AXI_BREADY       ),
    // Slave Interface Read Address Ports
    .s_axi_arid                         (ddr_s_AXI_ARID         ),
    .s_axi_araddr                       (ddr_s_AXI_ARADDR       ),
    .s_axi_arlen                        (ddr_s_AXI_ARLEN        ),
    .s_axi_arsize                       (ddr_s_AXI_ARSIZE       ),
    .s_axi_arburst                      (ddr_s_AXI_ARBURST      ),
    .s_axi_arlock                       (ddr_s_AXI_ARLOCK       ),
    .s_axi_arcache                      (ddr_s_AXI_ARCACHE      ),
    .s_axi_arprot                       (ddr_s_AXI_ARPROT       ),
    .s_axi_arqos                        (4'h0                   ),
    .s_axi_arvalid                      (ddr_s_AXI_ARVALID      ),
    .s_axi_arready                      (ddr_s_AXI_ARREADY      ),
    // Slave Interface Read Data Ports
    .s_axi_rid                          (ddr_s_AXI_RID          ),
    .s_axi_rdata                        (ddr_s_AXI_RDATA        ),
    .s_axi_rresp                        (ddr_s_AXI_RRESP        ),
    .s_axi_rlast                        (ddr_s_AXI_RLAST        ),
    .s_axi_rvalid                       (ddr_s_AXI_RVALID       ),
    .s_axi_rready                       (ddr_s_AXI_RREADY       ),
    //DEBUG       
    .wrlvl_dq_check                     (wrlvl_dq_check         ) ,
    .rd_level_dqs_check                 (rd_level_dqs_check     ) ,
    .rdlvl_shift                        (rdlvl_shift            ) ,
    .wrlvl_shift                        (wrlvl_shift            ) ,
    .init_cur_state                     (init_cur_state         ) ,
    .idelay_ld                          (idelay_ld              ) ,
    .mpr_rdlvl_dly                      (mpr_rdlvl_dly          ) ,
    .ddr_debug_port                     (ddr_debug_port         ) ,
    .cal_done                           (cal_done               ) 
);
// End of User Design top instance

//------------------------------------------------------------------------------
// APB3 port0 Interface
//   APB0_Interconnect = APB 子系统:地址译码 + 各外设的"总线解析层"
//------------------------------------------------------------------------------
APB0_Interconnect #(
    .KEY_NUM                            (KEY_NUM)
) u_APB0_Interconnect (
    .clk                                (user_clk),
    .rst_n                              (sys_rst),
    .APB_0_PADDR                        (APB_0_PADDR),
    .APB_0_PSEL                         (APB_0_PSEL),
    .APB_0_PENABLE                      (APB_0_PENABLE),
    .APB_0_PWRITE                       (APB_0_PWRITE),
    .APB_0_PWDATA                       (APB_0_PWDATA),
    .APB_0_PRDATA                       (APB_0_PRDATA),
    .APB_0_PREADY                       (APB_0_PREADY),
    .APB_0_PSLVERROR                    (APB_0_PSLVERROR),

    // 外设 0 key 的功能侧(接下面例化的 key 模块)
    .key_state                          (key_state),
    .key_edge                           (key_edge),
    .IRQ_frame                          (IRQ_frame),

    // 外设 1: GPU_2D(槽 2, 一整块)
    //   ★ 总线层(APB0_gpu2d)就在互连里 ⇒ 这里接的是功能侧线，连给下面的 GPU_2D
    .acc_frame_done                     (GPU2D_acc_frame_done),
    .done_clr                           (GPU2D_done_clr),
    .fps_acc_val                        (GPU2D_fps_acc_val),
    .fps_cpu_val                        (GPU2D_fps_cpu_val),
    .busy_val                           (GPU2D_busy_val),
    .cmd_done_val                       (GPU2D_cmd_done_val),
    .frame_done_val                     (GPU2D_frame_done_val),
    .wr_beats_val                       (GPU2D_wr_beats_val),
    .rd_beats_val                       (GPU2D_rd_beats_val),
    .busy_cyc_val                       (GPU2D_busy_cyc_val),
    .enable                             (GPU2D_enable),
    .split_pending                      (GPU2D_split_pending),
    .framea_base_addr                   (GPU2D_framea_base_addr),
    .frameb_base_addr                   (GPU2D_frameb_base_addr),
    .frame_full_base_addr               (GPU2D_frame_full_base_addr),
    .done_gpu2d                         (GPU2D_done_gpu2d),
    .done_cpu                           (GPU2D_done_cpu),
    .ckey_color                         (GPU2D_ckey_color)
);

//------------------------------------------------------------------------------
// Peripherial(功能层)
//   只放外设的"功能逻辑";它们的"总线解析层"在 APB0_Interconnect 里面
//------------------------------------------------------------------------------

// 外设 0:按键消抖(key)
//   总线层 APB0_key 在 APB0_Interconnect 内,这里只管消抖
key #(
    .KEY_NUM                            (KEY_NUM),
    .DEBOUNCE_CNT                       (DEBOUNCE_CNT)
) u_key (
    .clk                                (user_clk),
    .rst_n                              (sys_rst),

    .key_pin                            (key_pin),
    .key_state                          (key_state),
    .key_edge                           (key_edge)
);

//------------------------------------------------------------------------------
// 外设 1:2D 加速器 + 显示通道(功能层)
//   CI 8 根线进；两组 AXI 主口出(加速器读写 → S2/S3，显示取数 → S1)
//   里面: CmdChannel(CI→命令→行任务) + WriteController(一行→若干笔 AXI 写)
//         + AXIMastersHandler
//         + lineBuffer
//   时钟: clk_ci / clk_acc 都接 user_clk(100MHz)；显示通道另吃 clk_HDMI_x1(148.5MHz)
//   复位: 命令侧都接 axi_rst_n —— DDR 没校准完加速器一律复位，不许发 AXI；
//         显示通道接 rst_hdmi_n(= axi_rst_n & PLL 锁定)
//   配置: 行模式(全屏/分屏)在 GPU_2D 内部于帧边界锁存，顶层不再有这根线
GPU_2D #(
    .FIFO_AW                            (9),
    .LINE_BYTES                         (5760),
    .AXI_ADDR_WIDTH                     (32),
    .AXI_DATA_WIDTH                     (128),
    .AXI_STRB_WIDTH                     (16),
    .AXI_ID_WIDTH                       (8),
    .AXI_LEN_WIDTH                      (8),
    .AXI_ID                             (8'h00)
) u_GPU_2D (
    .clk_ci                             (user_clk),
    .rst_ci_n                           (axi_rst_n),
    .clk_acc                            (user_clk),
    .rst_acc_n                          (axi_rst_n),

    // CI(自定义指令)
    .ci_cmd_valid                       (cpu0_ci_cmd_valid),
    .ci_cmd_ready                       (cpu0_ci_cmd_ready),
    .ci_function_id                     (cpu0_ci_function_id),
    .ci_inputs_0                        (cpu0_ci_inputs_0),
    .ci_inputs_1                        (cpu0_ci_inputs_1),
    .ci_rsp_valid                       (cpu0_ci_rsp_valid),
    .ci_rsp_ready                       (cpu0_ci_rsp_ready),
    .ci_outputs_0                       (cpu0_ci_outputs_0),

    // 配置口：行模式(全屏/分屏)已改为 GPU_2D 内部于帧边界锁存(显示通道也在里面)，
    //         不再是端口；源和目的同跨距，都在 CmdExec 内部派生

    // APB 寄存器侧：总线层在 APB0_Interconnect 里，这里只接"翻译好的寄存器值"
    //   ★ C 驱动只有一个基址(0xf8102000)，显示寄存器的偏移是 0x2040
    .enable                             (GPU2D_enable),
    .split_pending                      (GPU2D_split_pending),
    .framea_base_addr                   (GPU2D_framea_base_addr),
    .frameb_base_addr                   (GPU2D_frameb_base_addr),
    .frame_full_base_addr               (GPU2D_frame_full_base_addr),
    .done_gpu2d                         (GPU2D_done_gpu2d),
    .done_cpu                           (GPU2D_done_cpu),
    //   颜色键控(ck)的键色: APB 0x2078 -> 源侧(colorKey)比像素用
    .ckey_color                         (GPU2D_ckey_color),
    .acc_frame_done                     (GPU2D_acc_frame_done),
    .done_clr                           (GPU2D_done_clr),
    .fps_acc_val                        (GPU2D_fps_acc_val),
    .fps_cpu_val                        (GPU2D_fps_cpu_val),
    .busy_val                           (GPU2D_busy_val),
    .cmd_done_val                       (GPU2D_cmd_done_val),
    .frame_done_val                     (GPU2D_frame_done_val),
    .wr_beats_val                       (GPU2D_wr_beats_val),
    .rd_beats_val                       (GPU2D_rd_beats_val),
    .busy_cyc_val                       (GPU2D_busy_cyc_val),

    // ---- 显示通道(例化在 GPU_2D 里) ----
    .clk_HDMI_x1                        (clk_HDMI_x1),
    .rst_hdmi_n                         (rst_hdmi_n),

    .lineBuffer_m_AXI_AWID              (lineBuffer_m_AXI_AWID),
    .lineBuffer_m_AXI_AWADDR            (lineBuffer_m_AXI_AWADDR),
    .lineBuffer_m_AXI_AWLEN             (lineBuffer_m_AXI_AWLEN),
    .lineBuffer_m_AXI_AWSIZE            (lineBuffer_m_AXI_AWSIZE),
    .lineBuffer_m_AXI_AWBURST           (lineBuffer_m_AXI_AWBURST),
    .lineBuffer_m_AXI_AWLOCK            (lineBuffer_m_AXI_AWLOCK),
    .lineBuffer_m_AXI_AWVALID           (lineBuffer_m_AXI_AWVALID),
    .lineBuffer_m_AXI_AWREADY           (lineBuffer_m_AXI_AWREADY),
    .lineBuffer_m_AXI_WDATA             (lineBuffer_m_AXI_WDATA),
    .lineBuffer_m_AXI_WSTRB             (lineBuffer_m_AXI_WSTRB),
    .lineBuffer_m_AXI_WLAST             (lineBuffer_m_AXI_WLAST),
    .lineBuffer_m_AXI_WVALID            (lineBuffer_m_AXI_WVALID),
    .lineBuffer_m_AXI_WREADY            (lineBuffer_m_AXI_WREADY),
    .lineBuffer_m_AXI_BID               (lineBuffer_m_AXI_BID),
    .lineBuffer_m_AXI_BRESP             (lineBuffer_m_AXI_BRESP),
    .lineBuffer_m_AXI_BVALID            (lineBuffer_m_AXI_BVALID),
    .lineBuffer_m_AXI_BREADY            (lineBuffer_m_AXI_BREADY),
    .lineBuffer_m_AXI_ARID              (lineBuffer_m_AXI_ARID),
    .lineBuffer_m_AXI_ARADDR            (lineBuffer_m_AXI_ARADDR),
    .lineBuffer_m_AXI_ARLEN             (lineBuffer_m_AXI_ARLEN),
    .lineBuffer_m_AXI_ARSIZE            (lineBuffer_m_AXI_ARSIZE),
    .lineBuffer_m_AXI_ARBURST           (lineBuffer_m_AXI_ARBURST),
    .lineBuffer_m_AXI_ARLOCK            (lineBuffer_m_AXI_ARLOCK),
    .lineBuffer_m_AXI_ARVALID           (lineBuffer_m_AXI_ARVALID),
    .lineBuffer_m_AXI_ARREADY           (lineBuffer_m_AXI_ARREADY),
    .lineBuffer_m_AXI_RID               (lineBuffer_m_AXI_RID),
    .lineBuffer_m_AXI_RDATA             (lineBuffer_m_AXI_RDATA),
    .lineBuffer_m_AXI_RRESP             (lineBuffer_m_AXI_RRESP),
    .lineBuffer_m_AXI_RLAST             (lineBuffer_m_AXI_RLAST),
    .lineBuffer_m_AXI_RVALID            (lineBuffer_m_AXI_RVALID),
    .lineBuffer_m_AXI_RREADY            (lineBuffer_m_AXI_RREADY),

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

    // AXI 写主口 → Axi_Mux S3
    .m_AXI_AWID                         (GPU2D_m_AXI_AWID),
    .m_AXI_AWADDR                       (GPU2D_m_AXI_AWADDR),
    .m_AXI_AWLEN                        (GPU2D_m_AXI_AWLEN),
    .m_AXI_AWSIZE                       (GPU2D_m_AXI_AWSIZE),
    .m_AXI_AWBURST                      (GPU2D_m_AXI_AWBURST),
    .m_AXI_AWLOCK                       (GPU2D_m_AXI_AWLOCK),
    .m_AXI_AWCACHE                      (),
    .m_AXI_AWPROT                       (),
    .m_AXI_AWQOS                        (),
    .m_AXI_AWREGION                     (),
    .m_AXI_AWVALID                      (GPU2D_m_AXI_AWVALID),
    .m_AXI_AWREADY                      (GPU2D_m_AXI_AWREADY),

    .m_AXI_WDATA                        (GPU2D_m_AXI_WDATA),
    .m_AXI_WSTRB                        (GPU2D_m_AXI_WSTRB),
    .m_AXI_WLAST                        (GPU2D_m_AXI_WLAST),
    .m_AXI_WVALID                       (GPU2D_m_AXI_WVALID),
    .m_AXI_WREADY                       (GPU2D_m_AXI_WREADY),

    .m_AXI_BID                          (GPU2D_m_AXI_BID),
    .m_AXI_BRESP                        (GPU2D_m_AXI_BRESP),
    .m_AXI_BVALID                       (GPU2D_m_AXI_BVALID),
    .m_AXI_BREADY                       (GPU2D_m_AXI_BREADY),

    // AXI 读主口 → Axi_Mux S2（M2 块拷贝读源用）
    .m_AXI_ARID                         (GPU2D_m_AXI_ARID),
    .m_AXI_ARADDR                       (GPU2D_m_AXI_ARADDR),
    .m_AXI_ARLEN                        (GPU2D_m_AXI_ARLEN),
    .m_AXI_ARSIZE                       (GPU2D_m_AXI_ARSIZE),
    .m_AXI_ARBURST                      (GPU2D_m_AXI_ARBURST),
    .m_AXI_ARLOCK                       (GPU2D_m_AXI_ARLOCK),
    .m_AXI_ARCACHE                      (),
    .m_AXI_ARPROT                       (),
    .m_AXI_ARQOS                        (),
    .m_AXI_ARREGION                     (),
    .m_AXI_ARVALID                      (GPU2D_m_AXI_ARVALID),
    .m_AXI_ARREADY                      (GPU2D_m_AXI_ARREADY),

    .m_AXI_RID                          (GPU2D_m_AXI_RID),
    .m_AXI_RDATA                        (GPU2D_m_AXI_RDATA),
    .m_AXI_RRESP                        (GPU2D_m_AXI_RRESP),
    .m_AXI_RLAST                        (GPU2D_m_AXI_RLAST),
    .m_AXI_RVALID                       (GPU2D_m_AXI_RVALID),
    .m_AXI_RREADY                       (GPU2D_m_AXI_RREADY)
);


//------------------------------------------------------------------------------
// AXI MUX
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// AXI MUX —— 插在四个主与 DDR3(s_axi) 之间
//   S0 = SoC 的 AXI 主口 io_ddrA（CPU 唯一的 DDR 出口）
//   S1 = lineBuffer（显示取数：每行 360 拍，373MB/s）
//   S2 = 加速器的【读】通道（GPU_2D 已经出了 m_AXI_AR*/R* 这一组口，等顶层接上即可）
//   S3 = 2D 加速器的【写】通道（M1 用这一路）
//   ★ lineBuffer = S1, 加速器 = S2/S3 (APB 侧两者已并进同一个槽 2)
//   M  = DDR3 控制器的 AXI 从口
//   参数：
//       AXI_CHANNEL_NUM = 4  （由 Axi_Mux.v 里的 `define X4 决定）
//       SLAVE_ID_WIDTH  = 8 / ID_MODE_SEL = "Normal" → 主端口 ID 也是 8
//       AXI_DATA_WIDTH  = 128
//   仲裁：Cfg_Arb_Type = 0 轮询共享（三个主均分）。若上板发现显示被抢到欠载
//         （画面撕裂/闪烁），可试 1'b1 固定优先级，但要先确认优先级顺序。
//------------------------------------------------------------------------------

Axi_Mux  u_Axi_Mux (
    .Sys_Clk                            (user_clk                  ),
    .Sys_Rst_N                          (sys_rst                   ),   // sys_rst 为低有效，可直接相接

    .Cfg_Arb_Type                       (1'b0                      ),   // 0=轮询(共享) / 1=固定优先级
    .Cfg_Cnl_Mask                       (4'b1111                   ),   // ★必须全 1，否则对应通道被屏蔽

    //==================== Master Port → DDR3 从口 (s_axi) ====================
    .O_M_AW_ID                          (ddr_s_AXI_AWID            ),   // 8bit→4bit 截断(CPU 的 ID 恒 0x00，无影响)
    .O_M_AW_ADDR                        (ddr_s_AXI_AWADDR          ),   // 32bit→28bit 截断(地址<256MB，无影响)
    .O_M_AW_LEN                         (ddr_s_AXI_AWLEN           ),
    .O_M_AW_SIZE                        (ddr_s_AXI_AWSIZE          ),
    .O_M_AW_BURST                       (ddr_s_AXI_AWBURST         ),
    .O_M_AW_LOCK                        (ddr_s_AXI_AWLOCK          ),   // 2bit→1bit 取低位
    .O_M_AW_VALID                       (ddr_s_AXI_AWVALID         ),
    .I_M_AW_READY                       (ddr_s_AXI_AWREADY         ),
    .O_M_W_ID                           (),                             // AXI4 无 WID，DDR 从口无此信号，不接
    .O_M_W_DATA                         (ddr_s_AXI_WDATA           ),
    .O_M_W_LAST                         (ddr_s_AXI_WLAST           ),
    .O_M_W_STRB                         (ddr_s_AXI_WSTRB           ),
    .O_M_W_VALID                        (ddr_s_AXI_WVALID          ),
    .I_M_W_READY                        (ddr_s_AXI_WREADY          ),
    .I_M_B_ID                           ({4'h0, ddr_s_AXI_BID}     ),   // 4bit→8bit 高位补零
    .I_M_B_VALID                        (ddr_s_AXI_BVALID          ),
    .O_M_B_READY                        (ddr_s_AXI_BREADY          ),
    .O_M_AR_ID                          (ddr_s_AXI_ARID            ),
    .O_M_AR_ADDR                        (ddr_s_AXI_ARADDR          ),
    .O_M_AR_LEN                         (ddr_s_AXI_ARLEN           ),
    .O_M_AR_SIZE                        (ddr_s_AXI_ARSIZE          ),
    .O_M_AR_BURST                       (ddr_s_AXI_ARBURST         ),
    .O_M_AR_LOCK                        (ddr_s_AXI_ARLOCK          ),
    .O_M_AR_VALID                       (ddr_s_AXI_ARVALID         ),
    .I_M_AR_READY                       (ddr_s_AXI_ARREADY         ),
    .I_M_R_ID                           ({4'h0, ddr_s_AXI_RID}     ),
    .I_M_R_DATA                         (ddr_s_AXI_RDATA           ),
    .I_M_R_LAST                         (ddr_s_AXI_RLAST           ),
    .I_M_R_RESP                         (ddr_s_AXI_RRESP           ),
    .I_M_R_VALID                        (ddr_s_AXI_RVALID          ),
    .O_M_R_READY                        (ddr_s_AXI_RREADY          ),

    //==================== Slave0 ← SoC io_ddrA ====================
    .I_S0_AW_ID                         (soc_m_AXI_AWID            ),
    .I_S0_AW_ADDR                       (soc_m_AXI_AWADDR          ),
    .I_S0_AW_LEN                        (soc_m_AXI_AWLEN           ),
    .I_S0_AW_SIZE                       (soc_m_AXI_AWSIZE          ),
    .I_S0_AW_BURST                      (soc_m_AXI_AWBURST         ),
    .I_S0_AW_LOCK                       ({1'b0, soc_m_AXI_AWLOCK}  ),   // AXI4 是 1bit，补零成 2bit
    .I_S0_AW_VALID                      (soc_m_AXI_AWVALID         ),
    .O_S0_AW_READY                      (soc_m_AXI_AWREADY         ),
    .I_S0_W_ID                          (8'h0                      ),   // AXI4 无 WID
    .I_S0_W_DATA                        (soc_m_AXI_WDATA           ),
    .I_S0_W_LAST                        (soc_m_AXI_WLAST           ),
    .I_S0_W_STRB                        (soc_m_AXI_WSTRB           ),
    .I_S0_W_VALID                       (soc_m_AXI_WVALID          ),
    .O_S0_W_READY                       (soc_m_AXI_WREADY          ),
    .O_S0_B_ID                          (soc_m_AXI_BID             ),
    .O_S0_B_VALID                       (soc_m_AXI_BVALID          ),
    .I_S0_B_READY                       (soc_m_AXI_BREADY          ),
    .I_S0_AR_ID                         (soc_m_AXI_ARID            ),
    .I_S0_AR_ADDR                       (soc_m_AXI_ARADDR          ),
    .I_S0_AR_LEN                        (soc_m_AXI_ARLEN           ),
    .I_S0_AR_SIZE                       (soc_m_AXI_ARSIZE          ),
    .I_S0_AR_BURST                      (soc_m_AXI_ARBURST         ),
    .I_S0_AR_LOCK                       ({1'b0, soc_m_AXI_ARLOCK}  ),
    .I_S0_AR_VALID                      (soc_m_AXI_ARVALID         ),
    .O_S0_AR_READY                      (soc_m_AXI_ARREADY         ),
    .O_S0_R_ID                          (soc_m_AXI_RID             ),
    .O_S0_R_DATA                        (soc_m_AXI_RDATA           ),
    .O_S0_R_LAST                        (soc_m_AXI_RLAST           ),
    .O_S0_R_RESP                        (soc_m_AXI_RRESP           ),
    .O_S0_R_VALID                       (soc_m_AXI_RVALID          ),
    .I_S0_R_READY                       (soc_m_AXI_RREADY          ),

    //==================== Slave1 ← lineBuffer（显示取数，只读） ================
    .I_S1_AW_ID                          (lineBuffer_m_AXI_AWID      ),
    .I_S1_AW_ADDR                        (lineBuffer_m_AXI_AWADDR    ),
    .I_S1_AW_LEN                         (lineBuffer_m_AXI_AWLEN     ),
    .I_S1_AW_SIZE                        (lineBuffer_m_AXI_AWSIZE    ),
    .I_S1_AW_BURST                       (lineBuffer_m_AXI_AWBURST   ),
    .I_S1_AW_LOCK                        ({1'b0, lineBuffer_m_AXI_AWLOCK}),
    .I_S1_AW_VALID                       (lineBuffer_m_AXI_AWVALID   ),
    .O_S1_AW_READY                       (lineBuffer_m_AXI_AWREADY   ),
    .I_S1_W_ID                           (8'h0                       ),   // AXI4 无 WID
    .I_S1_W_DATA                         (lineBuffer_m_AXI_WDATA     ),
    .I_S1_W_STRB                         (lineBuffer_m_AXI_WSTRB     ),
    .I_S1_W_LAST                         (lineBuffer_m_AXI_WLAST     ),
    .I_S1_W_VALID                        (lineBuffer_m_AXI_WVALID    ),
    .O_S1_W_READY                        (lineBuffer_m_AXI_WREADY    ),
    .O_S1_B_ID                           (lineBuffer_m_AXI_BID       ),
    .O_S1_B_VALID                        (lineBuffer_m_AXI_BVALID    ),
    .I_S1_B_READY                        (lineBuffer_m_AXI_BREADY    ),
    .I_S1_AR_ID                          (lineBuffer_m_AXI_ARID      ),
    .I_S1_AR_ADDR                        (lineBuffer_m_AXI_ARADDR    ),
    .I_S1_AR_LEN                         (lineBuffer_m_AXI_ARLEN     ),
    .I_S1_AR_SIZE                        (lineBuffer_m_AXI_ARSIZE    ),
    .I_S1_AR_BURST                       (lineBuffer_m_AXI_ARBURST   ),
    .I_S1_AR_LOCK                        ({1'b0, lineBuffer_m_AXI_ARLOCK}),
    .I_S1_AR_VALID                       (lineBuffer_m_AXI_ARVALID   ),
    .O_S1_AR_READY                       (lineBuffer_m_AXI_ARREADY   ),
    .O_S1_R_ID                           (lineBuffer_m_AXI_RID       ),
    .O_S1_R_DATA                         (lineBuffer_m_AXI_RDATA     ),
    .O_S1_R_LAST                         (lineBuffer_m_AXI_RLAST     ),
    .O_S1_R_RESP                         (lineBuffer_m_AXI_RRESP     ),
    .O_S1_R_VALID                        (lineBuffer_m_AXI_RVALID    ),
    .I_S1_R_READY                        (lineBuffer_m_AXI_RREADY    ),

    //==================== Slave2 ← 2D 加速器（读通道） ========================
    //   加速器的【读】走这一路(BLIT/块拷贝读源)，它的【写】走 S3。
    //   所以这里只接 AR/R；AW/W/B 接常量(加速器不会从这一路写)。
    //   ★ 读写通道在 Axi_Mux 里是【独立仲裁】的，所以 S2 读、S3 写可以同时在飞 ——
    //     这正是块拷贝要的: 读源和写目的并行。
    .I_S2_AW_ID                          (8'h0                       ),
    .I_S2_AW_ADDR                        (32'h0                      ),
    .I_S2_AW_LEN                         (8'h0                       ),
    .I_S2_AW_SIZE                        (3'h0                       ),
    .I_S2_AW_BURST                       (2'h0                       ),
    .I_S2_AW_LOCK                        (2'h0                       ),
    .I_S2_AW_VALID                       (1'b0                       ),
    .O_S2_AW_READY                       (                           ),
    .I_S2_W_ID                           (8'h0                       ),
    .I_S2_W_DATA                         (128'h0                     ),
    .I_S2_W_STRB                         (16'h0                      ),
    .I_S2_W_LAST                         (1'b0                       ),
    .I_S2_W_VALID                        (1'b0                       ),
    .O_S2_W_READY                        (                           ),
    .O_S2_B_ID                           (                           ),
    .O_S2_B_VALID                        (                           ),
    .I_S2_B_READY                        (1'b0                       ),
    .I_S2_AR_ID                          (GPU2D_m_AXI_ARID           ),
    .I_S2_AR_ADDR                        (GPU2D_m_AXI_ARADDR         ),
    .I_S2_AR_LEN                         (GPU2D_m_AXI_ARLEN          ),
    .I_S2_AR_SIZE                        (GPU2D_m_AXI_ARSIZE         ),
    .I_S2_AR_BURST                       (GPU2D_m_AXI_ARBURST        ),
    .I_S2_AR_LOCK                        ({1'b0, gpu2d_m_AXI_ARLOCK} ),
    .I_S2_AR_VALID                       (GPU2D_m_AXI_ARVALID        ),
    .O_S2_AR_READY                       (GPU2D_m_AXI_ARREADY        ),
    .O_S2_R_ID                           (GPU2D_m_AXI_RID            ),
    .O_S2_R_DATA                         (GPU2D_m_AXI_RDATA          ),
    .O_S2_R_LAST                         (GPU2D_m_AXI_RLAST          ),
    .O_S2_R_RESP                         (GPU2D_m_AXI_RRESP          ),
    .O_S2_R_VALID                        (GPU2D_m_AXI_RVALID         ),
    .I_S2_R_READY                        (GPU2D_m_AXI_RREADY         ),

    //==================== Slave3 ← 2D 加速器（写通道） ========================
    //   加速器只会写(帧缓存)，所以只接 AW/W/B；读通道 M2 做 Block Copy 时才接
    .I_S3_AW_ID                          (GPU2D_m_AXI_AWID           ),
    .I_S3_AW_ADDR                        (GPU2D_m_AXI_AWADDR         ),
    .I_S3_AW_LEN                         (GPU2D_m_AXI_AWLEN          ),
    .I_S3_AW_SIZE                        (GPU2D_m_AXI_AWSIZE         ),
    .I_S3_AW_BURST                       (GPU2D_m_AXI_AWBURST        ),
    .I_S3_AW_LOCK                        ({1'b0, gpu2d_m_AXI_AWLOCK} ),
    .I_S3_AW_VALID                       (GPU2D_m_AXI_AWVALID        ),
    .O_S3_AW_READY                       (GPU2D_m_AXI_AWREADY        ),
    .I_S3_W_ID                           (8'h0                       ),   // AXI4 无 WID
    .I_S3_W_DATA                         (GPU2D_m_AXI_WDATA          ),
    .I_S3_W_STRB                         (GPU2D_m_AXI_WSTRB          ),
    .I_S3_W_LAST                         (GPU2D_m_AXI_WLAST          ),
    .I_S3_W_VALID                        (GPU2D_m_AXI_WVALID         ),
    .O_S3_W_READY                        (GPU2D_m_AXI_WREADY         ),
    .O_S3_B_ID                           (GPU2D_m_AXI_BID            ),
    .O_S3_B_VALID                        (GPU2D_m_AXI_BVALID         ),
    .I_S3_B_READY                        (GPU2D_m_AXI_BREADY         ),

    //   这里的读通道【故意】留空：加速器的读走 S2，这一路只管写。
    .I_S3_AR_ID                          (8'h0                       ),
    .I_S3_AR_ADDR                        (32'h0                      ),
    .I_S3_AR_LEN                         (8'h0                       ),
    .I_S3_AR_SIZE                        (3'h0                       ),
    .I_S3_AR_BURST                       (2'h0                       ),
    .I_S3_AR_LOCK                        (2'h0                       ),
    .I_S3_AR_VALID                       (1'b0                       ),
    .O_S3_AR_READY                       (                           ),
    .O_S3_R_ID                           (                           ),
    .O_S3_R_DATA                         (                           ),
    .O_S3_R_LAST                         (                           ),
    .O_S3_R_RESP                         (                           ),
    .O_S3_R_VALID                        (                           ),
    .I_S3_R_READY                        (1'b0                       ),

    //==================== 调试观测（激活通道号，接 ILA 用） ====================
    .O_AW_Cnl_Num                       (dbg_aw_cnl_num            ),
    .O_W_Cnl_Num                        (dbg_w_cnl_num             ),
    .O_B_Cnl_Num                        (dbg_b_cnl_num             ),
    .O_AR_Cnl_Num                       (dbg_ar_cnl_num            ),
    .O_R_Cnl_Num                        (dbg_r_cnl_num             )
);

endmodule