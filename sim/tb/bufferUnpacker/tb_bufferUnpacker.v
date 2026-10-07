////////////////////////////////////////////////////////////////////////////////
// tb_bufferUnpacker.v —— 读路径端到端（后级接真 HDMI_Top）
//
//  链路
//    TB 写侧灌数 ─► async_PingPongBuffer ─► bufferUnpacker ─► HDMI_Top
//                    （真模块，真翻 bank）     （真模块）       （真 pixel_timing
//                                                               + 真 TMDS 编码）
//  TB 不自己造行时序：pixel_req 由 HDMI_Top 的 HDMI_data_req_o 驱动，
//  采样点直接取编码器真正取数那一拍（u_hdmi 里的 pixel_de_o）。
//
//  验什么
//    1) 128bit 字节流切成 24bit 像素对不对（跨拍拼接、5/6 拍弹一拍）
//    2) 一块（360 拍 = 一行）交完、读侧翻 bank 之后，下一行接得上、不串行
//    3) 1080p 真实时序下不欠载：每个像素在编码器取数那一拍逐字节比对
//    4) 契约：pixel_de_o 必须等于 HDMI_data_req_o 延后一拍
//
//  数据模型
//    字节流：第 n 个字节 = n[7:0]，跨行连续递增（起 0）
//    期望像素：第 k 个 = {B[3k+2], B[3k+1], B[3k]}
//
//  节拍
//    写域 clk_wr 100MHz ：一行 360 拍，末拍同拍拉 buffer_wr_block_done
//    读域 clk_rd 148.5MHz：由 pixel_timing 产生 1920x1080 时序
//
//  编译运行（ModelSim）
//    vlog .../PingPongBuffer/BlockRAM.v
//    vlog .../PingPongBuffer/async_fifo.v
//    vlog .../PingPongBuffer/async_PingPongBuffer.v
//    vlog .../bufferUnpacker.v
//    vlog .../HDMI/asyn_rst_syn.v
//    vlog .../HDMI/HDMI_Top.v
//    vlog .../HDMI/Pixel_Timing/pixel_timing.v
//    vlog .../HDMI/TMDS/dvi_encoder_Top.v
//    vlog .../HDMI/TMDS/tmds_encoder.v
//    vlog tb_bufferUnpacker.v
//    vsim -c tb_bufferUnpacker -do "run -all; quit -f"
////////////////////////////////////////////////////////////////////////////////
`timescale 1ns/100ps

module tb_bufferUnpacker;

//==============================================================================
// 参数
//==============================================================================
localparam  DW          = 128;                          // 一拍位宽
localparam  DEPTH       = 512;                          // 单 bank 深度（必须 2^n）
localparam  BLOCK       = 360;                          // 一块拍数 = 一行 = 5760/16
localparam  H_ACTIVE    = 1920;                         // 一行有效像素（1080p）
localparam  CHECK_LINES = 10;                           // 校验多少行有效数据
localparam  CLK_WR_HALF = 5.0;                          // 写时钟 100MHz
localparam  CLK_RD_HALF = 3.367;                        // 像素时钟 148.5MHz
localparam  TIMEOUT     = 5_000_000;                    // 看门狗 ns

//==============================================================================
// 时钟与复位
//   rst_n 是写域复位（和 lineBuffer 一样，PingPongBuffer 拿它当单源复位）
//   读域用 asyn_rst_syn 同步释放，给 unpacker 和 HDMI
//==============================================================================
reg         clk_wr, clk_rd, rst_n;
wire        syn_reset;
wire        rd_rst_n;

initial begin clk_wr = 1'b0; forever #CLK_WR_HALF clk_wr = ~clk_wr; end
initial begin clk_rd = 1'b0; forever #CLK_RD_HALF clk_rd = ~clk_rd; end

initial begin
    rst_n = 1'b0;
    repeat (10) @(posedge clk_wr);
    @(negedge clk_rd);
    rst_n = 1'b1;
end

asyn_rst_syn u_rst_syn (
    .clk                        (clk_rd),
    .rst_n                      (rst_n),
    .syn_reset                  (syn_reset)
);
assign  rd_rst_n = ~syn_reset;

//==============================================================================
// 写侧：一行一行往里灌，末拍同拍交块
//==============================================================================
reg  [8:0]      beat_cnt;                               // 本块已写拍数 0..359
reg  [31:0]     byte_idx;                               // 下一个待写字节的全局编号
reg             wr_run;                                 // 正在灌这一块

wire [DW-1:0]   beat_data;
genvar gi;
generate for (gi = 0; gi < 16; gi = gi + 1) begin : g_wr_byte
    assign beat_data[gi*8 +: 8] = (byte_idx + gi) & 32'hFF;
end endgenerate

wire            buffer_wr_en         = wr_run;
wire [DW-1:0]   buffer_wr_data       = beat_data;
wire            buffer_wr_block_done = wr_run && (beat_cnt == BLOCK-1);
wire            buffer_wr_block_ready;

always @(posedge clk_wr or negedge rst_n) begin
    if (!rst_n) begin
        wr_run   <= 1'b0;
        beat_cnt <= 9'd0;
        byte_idx <= 32'd0;
    end
    else if (!wr_run) begin
        if (buffer_wr_block_ready)                      // 等读侧把 bank 让出来
            wr_run <= 1'b1;
    end
    else begin
        byte_idx <= byte_idx + 32'd16;
        if (beat_cnt == BLOCK-1) begin
            beat_cnt <= 9'd0;
            wr_run   <= 1'b0;                           // 交完一块，等下一次让位
        end
        else begin
            beat_cnt <= beat_cnt + 9'd1;
        end
    end
end

//==============================================================================
// DUT 链：PingPongBuffer → bufferUnpacker → HDMI_Top
//==============================================================================
wire            buffer_rd_en;
wire [DW-1:0]   buffer_rd_data;
wire            buffer_rd_valid;
wire            buffer_rd_block_ready;
wire            buf_select;

wire [23:0]     pixel_data;
wire            HDMI_data_req_o;
wire            HDMI_frame_start_o;

async_PingPongBuffer #(
    .DATA_WIDTH                 (DW),
    .DEPTH                      (DEPTH)
) u_pingpong (
    .clk_wr                     (clk_wr),
    .clk_rd                     (clk_rd),
    .rst_n                      (rst_n),
    .buffer_wr_en               (buffer_wr_en),
    .buffer_wr_data             (buffer_wr_data),
    .buffer_wr_block_done       (buffer_wr_block_done),
    .buffer_wr_block_ready      (buffer_wr_block_ready),
    .buffer_rd_en               (buffer_rd_en),
    .buffer_rd_data             (buffer_rd_data),
    .buffer_rd_valid            (buffer_rd_valid),
    .buffer_rd_block_ready      (buffer_rd_block_ready),
    .buf_select                 (buf_select)
);

bufferUnpacker #(
    .DIN_WIDTH                  (DW),
    .DOUT_WIDTH                 (24)
) u_unpacker (
    .clk                        (clk_rd),
    .rst_n                      (rd_rst_n),
    .unpacker_rd_req            (buffer_rd_en),
    .unpacker_rd_data           (buffer_rd_data),
    .unpacker_rd_valid          (buffer_rd_valid),
    .unpacker_rd_block_ready    (buffer_rd_block_ready),
    .pixel_req                  (HDMI_data_req_o),
    .pixel_data                 (pixel_data)
);

HDMI_Top #(
    .HS_POLOR                   (1'b1),
    .VS_POLOR                   (1'b1)
) u_hdmi (
    .clk_HDMI_x1                (clk_rd),
    .rst_n                      (rd_rst_n),
    .HDMI_data_req_o            (HDMI_data_req_o),
    .HDMI_frame_start_o         (HDMI_frame_start_o),
    .HDMI_data_i                (pixel_data),
    .tmds_blue_o                (),
    .tmds_green_o               (),
    .tmds_red_o                 (),
    .tmds_clk_o                 (),
    .tmds_data0_TX_OE           (),
    .tmds_data1_TX_OE           (),
    .tmds_data2_TX_OE           (),
    .tmds_clk_TX_OE             (),
    .tmds_data0_TX_RST          (),
    .tmds_data1_TX_RST          (),
    .tmds_data2_TX_RST          (),
    .tmds_clk_TX_RST            ()
);

//==============================================================================
// 检查
//   hdmi_de：编码器真正把 HDMI_data_i 取走的那一拍（HDMI_Top 内部 pixel_de_o）
//==============================================================================
wire            hdmi_de = u_hdmi.pixel_de_o;

reg             req_d;                                  // HDMI_data_req_o 延后一拍
integer         pixel_k;                                  // 已取走像素个数
integer         err;                                    // 像素错位个数
integer         de_err;                                 // 契约违例
integer         beats;                                  // 已弹出拍数
integer         flips;                                  // 读侧换 bank 次数
integer         frames;                                 // 看到几次帧起始
reg             buf_sel_d;
reg  [31:0]     b0, b1, b2;
reg  [23:0]     exp_pix;

initial begin
    req_d = 1'b0; pixel_k = 0; err = 0; de_err = 0; beats = 0; flips = 0; frames = 0; buf_sel_d = 1'b1;
end

always @(posedge clk_rd) begin
    if (rd_rst_n) begin
        req_d <= HDMI_data_req_o;

        if (buffer_rd_valid) beats = beats + 1;

        buf_sel_d <= buf_select;
        if (buf_select !== buf_sel_d) flips = flips + 1;

        if (HDMI_frame_start_o) frames = frames + 1;

        // 契约：编码器取数那一拍 = 请求延后一拍
        if (hdmi_de !== req_d) de_err = de_err + 1;

        // 逐字节比对：第 k 个像素 = {B[3k+2], B[3k+1], B[3k]}
        if (hdmi_de) begin
            b0 = 3*pixel_k; b1 = b0 + 1; b2 = b0 + 2;
            exp_pix = {b2[7:0], b1[7:0], b0[7:0]};
            if (pixel_data !== exp_pix) begin
                err = err + 1;
                if (err <= 5)
                    $display("  MISMATCH k=%0d got=%06h exp=%06h @%0t", pixel_k, pixel_data, exp_pix, $time);
            end
            pixel_k = pixel_k + 1;
        end
    end
end

//==============================================================================
// 结束与看门狗
//==============================================================================
initial begin
    #TIMEOUT;
    $display("TIMEOUT @%0t", $time);
    $display("FAIL");
    $finish;
end

initial begin
    wait (pixel_k >= CHECK_LINES*H_ACTIVE);
    #200;
    $display("---- pixels = %0d (expect %0d), mismatch = %0d, de_err = %0d ----",
             pixel_k, CHECK_LINES*H_ACTIVE, err, de_err);
    $display("---- beats popped = %0d, bank flips = %0d, frame_start = %0d ----",
             beats, flips, frames);
    if (err == 0 && de_err == 0 && pixel_k == CHECK_LINES*H_ACTIVE && flips >= CHECK_LINES)
        $display("PASS");
    else
        $display("FAIL");
    $finish;
end

endmodule
