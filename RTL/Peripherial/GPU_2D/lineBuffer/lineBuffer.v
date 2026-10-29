//==============================================================================
//  lineBuffer.v —— 显示通道(加速器外挂的"控制屏幕"外设)
//------------------------------------------------------------------------------
//  里面是什么
//      AXI4_Master      : 对 DDR 发起读命令(AXI4 主口 -> Axi_Mux S1)
//      bufferControler  : 按 {段0,段1} 基址把一行从 DDR 搬进来
//                         (全屏取 241 拍 = 240 + 1, 分屏 2x120 拍)
//      lineAligner      : 行首非 16B 对齐时丢掉前 skip 个字节, 241 拍 -> 240 拍
//      async_PingPongBuffer : 双 bank, 跨 100MHz <-> 74.25MHz
//      bufferUnpacker   : 16 字节/拍 -> 3 字节/像素
//      HDMI_Top         : 720p60 时序(pixel_timing 里 define 选) + TMDS
//      asyn_rst_syn     : 像素域复位同步(异步断言 / 同步释放)
//      frame_start 跨域桥: 读域 HDMI_frame_start_o -> toggle -> 写域 2FF+seen
//   ★ "寄存器 -> 有效信号"(翻页/选地址/帧率)的翻译【不在本模块】:
//     由 GPU_2D 里的 GPU2D_APBHandler 做完, 这里只收 enable / mode / addr 三根线
//
//  两级时钟
//      clk(100MHz)          : 寄存器值 / controller / aligner / PingPong 写侧 / AXI
//      clk_HDMI_x1(74.25MHz): PingPong 读侧 / unpacker / HDMI / frame_start 的产生
//      ★ aligner 只在写域: 74.25MHz 那条像素通路一个字节都不动
//
//  和外面怎么连
//      进: 已经翻译好的 enable / mode / {段0,段1} 基址（寄存器在 GPU2D_APBHandler 那边）
//      出: 帧边界单拍 lineBuffer_frame_start（回 GPU2D_APBHandler 翻页/计帧率）
//      端口名和 GPU2D_APBHandler 那侧保持一致, 顶层一一对接
//==============================================================================
module lineBuffer #(
    parameter AXI_ADDR_WIDTH    = 32,
    parameter AXI_DATA_WIDTH    = 128,
    parameter AXI_STRB_WIDTH    = AXI_DATA_WIDTH/8,
    parameter AXI_ID_WIDTH      = 8,
    parameter AXI_LEN_WIDTH     = 8,
    parameter AXI_ID            = {AXI_ID_WIDTH{1'b0}},
    parameter LINE_BYTES        = 3840,
    parameter ROW_STRIDE_BYTES  = 5760,
    //   ★ 帧尾停手行数；顶层实际传 720
    parameter FRAME_LINES       = 1080,
    parameter EXTERNAL_FRAME_COMMIT = 0
)(
    input  wire                         clk,
    input  wire                         clk_HDMI_x1,
    input  wire                         rst_n_wr,

    //------------------------------------------------------------
    // 显示通道(来自 GPU2D_APBHandler, 在 GPU_2D 里翻译好)
    //------------------------------------------------------------
    input  wire                         lineBuffer_enable,      // 生效的使能
    input  wire                         lineBuffer_mode,        // 0=全屏 1=分屏(帧边界锁存)
    input  wire [2*AXI_ADDR_WIDTH-1:0]  lineBuffer_addr,        // {段0基址, 段1基址}

    // 给 Handler/加速器: 写域单拍, "新的一帧现在开始"
    output wire                         lineBuffer_frame_start,
    input  wire                         lineBuffer_frame_commit,

    //------------------------------------------------------------
    // HDMI
    //------------------------------------------------------------
    output  wire    [9:0]               tmds_blue_o,
    output  wire    [9:0]               tmds_green_o,
    output  wire    [9:0]               tmds_red_o,
    output  wire    [9:0]               tmds_clk_o,

    output  wire                        tmds_data0_TX_OE,
    output  wire                        tmds_data1_TX_OE,
    output  wire                        tmds_data2_TX_OE,
    output  wire                        tmds_clk_TX_OE,
	output  wire                        tmds_data0_TX_RST,
	output  wire                        tmds_data1_TX_RST,
	output  wire                        tmds_data2_TX_RST,
	output  wire                        tmds_clk_TX_RST,

    //------------------------------------------------------------
    // AXI4 主口(接 Axi_Mux 的 S1)
    //------------------------------------------------------------
    output wire [AXI_ID_WIDTH-1:0]      m_AXI_AWID,
    output wire [AXI_ADDR_WIDTH-1:0]    m_AXI_AWADDR,
    output wire [AXI_LEN_WIDTH-1:0]     m_AXI_AWLEN,
    output wire [2:0]                   m_AXI_AWSIZE,
    output wire [1:0]                   m_AXI_AWBURST,
    output wire                         m_AXI_AWLOCK,
    output wire [3:0]                   m_AXI_AWCACHE,
    output wire [2:0]                   m_AXI_AWPROT,
    output wire [3:0]                   m_AXI_AWQOS,
    output wire [3:0]                   m_AXI_AWREGION,
    output wire                         m_AXI_AWVALID,
    input  wire                         m_AXI_AWREADY,

    output wire [AXI_DATA_WIDTH-1:0]    m_AXI_WDATA,
    output wire [AXI_STRB_WIDTH-1:0]    m_AXI_WSTRB,
    output wire                         m_AXI_WLAST,
    output wire                         m_AXI_WVALID,
    input  wire                         m_AXI_WREADY,

    input  wire [AXI_ID_WIDTH-1:0]      m_AXI_BID,
    input  wire [1:0]                   m_AXI_BRESP,
    input  wire                         m_AXI_BVALID,
    output wire                         m_AXI_BREADY,

    output wire [AXI_ID_WIDTH-1:0]      m_AXI_ARID,
    output wire [AXI_ADDR_WIDTH-1:0]    m_AXI_ARADDR,
    output wire [AXI_LEN_WIDTH-1:0]     m_AXI_ARLEN,
    output wire [2:0]                   m_AXI_ARSIZE,
    output wire [1:0]                   m_AXI_ARBURST,
    output wire                         m_AXI_ARLOCK,
    output wire [3:0]                   m_AXI_ARCACHE,
    output wire [2:0]                   m_AXI_ARPROT,
    output wire [3:0]                   m_AXI_ARQOS,
    output wire [3:0]                   m_AXI_ARREGION,
    output wire                         m_AXI_ARVALID,
    input  wire                         m_AXI_ARREADY,

    input  wire [AXI_ID_WIDTH-1:0]      m_AXI_RID,
    input  wire [AXI_DATA_WIDTH-1:0]    m_AXI_RDATA,
    input  wire [1:0]                   m_AXI_RRESP,
    input  wire                         m_AXI_RLAST,
    input  wire                         m_AXI_RVALID,
    output wire                         m_AXI_RREADY
);

localparam  aPingPongBuffer_DEPTH   = 512;
// 显示一行可见多少拍（1280 px = 3840B = 240 拍），lineAligner 的输出块长
localparam  DISPLAY_BEATS           = LINE_BYTES/(AXI_DATA_WIDTH/8);
// 全屏取数拍数 = 240 + 1：行首可能不是 16B 对齐(相机平移)，多读一拍给 lineAligner
localparam  FETCH_BYTES             = LINE_BYTES + (AXI_DATA_WIDTH/8);

//==============================================================================
// wire / reg
//==============================================================================
//------------------------------------------------------------------------------
// handler <-> controller
//------------------------------------------------------------------------------


//------------------------------------------------------------------------------
// controller -> lineAligner -> PingPong 写口
//------------------------------------------------------------------------------
wire                                ctrl_wr_en;             // controller 写口(241 拍/行)
wire  [AXI_DATA_WIDTH-1:0]          ctrl_wr_data;
wire                                ctrl_wr_block_done;
wire                                ctrl_wr_block_ready;    // 反压: aligner 能收 + bank 空闲
wire  [3:0]                         ctrl_line_skip;         // 行首字节偏移
wire                                align_in_ready;

// PingPong 写口(来自 lineAligner，固定 240 拍/行) / 读口(给 unpacker)
wire                                buffer_wr_en;
wire  [AXI_DATA_WIDTH-1:0]          buffer_wr_data;
wire                                buffer_wr_block_done;
wire                                buffer_wr_block_ready;
wire                                buffer_rd_en;
wire  [AXI_DATA_WIDTH-1:0]          buffer_rd_data;
wire                                buffer_rd_valid;
wire                                buffer_rd_block_ready;

//------------------------------------------------------------------------------
// 复位
//------------------------------------------------------------------------------
wire                                syn_reset;
wire                                rd_rst_n;                   // 读域同步释放后的复位

assign  rd_rst_n = ~syn_reset;

//------------------------------------------------------------------------------
// unpacker / HDMI
//------------------------------------------------------------------------------
wire                                unpacker_rd_req;
wire  [AXI_DATA_WIDTH-1:0]          unpacker_rd_data;
wire                                unpacker_rd_valid;
wire                                unpacker_rd_block_ready;

wire                                pixel_req;
wire  [23:0]                        pixel_data;
wire                                HDMI_data_req_o;
wire                                HDMI_frame_start_o;
wire  [23:0]                        HDMI_data_i;

//------------------------------------------------------------------------------
// AXI4_Master <-> controller
//------------------------------------------------------------------------------
wire                                cmd_valid;
wire                                cmd_ready;
wire                                cmd_write;
wire  [AXI_ADDR_WIDTH-1:0]          cmd_addr;
wire  [AXI_LEN_WIDTH-1:0]           cmd_len;
wire                                cmd_done;
wire                                cmd_err;

wire  [AXI_DATA_WIDTH-1:0]          wr_data;
wire  [AXI_STRB_WIDTH-1:0]          wr_strb;
wire                                wr_valid;
wire                                wr_ready;

wire  [AXI_DATA_WIDTH-1:0]          rd_data;
wire                                rd_valid;
wire                                rd_last;
wire                                rd_ready;

//==============================================================================
// assign 区：模块之间的连线
//==============================================================================

assign  buffer_rd_en                = unpacker_rd_req;
assign  unpacker_rd_data            = buffer_rd_data;
assign  unpacker_rd_valid           = buffer_rd_valid;
assign  unpacker_rd_block_ready     = buffer_rd_block_ready;

assign  pixel_req                   = HDMI_data_req_o;
assign  HDMI_data_i                 = pixel_data;

//==============================================================================
// frame_start 跨域桥
//   读域: HDMI_frame_start_o(6.73ns 宽) -> toggle
//   写域: 2FF + seen -> 单拍事件
//   ★ 不能直接拿读域脉冲过 2FF: 写域 10ns 才采一次, 约 1/3 的帧会被整帧漏掉
//==============================================================================
reg                                 rd_frame_start_flip;        // [读域] 帧起始 toggle: 每来一次翻一次
reg                                 rd_frame_start_flip_d;      // [写域] 同步第 1 级
reg                                 rd_frame_start_flip_dd;     // [写域] 同步第 2 级
reg                                 wr_frame_start_r;        // [写域] 已经消费到的 toggle 值

always @(posedge clk_HDMI_x1 or negedge rd_rst_n) begin
    if (!rd_rst_n)
        rd_frame_start_flip <= 1'b0;
    else if (HDMI_frame_start_o)
        rd_frame_start_flip <= ~rd_frame_start_flip;
end

always @(posedge clk or negedge rst_n_wr) begin
    if (!rst_n_wr) begin
        rd_frame_start_flip_d    <= 1'b0;
        rd_frame_start_flip_dd   <= 1'b0;
        wr_frame_start_r      <= 1'b0;
    end
    else begin
        rd_frame_start_flip_d    <= rd_frame_start_flip;
        rd_frame_start_flip_dd   <= rd_frame_start_flip_d;
        if (rd_frame_start_flip_dd != wr_frame_start_r)
            wr_frame_start_r  <= rd_frame_start_flip_dd;
    end
end

// 组合输出: 两边的 toggle 不一致 = 有一个新的帧起始还没被写域消费 —— 保持高
assign  lineBuffer_frame_start = (rd_frame_start_flip_dd != wr_frame_start_r);

//==============================================================================
// 例化
//==============================================================================
// 行预取控制器: 按 {段0,段1} 把一行的拍数搬进 PingPong
//   ★ 全屏取 241 拍(多 1 拍给 lineAligner), 分屏两段各 120 拍
//==============================================================================
bufferControler # (
    .AXI_ADDR_WIDTH             (AXI_ADDR_WIDTH),
    .AXI_DATA_WIDTH             (AXI_DATA_WIDTH),
    .AXI_LEN_WIDTH              (AXI_LEN_WIDTH),
    .LINE_BYTES                 (FETCH_BYTES),
    .ROW_STRIDE_BYTES           (ROW_STRIDE_BYTES),
    .FRAME_LINES                (FRAME_LINES)
  ) u_bufferControler (
    .clk                        (clk),
    .rst_n                      (rst_n_wr),

    .lineBuffer_enable          (lineBuffer_enable),
    .lineBuffer_mode            (lineBuffer_mode),
    .lineBuffer_addr            (lineBuffer_addr),
    .lineBuffer_frame_start     (EXTERNAL_FRAME_COMMIT ? lineBuffer_frame_commit : lineBuffer_frame_start),
    .line_skip                  (ctrl_line_skip),

    .buffer_wr_en               (ctrl_wr_en),
    .buffer_wr_data             (ctrl_wr_data),
    .buffer_wr_block_done       (ctrl_wr_block_done),
    .buffer_wr_block_ready      (ctrl_wr_block_ready),

    .cmd_valid                  (cmd_valid),
    .cmd_write                  (cmd_write),
    .cmd_addr                   (cmd_addr),
    .cmd_len                    (cmd_len),
    .cmd_ready                  (cmd_ready),
    .cmd_done                   (cmd_done),
    .cmd_err                    (cmd_err),

    .rd_data                    (rd_data),
    .rd_valid                   (rd_valid),
    .rd_last                    (rd_last),
    .rd_ready                   (rd_ready)
  );

