///////////////////////////////////////////////////////////////////////////////////
//  Copyright (c) 2026
//  SPDX license identifier: MIT
//  Full license header bsp/efinix/EfxSapphireSoc/include/LICENSE.MD
///////////////////////////////////////////////////////////////////////////////////

/*******************************************************************************
 * @file CI_gpu2d.h
 * @brief 2D 加速器 GPU_2D 的绘图 API —— 走自定义指令(CI), 【不占 APB 地址】。
 *        实现: CI_gpu2d.c    RTL: GPU_2D/GPU_2D.v, cmd/CIUnpack.v, cmd/CmdExec.v
 *
 * 一条绘图命令 = N+1 条 CI(N = 这条命令画几张图; 单张图 N=1, 合成 N=2)。
 * 位序必须和 RTL CIUnpack.v 一致, 改一边就得改另一边:
 *   第 1 条  function_id = 操作类型(见 GPU2D_FID_*)
 *            inputs_0    = { 宽[31:16], 高[15:0] }   单位: 像素 / 行(几张图共用)
 *            inputs_1    = 目的地址                    (几张图合成到同一块地方)
 *   第 2 条起 function_id = ARGS  —— 【一条 CI = 一张图】
 *            inputs_0    = 这张图的源地址(纯色图传 0)
 *            inputs_1    = { more[31], flags[30:24], 这张图的参数[23:0] }
 *            more = 1 后面还有一张图, more = 0 这条是最后一张(命令结束词)
 *   ★ 几条 CI 只是"一条装不下一张图的全部资料", 命令本身仍然只有一条。
 *   ★ 画【两张图】就用【两条参数 CI】（图片合成, 见下面 CI_gpu2d_combine）:
 *     第 1 张图 more=1、最后一张 more=0。
 *
 * 命令是【发完不管】的: 硬件收下立刻回 rsp, 绘制在后台做, CPU 不等。
 * 一帧画完发一条 FENCE —— 它等前面所有行都写进 DDR 才置 DONE_ACC,
 * 显示端在下一个帧边界翻页(见 user_gpu2d.h 的 gpu2d_waitFrame)。
 *
 * 地址是【软件算】的, 硬件只认地址不认识坐标:
 *     目的地址 = 帧缓存基址 + y * 行字节数 + x * 3
 *   ★ 行字节数是【画布】宽度(全屏 5760 / 分屏 2880), 不是矩形宽
 *   ★ 地址必须 < 0x1000_0000: Axi_Mux 只透传 28 位, 超了会写飞
 ******************************************************************************/

#pragma once

#include "type.h"

/* 功能号 = { funct7[6:0], funct3[2:0] }。v1 里 funct7 恒 0, 只用 funct3 区分。
 * ★ 这几个值不要加 u 后缀: cfu_push 会把它们原样拼进汇编的 .word 表达式。 */
#define GPU2D_FID_FILL      0x001       /* 纯色填充 */
#define GPU2D_FID_BLIT      0x002       /* 块拷贝(源 -> 目的) */
#define GPU2D_FID_FENCE     0x003       /* 帧结束(单条 CI) */
#define GPU2D_FID_ARGS      0x007       /* 参数 CI / 命令结束词 */

/* 合成模式 = ARGS 的 flags[2:0], 硬件在 CmdExec 里解码成"要哪几路源"。
 * 发错的表现: BLIT 忘了填 REPLACE -> 硬件按 FILL 走, 不读源, 整块刷成常数色。 */
#define GPU2D_MODE_FILL     0
#define GPU2D_MODE_REPLACE  1
#define GPU2D_MODE_ALPHA    2           /* alpha 混合: 已实现, 见 docs/spec/硬件_加速器.md §5.3 */
#define GPU2D_MODE_CKEY     3           /* 纯色层标记（颜色合成的第 2 层就是它） */
#define GPU2D_MODE_TINT_ALPHA 4         /* 纯色层 tint + alpha 混合（源①背景 + 源②base） */

/* flags[3] = 源类型: 0 = DDR 里的 RGB888 帧, 1 = 片上 PRAM 的调色板素材。
 * 选 1 时颜色字段的含义换成【源行跨距】(单位: 字, 一个字 4 个索引)。 */
