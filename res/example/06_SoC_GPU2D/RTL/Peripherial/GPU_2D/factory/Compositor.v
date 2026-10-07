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
//  本模块的两类接口（★ 它是"接数据输入 + 接 cmd"的那一级）
//      cmd 口   ：行任务广播里它要的那几根（line_start / line_flags / line_param）
//      数据口   ：每一路源一组（src0_* …），外加每路一根 free 汇总成 buf_free
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
    input  wire                     clk,
    input  wire                     rst_n,

    //==================== cmd 口：行任务（CmdExec 广播）====================
    //   行任务的字段只在 line_start 那一拍有效 → 本模块自己锁存要用的
    input  wire                     line_start,     //(I) 单拍脉冲：这一行的任务在这拍有效
    input  wire [6:0]               line_flags,     //(I) [2:0] = 合成模式，[4] = 颜色键控(ck) 使能
    input  wire                     line_done,      //(I) 单拍脉冲：本行的写侧已经完成（写控制器给的）
    input  wire [23:0]              line_param,     //(I) FILL 的填充色
    //   ★ line_flags / 目的地址 / 行宽本模块都用不着：前者是给合成算法留的
    //     （alpha 的混合系数），后两者是写侧的几何，合成器不需要知道

    //==================== 数据口：源①（DDR 读回的一行）====================
    input  wire [PIXEL_WIDTH-1:0]   src0_data,      //(I) 一拍 4 个像素，已对齐到目的字节 0
    input  wire                     src0_valid,     //(I) 这一拍有效
    output wire                     src0_rd_en,     //(O) 取走一拍（本行不用源① 时恒 0）

    //==================== 数据口：源②（调色板素材源，片上 PRAM）====================
    //   出口契约和源①一模一样 ⇒ 只是多一组口，选路逻辑多一行（见 assign 区）
    input  wire [PIXEL_WIDTH-1:0]   src1_data,      //(I) 一拍 4 个像素（同样已对齐）
    input  wire [PIXEL_WIDTH/8-1:0] src1_mask,      //(I) 这一拍的字节掩码（ck 用；1=写）
    input  wire [(PIXEL_WIDTH/24)*ALPHA_WIDTH-1:0] src1_alpha,   //(I) 源② 那 4 个像素的 α（旁路来的，alpha 混合用）
    input  wire                     src1_valid,     //(I) 这一拍有效
    output wire                     src1_rd_en,     //(O) 取走一拍（本行不用源② 时恒 0）

    //==================== 各源"我能接下一行" → 汇总成 buf_free ====================
    input  wire                     src0_free,      //(I) 源① 能接下一行
    input  wire                     src1_free,      //(I) 源② 能接下一行
    output wire                     buf_free,       //(O) 本命令要的几路源都还能接（→ CmdExec）

    //==================== 数据口：合成后的一路（→ DDRPacker）====================
    output wire [PIXEL_WIDTH-1:0]   pixel_data,       //(O) 一拍 4 个像素
    output wire [PIXEL_WIDTH/8-1:0] pixel_mask,       //(O) 这一拍的字节掩码（→ DDRPacker）
    output wire                     pixel_valid,      //(O) 这一拍有效
    input  wire                     pixel_ready       //(I) 下游收下了
);

//==============================================================================
// wire / reg
//==============================================================================
// ---- 行上下文（line_start 那一拍采一次）----
reg  [6:0]              flags_r;                        // 本行的 flags（模式 + ck）—— 行任务那拍锁存
reg  [1:0]              src_en_r;                       // 这一行要哪几路源（★ 从 flags_r 自己解出来的）
reg  [23:0]             color_r;                        // FILL 的填充色
reg                     ck_en_r;                        // 本行开不开 ck（行任务那拍锁存）
reg                     row_done_r;                     // 本行写侧已完成（line_done 锁一拍用）


// ---- 选路 ----
wire [PIXEL_WIDTH-1:0]  fill_data;                      // 常数色源的"一拍 4 个像素"
wire                    src_used;                       // 本行要不要源（任一路）
wire                    sel_src1;                       // 本行选的是源②（调色板）
wire                    alpha_mode;                     // 两路同时用 ⇒ 走混合

// ---- 混合器的输出（只在 alpha_mode 那几拍有意义）----
wire [PIXEL_WIDTH-1:0]  ab_data;
wire [PIXEL_WIDTH/8-1:0] ab_mask;

