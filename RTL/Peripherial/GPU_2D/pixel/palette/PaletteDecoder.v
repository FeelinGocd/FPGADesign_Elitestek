//==============================================================================
//  PaletteDecoder.v —— 索引字 → 写侧要的调色板数据流（查表 + 相位整形）
//------------------------------------------------------------------------------
//  干什么
//      palette/ 这一组的分工：
//          PaletteSource  ：行任务 + 素材 RAM 取数 + 节奏（src_free）——"料从哪来"
//          PaletteDecoder ：本模块 ——"把索引变成写侧要的调色板数据"
//          PaletteLUT     ：索引 → RGB888 的表（4 个读口，带握手）
//      本模块只干一件事：查表（交给 PaletteLUT）。
//
//  本模块【不做相位整形】（★ 2026-10 改，见 docs/spec/硬件_加速器.md §4.3）
//      以前这里有个 36 字节窗口，干的事是"在像素流前面补 dst_off 个字节" ——
//      那是为了对齐写侧的 16 字节 AXI 拍（开头 dst_off = dst_addr[3:0] 个字节会被
//      WSTRB 遮掉）。但那是【打包的事】，不是【源的事】：
//      混在这一级，下游拿到的"一拍 12 字节"就不是整像素了（dst_off 不是 3 的倍数时
//      会横跨 5 个像素），alpha 这种"一个像素一个"的属性根本配不上。
//      现在改成：源侧只保证【像素对齐】（本模块直通 —— LUT 出 4 个像素就是 4 个像素），
//      "补 dst_off 个哑字节"下沉到 DDRPacker（行首把字节计数预置成 dst_off）。
//      ⇒ 合成器（Compositor）拿到的永远是【4 个整像素】。
//
//  行尾怎么办
//      多读回来的那几组本来就是垃圾，由 DDRPacker 在行边界丢掉 ——
//      现有通路已有的约定，不新增机制。
//
//  流水（★ 现在只剩 LUT 自己的那一级）
//      LUT 是【带握手的读口】：lookup_data 和 lookup_valid 同拍产生、且一直保持到
//      下游取走 ⇒ 本模块不用维护任何"差一拍"的标志，出口就是 LUT 的出口。
//
//      ★ 历史：这里曾经有 ① 一级 skid + 两个"入口/中间级有效"标志（兜"LUT 输出只在
//        一拍有效"这个矛盾，根因是当时 PaletteLUT 读使能恒为 1 —— 已封进 PaletteRAM.v）；
//        ② 一个 36 字节相位窗口（补 dst_off 个哑字节，已下沉到 DDRPacker，见文件头）。
//
//  一条不变式（改这里之前先守住）
//      busy 要把 LUT 手上那个字也算上：少报一级，上游会在流水没排空时开新行，
//      把还没吐出去的像素【直接吃掉】。
//
//  水平镜像(MX)为什么也在这里做
//      倒着读字的时候，一个字里的 4 个索引也要【反序】（低位字节 = 最左那个像素）。
//      本模块在入口把 index_data 的 4 个索引按 flags[5] 反过来再交给 PaletteLUT ——
//      只是一个 32 位 mux，不动 LUT 里"读口 → 输出 lane"那条写死的对应。
//      ★ 和相位无关：只是把字内 4 个索引反序，不动 LUT 里"读口 → 输出 lane"的对应。
//
//  颜色键控(ck)为什么在这一级做（★ 不是随手放的位置）
//      "比颜色"必须【按像素】比，而这一级手上正好是 lookup_data：
//      一拍 4 个像素、【按像素对齐】的 24 位组 —— 全工程唯一满足这个条件的地方。
//      掩码在这里生成（调 process/colorKey.v），再跟着颜色一起往下走、按字节落地。
//
//  ★ 字节序（全工程统一）
//      像素流里【低位字节 = B】（帧缓存就是 B,G,R 序，显示侧 dvi_encoder_Top 拿
//      [7:0] 当蓝）。本模块只搬运，不翻字节。
//
//  参考：docs/spec/硬件_加速器.md §4.1（封装修法）、docs/spec/硬件_加速器.md §4.3（拍对齐下沉）//  上一级是谁
//      pixel/palette/PaletteSource.v

