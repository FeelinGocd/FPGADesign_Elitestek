//==============================================================================
//  alphaBlending.v —— 前景 × α + 背景 × (1−α)，一拍 4 个像素、逐通道算
//------------------------------------------------------------------------------
//  干什么
//      收进【同一拍】的两路 4 像素 + 前景 4 个像素的 α，组合出混合结果：
//
//          out = bg + (fg − bg) × α / 255
//
//      ★ 用这个形式而不是 fg×α + bg×(1−α)：**每通道只要 1 个乘法器**
//        （后者要 2 个）。一拍 4 像素 × 3 通道 = 12 个乘法器 ——
//        综合报告里 DSP 只用了 7/160，正好喂它。
//
//  为什么必须在这一级做（和 colorKey 同一个理由）
//      混合是【按像素】的算术：一个像素的 3 个字节共用同一个 α。
//      而下游（DDRPacker / WriteController / AXI）手上是【按字节】的流，
//      "24 位一组"并不等于一个像素。
//      ⇒ 只有"按像素对齐"的那一级能做 —— 也就是这里（Compositor 入口）。
//      （这一条成立的前提，是"补 dst_off 个哑字节"已经下沉到 DDRPacker 了；
//        以前它留在源侧，一拍 12 字节会跨 5 个像素，这里就没法按像素算。
//        见 docs/spec/硬件_加速器.md §4.3）
//
//  α 从哪来（不在这里，也不在命令里）
//      调色板一项从 RGB888 变 ARGB8888，α 是**数据**不是控制：
//      PaletteDecoder 查出 4 个像素的 α 后，用一根【旁路】送过来（4×8 = 32 位），
//      不并进 96 位像素流 —— 那样整条写通路的位宽都要跟着改，没必要。
//
//  α = 0 怎么办
//      那 3 个字节【不写】(等价于现在的颜色键控效果)。本模块把这件事表达成
//      blend_mask 的字节位 0，下游 DDRPacker/WriteController 那条掩码通路
//      是现成的（ck 已经在用），**不新增任何机制**。
//
//  /255 怎么算（★ 不用除法器）
//      q = (|fg−bg| × α + 1 + ((|fg−bg| × α) >> 8)) >> 8
//      这是 (x/255) 在 0 ≤ x < 65536 上的【精确】算法 ⇒
//      **α = 255 时结果逐位等于 fg**（"开 α 且 α=255" 与 REPLACE 完全一致），
//      这条正好当自检判据用。
//
//  字节序（全工程统一）
//      像素流里【低位字节 = B】。本模块按字节独立算，不关心谁是谁，
//      所以字节序天然跟随输入。
//
//  参考：docs/spec/硬件_加速器.md §5.3、process/colorKey.v（同一种"在哪儿做"的论证）
//==============================================================================
`timescale 1ns/100ps

module alphaBlending #(
    parameter PIXEL_WIDTH       = 96,               // 一拍几位的像素数据（4 × RGB888）
    parameter PIXELS            = 4,                // 一拍几个像素
    parameter BYTES_PER_PIXEL   = 3,                // 一个像素几字节（RGB888 = 3）
    parameter ALPHA_WIDTH       = 8                 // 一个像素的 α 几位
)(
    //==================== 前景（源② PRAM 精灵）====================
    input  wire [PIXEL_WIDTH-1:0]               fg_data,    //(I) 一拍 PIXELS 个像素
    input  wire [PIXELS*ALPHA_WIDTH-1:0]        fg_alpha,   //(I) 同上那些像素的 α（旁路来的）

    //==================== 背景（源① DDR 读回的目的像素）====================
    input  wire [PIXEL_WIDTH-1:0]               bg_data,    //(I) 一拍 PIXELS 个像素

    //==================== 结果 ====================
    output wire [PIXEL_WIDTH-1:0]               blend_data, //(O) 混合后的 PIXELS 个像素
    output wire [PIXELS*BYTES_PER_PIXEL-1:0]    blend_mask  //(O) 每字节要不要写（α=0 ⇒ 0）
);

//==============================================================================
// 逐像素、逐通道展开
//==============================================================================
genvar gp, gc;
generate
    for (gp = 0; gp < PIXELS; gp = gp + 1) begin : g_pixel
        //   ---- 这个像素的 α（4 个像素共用同一位宽，各取各的）----
        wire [ALPHA_WIDTH-1:0] alpha = fg_alpha[gp*ALPHA_WIDTH +: ALPHA_WIDTH];

        for (gc = 0; gc < BYTES_PER_PIXEL; gc = gc + 1) begin : g_channel
            wire [7:0] fg = fg_data[(gp*BYTES_PER_PIXEL + gc)*8 +: 8];
            wire [7:0] bg = bg_data[(gp*BYTES_PER_PIXEL + gc)*8 +: 8];

            //   ---- 差值：带符号，范围 −255..255 ----
            wire signed [8:0] diff      = $signed({1'b0, fg}) - $signed({1'b0, bg});
            //   ---- 取绝对值参与乘法（负号最后再补回去）----
            wire        [8:0] diff_abs  = diff[8] ? (~diff + 9'd1) : diff;
            //   ---- 唯一的乘法：9 位 × 8 位 ⇒ 17 位（最大 255×255 = 65025）----
            wire        [16:0] product  = diff_abs * alpha;
            //   ---- ÷255（精确，无除法器）：q = (p + 1 + (p>>8)) >> 8 ----
            wire        [16:0] quotient = (product + 17'd1 + (product >> 8)) >> 8;
            //   ---- 加回背景（带上原来的符号）----
            wire        [8:0]  mixed    = diff[8] ? ({1'b0, bg} - quotient)
                                                  : ({1'b0, bg} + quotient);

            assign blend_data[(gp*BYTES_PER_PIXEL + gc)*8 +: 8] = mixed[7:0];

            //   ★ α = 0 ⇒ 这个字节不写（复用 ck 那条掩码通路，不新增机制）
            assign blend_mask[gp*BYTES_PER_PIXEL + gc] = (alpha != {ALPHA_WIDTH{1'b0}});
        end
    end
endgenerate

endmodule
