//============================================================================
// HDMI_onBoard_Top.v —— HDMI 上板彩条测试顶层（从零建工程用，不复用官方 demo）
//
// 显示内容：8 竖彩条（EBU 顺序：白/黄/青/绿/品红/红/蓝/黑）
// 像素源：内部彩条发生器，按 HDMI_data_req_o(=de_w) 提前 1 拍供数，
//         数据寄存一拍后与 pixel_de_o 同拍进入编码器（对齐契约见 HDMI_Top.v）
//
// 对接 Interface Designer（新建 peri 时端口名照此填）：
//   - clk_HDMI_x1 ：像素时钟 1×（peri PLL slow，1080p ≈148.75MHz）
//   - pll_HDMI_locked  ：peri PLL 锁定指示（低 = 未锁 → 复位）
//   - tmds_blue_o / tmds_green_o / tmds_red_o / tmds_clk_o : 4×10bit 并行字
//   - tmds_*_TX_OE / tmds_*_TX_RST : 4 个 LVDS TX 硬核的控制脚
//============================================================================

module HDMI_onBoard_Top(
    input   wire            clk_HDMI_x1,        // 像素时钟（peri PLL slow 输出）
    input   wire            pll_HDMI_locked,         // peri PLL 锁定指示

    // ---- 4×10bit 并行 TMDS（到硬核 LVDS TX）----
    output  wire    [9:0]   tmds_blue_o,        // 通道0 = 蓝（带 hs/vs 控制码）
    output  wire    [9:0]   tmds_green_o,       // 通道1 = 绿
    output  wire    [9:0]   tmds_red_o,         // 通道2 = 红
    output  wire    [9:0]   tmds_clk_o,         // 时钟通道

    // ---- 硬核控制脚 ----
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
// 复位（测试顶层临时本地同步；正式接入 SoC 后由 AXI 外设统一提供）
//------------------------------------------------------------------------------
wire            rst_syn;            // 高有效、已同步
wire            rst_n;              // 低有效（分发到子模块）
asyn_rst_syn u_rst(
    .rst_n      (pll_HDMI_locked),       // 未锁 = 复位；锁定 = 释放（同步）
    .clk        (clk_HDMI_x1),
    .syn_reset  (rst_syn)
);
assign  rst_n   = ~rst_syn;

//------------------------------------------------------------------------------
// HDMI 子核
//------------------------------------------------------------------------------
wire            hdmi_data_req_o;    // 数据请求（= de_w，早 1 拍）
wire [23:0]     hdmi_data_i;        // 像素输入（与 de 同拍）

wire [9:0]      enc_tmds_blue_o;
wire [9:0]      enc_tmds_green_o;
wire [9:0]      enc_tmds_red_o;
wire [9:0]      enc_tmds_clk_o;

HDMI_Top #(
    .HS_POLOR   (1'b1),             // 1080p VESA 正极性
    .VS_POLOR   (1'b1)
) u_HDMI_Top(
    .clk_HDMI_x1        (clk_HDMI_x1),
    .rst_n              (rst_n),
    .HDMI_data_req_o    (hdmi_data_req_o),
    .HDMI_data_i        (hdmi_data_i),
    .tmds_blue_o        (enc_tmds_blue_o),
    .tmds_green_o       (enc_tmds_green_o),
    .tmds_red_o         (enc_tmds_red_o),
    .tmds_clk_o         (enc_tmds_clk_o),
    .tmds_data0_TX_OE   (tmds_data0_TX_OE),
    .tmds_data1_TX_OE   (tmds_data1_TX_OE),
    .tmds_data2_TX_OE   (tmds_data2_TX_OE),
    .tmds_clk_TX_OE     (tmds_clk_TX_OE),
    .tmds_data0_TX_RST  (tmds_data0_TX_RST),
    .tmds_data1_TX_RST  (tmds_data1_TX_RST),
    .tmds_data2_TX_RST  (tmds_data2_TX_RST),
    .tmds_clk_TX_RST    (tmds_clk_TX_RST)
);

assign  tmds_blue_o     = enc_tmds_blue_o;
assign  tmds_green_o    = enc_tmds_green_o;
assign  tmds_red_o      = enc_tmds_red_o;
assign  tmds_clk_o      = enc_tmds_clk_o;

//------------------------------------------------------------------------------
// 彩条像素源：8 竖彩条（每 240 列一色，EBU 顺序）
//   对齐：req 拉高那拍寄存"当前列颜色" → 下一拍数据 = pixel_de_o 高 → 同拍进编码器
//------------------------------------------------------------------------------
reg  [12:0]  bar_col;        // 当前列 0..H_ACTIVE-1
reg  [23:0]  bar_rgb;        // RGB888 输出

always @(posedge clk_HDMI_x1 or negedge rst_n) begin
    if (!rst_n) begin
        bar_col <= 13'd0;
        bar_rgb <= 24'h0;
    end
    else if (hdmi_data_req_o) begin
        bar_col <= bar_col + 13'd1;
        bar_rgb <= bar_color(bar_col);      // 用当前列算色，寄存一拍送出
    end
    else begin
        bar_col <= 13'd0;                   // 行消隐：清零等下一行
        bar_rgb <= 24'h0;
    end
end

assign hdmi_data_i = bar_rgb;

function [23:0] bar_color;
    input [12:0] x;
    begin
        if      (x < 13'd240)  bar_color = 24'hFFFFFF; // 白
        else if (x < 13'd480)  bar_color = 24'hFFFF00; // 黄
        else if (x < 13'd720)  bar_color = 24'h00FFFF; // 青
        else if (x < 13'd960)  bar_color = 24'h00FF00; // 绿
        else if (x < 13'd1200) bar_color = 24'hFF00FF; // 品红
        else if (x < 13'd1440) bar_color = 24'hFF0000; // 红
        else if (x < 13'd1680) bar_color = 24'h0000FF; // 蓝
        else                   bar_color = 24'h000000; // 黑
    end
endfunction

endmodule
