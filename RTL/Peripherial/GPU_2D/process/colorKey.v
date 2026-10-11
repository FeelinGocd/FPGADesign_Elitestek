//==============================================================================
//  colorKey.v —— 颜色键控(color key / ck) 比较器：一拍像素 → 每像素 3 位字节掩码
//------------------------------------------------------------------------------
//  干什么
//      “等于键色的像素 = 透明 = 不写”。
//      一拍 4 个像素逐个和键色比：
//          命中   → 这 3 位清 0（写侧 WSTRB 对应位 = 0 ⇒ 那几个字节不落盘）
//          不命中 → 3 位全 1（照写）
//      掩码按【字节】给（每像素 3 位）：3 字节/像素、16 字节/AXI 拍，3 不整除 16
//      ⇒ 一个像素会横跨两拍。按字节给掩码，跨拍自动就对。
//
//  为什么在这一级比（★ 不是随手放的位置）
//      "比颜色"必须【按像素】比：一拍 12 字节 = 4 个整像素，24 位一组正好一个像素。
//      而本模块的输入是查表输出，天然就是【按像素对齐的 24 位组】——
//      下游（Compositor / DDRPacker / WriteController）手上是按字节流动的流
//      （还要摆进 16 字节 AXI 拍），在那里按 24 位切会横跨像素 ⇒ 比错。
//      ⇒ 掩码在【源】这一级、按像素生成好，再跟着颜色一起往下走；
//        下游只是把掩码和颜色【用同一套移位/掩码兜】搬下去，所以永远不会错位。
//
//  纯组合
//      没有 clk、不插流水级、不碰任何握手 —— 对整条通路只加一个比较器 + 一个与门。
//      它和 lookup_data 天然同拍，不用去对 PaletteLUT 的流水延迟。
//
//  谁例化它：pixel/palette/PaletteDecoder.v（u_colorKey）
//  掩码往下走：PaletteDecoder 直通 → Compositor 选路(flags[4]) → DDRPacker 掩码兜
//              → WriteController:  wr_strb = head_mask & tail_mask & ck_mask
//
//  键色（key_color）
//      来自 APB 寄存器：显示子块 0x2078 CKEY_COLOR，复位默认 0xFFFFFF(白)。
//      软件（lineBuffer_setCkeyColor）不写它时就是白 —— 配套约定见 pic2pram.py：
//      透明像素统一落成调色板第 0 项 = ffffff。
////  上一级是谁
//      pixel/palette/PaletteDecoder.v（逐像素比键色，生成字节掩码）

//==============================================================================
`timescale 1ns/100ps

module colorKey #(
    parameter PIXEL_WIDTH       = 96,               // 一拍像素流（4 个 RGB888 像素）
    parameter PIXELS            = 4,                // 一拍几个像素
    parameter COLOR_W           = 24,               // 一个像素多少位
    parameter BYTES_PER_PIXEL   = 3                 // RGB888 ⇒ 3 字节/像素
)(
    input  wire [COLOR_W-1:0]                   key_color,  //(I) 键色（APB 0x2078，复位默认白）
    input  wire [PIXEL_WIDTH-1:0]                     pixel_data,   //(I) 一拍像素（已按像素对齐）
    output wire [PIXELS*BYTES_PER_PIXEL-1:0]    pixel_mask    //(O) 每像素 3 位：命中=000，否则=111
);

//==============================================================================
// 逐像素比较（generate：以后加像素 = 改 PIXELS 一个数，别处不用动）
//==============================================================================
genvar gi;
generate
    for (gi = 0; gi < PIXELS; gi = gi + 1) begin : g_pix
        wire [COLOR_W-1:0] slot = pixel_data[gi*COLOR_W +: COLOR_W];

        //   命中键色 ⇒ 这 3 个字节都不写；否则照写
        assign pixel_mask[gi*BYTES_PER_PIXEL +: BYTES_PER_PIXEL] =
               (slot == key_color) ? {BYTES_PER_PIXEL{1'b0}}
                                   : {BYTES_PER_PIXEL{1'b1}};
    end
endgenerate

endmodule
