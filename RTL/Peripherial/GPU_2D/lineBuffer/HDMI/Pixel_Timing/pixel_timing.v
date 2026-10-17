`timescale 1ns / 1ps

// 1280x720 game viewport; DDR physical row stride remains 5760 bytes.
`define VIDEO_1280_720
// `define VIDEO_1920_1080
// `define VIDEO_1680_1050
// `define VIDEO_1280_1024
// `define VIDEO_1024_768
// `define VIDEO_800_600
// `define VIDEO_640_480

module pixel_timing #(
    // 极性：1 = 正极性(高有效脉冲)，0 = 负极性
    parameter           HS_POLOR   = 1'b0,
    parameter           VS_POLOR   = 1'b0
)(
    input  wire         clk_pixel_x1,           // 像素时钟（下面按照宏有对应的设定时钟频率）
    input  wire         rst_n,                  // 复位，低有效

    // 输出（全部寄存一级，互相同拍）
    output wire         pixel_hsync_o,          // 行同步（已按 HS_POLOR 输出）
    output wire         pixel_vsync_o,          // 场同步（已按 VS_POLOR 输出）
    output wire         pixel_de_o,             // 数据有效（= 可见区）
    output wire [12:0]  pixel_x_o,              // 可见区像素列坐标 0..H_ACTIVE-1（o_de=1 时有效）
    output wire [12:0]  pixel_y_o,              // 可见区像素行坐标 0..V_ACTIVE-1（o_de=1 时有效）
    output wire         pixel_frame_start_o,    // 帧起始脉冲：( h_cnt , v_cnt ) = （0,0）
    output wire         pixel_req_o             // 外部数据请求信号 拉高的下一个时钟要输入有效数据
);

//1920x1080 148.5Mhz
`ifdef  VIDEO_1920_1080
parameter  H_ACTIVE 		= 1920;// 行数据有效时间
parameter  H_FRONT 	        = 88;  // 行消隐前肩时间
parameter  H_SYNC 		    = 44;  // 行同步信号时间
parameter  H_BACK 	        = 148; // 行消隐后肩时间 
                                   
parameter  V_ACTIVE 		= 1080;// 列数据有效时间
parameter  V_FRONT 	        = 4;   // 列消隐前肩时间
parameter  V_SYNC  	        = 5;   // 列同步信号时间
parameter  V_BACK 	        = 36;  // 列消隐后肩时间
`endif

//1680x1050 119Mhz
`ifdef  VIDEO_1680_1050
parameter  H_ACTIVE 		= 1680;// 行数据有效时间
parameter  H_FRONT 	        = 48;  // 行消隐前肩时间
parameter  H_SYNC 		    = 32;  // 行同步信号时间
parameter  H_BACK 	        = 80;  // 行消隐后肩时间
                                   
parameter  V_ACTIVE 		= 1050;// 列数据有效时间
parameter  V_FRONT 	        = 3;   // 列消隐前肩时间
parameter  V_SYNC  	        = 6;   // 列同步信号时间
parameter  V_BACK 	        = 21;  // 列消隐后肩时间
`endif

//1280x1024 108Mhz
`ifdef  VIDEO_1280_1024
parameter  H_ACTIVE 		= 1280;// 行数据有效时间
parameter  H_FRONT 	        = 48;  // 行消隐前肩时间
parameter  H_SYNC 		    = 112; // 行同步信号时间
parameter  H_BACK 	        = 248; // 行消隐后肩时间 
                                   
parameter  V_ACTIVE 		= 1024;// 列数据有效时间
parameter  V_FRONT 	        = 1;   // 列消隐前肩时间
parameter  V_SYNC  	        = 3;   // 列同步信号时间
parameter  V_BACK 	        = 38;  // 列消隐后肩时间
`endif

//1280X720 74.25MHZ
`ifdef  VIDEO_1280_720
parameter  H_ACTIVE 		= 1280;// 行数据有效时间
parameter  H_FRONT 	        = 110; // 行消隐前肩时间
parameter  H_SYNC 		    = 40;  // 行同步信号时间
parameter  H_BACK 	        = 220; // 行消隐后肩时间    
                                    
parameter  V_ACTIVE 		= 720; // 列数据有效时间
parameter  V_FRONT 	        = 5;   // 列消隐前肩时间
parameter  V_SYNC  	        = 5;   // 列同步信号时间
parameter  V_BACK 	        = 20;  // 列消隐后肩时间
`endif

//1024x768 65Mhz
`ifdef  VIDEO_1024_768
parameter  H_ACTIVE 		= 1024;// 行数据有效时间
parameter  H_FRONT 	        = 24;  // 行消隐前肩时间
parameter  H_SYNC 		    = 136; // 行同步信号时间
parameter  H_BACK 	        = 160; // 行消隐后肩时间   
                                    
parameter  V_ACTIVE 		= 768; // 列数据有效时间
parameter  V_FRONT 	        = 3;   // 列消隐前肩时间
parameter  V_SYNC  	        = 6;   // 列同步信号时间
parameter  V_BACK 	        = 29;  // 列消隐后肩时间
`endif

//800x600 40Mhz
`ifdef  VIDEO_800_600
parameter  H_ACTIVE 		= 800;// 行数据有效时间
parameter  H_FRONT 	        = 40 ;// 行消隐前肩时间 
parameter  H_SYNC 		    = 128;// 行同步信号时间
parameter  H_BACK 	        = 88 ;// 行消隐后肩时间    
                                    
parameter  V_ACTIVE 		= 600;// 列数据有效时间
parameter  V_FRONT 	        = 1  ;// 列消隐前肩时间  
parameter  V_SYNC  	        = 4  ;// 列同步信号时间  
parameter  V_BACK 	        = 23 ;// 列消隐后肩时间 
`endif