//==============================================================================
// 行首字节对齐: 把 controller 的 241 拍削成 240 拍/行
//   全屏 skip = 行首 addr[3:0]; 分屏 skip = 0(纯直通)
//   输出侧仍然是"240 拍/行 + 块尾单拍"，下游 unpacker/HDMI 完全不知道它的存在
//==============================================================================
lineAligner # (
    .DIN_WIDTH                  (AXI_DATA_WIDTH),
    .OUT_BEATS                  (DISPLAY_BEATS)
  ) u_lineAligner (
    .clk                        (clk),
    .rst_n                      (rst_n_wr),
    .enable                     (1'b1),
    .skip                       (ctrl_line_skip),
    .in_valid                   (ctrl_wr_en),
    .in_ready                   (align_in_ready),
    .in_data                    (ctrl_wr_data),
    .in_block_done              (ctrl_wr_block_done),
    .out_valid                  (buffer_wr_en),
    .out_ready                  (buffer_wr_block_ready),
    .out_data                   (buffer_wr_data),
    .out_block_done             (buffer_wr_block_done)
  );

// controller 的"能收": aligner 收得下 且 当前 bank 可写
//   ★ 只给 aligner 的 in_ready 是不够的: 空闲时它恒为 1(out_valid=0),
//     会让 controller 在 bank 还没让出来时就发读命令
assign  ctrl_wr_block_ready = buffer_wr_block_ready & align_in_ready;

//==============================================================================
// 双 bank 跨域缓冲
//==============================================================================
async_PingPongBuffer # (
    .DATA_WIDTH                 (AXI_DATA_WIDTH),
    .DEPTH                      (aPingPongBuffer_DEPTH)
  ) u_async_PingPongBuffer (
    .clk_wr                     (clk),
    .clk_rd                     (clk_HDMI_x1),
    .rst_n                      (rst_n_wr),
    .buffer_wr_en               (buffer_wr_en),
    .buffer_wr_data             (buffer_wr_data),
    .buffer_wr_block_done       (buffer_wr_block_done),
    .buffer_wr_block_ready      (buffer_wr_block_ready),
    .buffer_rd_en               (buffer_rd_en),
    .buffer_rd_data             (buffer_rd_data),
    .buffer_rd_valid            (buffer_rd_valid),
    .buffer_rd_block_ready      (buffer_rd_block_ready),
    .buf_select                 ()
);