//==============================================================================
// assign 区：源的选路与合成
//==============================================================================
//   ★★ 常数色【不再】做相位轮转（2026-10 改，踩过两次，写清楚免得再走回去）
//
//     历史：常数色原先按【拍网格】铺 —— 写侧的字节流从目的 16 字节对齐基址开始，
//     而目的块从拍内第 dst_off 个字节才开始，所以块内第 0 个字节拿到的是颜色字节
//     (dst_off mod 3)，只有 dst_off mod 3 == 0 时才是 B。当时的办法是【把颜色按
//     dst_off mod 3 轮转一下】去迁就那个网格（目的：让块内第 0 个字节 = B）。
//     ★ 旧实测：x=300（dst_off=4）绿块画出来是蓝的；x=100（dst_off=12）刚好对。
//
//     现在："拍对齐"已经下沉进 DDRPacker（行首 cnt_byte <= dst_off，见它文件头），
//     流的第 0 个字节本来就落在【目的块的第 0 个字节】上 ⇒ 常数色按 B,G,R 原样铺即可，
//     再转一次反而转错（R/B 互换）—— 实测症状：方块颜色随位置变、擦除后留彩色残影。
//     ⇒ 本模块【不看目的地址的相位】。
//     ★ 教训：这条错只在 dst_off mod 3 != 0 时露出来；而且只看"两遍一致 + 掩码"的
//       TB 查不出绝对颜色 —— 现在 Factory_ck_tb 里有 chk_fill_abs：
//       dst_off 0~15 全扫、逐字节比绝对颜色。
//   （mod3 已删：常数色不再做相位轮转，见上面那段说明）

//==============================================================================
// ★ 合成模式（flags[2:0]）→ 本行取哪几路源
//------------------------------------------------------------------------------
//   这是本模块【自己的】解码：CmdExec 只广播 flags，谁要不要动由各人自己判。
//   权威表在 docs/spec/硬件_加速器.md §1.3；同一份判断在 DDRSource（要 bit0）
//   和 PaletteSource（要 bit1）里各有一份 —— 【改表要一起改这三处】。
//       0 = FILL    ：不要源，出常数色
//       1 = REPLACE ：按 flags[3] 选一路（0=DDR 源① / 1=PRAM 源②）
//       2 = ALPHA   ：两路同时用 ⇒ 11，源② 是前景(带 α)，源① 是背景(读回的目的)
//       3 = CKEY    ：解码同 2（这一档没用，留给颜色键控当合成模式）
//       4~7 = 未定义：不要源，宁可少画
//==============================================================================
localparam  MODE_FILL    = 3'd0;
localparam  MODE_REPLACE = 3'd1;
localparam  MODE_ALPHA   = 3'd2;
localparam  MODE_CKEY    = 3'd3;
function [1:0] src_en_of;
    input [2:0] mode;
    input       pal_src;                            // flags[3]：这次要的是调色板源
    begin
        case (mode)
            MODE_FILL    : src_en_of = 2'b00;
            MODE_REPLACE : src_en_of = pal_src ? 2'b10 : 2'b01;
            MODE_ALPHA   : src_en_of = 2'b11;
            MODE_CKEY    : src_en_of = 2'b11;
            default      : src_en_of = 2'b00;
        endcase
    end
endfunction

//   常数色：原样铺（低字节 = B，全工程字节序），一拍 4 个像素同一个颜色。
//   ★ 目的块的第 0 个字节拿到的就是这里的颜色字节 0（= B）—— 拍内偏移由 DDRPacker 负责。
assign  fill_data = {4{color_r}};

//   ★ src_en_r 是上面那个函数解出来的（本模块自己解，不再从 CmdExec 牵线过来）：
//       00 = 不要源（常数色 FILL）
//       01 = 源①：DDR 读回的一行
//       10 = 源②：调色板素材源（片上 PRAM 索引 → 调色板）
//       11 = ★ alpha 混合：**两路同时用** —— 源② 是【前景】(带 α)，
//            源① 是【背景】(它自己按 flags 判断读的是【目的自己】，读回来的就是
//            目的那块现有的像素)，在这一级配对后逐像素混合。
//            (这就是本模块文件头里"源③ 读回目的"预留的那个位置)
assign  src_used    = |src_en_r;
assign  sel_src1    = src_en_r[1];
assign  alpha_mode  = src_en_r[1] & src_en_r[0];            // 11 ⇒ 混合模式

//   ---- 混合器（逐像素、逐通道；α 是"一个像素一个"，所以必须在这里按像素算）----
alphaBlending #(
    .PIXEL_WIDTH        (PIXEL_WIDTH        ),
    .PIXELS             (PIXEL_WIDTH/24     ),
    .BYTES_PER_PIXEL    (3                  ),
    .ALPHA_WIDTH        (ALPHA_WIDTH        )
) u_alphaBlending (
    .fg_data            (src1_data          ),   // 前景 = 源②（调色板精灵）
    .fg_alpha           (src1_alpha         ),   // 它的 α（旁路，不并进像素流）
    .bg_data            (src0_data          ),   // 背景 = 源①（DDR 读回的目的像素）
    .blend_data         (ab_data            ),
    .blend_mask         (ab_mask            )
);