#define GPU2D_MODE_SPRITE   0x08u

/* flags[4] = 颜色键控(ck) 使能 —— 等于键色的像素【不写】(= 透明), 其余照写。
 *   透明处显示的是帧缓存里【原有的内容】(背景), 所以画精灵不用先擦背景。
 *   ★ 现在只有 sprite(调色板素材源) 这条路实现了; 键色写死白 0xFFFFFF
 *     (pic2pram.py 把透明像素统一转成调色板第 0 项 = 白)。
 *   ★ 不带这一位的命令 = 老行为, 一个字节都不变。FILL 拿不到源, ck 对它无效。 */
#define GPU2D_MODE_CKEY_EN  0x10u

/* flags[5] = MX 水平镜像 / flags[6] = MY 垂直镜像（见 docs/spec/硬件_加速器.md §1.6）
 *   ★ 软件的地址约定【不变】：源地址还是"要画那一段(未镜像时)的左边缘"。
 *     倒读的起点（行尾那个字）由硬件自己算：行首 + ceil(w/4) − 1。
 *   ★ MX 要求 w 是 4 的倍数（素材行按 4 像素打包；否则行尾的 padding 会被翻到前面）。
 *   ★ MY 对两类源都成立；MX 目前只支持 PRAM 精灵源（DDR 源的行缓冲是 FIFO，倒着取不了）。 */
#define GPU2D_FLIP_X   0x1u
#define GPU2D_FLIP_Y   0x2u
#define GPU2D_ROT90    0x4u   /* screen CW 90deg: right-facing source -> down; add FLIP_Y for up */

/* 硬件对每条 CI 回的固定标识("CMD1"), 必须和 CmdBuffer.v 的 CI_ID 一致 */
#define GPU2D_CI_ID        0x434D4431u

/* 颜色统一传 0xRRGGBB。帧缓存里的字节序是 B,G,R, 那是显示端的事,
 * 驱动和 RTL 都不做转换。 */

/* 确认 CI 通路: 发一条 NOP 再读硬件回的 rsp, 正常恒为 GPU2D_CI_ID */
u32  CI_gpu2d_checkId(void);

/* 纯色填充矩形。dst_addr = 矩形左上角字节地址; w/h 单位 像素/行, 不能为 0 */
void CI_gpu2d_fill(u32 dst_addr, u32 w, u32 h, u32 rgb);

/* 按坐标填充(坐标 -> 地址在软件算), fb_base = 帧缓存基址 */
void CI_gpu2d_fillRect(u32 fb_base, u32 x, u32 y, u32 w, u32 h, u32 rgb);

/* 块拷贝(BLIT + REPLACE)。
 * ★ 源和目的必须【同跨距】: 硬件用同一个行跨距递推两边,
 *   所以两块得在同一种画布几何里(都在全屏帧 / 都在分屏半幅)。 */
void CI_gpu2d_blit(u32 src_addr, u32 dst_addr, u32 w, u32 h);

/* 按坐标块拷贝 */
void CI_gpu2d_blitRect(u32 src_fb, u32 sx, u32 sy,
                       u32 dst_fb, u32 dx, u32 dy,
                       u32 w, u32 h);

/* 画调色板素材(片上 PRAM 索引图), 全程不读 DDR。
 * asset_base = 素材在 PRAM 里的起始【字】偏移, stride = 源行跨距(字/行)。
 * w 小于素材宽就是右侧裁剪 —— 源、目的跨距互相独立。 */
void CI_gpu2d_sprite(u32 asset_base, u32 dst_addr, u32 w, u32 h, u32 stride);

/* 同上, 坐标版(只有目的要换算地址) */
void CI_gpu2d_spriteRect(u32 asset_base, u32 stride,
                         u32 dst_fb, u32 dx, u32 dy,
                         u32 w, u32 h);

/* 同上，但带镜像：flip = bit0(MX) | bit1(MY)，0 = 不镜像（= CI_gpu2d_spriteRect）。 */
void CI_gpu2d_spriteFlip(u32 asset_base, u32 stride, u32 dst_fb,
                         u32 dx, u32 dy, u32 w, u32 h, u32 flip);

