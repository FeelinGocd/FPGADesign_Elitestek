// 易灵思要求 HS/VS 极性为 1 即像素无效时为 1
// 分辨率只能手动去 pixel_timing 模块里面修改宏
module HDMI_Top #(
    parameter               HS_POLOR = 1'b1,
    parameter               VS_POLOR = 1'b1
)(
    input   wire            clk_HDMI_x1,
    // input   wire            clk_HDMI_x5,        // x5时钟用于并转串使用 硬核直接接入了 这里不需要
    input   wire            rst_n,
    
    // pixel_Timing output
    output  wire            HDMI_data_req_o,
    output  wire            HDMI_frame_start_o,
    input   wire    [23:0]  HDMI_data_i,
    // TMDS output
    output  wire    [9:0]   tmds_blue_o,
    output  wire    [9:0]   tmds_green_o,
    output  wire    [9:0]   tmds_red_o,
    output  wire    [9:0]   tmds_clk_o,

    // 易灵思 P2S 硬核需要显式给使能和复位
    output  wire            tmds_data0_TX_OE,
    output  wire            tmds_data1_TX_OE,
    output  wire            tmds_data2_TX_OE,
    output  wire            tmds_clk_TX_OE,
	output  wire            tmds_data0_TX_RST,
	output  wire            tmds_data1_TX_RST,
	output  wire            tmds_data2_TX_RST,
	output  wire            tmds_clk_TX_RST
);

//------------------------------------------------------------------------------
// HDMI_Top inst
//------------------------------------------------------------------------------
assign  tmds_data0_TX_OE    = 1'b1;
assign  tmds_data1_TX_OE    = 1'b1;
assign  tmds_data2_TX_OE    = 1'b1;
assign  tmds_clk_TX_OE      = 1'b1;

assign  tmds_data0_TX_RST   = 1'b0;
assign  tmds_data1_TX_RST   = 1'b0;
assign  tmds_data2_TX_RST   = 1'b0;
assign  tmds_clk_TX_RST     = 1'b0;

//------------------------------------------------------------------------------
// pixel_Timing_Top inst
//------------------------------------------------------------------------------
wire                clk_pixel_x1;
wire                pixel_hsync_o;
wire                pixel_vsync_o;
wire                pixel_de_o;
// ref signals
// xy 调试用 实际使用不用管
// wire [12:0]         pixel_x_o;
// wire [12:0]         pixel_y_o;
wire                pixel_frame_start_o;
wire                pixel_req_o;

assign  clk_pixel_x1        = clk_HDMI_x1;
assign  HDMI_frame_start_o  = pixel_frame_start_o;
assign  HDMI_data_req_o     = pixel_req_o;

//------------------------------------------------------------------------------
// dvi_encoder_Top inst
//------------------------------------------------------------------------------
wire                clk_dvi_x1;
wire [23:0]         dvi_video_Data_i;
wire                dvi_video_hsync_i;
wire                dvi_video_vsync_i;
wire                dvi_video_de_i;

wire [9:0]          dvi_tmds_blue_o;
wire [9:0]          dvi_tmds_green_o;
wire [9:0]          dvi_tmds_red_o;
wire [9:0]          dvi_tmds_clk_o;

assign  clk_dvi_x1          = clk_HDMI_x1;
assign  dvi_video_Data_i    = HDMI_data_i;

// 易灵思 硬核极性约定 输出需要取反
assign  tmds_blue_o         = ~dvi_tmds_blue_o;
assign  tmds_green_o        = ~dvi_tmds_green_o;
assign  tmds_red_o          = ~dvi_tmds_red_o;
assign  tmds_clk_o          = ~dvi_tmds_clk_o;

// 像素时序 -> tmds
assign  dvi_video_hsync_i   = pixel_hsync_o;
assign  dvi_video_vsync_i   = pixel_vsync_o;
assign  dvi_video_de_i      = pixel_de_o;

//------------------------------------------------------------------------------
// pixel_Timing 提供像素时钟
//------------------------------------------------------------------------------

pixel_timing # (
    .HS_POLOR               (HS_POLOR),
    .VS_POLOR               (VS_POLOR)
) inst_pixel_timing (
    .clk_pixel_x1           (clk_pixel_x1),
    .rst_n                  (rst_n),
    .pixel_hsync_o          (pixel_hsync_o),
    .pixel_vsync_o          (pixel_vsync_o),
    .pixel_de_o             (pixel_de_o),
    .pixel_x_o              (),
    .pixel_y_o              (),
    .pixel_frame_start_o    (pixel_frame_start_o),
    .pixel_req_o            (pixel_req_o)
);

//------------------------------------------------------------------------------
// dvi 2 HDMI
//------------------------------------------------------------------------------

dvi_encoder_Top  inst_dvi_encoder_Top (
    .clk_dvi_x1             (clk_dvi_x1),
    .rst_n                  (rst_n),
    .dvi_video_Data_i       (dvi_video_Data_i),
    .dvi_video_hsync_i      (dvi_video_hsync_i),
    .dvi_video_vsync_i      (dvi_video_vsync_i),
    .dvi_video_de_i         (dvi_video_de_i),

    .dvi_tmds_blue_o        (dvi_tmds_blue_o),
    .dvi_tmds_green_o       (dvi_tmds_green_o),
    .dvi_tmds_red_o         (dvi_tmds_red_o),
    .dvi_tmds_clk_o         (dvi_tmds_clk_o)
);

endmodule
