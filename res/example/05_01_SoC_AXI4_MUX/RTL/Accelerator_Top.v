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
    input   wire   [3:0]            key_pin
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

// Axi_Mux 调试观测：各通道当前激活的通道号（位宽 = clog2(通道数) = 1 bit）
wire [0:0]                          dbg_aw_cnl_num;
wire [0:0]                          dbg_w_cnl_num;
wire [0:0]                          dbg_b_cnl_num;
wire [0:0]                          dbg_ar_cnl_num;
wire [0:0]                          dbg_r_cnl_num;

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

// DDR3 Debug VIO is it still use?
// wire                                app_rdy;  
// wire [2:0]                          vio_pll_shift;  
// wire [4:0]                          vio_pll_shift_sel;
// wire                                vio_pll_shift_ena; 
// wire                                pos_pll_shift_ena; 
// wire [2:0]                          phy_pll_shift;  
// wire [4:0]                          phy_pll_shift_sel;
// wire                                phy_pll_shift_ena; 
// wire                                ddr_reset; 
// wire  [2:0]                         phy_wr_pll_shift;
// wire  [15:0]                        debug_fifo;
// wire  [15:0]                        overflow_fifo;
// wire                                phy_rddata_valid;
// wire [2*CK_RATIO*DQ_WIDTH-1:0]      phy_rd_data;
// wire [2*CK_RATIO*DATA_WIDTH-1:0]    app_rd_data_to_axi; 
// wire                                app_rd_data_end;
// wire                                app_rd_data_valid;
// wire [2*CK_RATIO*DQ_WIDTH-1:0]      rd_data;          
// wire                                rd_data_en;            
// wire                                rd_data_end;   


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


// SoC IRQ
wire                                key_irq;

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
    .io_ddrA_b_payload_resp             (                                 ),//(                                  ),
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
    .userInterruptA                     (key_irq),
    
    // SPI 0
    .system_spi_0_io_sclk_write         (system_spi_0_io_sclk_write),
    .system_spi_0_io_data_0_writeEnable (system_spi_0_io_data_0_OE),
    .system_spi_0_io_data_0_read        (system_spi_0_io_data_0_IN),
    .system_spi_0_io_data_0_write       (system_spi_0_io_data_0_OUT),
    .system_spi_0_io_data_1_writeEnable (system_spi_0_io_data_1_OE),
    .system_spi_0_io_data_1_read        (system_spi_0_io_data_1_IN),
    .system_spi_0_io_data_1_write       (system_spi_0_io_data_1_OUT),
    .system_spi_0_io_data_2_writeEnable (),
    .system_spi_0_io_data_2_read        (),
    .system_spi_0_io_data_2_write       (),
    .system_spi_0_io_data_3_writeEnable (),
    .system_spi_0_io_data_3_read        (),
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
    .jtagCtrl_reset                     (jtag_inst1_RESET)
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
    .s_axi_awqos                        (4'h0              ),
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
    .s_axi_arqos                        (4'h0              ),
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
    .wrlvl_dq_check                     (wrlvl_dq_check    ) ,
    .rd_level_dqs_check                 (rd_level_dqs_check) ,
    .rdlvl_shift                        (rdlvl_shift) ,
    .wrlvl_shift                        (wrlvl_shift) ,
    .init_cur_state                     (init_cur_state    ) ,
    .idelay_ld                          (idelay_ld         ) ,
    .mpr_rdlvl_dly                      (mpr_rdlvl_dly     ) ,
    .ddr_debug_port                     (ddr_debug_port    ) ,
    .cal_done                           (cal_done          ) 
);
// End of User Design top instance

//------------------------------------------------------------------------------
// APB3 port0 Interface
//------------------------------------------------------------------------------
APB0_Interconnect  u_APB0_Interconnect (
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

    .key_pin                            (key_pin),
    .key_irq                            (key_irq)
);

//------------------------------------------------------------------------------
// AXI MUX
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// AXI MUX —— 插在 CPU(io_ddrA) 与 DDR3(s_axi) 之间
//   S0 = SoC 的 AXI 主口 io_ddrA（CPU 唯一的 DDR 出口）
//   S1 = 预留（第二个主：加速器）—— 输入先接常量，接第二个主时再改这一组
//   M  = DDR3 控制器的 AXI 从口
//   参数（Axi_Mux.v 内部 parameter 默认值，本工程不改）：
//       AXI_CHANNEL_NUM = 2  （由 Axi_Mux.v 里的 `define X2 决定）
//       SLAVE_ID_WIDTH  = 8 / ID_MODE_SEL = "Normal" → 主端口 ID 也是 8
//       AXI_DATA_WIDTH  = 128
//------------------------------------------------------------------------------

Axi_Mux  u_Axi_Mux (
    .Sys_Clk                            (user_clk                  ),
    .Sys_Rst_N                          (sys_rst                   ),   // sys_rst 为低有效，可直接相接

    .Cfg_Arb_Type                       (1'b0                      ),   // 0=轮询(共享) / 1=固定优先级
    .Cfg_Cnl_Mask                       (2'b11                     ),   // ★必须全 1，否则对应通道被屏蔽

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

    //==================== Slave1 ← 预留（第二个主，输入先接常量） ====================
    .I_S1_AW_ID                         (8'h0                      ),
    .I_S1_AW_ADDR                       (32'h0                     ),
    .I_S1_AW_LEN                        (8'h0                      ),
    .I_S1_AW_SIZE                       (3'h0                      ),
    .I_S1_AW_BURST                      (2'h0                      ),
    .I_S1_AW_LOCK                       (2'h0                      ),
    .I_S1_AW_VALID                      (1'b0                      ),
    .I_S1_W_ID                          (8'h0                      ),
    .I_S1_W_DATA                        (128'h0                    ),
    .I_S1_W_LAST                        (1'b0                      ),
    .I_S1_W_STRB                        (16'h0                     ),
    .I_S1_W_VALID                       (1'b0                      ),
    .I_S1_B_READY                       (1'b1                      ),
    .I_S1_AR_ID                         (8'h0                      ),
    .I_S1_AR_ADDR                       (32'h0                     ),
    .I_S1_AR_LEN                        (8'h0                      ),
    .I_S1_AR_SIZE                       (3'h0                      ),
    .I_S1_AR_BURST                      (2'h0                      ),
    .I_S1_AR_LOCK                       (2'h0                      ),
    .I_S1_AR_VALID                      (1'b0                      ),
    .I_S1_R_READY                       (1'b1                      ),

    //==================== 调试观测（激活通道号，接 ILA 用） ====================
    .O_AW_Cnl_Num                       (dbg_aw_cnl_num            ),
    .O_W_Cnl_Num                        (dbg_w_cnl_num             ),
    .O_B_Cnl_Num                        (dbg_b_cnl_num             ),
    .O_AR_Cnl_Num                       (dbg_ar_cnl_num            ),
    .O_R_Cnl_Num                        (dbg_r_cnl_num             )
);

endmodule