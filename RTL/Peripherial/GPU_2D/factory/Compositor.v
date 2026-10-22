//==============================================================================
//  Compositor.v —— 合成器：多路像素流 → 一路像素流
//------------------------------------------------------------------------------
//  结构（多输入对单输出）
//
//      源①  DDR 读回的一行   ← DDRSource 的像素流（96 位/拍 = 4 个 RGB888 像素）
//      常数色 一行           ← FILL，本模块内部生成（同样 96 位/拍，不占外部端口）
//      源②  调色板素材源     ← 片上 PRAM 索引 → 调色板（PaletteSource）
//      源③  读回目的的一行   ← 以后做 alpha 时再加一路像素流
//                  │
//                  ▼
//            ┌───────────┐
//            │  选路/合成  │  ← 自己按 line_flags 的模式位决定用哪几路、怎么算
//            └───────────┘
//                  │  一路像素流，96 位/拍
//                  ▼
//             DDRPacker（96 位 → 128 位，补齐成 AXI 一拍）→ WriteController
//
//  上一级是谁
//      上一层是 factory/Factory.v（它把本模块 + DDRPacker + WriteController 装成"加工面"）；
//      行任务来自 cmd/CmdExec.v 的广播，像素流来自 pixel/ 下的两路源。
//
//  本模块的两类接口（★ 它是"接数据输入 + 接 cmd"的那一级）
//      cmd 口   ：行任务广播里它要的那几根（line_start / line_flags / line_param）
//      数据口   ：每一路源一组（src0_* …），外加每路一根 free 汇总成 line_buf_free
//      ★ 合成器是【唯一】把多路数据口收成一路的地方，下游（Packer/Writer）都只看见一路
//
//  为什么 FILL 也归进来
//      否则写侧要"二选一"—— 两条数据通路、两个主人，多路分选逻辑会绕晕。
//      归进来以后: 写侧只有一条数据入口, 彻底退化成"纯写入器";
//      以后加 alpha / 多图层, 只在合成器里加源和加算法, 写侧一行不用动。
//
//  为什么 bank 不在这里
//      bank 的唯一理由是吸收 DDR 读的突发与空隙，而"谁去读 DDR"是各源自己的事；
//      同理每路源的数据【格式也归它自己】（帧缓存读回是 RGB888，调色板译码是 ARGB）。
//      合成器只管"几路已经对齐好、格式统一的像素流怎么合"，自己不存数据。
//      ⇒ 加一路源 = 加一个 pixel/ 模块 + 加一路输入端口，本模块只多一行选路。
//
//  为什么这里不管"本行几拍"
//      ★ 拍数（16 字节一拍）只由 WriteController 算一份：它按行几何决定"本行取几拍"。
//        合成器只管一路一路地合；多读回来的那点尾巴由 DDRPacker 在行边界丢掉。
//        同一件事只算一遍，就不会出现两个模块各算一套、边界对不上的情况。
//      ★ 合成器自己是【无状态】的（只有行任务那几个锁存），所以它对发送节奏
//        没有任何要求 —— 要表态 free 的是"有缓冲的那几级"（各源 + 写侧）。
//
//  常数色的相位：本模块【完全不管】，也不需要管（★ 为什么）
//      本模块只保证一件事：这一拍 4 个像素按 B,G,R 原样铺，流的第 0 个字节
//      就是本行第 0 个像素的 B。
//      "流的第 0 个字节落在目的块的哪个字节上"是【打包级】的事：
//      DDRPacker 行首把字节计数预置成 dst_off（见它文件头）⇒ 流的第 0 个字节
//      正好落在目的块的第 0 个字节上。整段是【整数个字节】的平移，
//      所以像素边界跟着一起对，dst_off 是不是 3 的倍数都无所谓。
//      ★ 反过来说：本模块【不许】再按 dst_off mod 3 去轮转颜色 ——
//        那是老约定（打包级还没接这件事的时候）留下的双份补偿，会把 R/B 转反。
//
//  调色板以后插在哪
//      插在源的像素流出口和本模块之间（流水级：1 字节索引 → 1 像素），
//      而且必须在【相位对齐之后】—— 对齐按目的字节（3 字节/像素）算，
//      索引数据是 1 字节/像素，展开之前根本没法对齐。
//
//  参考: docs/spec/硬件_加速器.md §4.1 / §4.2
//==============================================================================
`timescale 1ns/100ps

module Compositor #(
    parameter PIXEL_WIDTH = 96,                       // 一路像素流一拍 = 4 个 RGB888 像素
    parameter ALPHA_WIDTH = 8                         // 每个像素的 α 位宽（调色板出来的）
)(
    input  wire                                     clk,
    input  wire                                     rst_n,

    //==================== cmd 口：行任务（CmdExec 广播）  ====================
    //   行任务的字段只在 line_start 那一拍有效 → 本模块自己锁存要用的
    input  wire                                     line_start,     //(I) 单拍脉冲：这一行的任务在这拍有效
    input  wire [7:0]                               line_flags,     //(I) [2:0] = 合成模式，[4] = 颜色键控(ck) 使能
    input  wire                                     line_done,      //(I) 单拍脉冲：本行的写侧已经完成（写控制器给的）
    input  wire [23:0]                              line_param,     //(I) FILL 的填充色
    input  wire [31:0]                              line_src_addr_last, //(I) 最后一格那格的源地址；纯色层时它就是颜色 {A,R,G,B}  <- CmdExec
    input  wire                                     line_fx_en,     //(I) 本命令有没有第二层（合成）  <- CmdExec
    //   ★ 图片合成那一路要的三根（2026-10 加：合成与 ck 都在本模块里做）
    input  wire                                     line_image_next,     //(I) 单拍：源② 那条流的"图一到此为止"
    //   ★ line_flags / 目的地址 / 行宽本模块都用不着：前者是给合成算法留的
    //     （alpha 的混合系数），后两者是写侧的几何，合成器不需要知道

    //==================== 数据口：源①（DDR 读回的一行）  ← DDRSource  ====================
    input  wire [PIXEL_WIDTH-1:0]                   src0_data,      //(I) 一拍 4 个像素  ← DDRSource.pixel_rd_data
    input  wire                                     src0_valid,     //(I) 这一拍有效    ← DDRSource.pixel_rd_valid
    output wire                                     src0_rd_en,     //(O) 取走一拍      → DDRSource.pixel_rd_en

    //==================== 数据口：源②（调色板素材源，片上 PRAM）  ← PaletteSource  ====================
    //   出口契约和源①一模一样 ⇒ 只是多一组口，选路逻辑多一行（见 assign 区）
    input  wire [PIXEL_WIDTH-1:0]                   src1_data,      //(I) 一拍 4 个像素  ← PaletteSource.palette_data
    input  wire [PIXEL_WIDTH/8-1:0]                 src1_mask,      //(I) 字节掩码（1=写）← PaletteSource.palette_mask
    input  wire [(PIXEL_WIDTH/24)*ALPHA_WIDTH-1:0]  src1_alpha,     //(I) 4 个像素的 α    ← PaletteSource.palette_alpha
    input  wire                                     src1_valid,     //(I) 这一拍有效    ← PaletteSource.palette_valid
    output wire                                     src1_rd_en,     //(O) 取走一拍      → PaletteSource.palette_ready

    //==================== 各源"我能接下一行" → 汇总成 line_buf_free ====================
    input  wire                                     line_src0_free, //(I) 源① 能接下一行 ← DDRSource.line_src0_free
    input  wire                                     line_src1_free, //(I) 源② 能接下一行 ← PaletteSource.line_src1_free
    output wire                                     line_buf_free,  //(O) 几路源都还能接 → Factory（再汇总成 line_buffer_free）

    //==================== 数据口：合成后的一路（→ DDRPacker）====================
    output wire [PIXEL_WIDTH-1:0]                   pixel_data,     //(O) 一拍 4 个像素  → DDRPacker（像素流入口）
    output wire [PIXEL_WIDTH/8-1:0]                 pixel_mask,     //(O) 字节掩码（1=写）→ DDRPacker
    output wire                                     pixel_valid,    //(O) 这一拍有效    → DDRPacker
    input  wire                                     pixel_ready     //(I) 下游收下了    ← DDRPacker
);
//==============================================================================
// localparam / 函数
//==============================================================================
localparam  PIXEL_BYTES     = PIXEL_WIDTH/8;                       // 一拍几字节（12）
localparam  PIXELS          = PIXEL_WIDTH/24;                      // 一拍几个像素（4）
localparam  BYTES_PER_PIXEL = 3;                                   // RGB888

localparam  MODE_FILL       = 3'd0;
localparam  MODE_REPLACE    = 3'd1;
localparam  MODE_ALPHA      = 3'd2;
localparam  MODE_CKEY       = 3'd3;
localparam  MODE_TINT_ALPHA = 3'd4;   // 纯色层 tint + alphaBlending

function [1:0] src_en_of;
    input [2:0]         mode;
    input               pal_src;    // flags[3]：这次要的是调色板源
    begin
        case (mode)
            MODE_FILL       : src_en_of = 2'b00;
            MODE_REPLACE    : src_en_of = pal_src ? 2'b10 : 2'b01;
            MODE_ALPHA      : src_en_of = 2'b11;
            MODE_CKEY       : src_en_of = 2'b10;   // ★ 纯色层（第 2 层 flags[2:0]=3）：只要源②，上层由合成级造
                                                    //   ★★【不要改成 2'b11】：mode 3 同时也是所有 CK 精灵的模式，
                                                    //      改成两路会让每个精灵都多读回一次目的行 —— 帧内写不完
                                                    //      ⇒ 画面撕裂/抽搐（2026-10-20 踩过，别再来一次）
            MODE_TINT_ALPHA : src_en_of = 2'b11;   // ★ 纯色层 tint + AB：源① 背景 + 源② base
                                                    //   ★ 新 mode，不复用 mode 3，CK 精灵完全不受影响
            default         : src_en_of = 2'b00;
        endcase
    end
endfunction

//==============================================================================
// wire：子模块各一组（名字用子模块自己的端口名）
//==============================================================================
// ---- imageCombine inst：源② 那条流（先图一、后图二）先过它 ----
wire [PIXEL_WIDTH-1:0]              imgCombine_data;
wire [PIXEL_BYTES-1:0]              imgCombine_mask;       // 合成后整张图的字节掩码
wire [PIXELS*ALPHA_WIDTH-1:0]       imgCombine_alpha;
wire                                imgCombine_valid;
wire                                imgCombine_ready;      // 本模块取它一拍（它的 imgCombine_ready）
wire                                imgCombine_srcReady;   // 它回给素材源的取数（它的 source_ready）
wire                                line_imgRow_free;      // 合成级缓冲空（能不能接下一行）

// ---- alphaBlending inst：前景 = 合成结果，背景 = 源① ----
wire [PIXEL_WIDTH-1:0]              blend_data;
wire [PIXEL_BYTES-1:0]              blend_mask;

//==============================================================================
// wire / reg：本模块内部
//==============================================================================
// ---- 行上下文（line_start 那一拍采一次）----
reg  [7:0]                          flags_r;        // 本行的 flags（模式 + ck）—— 行任务那拍锁存
reg  [1:0]                          src_en_r;       // 这一行要哪几路源（★ 从 flags_r 自己解出来的）
reg  [23:0]                         color_r;        // FILL 的填充色
reg  [31:0]                         fx_color_r;     // 纯色层颜色 {A,R,G,B}（line_src_addr 第 2 格）
wire                                fx_solid_en;    // ★ 1 = 上面那层是纯色（flags[2:0]=3 且 tint 开了）
wire [PIXEL_WIDTH-1:0]              fx_solid_data;  // 纯色层颜色（4 个像素同色）
wire [PIXELS*ALPHA_WIDTH-1:0]       fx_solid_alpha; // 纯色层浓度（= strength）
reg  [2:0]                          fx_tone_r;      // 色调模式 line_param[23:21]（0 = 关）
reg  [7:0]                          fx_strength_r;  // 强度     line_param[19:12]
reg                                 fx_over_r;      // 1 = 只上层在上（默认）；bit20=0 时为 1
reg                                 row_done_r;     // 本行写侧已完成（line_done 锁一拍用）

// ---- 选路 ----
wire [PIXEL_WIDTH-1:0]              fill_data;      // 常数色源的"一拍 4 个像素"
wire                                src_used;       // 本行要不要源（任一路）
wire                                sel_src1;       // 本行选的是源②（调色板）
wire                                alpha_mode;     // 两路同时用 ⇒ 走混合

//==============================================================================
// assign：子模块 ↔ 本模块的连线（★ 一行一条；左边永远是【被驱动】的那一侧）
//==============================================================================
//   ★ 本模块取前景一拍 = 合成级的 imgCombine_ready；理由见下面"取数门控"那段
assign  imgCombine_ready    = src_en_r[1] & ((pixel_ready & pixel_valid) | row_done_r);
//   ★ 合成级回给素材源的取数（它内部就是 source_ready）
assign  src1_rd_en          = imgCombine_srcReady;

//==============================================================================
// assign：源的选路（本模块自己解模式位）
//------------------------------------------------------------------------------
//   权威表在 docs/spec/硬件_加速器.md §1.3；同一份判断在 DDRSource（要 bit0）
//   和 PaletteSource（要 bit1）里各有一份 —— 【改表要一起改这三处】。
//       0 = FILL    ：不要源，出常数色
//       1 = REPLACE ：按 flags[3] 选一路（0=DDR 源① / 1=PRAM 源②）
//       2 = ALPHA   ：两路同时用 ⇒ 11，源② 是前景(带 α)，源① 是背景(读回的目的)
//       3 = CKEY    ：解码同 2（这一档没用，留给颜色键控当合成模式）
//       4 = TINT_ALPHA：纯色层 tint + 两路混合（源① 背景 + 源② base）
//       5~7 = 未定义：不要源，宁可少画
//
//   ★★ 常数色【不做】相位轮转（2026-10 删，踩过两次）：拍对齐已经下沉进 DDRPacker
//      （行首 cnt_byte <= dst_off），流的第 0 个字节本来就落在目的块的第 0 个字节上，
//      再按 dst_off mod 3 轮转一次反而把 R/B 转反（症状：方块颜色随位置变、擦除留残影）。
//==============================================================================
assign  fill_data           = {4{color_r}};
assign  src_used            = |src_en_r;
assign  sel_src1            = src_en_r[1];
assign  alpha_mode          = src_en_r[1] & src_en_r[0];            // 11 ⇒ 混合模式

//   ★ 纯色层：第 2 层 flags[2:0]=3（不读素材）或 4（tint+AB）且开了色调模式 ⇒ 上层 = 常量色 + 浓度
assign  fx_solid_en    = line_fx_en & ((flags_r[2:0] == 3'd3) ||
                                       (flags_r[2:0] == 3'd4)) & (fx_tone_r != 3'd0);
assign  fx_solid_data  = {PIXELS{fx_color_r[23:0]}};
assign  fx_solid_alpha = {PIXELS{fx_strength_r}};

assign  pixel_data          = alpha_mode      ? blend_data          // 混合：前景 ⊕ 背景
                            : src_en_r[1]     ? imgCombine_data     // 源②（单图 = 那一张；合成 = 合成结果）
                            : src_en_r[0]     ? src0_data           // 源①
                            :                   fill_data;
//   混合模式下两路都要有货才算一拍有效
assign  pixel_valid         = alpha_mode      ? (imgCombine_valid & src0_valid)
                            : src_en_r[1]     ? imgCombine_valid
                            : src_en_r[0]     ? src0_valid
                            :                   1'b1;               // 常数色要多少有多少

//   ---- 掩码：和数据同一条选路 ----
//     ① 混合模式：α=0 的字节不写（blend_mask），再和前景（源②）给的掩码相与
//     ② 只有源②：用源给的掩码（ck 判在源里 / 合成级按 line_colorKey_en 一处 gate）
//     ③ 其余（FILL / BLIT）：全 1（每个字节都写）
assign  pixel_mask          = alpha_mode
                            ? (blend_mask & (src_en_r[1] ? imgCombine_mask : {PIXEL_BYTES{1'b1}}))
                            : (src_en_r[1] ? imgCombine_mask : {PIXEL_BYTES{1'b1}});

//==============================================================================
// ★★ 取数门控：什么时候才把源手上那一拍取走（两路同时用时踩过，上板卡死）
//------------------------------------------------------------------------------
//   下游【确实收下】这一拍的条件 = pixel_valid & pixel_ready —— 打包器就是这么算的。
//   ⇒ pop 必须用【同一个条件】。少一路都不能 pop：
//      alpha 时 PRAM 源 2 拍就到、DDR 背景还在等 AXI 读回来 —— 中间这几拍
//      如果"只要下游想收就 pop"，先到的那一路就被取走但没人要地丢掉了 ⇒ 整条链卡死。
//   + row_done_r：本行写侧完成之后，源手上剩的都是写侧不要的尾巴 ⇒ 各取各的丢掉，
//     好让 line_src0_free / line_src1_free 拉高（谁多出一拍谁的 free 就永远不回来）。
//     ★ line_start 清 row_done_r；line_done 是单拍脉冲，所以锁一下。
//==============================================================================
assign  src0_rd_en          = src_en_r[0] & ((pixel_ready & pixel_valid) | row_done_r);

//==============================================================================
// assign：各源 free 汇总（→ CmdExec 的发送节奏）
//------------------------------------------------------------------------------
//   ★ 只认【本行选中的那一路】：没被选中的源在等自己的行任务，本来就 free。
//   ★ 两路同时用（alpha）时两路都得能接下一行。
//   ★ 前景那一路要【两方都点头】：素材源（两趟都排空）+ 合成级（行缓冲空）；
//     单图 / FILL 命令合成级是旁路的 ⇒ line_imgRow_free 恒 1 ⇒ 和以前一样。
//==============================================================================
assign  line_buf_free       = ~src_used
                            | (alpha_mode ? (line_src0_free & line_src1_free & line_imgRow_free)
                                          : (sel_src1 ? (line_src1_free & line_imgRow_free) : line_src0_free));

//==============================================================================
// 时序逻辑：只在行开始采一次上下文
//==============================================================================
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        flags_r    <= 7'd0;
        src_en_r   <= 2'd0;
        color_r    <= 24'd0;
        fx_color_r    <= 32'd0;
        fx_tone_r     <= 3'd0;
        fx_strength_r <= 8'd0;
        fx_over_r     <= 1'b1;
        row_done_r <= 1'b0;
    end
    else if (line_start) begin
        flags_r    <= line_flags;
        src_en_r   <= src_en_of(line_flags[2:0], line_flags[3]);   // ★ 自己解
        color_r    <= line_param;
        fx_color_r    <= line_src_addr_last;
        fx_tone_r     <= line_param[23:21];
        fx_strength_r <= line_param[19:12];
        fx_over_r     <= ~line_param[20];   // bit=0(默认) = 特效层在上 = 老行为
        row_done_r <= 1'b0;                     // 新的一行：写完了重新算
    end
    else if (line_done) begin
        row_done_r <= 1'b1;                     // 单拍脉冲 ⇒ 锁住给取数门控用
    end
end

//==============================================================================
// 例化：多路进一路出（顺序 = 源② 的流先合成，再和源① 混合）
//==============================================================================
imageCombine #(
    .PIXEL_WIDTH                        (PIXEL_WIDTH        ),
    .PIXEL_BYTES                        (PIXEL_BYTES        ),
    .PIXELS                             (PIXELS             ),
    .ALPHA_WIDTH                        (ALPHA_WIDTH        ),
    .ROW_PIXELS                         (1920               )
) u_imageCombine (
    .clk                                (clk                ),
    .rst_n                              (rst_n              ),
    .line_start                         (line_start         ),
    .line_solid_en                      (fx_solid_en        ),   // ★ 纯色层
    .line_solid_data                    (fx_solid_data      ),
    .line_solid_alpha                   (fx_solid_alpha     ),
    .line_tint                          (fx_tone_r          ),   // ★ 色调模式 0~4
    .line_fx_en                         (line_fx_en         ),
    .line_colorKey_en                   (flags_r[4]         ),   // ★ 命令级 ck 开关（本行锁存的 flags）
    .line_fx_over                       (fx_over_r          ),   // ★ 上下方向（1 = 上层在上，默认）
    .line_image_next                    (line_image_next    ),
    .line_src_free                      (line_src1_free     ),
    .source_data                        (src1_data          ),
    .source_mask                        (src1_mask          ),
    .source_alpha                       (src1_alpha         ),
    .source_valid                       (src1_valid         ),
    .source_ready                       (imgCombine_srcReady),
    .imgCombine_data                    (imgCombine_data    ),
    .imgCombine_mask                    (imgCombine_mask    ),
    .imgCombine_alpha                   (imgCombine_alpha   ),
    .imgCombine_valid                   (imgCombine_valid   ),
    .imgCombine_ready                   (imgCombine_ready   ),
    .line_imgRow_free                   (line_imgRow_free   )
);

alphaBlending #(
    .PIXEL_WIDTH                        (PIXEL_WIDTH        ),
    .PIXELS                             (PIXELS             ),
    .BYTES_PER_PIXEL                    (BYTES_PER_PIXEL    ),
    .ALPHA_WIDTH                        (ALPHA_WIDTH        )
) u_alphaBlending (
    .fg_data                            (imgCombine_data    ),   // 前景 = 合成结果（单图时 = 源② 那张图）
    .fg_alpha                           (imgCombine_alpha   ),   // 它的 α（旁路，不并进像素流）
    .bg_data                            (src0_data          ),   // 背景 = 源①（DDR 读回的目的像素）
    .blend_data                         (blend_data         ),
    .blend_mask                         (blend_mask         )
);

endmodule