/* 镜像 + 颜色键控一起开 —— 人物左右跑就是这一条（透明 + 左右翻转）。 */
void CI_gpu2d_spriteFlipCK(u32 asset_base, u32 stride, u32 dst_fb,
                           u32 dx, u32 dy, u32 w, u32 h, u32 flip);

/* 和上面两个一样, 但【开颜色键控】: 素材里等于白色的像素不写(透明)。
 *   ★ 精灵外面那圈"透明背景"就不会再盖住背景了 —— 不用先擦、也不用先补背景。
 *   ★ 不带 CK 的老函数原样保留: 要"白框"的行为时还用它们。 */
void CI_gpu2d_spriteCK(u32 asset_base, u32 dst_addr, u32 w, u32 h, u32 stride);

void CI_gpu2d_spriteRectCK(u32 asset_base, u32 stride,
                           u32 dst_fb, u32 dx, u32 dy,
                           u32 w, u32 h);

/* * alpha 混合（MODE=2，已实现）：把 PRAM 精灵按调色板里那一项的 α 混到目的现有的像素上。
 *     out = 背景 + (精灵 − 背景) × α / 255      （背景 = 硬件读回的目的那块矩形）
 *   · 背景不用给、也没法给：硬件读的就是这条命令要写的那块矩形（不需要新字段）。
 *   · α 不在命令里，在【调色板每一项的高 8 位】（palette.hex 一项 = AARRGGBB）——
 *     所以一个索引 = 一种 (颜色, α)：画素材时用固定档位（0/128/255），别用连续渐变。
 *   · extra = GPU2D_MODE_CKEY_EN（开颜色键控）| GPU2D_FLIP_X / GPU2D_FLIP_Y（镜像，
 *     函数内部帮你左移 5 位）；传 0 = 纯 alpha 混合。ck 掩码和 α 掩码在硬件里【相与】。
 *   · 只支持 PRAM 精灵（flags[3]=1）：DDR 帧源没有 α。
 *   · 警告：别拿它刷整屏 —— 读回量 = 矩形那一段，整屏读回会把 DDR 读带宽吃满。
 *   和 sprite/spriteCK 的关系：地址、跨距、画到哪全都一样，只是多读回一次背景。 */
void CI_gpu2d_spriteAlpha(u32 asset_base, u32 dst_addr, u32 w, u32 h,
                          u32 stride, u32 extra);

/* alpha 的坐标版（extra 同上；extra=0 就是纯 alpha 混合） */
void CI_gpu2d_spriteAlphaRect(u32 asset_base, u32 stride, u32 dst_fb,
                              u32 dx, u32 dy, u32 w, u32 h, u32 extra);

/*   —— tint + alpha（MODE=4，新增）——
 *   一条命令 = 3 条 CI：
 *       ① 命令开始: { 宽, 高 } + 目的地址
 *       ② 底图: PRAM 地址 + REPLACE|SPRITE + 行跨距 / ROT90
 *       ③ tint: ARGB 颜色 + TINT_ALPHA|SPRITE|CKEY_EN + tone/strength
 *   · 底图的 α 完整保留（0/128/255），先由 imageCombine 上色，再由 alphaBlending 用底图 α
 *     和背景混合；tint 不碰背景。
 *   · argb 的 A 不用（填 0）；strength 0..255；tone: 1 插值 / 2 加光 / 3 乘系数 / 4 正片叠底。
 *   · extra = GPU2D_MODE_CKEY_EN | GPU2D_FLIP_X/Y | GPU2D_ROT90（底图用，tint 层不带形状）。
 *   · 与 spriteAlpha 相比只多了一条 tint CI；适合"同一张 base 弹换颜色"的运行时染色。 */
void CI_gpu2d_spriteAlphaTint(u32 asset_base, u32 stride, u32 dst_addr,
                              u32 w, u32 h, u32 argb, u32 strength, u32 tone,
                              u32 extra);

/* tint + alpha 的坐标版 */
void CI_gpu2d_spriteAlphaTintRect(u32 asset_base, u32 stride, u32 dst_fb,
                                  u32 dx, u32 dy, u32 w, u32 h,
                                  u32 argb, u32 strength, u32 tone, u32 extra);

/* 带白色键控的 tint，不读取目的像素的 alpha。
 * 源素材 alpha 通道会被忽略，因此半透明边缘会变成不透明。 */
