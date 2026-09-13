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
wire [AXI_ID_WIDTH-1:0]             s_axi_awid;
wire [AXI_ADDR_WIDTH-1:0]           s_axi_awaddr;
wire [7:0]                          s_axi_awlen;
wire [2:0]                          s_axi_awsize;
wire [1:0]                          s_axi_awburst;
wire [0:0]                          s_axi_awlock;
wire [3:0]                          s_axi_awcache;
wire [2:0]                          s_axi_awprot;
wire                                s_axi_awvalid;
wire                                s_axi_awready;
// Slave Interface Write Data Ports
wire [AXI_DATA_WIDTH-1:0]           s_axi_wdata;
wire [(AXI_DATA_WIDTH/8)-1:0]       s_axi_wstrb;
wire                                s_axi_wlast;
wire                                s_axi_wvalid;
wire                                s_axi_wready;
// Slave Interface Write Response Ports
wire [AXI_ID_WIDTH-1:0]             s_axi_bid;
wire [1:0]                          s_axi_bresp;
wire                                s_axi_bvalid;
wire                                s_axi_bready;
// Slave Interface Read Address Ports
wire [AXI_ID_WIDTH-1:0]             s_axi_arid;
wire [AXI_ADDR_WIDTH-1:0]           s_axi_araddr;
wire [7:0]                          s_axi_arlen;
wire [2:0]                          s_axi_arsize;
wire [1:0]                          s_axi_arburst;
wire [0:0]                          s_axi_arlock;
wire [3:0]                          s_axi_arcache;
wire [2:0]                          s_axi_arprot;
wire                                s_axi_arvalid;
wire                                s_axi_arready;
// Slave Interface Read Data Ports
wire [AXI_ID_WIDTH-1:0]             s_axi_rid;
wire [AXI_DATA_WIDTH-1:0]           s_axi_rdata;
wire [1:0]                          s_axi_rresp;
wire                                s_axi_rlast;
wire                                s_axi_rvalid;
wire                                s_axi_rready;
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
    .io_ddrA_aw_payload_lock            (s_axi_awlock                     ),//(m1_axi_awlock                     ),
    .io_ddrA_aw_payload_burst           (s_axi_awburst                    ),//(m1_axi_awburst                    ),
    .io_ddrA_aw_payload_size            (s_axi_awsize                     ),//(m1_axi_awsize                     ),
    .io_ddrA_aw_payload_len             (s_axi_awlen                      ),//(m1_axi_awlen                      ),
    .io_ddrA_aw_payload_region          (                                 ),//(                                  ),
    .io_ddrA_aw_payload_id              (s_axi_awid                       ),//(m1_axi_awid                       ),
    .io_ddrA_aw_payload_addr            (s_axi_awaddr                     ),//(m1_axi_awaddr                     ),
    .io_ddrA_aw_ready                   (s_axi_awready                    ),//(m1_axi_awready                    ),
    .io_ddrA_aw_valid                   (s_axi_awvalid                    ),//(m1_axi_awvalid                    ),
    .io_ddrA_w_payload_last             (s_axi_wlast                      ),//(m1_axi_wlast                      ),
    .io_ddrA_w_ready                    (s_axi_wready                     ),//(m1_axi_wready                     ),
    .io_ddrA_w_valid                    (s_axi_wvalid                     ),  //(m1_axi_wvalid                     ),    
    .io_ddrA_w_payload_strb             (s_axi_wstrb                      ),//(m1_axi_wstrb                      ),
    .io_ddrA_w_payload_data             (s_axi_wdata                      ),//(m1_axi_wdata                      ),
    .io_ddrA_b_payload_resp             (                                 ),//(                                  ),
    .io_ddrA_b_payload_id               (s_axi_bid                        ),//(m1_axi_bid                        ),
    .io_ddrA_b_ready                    (s_axi_bready                     ),//(m1_axi_bready                     ),
    .io_ddrA_b_valid                    (s_axi_bvalid                     ),//(m1_axi_bvalid                     ),
    .io_ddrA_ar_payload_prot            (                                 ),//(                                  ),
    .io_ddrA_ar_payload_qos             (                                 ),//(                                  ),
    .io_ddrA_ar_payload_cache           (                                 ),//(                                  ),
    .io_ddrA_ar_payload_region          (                                 ),//(                                  ),
    .io_ddrA_ar_payload_lock            (s_axi_arlock                     ),//(m1_axi_arlock                     ),
    .io_ddrA_ar_payload_burst           (s_axi_arburst                    ),//(m1_axi_arburst                    ),
    .io_ddrA_ar_payload_size            (s_axi_arsize                     ),//(m1_axi_arsize                     ),
    .io_ddrA_ar_payload_len             (s_axi_arlen                      ),//(m1_axi_arlen                      ),
    .io_ddrA_ar_payload_id              (s_axi_arid                       ),//(m1_axi_arid                       ),
    .io_ddrA_ar_payload_addr            (s_axi_araddr                     ),//(m1_axi_araddr                     ),
    .io_ddrA_ar_ready                   (s_axi_arready                    ),//(m1_axi_arready                    ),
    .io_ddrA_ar_valid                   (s_axi_arvalid                    ),//(m1_axi_arvalid                    ),
    .io_ddrA_r_payload_last             (s_axi_rlast                      ),//(m1_axi_rlast                      ),
    .io_ddrA_r_payload_resp             (s_axi_rresp                      ),//(m1_axi_rresp                      ),
    .io_ddrA_r_payload_id               (s_axi_rid                        ),//(m1_axi_rid                        ),
    .io_ddrA_r_payload_data             (s_axi_rdata                      ),//(m1_axi_rdata                      ),
    .io_ddrA_r_ready                    (s_axi_rready                     ),//(m1_axi_rready                     ),
    .io_ddrA_r_valid                    (s_axi_rvalid                     ),//(m1_axi_rvalid                    ),

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
    .s_axi_awid                         (s_axi_awid        ),
    .s_axi_awaddr                       (s_axi_awaddr      ),
    .s_axi_awlen                        (s_axi_awlen       ),
    .s_axi_awsize                       (s_axi_awsize      ),
    .s_axi_awburst                      (s_axi_awburst     ),
    .s_axi_awlock                       (s_axi_awlock      ),
    .s_axi_awcache                      (s_axi_awcache     ),
    .s_axi_awprot                       (s_axi_awprot      ),
    .s_axi_awqos                        (4'h0              ),
    .s_axi_awvalid                      (s_axi_awvalid     ),
    .s_axi_awready                      (s_axi_awready     ),
    // Slave Interface Write Data Ports
    .s_axi_wdata                        (s_axi_wdata       ),
    .s_axi_wstrb                        (s_axi_wstrb       ),
    .s_axi_wlast                        (s_axi_wlast       ),
    .s_axi_wvalid                       (s_axi_wvalid      ),
    .s_axi_wready                       (s_axi_wready      ),
    // Slave Interface Write Response Ports
    .s_axi_bid                          (s_axi_bid         ),
    .s_axi_bresp                        (s_axi_bresp       ),
    .s_axi_bvalid                       (s_axi_bvalid      ),
    .s_axi_bready                       (s_axi_bready      ),
    // Slave Interface Read Address Ports
    .s_axi_arid                         (s_axi_arid        ),
    .s_axi_araddr                       (s_axi_araddr      ),
    .s_axi_arlen                        (s_axi_arlen       ),
    .s_axi_arsize                       (s_axi_arsize      ),
    .s_axi_arburst                      (s_axi_arburst     ),
    .s_axi_arlock                       (s_axi_arlock      ),
    .s_axi_arcache                      (s_axi_arcache     ),
    .s_axi_arprot                       (s_axi_arprot      ),
    .s_axi_arqos                        (4'h0              ),
    .s_axi_arvalid                      (s_axi_arvalid     ),
    .s_axi_arready                      (s_axi_arready     ),
    // Slave Interface Read Data Ports
    .s_axi_rid                          (s_axi_rid         ),
    .s_axi_rdata                        (s_axi_rdata       ),
    .s_axi_rresp                        (s_axi_rresp       ),
    .s_axi_rlast                        (s_axi_rlast       ),
    .s_axi_rvalid                       (s_axi_rvalid      ),
    .s_axi_rready                       (s_axi_rready      ),
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

endmodule