//==============================================================================
`timescale 1ns/100ps

module PaletteDecoder #(
    parameter INDEX_WIDTH            = 8,           // 索引位宽（8bpp）
    parameter COLOR_WIDTH            = 24,          // 像素颜色位宽（RGB888）
    parameter PALETTE_ENTRY_WIDTH    = 32,          // 调色板【一项】位宽：AARRGGBB（α 在高字节）
    parameter INDEXES_PER_WORD       = 4,           // 一个索引字里几个索引（= 一拍进几个）
    parameter BYTES_PER_BEAT         = 12,          // 一拍出去几字节（3 × 4 = 12 ⇒ 96 位）
    parameter PALETTE_INIT_FILE      = "palette.hex"    // 调色板初值文件（4 份副本共用）
)(
    //==================== 时钟 / 复位 ====================
    input  wire                                     clk,
    input  wire                                     rst_n,

    //==================== 行边界（line_start 那一拍有效）====================
    input  wire                                     line_start,     //(I) 单拍脉冲：锁行上下文（MX）

    //==================== 颜色键控(ck) 的键色（静态配置，不随行变）====================
    input  wire [COLOR_WIDTH-1:0]                   key_color,      //(I) 键色(APB 0x2078, 复位默认白)
    input  wire [6:0]                               line_flags,     //(I) [5] = MX 水平镜像

    //==================== 索引字入口（接 PaletteSource 的素材 RAM 读口）  ====================
    input  wire [INDEXES_PER_WORD*INDEX_WIDTH-1:0]  index_data,     //(I) 一个字 = 4 个索引
    input  wire                                     index_valid,    //(I) 这个字有效
    output wire                                     index_ready,    //(O) 我还能收（下游没堵住）

    //==================== 调色板数据出口（标准 valid/ready）→ Compositor ====================
    output wire [BYTES_PER_BEAT*8-1:0]              palette_data,   //(O) 12 字节 = 4 个 RGB888
    output wire [BYTES_PER_BEAT-1:0]                palette_mask,   //(O) 12 位字节掩码（颜色键控：1=写）
    output wire [INDEXES_PER_WORD*8-1:0]            palette_alpha,  //(O) 这 4 个像素的 α（8 位/像素，旁路给合成器）
    output wire                                     palette_valid,  //(O) 手上有货（保持到 palette_ready）
    input  wire                                     palette_ready,  //(I) 下游收下了
    output wire                                     busy            //(O) 流水里还有货没吐完
);

//==============================================================================
// 参数换算
//==============================================================================
localparam  MASK_BYTES          = 3;                        // 每个像素几字节
localparam  LOOKUP_MASK_WIDTH   = INDEXES_PER_WORD * MASK_BYTES;   // 一组索引展开成几位掩码（12）

//==============================================================================
// wire / reg
//==============================================================================
//   ---- 查表出口（带握手：一直保持到被下游取走）----
//   ★ LUT 一项是 AARRGGBB（32 位）⇒ 出口 128 位。这里把它拆成两半：
//     ① 96 位 RGB 像素流（下游老通路【一个字节都不用改】）
//     ② 32 位 α 旁路（只给合成器；不并进像素流，否则整条写通路位宽都得动）
wire [INDEXES_PER_WORD*PALETTE_ENTRY_WIDTH-1:0] lut_data;    // LUT 吐出的 4 项 AARRGGBB
wire [INDEXES_PER_WORD*COLOR_WIDTH-1:0]     lookup_data;    // 拆出来的 4 个 RGB888
wire [INDEXES_PER_WORD*8-1:0]               lookup_alpha;   // 拆出来的 4 个 α（8 位/像素）
wire                                        lookup_valid;   // 与 lookup_data 同拍
wire [LOOKUP_MASK_WIDTH-1:0]                lookup_mask;    // ★ 这 4 个像素的透明标志（3 位/像素）
wire [INDEXES_PER_WORD*INDEX_WIDTH-1:0]     index_data_sw;  // ★ MX 时字内 4 个索引反序后的字

//   ---- 水平镜像(MX)的行上下文 ----
reg                                         mx_r;           // 本行要不要把字内 4 个索引反序

//==============================================================================
// 组合输出（★ 直通：LUT 出来什么就是什么，不再做任何相位整形）
//==============================================================================
assign  palette_data    = lookup_data;
assign  palette_mask    = lookup_mask;
assign  palette_valid   = lookup_valid;

//   ★ busy 给上游看：本行最后一个像素字还在 LUT 出口时就还不许开新行
//     （上游自己那拍由素材 RAM 读口的 read_valid 兜住 —— 它要等 Decoder 收下才放手。）
assign  busy            = lookup_valid;

//==============================================================================
// 例化
//==============================================================================

//==============================================================================
// 水平镜像(MX)：字内 4 个索引反序
//==============================================================================
//   一个字 = 4 个索引，【低位字节 = 最左边那个像素】。
//   倒着读字时，第一个字要【从最高的那个索引开始】，所以 lane 顺序要反过来。
//   ★ 不写这条的表现：图整体位置对，但每 4 个像素一组的内部顺序是错的（细竖条纹错位）。
//   ★ 为什么在这做而不是改 PaletteLUT：LUT 里"读口 → 输出 lane"的对应是写死的
//     （4 份 RAM 副本共用一张表），动它影响面大；这里只是一个 32 位 mux。
always @(posedge clk or negedge rst_n) begin
    if (!rst_n)
        mx_r <= 1'b0;
    else if (line_start)
        mx_r <= line_flags[5];
end

genvar si;
generate
    for (si = 0; si < INDEXES_PER_WORD; si = si + 1) begin : g_mx
        assign index_data_sw[si*INDEX_WIDTH +: INDEX_WIDTH] =
               mx_r ? index_data[(INDEXES_PER_WORD-1-si)*INDEX_WIDTH +: INDEX_WIDTH]
                    : index_data[si*INDEX_WIDTH +: INDEX_WIDTH];
    end
endgenerate

//==============================================================================
// 拆 LUT 的一项：低 24 位 = RGB（进像素流），高 8 位 = α（走旁路）
//   ★ 一项的字节序是 AARRGGBB ⇒ 低 24 位就是原来的 RRGGBB，
//     所以"低位字节 = B"这条全工程约定一点没变。
//   ★ 4 个 lane 的对应是写死的：第 i 项 → 第 i 个像素。（和 PaletteLUT 里一致）
//==============================================================================
genvar sl;
generate
    for (sl = 0; sl < INDEXES_PER_WORD; sl = sl + 1) begin : g_split
        assign lookup_data [sl*COLOR_WIDTH +: COLOR_WIDTH] =
               lut_data[sl*PALETTE_ENTRY_WIDTH +: COLOR_WIDTH];
        assign lookup_alpha[sl*8 +: 8] =
               lut_data[sl*PALETTE_ENTRY_WIDTH + COLOR_WIDTH +: 8];
    end
endgenerate

//   α 直接出到模块口（和 palette_data 同拍、同一个握手）
assign  palette_alpha = lookup_alpha;

//==============================================================================
// 颜色键控(color key)：像素 → 字节掩码
//   ★ 实现在 process/colorKey.v —— 那里写了"为什么必须在这一级比颜色"。
//==============================================================================
//   本模块只负责：把它接在【查表出口】上（那里是按像素对齐的 24 位组）。
//   ★ 输入必须是【按像素对齐】的 24 位组 —— lookup_data 正好是，所以掩码和
//     lookup_data 【天然同拍】，不用去对 PaletteLUT 的流水延迟。
//   ★ 键色来自 APB 寄存器（显示子块 0x2078，复位默认白 0xFFFFFF），
//     从顶层一路接进来。软件不写它时，行为和"写死白"完全一样。
colorKey #(
    .PIXEL_WIDTH        (BYTES_PER_BEAT * 8),   // 96 = 一拍 12 字节
    .PIXELS             (INDEXES_PER_WORD),     // 4 = 一拍出几个像素
    .COLOR_W            (COLOR_WIDTH),          // 24
    .BYTES_PER_PIXEL    (MASK_BYTES)            // 3
) u_colorKey (
    .key_color          (key_color),
    .pixel_data           (lookup_data),
    .pixel_mask           (lookup_mask)
);

//------------------------------------------------------------------------------
// 查表：索引字 → 4 个像素（★ 出口带握手：lookup_data 一直保持到下游取走）
//------------------------------------------------------------------------------
PaletteLUT #(
    .INDEX_WIDTH        (INDEX_WIDTH       ),
    .COLOR_WIDTH        (PALETTE_ENTRY_WIDTH),   // ★ 一项 32 位 = AARRGGBB
    .INDEXES_PER_WORD   (INDEXES_PER_WORD  ),
    .INIT_FILE          (PALETTE_INIT_FILE )
) u_PaletteLUT (
    .clk                (clk),
    .rst_n              (rst_n),
    .clear              (line_start),           // 行边界：丢掉上一行残留的那个像素字
    .index_data         (index_data_sw),        // ★ MX 时字内 4 个索引已反序
    .index_valid        (index_valid),
    .index_ready        (index_ready),          // 直接给上游：下游能收就说明收得下
    .palette_data       (lut_data),             // 128 位 = 4 项 AARRGGBB
    .palette_valid      (lookup_valid),
    .palette_ready      (palette_ready)         // 下游取走 ⇒ 把这 4 个像素交出去
);

endmodule