//==============================================================================
// 读域复位: 写域复位同步到像素域释放
//==============================================================================
asyn_rst_syn u_rst(
    .rst_n                      (rst_n_wr),
    .clk                        (clk_HDMI_x1),
    .syn_reset                  (syn_reset)
);

//==============================================================================
// 16 字节/拍 -> 3 字节/像素
//==============================================================================
bufferUnpacker # (
    .DIN_WIDTH                  (AXI_DATA_WIDTH),
    .DOUT_WIDTH                 (24)
  ) u_bufferUnpacker (
    .clk                        (clk_HDMI_x1),
    .rst_n                      (rd_rst_n),
    .unpacker_rd_req            (unpacker_rd_req),
    .unpacker_rd_data           (unpacker_rd_data),
    .unpacker_rd_valid          (unpacker_rd_valid),
    .unpacker_rd_block_ready    (unpacker_rd_block_ready),
    .pixel_req                  (pixel_req),
    .pixel_data                 (pixel_data)
);

//==============================================================================
// HDMI 时序 + TMDS
//==============================================================================
HDMI_Top # (
    .HS_POLOR                   (1'b1),
    .VS_POLOR                   (1'b1)
  ) u_HDMI_Top (
    .clk_HDMI_x1                (clk_HDMI_x1),
    .rst_n                      (rd_rst_n),
    .HDMI_data_req_o            (HDMI_data_req_o),
    .HDMI_frame_start_o         (HDMI_frame_start_o),
    .HDMI_data_i                (HDMI_data_i),
    .tmds_blue_o                (tmds_blue_o),
    .tmds_green_o               (tmds_green_o),
    .tmds_red_o                 (tmds_red_o),
    .tmds_clk_o                 (tmds_clk_o),
    .tmds_data0_TX_OE           (tmds_data0_TX_OE),
    .tmds_data1_TX_OE           (tmds_data1_TX_OE),
    .tmds_data2_TX_OE           (tmds_data2_TX_OE),
    .tmds_clk_TX_OE             (tmds_clk_TX_OE),
    .tmds_data0_TX_RST          (tmds_data0_TX_RST),
    .tmds_data1_TX_RST          (tmds_data1_TX_RST),
    .tmds_data2_TX_RST          (tmds_data2_TX_RST),
    .tmds_clk_TX_RST            (tmds_clk_TX_RST)
  );

//==============================================================================
// AXI4 主口
//==============================================================================
AXI4_Master #(
    .ADDR_WIDTH                 (AXI_ADDR_WIDTH),
    .DATA_WIDTH                 (AXI_DATA_WIDTH),
    .STRB_WIDTH                 (AXI_STRB_WIDTH),
    .ID_WIDTH                   (AXI_ID_WIDTH),
    .LEN_WIDTH                  (AXI_LEN_WIDTH),
    .AXI_ID                     (AXI_ID)
) u_AXI4_Master_linebuffer (
    .clk                        (clk),
    .rst_n                      (rst_n_wr),

    .cmd_valid                  (cmd_valid),
    .cmd_ready                  (cmd_ready),
    .cmd_write                  (cmd_write),
    .cmd_addr                   (cmd_addr),
    .cmd_len                    (cmd_len),
    .cmd_done                   (cmd_done),
    .cmd_err                    (cmd_err),

    .wr_data                    (wr_data),
    .wr_strb                    (wr_strb),
    .wr_valid                   (wr_valid),
    .wr_ready                   (wr_ready),

    .rd_data                    (rd_data),
    .rd_valid                   (rd_valid),
    .rd_last                    (rd_last),
    .rd_ready                   (rd_ready),

    .m_AXI_AWID                 (m_AXI_AWID),
    .m_AXI_AWADDR               (m_AXI_AWADDR),
    .m_AXI_AWLEN                (m_AXI_AWLEN),
    .m_AXI_AWSIZE               (m_AXI_AWSIZE),
    .m_AXI_AWBURST              (m_AXI_AWBURST),
    .m_AXI_AWLOCK               (m_AXI_AWLOCK),
    .m_AXI_AWCACHE              (m_AXI_AWCACHE),
    .m_AXI_AWPROT               (m_AXI_AWPROT),
    .m_AXI_AWQOS                (m_AXI_AWQOS),
    .m_AXI_AWREGION             (m_AXI_AWREGION),
    .m_AXI_AWVALID              (m_AXI_AWVALID),
    .m_AXI_AWREADY              (m_AXI_AWREADY),

    .m_AXI_WDATA                (m_AXI_WDATA),
    .m_AXI_WSTRB                (m_AXI_WSTRB),
    .m_AXI_WLAST                (m_AXI_WLAST),
    .m_AXI_WVALID               (m_AXI_WVALID),
    .m_AXI_WREADY               (m_AXI_WREADY),

    .m_AXI_BID                  (m_AXI_BID),
    .m_AXI_BRESP                (m_AXI_BRESP),
    .m_AXI_BVALID               (m_AXI_BVALID),
    .m_AXI_BREADY               (m_AXI_BREADY),

    .m_AXI_ARID                 (m_AXI_ARID),
    .m_AXI_ARADDR               (m_AXI_ARADDR),
    .m_AXI_ARLEN                (m_AXI_ARLEN),
    .m_AXI_ARSIZE               (m_AXI_ARSIZE),
    .m_AXI_ARBURST              (m_AXI_ARBURST),
    .m_AXI_ARLOCK               (m_AXI_ARLOCK),
    .m_AXI_ARCACHE              (m_AXI_ARCACHE),
    .m_AXI_ARPROT               (m_AXI_ARPROT),
    .m_AXI_ARQOS                (m_AXI_ARQOS),
    .m_AXI_ARREGION             (m_AXI_ARREGION),
    .m_AXI_ARVALID              (m_AXI_ARVALID),
    .m_AXI_ARREADY              (m_AXI_ARREADY),

    .m_AXI_RID                  (m_AXI_RID),
    .m_AXI_RDATA                (m_AXI_RDATA),
    .m_AXI_RRESP                (m_AXI_RRESP),
    .m_AXI_RLAST                (m_AXI_RLAST),
    .m_AXI_RVALID               (m_AXI_RVALID),
    .m_AXI_RREADY               (m_AXI_RREADY)
);

endmodule