assign  pixel_data  = alpha_mode ? ab_data
                    : src_en_r[1] ? src1_data
                    : src_en_r[0] ? src0_data
                    :               fill_data;
//   混合模式下两路都要有货才算一拍有效
assign  pixel_valid = alpha_mode ? (src1_valid & src0_valid)
                    : src_en_r[1] ? src1_valid
                    : src_en_r[0] ? src0_valid
                    :               1'b1;                    // 常数色要多少有多少

//   ---- 掩码：和数据同一条选路 ----
//     ① 混合模式：α=0 的字节不写（blend_mask），再和 ck 掩码相与（两者可以同时开）
//     ② 只有源② 且开了 ck：用源给的掩码
//     ③ 其余（FILL / BLIT / 没开 ck）：全 1（每个字节都写）
//     ★ 前两种之外和改造前【一位不差】—— 这就是回归的保证。
assign  pixel_mask = alpha_mode
                   ? (ab_mask & (ck_en_r ? src1_mask : {PIXEL_WIDTH/8{1'b1}}))
                   : ((src_en_r[1] & ck_en_r) ? src1_mask : {PIXEL_WIDTH/8{1'b1}});

//==============================================================================
// ★★ 取数门控：什么时候才把源手上那一拍取走（两路同时用时踩过，上板卡死）
//------------------------------------------------------------------------------
//   下游【确实收下】这一拍的条件 = pixel_valid & pixel_ready —— 打包器就是这么算的
//   （它的 in_hs = in_valid & in_ready，而 in_valid 就是这里的 pixel_valid）。
//   ⇒ pop 必须用【同一个条件】。少一路都不能 pop：
//      alpha 时 PRAM 源 2 拍就到、DDR 背景还在等 AXI 读回来 —— 中间这几拍
//      如果照旧"只要下游想收就 pop"，先到的那一路就被【取走但没人要】地丢掉了，
//      打包器一个字节都收不到 ⇒ 写侧永远等 ⇒ line_buffer_free 不拉高 ⇒ 整条链卡死。
//      （单路源时 pixel_ready 恰好等价于"确实收下"，所以两条老路一直没暴露。）
//
//   + row_done_r：本行【写侧已经完成】之后，源手上剩的都是写侧不要的尾巴
//     ⇒ 两路各取各的丢掉，好让 src_free 拉高。
//     没有这一项：谁多出一拍谁的 free 就永远不回来（DDR 源比 PRAM 源多几拍），
//     一样会卡死。
//     ★ line_start 清 row_done_r；line_done 是单拍脉冲，所以锁一下。
//==============================================================================
assign  src0_rd_en = src_en_r[0] & ((pixel_ready & pixel_valid) | row_done_r);
assign  src1_rd_en = src_en_r[1] & ((pixel_ready & pixel_valid) | row_done_r);

//==============================================================================
// assign 区：各源 free 汇总
//==============================================================================
//   ★ 汇总放在这里，CmdExec 的接口就不会随源的个数变长。
//     只认【本行选中的那一路】：没被选中的源在等自己的行任务，本来就 free；
//     按选中来判更干净，以后两路同时跑（alpha）时这里也不会打架。
//   ★ 两路同时用（alpha）时，两路都得能接下一行 —— 有一路没准备好就不能开新行
assign  buf_free = ~src_used
                 | (alpha_mode ? (src0_free & src1_free)
                               : (sel_src1 ? src1_free : src0_free));


//==============================================================================
// 时序逻辑：只在行开始采一次上下文
//==============================================================================
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        flags_r    <= 7'd0;
        src_en_r   <= 2'd0;
        color_r    <= 24'd0;
        ck_en_r    <= 1'b0;
        row_done_r <= 1'b0;

    end
    else if (line_start) begin
        flags_r    <= line_flags;
        src_en_r   <= src_en_of(line_flags[2:0], line_flags[3]);   // ★ 自己解，见下面函数
        color_r    <= line_param;
        ck_en_r    <= line_flags[4];
        row_done_r <= 1'b0;                     // 新的一行："写完了"重新算

    end
    else if (line_done) begin
        row_done_r <= 1'b1;                     // 单拍脉冲 ⇒ 锁住给取数门控用
    end
end

endmodule
