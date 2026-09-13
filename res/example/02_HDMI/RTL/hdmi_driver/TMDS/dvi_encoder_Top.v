module dvi_encoder_Top(
    input   wire            clk_dvi_x1,             //pixel clock
    input   wire            rst_n,                  //rst_n
    
    // video signals in
    input   wire    [23:0]  dvi_video_Data_i,       //RGB888 video in
    input   wire            dvi_video_hsync_i,      //hsync data
    input   wire            dvi_video_vsync_i,      //vsync data
    input   wire            dvi_video_de_i,         //data enable
    
    // TMDS signals
    output  wire    [9:0]   dvi_tmds_blue_o,        // 蓝色通道(带 hs/vs 控制码)
    output  wire    [9:0]   dvi_tmds_green_o,       // 绿
    output  wire    [9:0]   dvi_tmds_red_o,         // 红
    output  wire    [9:0]   dvi_tmds_clk_o          // 时钟通道 = 10'b1111100000
    );

assign dvi_tmds_clk_o    = 10'b1111100000;

//------------------------------------------------------------------------------
// step1
// 对三个颜色通道进行编码
//------------------------------------------------------------------------------

tmds_encoder encoder_b(
    .clkin              (clk_dvi_x1),
    .rstin              (!rst_n),
    
    .din                (dvi_video_Data_i[7:0]),
    .c0                 (dvi_video_hsync_i),
    .c1                 (dvi_video_vsync_i),
    .de                 (dvi_video_de_i),
    .dout               (dvi_tmds_blue_o)
);
    
tmds_encoder encoder_g(
    .clkin              (clk_dvi_x1),
    .rstin              (!rst_n),
    
    .din                (dvi_video_Data_i[15:8]),
    .c0                 (1'b0),
    .c1                 (1'b0),
    .de                 (dvi_video_de_i),
    .dout               (dvi_tmds_green_o)
);
    
tmds_encoder encoder_r(
    .clkin              (clk_dvi_x1),
    .rstin              (!rst_n),
    
    .din                (dvi_video_Data_i[23:16]),
    .c0                 (1'b0),
    .c1                 (1'b0),
    .de                 (dvi_video_de_i),
    .dout               (dvi_tmds_red_o)
);


// 串行数据
// 易灵思有专门的并转串硬核 故删去

endmodule
