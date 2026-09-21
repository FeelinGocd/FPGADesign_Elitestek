//////////////////////////////////////////////////////////////////////////////
// AXI4_Master —— AXI4 主设备（顶层）
//   按 cmd_write 把命令分发给读写两条通路，并聚合 AXI 端口。
//
// 接口组：
//   clk / rst_n     时钟、复位（异步断言 / 同步撤销）
//                   ★ 系统级门控（例如"DDR 校准完成"）由顶层并进 rst_n 后再送进来。
//                     本模块不认识任何系统级信号，只认 rst_n，可独立搬用。
//   cmd_*           命令接口：提交一笔事务（读还是写 / 去哪 / 多长），干完回报
//   wr_*            写数据接口：数据源供货（拉式）
//   rd_*            读数据接口：数据交给下游（推式）
//   m_AXI_*         AXI4 主端口：AW / W / B / AR / R
//
// 约束：单 outstanding；AXI ID 恒为 0；单笔不跨 4KB。
//   1. cmd_valid 拉高后 payload 必须稳定，直到 cmd_ready
//   2. wr_valid 不得由 wr_ready 组合产生
//   3. rd_valid 不得由 rd_ready 组合产生
//
// 参考：docs/spec/AXI4FULL解析.md §9 / §17 / §18 / §19
//////////////////////////////////////////////////////////////////////////////
`timescale 1ns/100ps

module AXI4_Master #(
    parameter ADDR_WIDTH = 32,
    parameter DATA_WIDTH = 128,
    parameter STRB_WIDTH = DATA_WIDTH/8,        // 16 字节使能位
    parameter ID_WIDTH   = 8,                   // 与 Axi_Mux 的 SLAVE_ID_WIDTH 一致
    parameter LEN_WIDTH  = 8,                   // 突发拍数-1（最大 256 拍 = 4KB）
    parameter AXI_ID     = {ID_WIDTH{1'b0}}     // 全链路 ID = 0
)(
    //==================== 时钟 / 复位 ====================
    input  wire                     clk,
    input  wire                     rst_n,          // 异步复位，低有效

    //==================== 命令接口 ====================
    input  wire                     cmd_valid,      //(I) 命令有效
    output wire                     cmd_ready,      //(O) 可接收命令（空闲且不在复位）
    input  wire                     cmd_write,      //(I) 1 = 写，0 = 读
    input  wire [ADDR_WIDTH-1:0]    cmd_addr,       //(I) 起始字节地址
    input  wire [LEN_WIDTH-1:0]     cmd_len,        //(I) 突发拍数 - 1
    output wire                     cmd_done,       //(O) 命令完成脉冲（1 拍）
    output wire                     cmd_err,        //(O) 响应非 OKAY（读通路才有）

    //==================== 写数据接口（写命令时由数据源供货）====================
    input  wire [DATA_WIDTH-1:0]    wr_data,
    input  wire [STRB_WIDTH-1:0]    wr_strb,
    input  wire                     wr_valid,
    output wire                     wr_ready,

    //==================== 读数据接口（读命令时逐拍吐出）====================
    output wire [DATA_WIDTH-1:0]    rd_data,
    output wire                     rd_valid,
    output wire                     rd_last,
    input  wire                     rd_ready,

    //==================== AXI4 主端口 ====================
    // ---- AW ----
    output wire [ID_WIDTH-1:0]      m_AXI_AWID,
    output wire [ADDR_WIDTH-1:0]    m_AXI_AWADDR,
    output wire [LEN_WIDTH-1:0]     m_AXI_AWLEN,
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
    output wire [DATA_WIDTH-1:0]    m_AXI_WDATA,
    output wire [STRB_WIDTH-1:0]    m_AXI_WSTRB,
    output wire                     m_AXI_WLAST,
    output wire                     m_AXI_WVALID,
    input  wire                     m_AXI_WREADY,
    // ---- B ----
    input  wire [ID_WIDTH-1:0]      m_AXI_BID,
    input  wire [1:0]               m_AXI_BRESP,
    input  wire                     m_AXI_BVALID,
    output wire                     m_AXI_BREADY,
    // ---- AR ----
    output wire [ID_WIDTH-1:0]      m_AXI_ARID,
    output wire [ADDR_WIDTH-1:0]    m_AXI_ARADDR,
    output wire [LEN_WIDTH-1:0]     m_AXI_ARLEN,
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
    input  wire [ID_WIDTH-1:0]      m_AXI_RID,
    input  wire [DATA_WIDTH-1:0]    m_AXI_RDATA,
    input  wire [1:0]               m_AXI_RRESP,
    input  wire                     m_AXI_RLAST,
    input  wire                     m_AXI_RVALID,
    output wire                     m_AXI_RREADY
);

//------------------------------------------------------------------------------
// 命令分发：按 cmd_write 把命令送给对应通路
//------------------------------------------------------------------------------
wire wr_cmd_valid = cmd_valid &&  cmd_write;
wire rd_cmd_valid = cmd_valid && !cmd_write;

wire wr_cmd_ready;
wire rd_cmd_ready;
wire wr_cmd_done;
wire rd_cmd_done;
wire wr_cmd_err;
wire rd_cmd_err;

assign cmd_ready = cmd_write ? wr_cmd_ready : rd_cmd_ready;
assign cmd_done  = wr_cmd_done  | rd_cmd_done;
assign cmd_err   = wr_cmd_err   | rd_cmd_err;   // 写通路恒 0（BRESP 拿不到）

//------------------------------------------------------------------------------
// 写通路
//------------------------------------------------------------------------------
AXI4_Master_Write # (
    .ADDR_WIDTH     (ADDR_WIDTH),
    .DATA_WIDTH     (DATA_WIDTH),
    .STRB_WIDTH     (STRB_WIDTH),
    .ID_WIDTH       (ID_WIDTH),
    .LEN_WIDTH      (LEN_WIDTH),
    .AXI_ID         (AXI_ID)
  )
  AXI4_Master_Write_inst (
    .clk            (clk),
    .rst_n          (rst_n),

    .cmd_valid      (wr_cmd_valid),
    .cmd_ready      (wr_cmd_ready),
    .cmd_addr       (cmd_addr),
    .cmd_len        (cmd_len),
    .cmd_done       (wr_cmd_done),
    .cmd_err        (wr_cmd_err),

    .wr_data        (wr_data),
    .wr_strb        (wr_strb),
    .wr_valid       (wr_valid),
    .wr_ready       (wr_ready),

    .m_AXI_AWID     (m_AXI_AWID),
    .m_AXI_AWADDR   (m_AXI_AWADDR),
    .m_AXI_AWLEN    (m_AXI_AWLEN),
    .m_AXI_AWSIZE   (m_AXI_AWSIZE),
    .m_AXI_AWBURST  (m_AXI_AWBURST),
    .m_AXI_AWLOCK   (m_AXI_AWLOCK),
    .m_AXI_AWCACHE  (m_AXI_AWCACHE),
    .m_AXI_AWPROT   (m_AXI_AWPROT),
    .m_AXI_AWQOS    (m_AXI_AWQOS),
    .m_AXI_AWREGION (m_AXI_AWREGION),
    .m_AXI_AWVALID  (m_AXI_AWVALID),
    .m_AXI_AWREADY  (m_AXI_AWREADY),

    .m_AXI_WDATA    (m_AXI_WDATA),
    .m_AXI_WSTRB    (m_AXI_WSTRB),
    .m_AXI_WLAST    (m_AXI_WLAST),
    .m_AXI_WVALID   (m_AXI_WVALID),
    .m_AXI_WREADY   (m_AXI_WREADY),

    .m_AXI_BID      (m_AXI_BID),            // 单 outstanding 未使用
    .m_AXI_BRESP    (m_AXI_BRESP),          // Axi_Mux 不透传，恒 0
    .m_AXI_BVALID   (m_AXI_BVALID),
    .m_AXI_BREADY   (m_AXI_BREADY)
  );

//------------------------------------------------------------------------------
// 读通路
//------------------------------------------------------------------------------
AXI4_Master_Read # (
    .ADDR_WIDTH     (ADDR_WIDTH),
    .DATA_WIDTH     (DATA_WIDTH),
    .ID_WIDTH       (ID_WIDTH),
    .LEN_WIDTH      (LEN_WIDTH),
    .AXI_ID         (AXI_ID)
  )
  AXI4_Master_Read_inst (
    .clk            (clk),
    .rst_n          (rst_n),

    .cmd_valid      (rd_cmd_valid),
    .cmd_ready      (rd_cmd_ready),
    .cmd_addr       (cmd_addr),
    .cmd_len        (cmd_len),
    .cmd_done       (rd_cmd_done),
    .cmd_err        (rd_cmd_err),

    .rd_data        (rd_data),
    .rd_valid       (rd_valid),
    .rd_last        (rd_last),
    .rd_ready       (rd_ready),

    .m_AXI_ARID     (m_AXI_ARID),
    .m_AXI_ARADDR   (m_AXI_ARADDR),
    .m_AXI_ARLEN    (m_AXI_ARLEN),
    .m_AXI_ARSIZE   (m_AXI_ARSIZE),
    .m_AXI_ARBURST  (m_AXI_ARBURST),
    .m_AXI_ARLOCK   (m_AXI_ARLOCK),
    .m_AXI_ARCACHE  (m_AXI_ARCACHE),
    .m_AXI_ARPROT   (m_AXI_ARPROT),
    .m_AXI_ARQOS    (m_AXI_ARQOS),
    .m_AXI_ARREGION (m_AXI_ARREGION),
    .m_AXI_ARVALID  (m_AXI_ARVALID),
    .m_AXI_ARREADY  (m_AXI_ARREADY),

    .m_AXI_RID      (m_AXI_RID),            // 单 outstanding 未使用
    .m_AXI_RDATA    (m_AXI_RDATA),
    .m_AXI_RRESP    (m_AXI_RRESP),
    .m_AXI_RLAST    (m_AXI_RLAST),
    .m_AXI_RVALID   (m_AXI_RVALID),
    .m_AXI_RREADY   (m_AXI_RREADY)
  );

endmodule