//640x480 25.175Mhz
`ifdef  VIDEO_640_480
parameter H_ACTIVE 			= 640; // 行数据有效时间
parameter H_FRONT 	        = 16 ; // 行消隐前肩时间
parameter H_SYNC 		    = 96 ; // 行同步信号时间
parameter H_BACK 		    = 48 ; // 行消隐后肩时间
								 
parameter V_ACTIVE 			= 480; // 列数据有效时间
parameter V_FRONT 	        = 10 ; // 列消隐前肩时间
parameter V_SYNC 		    = 2	 ; // 列同步信号时间
parameter V_BACK 		    = 33 ; // 列消隐后肩时间
`endif

//============================================================================
// 内部常量
//============================================================================
// 整个像素时序长度
localparam [12:0] H_TOTAL   = H_ACTIVE + H_FRONT + H_SYNC + H_BACK; // 行长
localparam [12:0] V_TOTAL   = V_ACTIVE + V_FRONT + V_SYNC + V_BACK; // 帧高
// 像素可见区起点
localparam [12:0] H_ACT_ST  = H_SYNC + H_BACK;   // 每行可见起点计数
localparam [12:0] V_ACT_ST  = V_SYNC + V_BACK;   // 每帧可见起点行号

// h/v counter inst
reg  [12:0]         h_cnt;
reg  [12:0]         v_cnt;

// 边界条件信号
wire                h_sync_zone;
wire                h_active_zone;
wire                v_sync_zone;
wire                v_active_zone;

// 输出信号
wire                de_w;
wire                hs_w;
wire                vs_w;
wire [12:0]         x_w;
wire [12:0]         y_w;
wire                frame_start_w;

reg                 hs_r;
reg                 vs_r;
reg                 de_r;
reg                 frame_start_r;
reg [12:0]          x_r;
reg [12:0]          y_r;

// 外部req
assign  pixel_req_o = de_w;

//============================================================================
// 边界条件信号设置
//============================================================================
// h_cnt ∈ [0, H_SYNC)                  ：行同步脉冲
// h_cnt ∈ [H_ACT_ST, H_ACT_ST+H_ACTIVE)：行可见区
assign  h_sync_zone     = (h_cnt <  H_SYNC);
assign  h_active_zone   = (h_cnt >= H_ACT_ST) && (h_cnt < H_ACT_ST + H_ACTIVE);
// v_cnt ∈ [0, V_SYNC)                  ：场同步脉冲（整行）
// v_cnt ∈ [V_ACT_ST, V_ACT_ST+V_ACTIVE)：帧可见区
assign  v_sync_zone     = (v_cnt <  V_SYNC);
assign  v_active_zone   = (v_cnt >= V_ACT_ST) && (v_cnt < V_ACT_ST + V_ACTIVE);

//============================================================================
// 输出信号
//============================================================================
assign  de_w                = h_active_zone && v_active_zone;               // 有效像素
assign  hs_w                = h_sync_zone ? HS_POLOR : ~HS_POLOR;           // 根据设置极性变化
assign  vs_w                = v_sync_zone ? VS_POLOR : ~VS_POLOR;           // 根据设置极性变化
assign  x_w                 = h_active_zone ? (h_cnt - H_ACT_ST) : 13'd0;   // 可见列坐标
assign  y_w                 = v_active_zone ? (v_cnt - V_ACT_ST) : 13'd0;   // 可见行号
assign  frame_start_w       = (v_cnt == 13'd0) && (h_cnt == 13'd0);         // 帧起始脉冲

assign  pixel_hsync_o       = hs_r;
assign  pixel_vsync_o       = vs_r;
assign  pixel_de_o          = de_r;
assign  pixel_x_o           = x_r;
assign  pixel_y_o           = y_r;
assign  pixel_frame_start_o = frame_start_r;

//============================================================================
// 行/场计数器（0..TOTAL-1）
//============================================================================
always @(posedge clk_pixel_x1 or negedge rst_n) begin
    if (!rst_n) begin
        h_cnt <= 13'd0;
        v_cnt <= 13'd0;
    end
    else if (h_cnt == H_TOTAL - 13'd1) begin   // 行满，换行
        h_cnt <= 13'd0;
        v_cnt <= (v_cnt == V_TOTAL - 13'd1) ? 13'd0 : v_cnt + 13'd1;
    end
    else begin
        h_cnt <= h_cnt + 13'd1;
    end
end

//============================================================================
// 输出寄存一拍（hs/vs/de/x/y/frame_start 同一拍更新，互相对齐）
//============================================================================

always @(posedge clk_pixel_x1 or negedge rst_n) begin
    if (!rst_n) begin
        hs_r          <= 1'b0;
        vs_r          <= 1'b0;
        de_r          <= 1'b0;
        x_r           <= 13'd0;
        y_r           <= 13'd0;
        frame_start_r <= 1'b0;
    end
    else begin
        hs_r          <= hs_w;
        vs_r          <= vs_w;
        de_r          <= de_w;
        x_r           <= x_w;
        y_r           <= y_w;
        frame_start_r <= frame_start_w;
    end
end

endmodule