void CI_gpu2d_spriteTintCKRect(u32 asset_base, u32 stride, u32 dst_fb,
                                u32 dx, u32 dy, u32 w, u32 h,
                                u32 argb, u32 strength, u32 tone, u32 flip);

/*   —— 图片合成：把两张 PRAM 素材图合成一张再写进目的 ——
 *   一条命令 = 3 条 CI（硬件只发一条命令，软件发三条）:
 *        ① 命令开始: { 宽, 高 } + 目的地址          （两张图共用）
 *        ② 图一（底图）: 素材基址 + 模式 + 行跨距     more = 1
 *        ③ 图二（特效图）: 素材基址 + 模式 + 行跨距   more = 0（结束词）
 *   · base_/fx_ 的【模式各按自己那条 CI 给】: GPU2D_MODE_REPLACE | GPU2D_MODE_SPRITE
 *     再按需要或上 GPU2D_MODE_CKEY_EN(抠色) / GPU2D_FLIP_X / GPU2D_FLIP_Y(镜像)。
 *   · 合成结果 = 两张图叠起来（图二在上面那张）:
 *         某个字节上"图二不透明" ⇒ 用图二的颜色；否则用图一的颜色
 *         掩码 = 两张的掩码相或 ⇒ 图二把原图的透明洞填上；图二透明的地方保留图一
 *   · 要不要和【背景】(目的里原有的像素)做 alpha 混合，由【图二那张】的模式决定
 *     （模式给 GPU2D_MODE_ALPHA，硬件自己把目的那一行读回来当背景，不用软件给）。
 *   · w/h 是目标矩形，两张图共用；两张素材都要 ≥ 这个尺寸（宽不够会读到相邻行）。
 *   · 现在一条命令最多 2 张图（硬件 IMAGE_NUM = 2）。 */
void CI_gpu2d_combine(u32 base_addr, u32 base_stride, u32 base_flags,
                      u32 fx_addr,   u32 fx_stride,   u32 fx_flags,
                      u32 dst_addr,  u32 w, u32 h);

/* 合成的坐标版（只有目的要换算地址） */
void CI_gpu2d_combineRect(u32 base_addr, u32 base_stride, u32 base_flags,
                          u32 fx_addr,   u32 fx_stride,   u32 fx_flags,
                          u32 dst_fb, u32 dx, u32 dy, u32 w, u32 h);

/* 帧结束: 单独一条 CI。硬件把前面所有绘制都落盘后置 DONE_ACC,
 * 显示端在下一个帧边界翻页并清掉它 —— 一帧发一次。 */
/*   —— 颜色合成（图片合成 + 色调）：第 2 层不给素材，给一个纯色（argb + 强度 + 色调模式）——
 *   一条命令 = 3 条 CI，和 combine 一模一样，只是第 2 格换成了"颜色"而不是"素材地址"：
 *        inputs_0 = {A,R,G,B}（A 不用，填 0）
 *        flags    = REPLACE | SPRITE | CKEY_EN | 3   ← 3 = flags[2:0]，"纯色层"
 *        param    = (色调模式 << 21) | (强度 << 12)  ；色调模式 0 = 关，1 插值染色 2 加光 3 乘系数 4 正片叠底
 *   · 结果：底色那张图的不透明处，按浓度往这个颜色偏；透明处【不动】（ck 掩码说了算）
 *     ⇒ 拿来当"撞击闪红 / 上色 / 高亮"最省事。
 *   · strength=0 等价于不开（硬件直通），255 = 全是这个颜色（插值模式下差 1 个色阶）。
 *   · 见 docs/spec/硬件_加速器.md §1.3.1 / §5.6。 */
void CI_gpu2d_combineTint(u32 base_addr, u32 base_stride, u32 base_flags,
                          u32 argb, u32 strength, u32 tone,
                          u32 dst_addr, u32 w, u32 h);

/* 颜色合成的坐标版（只有目的要换算地址） */
void CI_gpu2d_combineTintRect(u32 base_addr, u32 base_stride, u32 base_flags,
                              u32 argb, u32 strength, u32 tone,
                              u32 dst_fb, u32 dx, u32 dy, u32 w, u32 h);

void CI_gpu2d_fence(void);
