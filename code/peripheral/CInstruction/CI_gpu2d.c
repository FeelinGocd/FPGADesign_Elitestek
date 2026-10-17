///////////////////////////////////////////////////////////////////////////////////
//  Copyright (c) 2026
//  SPDX license identifier: MIT
//  Full license header bsp/efinix/EfxSapphireSoc/include/LICENSE.MD
///////////////////////////////////////////////////////////////////////////////////

/*******************************************************************************
 * @file CI_gpu2d.c
 * @brief 2D 加速器 GPU_2D 绘图 API 实现。说明见 CI_gpu2d.h。
 *
 * ★★ 只能用 cfu_type_R (CUSTOM0 = opcode 0x0B) ★★
 *   硬件只译码这一个自定义 opcode: SoC_RISCV32.v 里 (INSTRUCTION & 0x7F) == 0x0B。
 *   BSP 另外两个宏 cfu_push(CUSTOM1) / cfu_pop(CUSTOM2) 是非法指令 ——
 *   踩过: 综合器发现 CFU 通道恒不工作, 把四条线整个剪掉, 板上 CPU 停在第一条
 *   自定义指令上, 串口再无输出。
 *
 *   cfu_type_R 一条指令就"发出去 + 等 rsp"; CmdBuffer 对每条 CI 立刻回 rsp,
 *   所以它不会把 CPU 卡在绘制上。
 *
 * ★ 宏的 func3/func7 会被字符串化拼进汇编的 .word 表达式 =>
 *   调用时必须写字面量, 不能传宏或变量。字面量集中在本文件里。
 ******************************************************************************/

#include "CI_gpu2d.h"
#include "riscv.h"              /* cfu_type_R() */
#include "user_lineBuffer.h"    /* LINEBUFFER_LINE_BYTES / _PIXEL_BYTES */

/* 宽高打包: { 宽[31:16], 高[15:0] }（和 RTL CIUnpack.v 的位序一致） */
static u32 ci_wh(u32 w, u32 h)
{
    return ((w & 0xFFFFu) << 16) | (h & 0xFFFFu);
}

/* 第 1 条 CI: 这条命令画多大 + 画到哪 —— 【几张图共用】, 里面不放某一张图的东西 */
static u32 ci_op(u32 fid3, u32 wh, u32 dst)
{
    if (fid3 == GPU2D_FID_FILL) return (u32)cfu_type_R(0x1, 0x0, wh, dst);
    if (fid3 == GPU2D_FID_BLIT) return (u32)cfu_type_R(0x2, 0x0, wh, dst);
    return (u32)cfu_type_R(0x3, 0x0, wh, dst);                  /* FENCE: 参数不用 */
}

/* 第 2 条起 CI: 【一张图的资料】 —— 这条 CI 就是一张图:
 *     src   = 这张图的源地址（纯色图填 0）
 *     flags 低 3 位 = 合成模式; flags[3] = 源类型; flags[4] = 颜色键控使能;
 *             flags[5]/[6] = 镜像
 *     val   = 这张图自己的参数（FILL 时是纯色, 素材源时是源行跨距）
 *     more  = 1 后面还有一张图; 0 这条是最后一张（命令结束词, 硬件收到就发命令）
 *   ★ more 是 inputs_1 的最高位, 必须和 flags/参数一起拼成一个 32 位字 */
static u32 ci_args_full(u32 src, u32 flags, u32 val, u32 more)
{
    u32 word1 = ((more & 1u) << 31)
              | (((u32)flags & 0x7Fu) << 24)
              | ((u32)val  & 0x00FFFFFFu);

    return (u32)cfu_type_R(0x7, 0x0, src, word1);
}

/* 只有一张图时用得最多的那种（more = 0: 这条就是结束词） */
static u32 ci_args(u32 src, u32 flags, u32 val)
{
    return ci_args_full(src, flags, val, 0u);
}

static u32 sprite_param(u32 stride, u32 rotate90)
{
    return (stride & 0xFFFu) | (rotate90 ? 0x00800000u : 0u);
}

/* 画布一行的字节数: 跟显示模式走(硬件那边同一套: 全屏 5760 / 分屏 2880) */
static u32 ci_line_bytes(void)
{
    return lineBuffer_isSplit() ? (LINEBUFFER_LINE_BYTES / 2u)
                                :  LINEBUFFER_LINE_BYTES;
}

/* NOP(功能号 0): 不产生任何绘制命令, 但硬件照样回一个 rsp -> 拿它验通路 */
u32 CI_gpu2d_checkId(void)
{
    return (u32)cfu_type_R(0x0, 0x0, 0u, 0u);
}

void CI_gpu2d_fill(u32 dst_addr, u32 w, u32 h, u32 rgb)
{
    (void)ci_op(GPU2D_FID_FILL, ci_wh(w, h), dst_addr);
    (void)ci_args(0u, GPU2D_MODE_FILL, rgb);        /* 纯色图: 地址填 0 */
}

void CI_gpu2d_fillRect(u32 fb_base, u32 x, u32 y, u32 w, u32 h, u32 rgb)
{
    u32 dst = fb_base
            + y * ci_line_bytes()
            + x * LINEBUFFER_PIXEL_BYTES;

    CI_gpu2d_fill(dst, w, h, rgb);
}

void CI_gpu2d_blit(u32 src_addr, u32 dst_addr, u32 w, u32 h)
{
    (void)ci_op(GPU2D_FID_BLIT, ci_wh(w, h), dst_addr);
    (void)ci_args(src_addr, GPU2D_MODE_REPLACE, 0u);
}

void CI_gpu2d_blitRect(u32 src_fb, u32 sx, u32 sy,
                       u32 dst_fb, u32 dx, u32 dy,
                       u32 w, u32 h)
{
    u32 lb  = ci_line_bytes();
    u32 src = src_fb + sy * lb + sx * LINEBUFFER_PIXEL_BYTES;
    u32 dst = dst_fb + dy * lb + dx * LINEBUFFER_PIXEL_BYTES;

    CI_gpu2d_blit(src, dst, w, h);
}

void CI_gpu2d_sprite(u32 asset_base, u32 dst_addr, u32 w, u32 h, u32 stride)
{
    (void)ci_op(GPU2D_FID_BLIT, ci_wh(w, h), dst_addr);
    (void)ci_args(asset_base, GPU2D_MODE_REPLACE | GPU2D_MODE_SPRITE, stride);
}

void CI_gpu2d_spriteRect(u32 asset_base, u32 stride,
                         u32 dst_fb, u32 dx, u32 dy,
                         u32 w, u32 h)
{
    u32 dst = dst_fb
            + dy * ci_line_bytes()
            + dx * LINEBUFFER_PIXEL_BYTES;

    CI_gpu2d_sprite(asset_base, dst, w, h, stride);
}

void CI_gpu2d_spriteCK(u32 asset_base, u32 dst_addr, u32 w, u32 h, u32 stride)
{
    (void)ci_op(GPU2D_FID_BLIT, ci_wh(w, h), dst_addr);
    (void)ci_args(asset_base,
                  GPU2D_MODE_REPLACE | GPU2D_MODE_SPRITE | GPU2D_MODE_CKEY_EN, stride);
}

void CI_gpu2d_spriteRectCK(u32 asset_base, u32 stride,
                           u32 dst_fb, u32 dx, u32 dy,
                           u32 w, u32 h)
{
    u32 dst = dst_fb
            + dy * ci_line_bytes()
            + dx * LINEBUFFER_PIXEL_BYTES;

    CI_gpu2d_spriteCK(asset_base, dst, w, h, stride);
}

/*   —— alpha 混合版（MODE=2）：把 PRAM 精灵按调色板那一项的 α 混到【目的现有的像素】上 ——
 *       out = 背景 + (精灵 − 背景) × α / 255
 *   命令形状和 sprite 完全一样，只把 MODE 从 REPLACE 换成 ALPHA。
 *   ★ 背景不用给：硬件读回的就是【这条命令要写的那块矩形】（源① 的行地址 = 目的行地址）。
 *   ★ α 不在命令里，在调色板每一项的高 8 位（一项 = AARRGGBB）——
 *     所以一个索引 = 一种 (颜色, α)，素材侧怎么画见 docs/spec/硬件_加速器.md §5.3。
 *   ★ extra 可以再拼 ck（GPU2D_MODE_CKEY_EN）和镜像位：
 *     本来要按 flip 左移 5 位，这里让调用方直接给【已经就位的 flags 位】更省事 ——
 *     传 0 就是纯 alpha 混合。两个掩码（ck 与 α=0）在硬件里相与。
 *   ★ 只支持 PRAM 精灵（flags[3]=1）：DDR 帧源没有 α。
 *   ★ 别拿它刷整屏：读回量 = 矩形那一段，整屏读回会把 DDR 读带宽吃满。 */
void CI_gpu2d_spriteAlpha(u32 asset_base, u32 dst_addr, u32 w, u32 h,
                          u32 stride, u32 extra)
{
    u32 rot = (extra & GPU2D_ROT90) != 0u;
    (void)ci_op(GPU2D_FID_BLIT, ci_wh(rot ? h : w, rot ? w : h), dst_addr);
    (void)ci_args(asset_base,
                  GPU2D_MODE_ALPHA | GPU2D_MODE_SPRITE
                  | (extra & GPU2D_MODE_CKEY_EN)
                  | ((extra << 5) & 0x60u),
                  sprite_param(stride, rot));
}

void CI_gpu2d_spriteAlphaRect(u32 asset_base, u32 stride, u32 dst_fb,
                              u32 dx, u32 dy, u32 w, u32 h, u32 extra)
{
    u32 dst = dst_fb
            + dy * ci_line_bytes()
            + dx * LINEBUFFER_PIXEL_BYTES;

    CI_gpu2d_spriteAlpha(asset_base, dst, w, h, stride, extra);
}

/*   —— 下面两个是"镜像"版：只多拼了 flags[5]/[6] ——
 *   ★ 源地址约定和普通 sprite 完全一样：要画那一段(未镜像时)的左边缘。
 *     倒读起点由硬件 CmdExec 自己算（行尾字 = 行首 + ceil(w/4) − 1），软件不用管。 */
void CI_gpu2d_spriteFlip(u32 asset_base, u32 stride, u32 dst_fb,
                         u32 dx, u32 dy, u32 w, u32 h, u32 flip)
{
    u32 dst = dst_fb + dy * ci_line_bytes() + dx * LINEBUFFER_PIXEL_BYTES;
    u32 rot = (flip & GPU2D_ROT90) != 0u;
    u32 out_w = rot ? h : w;
    u32 out_h = rot ? w : h;
    (void)ci_op(GPU2D_FID_BLIT, ci_wh(out_w, out_h), dst);
    (void)ci_args(asset_base,
                  GPU2D_MODE_REPLACE | GPU2D_MODE_SPRITE | ((flip & 0x3u) << 5),
                  sprite_param(stride, rot));
}

void CI_gpu2d_spriteFlipCK(u32 asset_base, u32 stride, u32 dst_fb,
                           u32 dx, u32 dy, u32 w, u32 h, u32 flip)
{
    u32 dst = dst_fb
            + dy * ci_line_bytes()
            + dx * LINEBUFFER_PIXEL_BYTES;

    u32 rot = (flip & GPU2D_ROT90) != 0u;
    (void)ci_op(GPU2D_FID_BLIT, ci_wh(rot ? h : w, rot ? w : h), dst);
    (void)ci_args(asset_base,
                  GPU2D_MODE_REPLACE | GPU2D_MODE_SPRITE | GPU2D_MODE_CKEY_EN
                  | ((flip & 0x3u) << 5),
                  sprite_param(stride, rot));
}

/*   —— 图片合成：两张 PRAM 素材图合成一张（硬件：process/imageCombine.v）——
 *   命令 = 3 条 CI:  [命令开始: 宽高+目的] + [图一: 地址+模式+跨距, more=1]
 *                                    + [图二: 地址+模式+跨距, more=0]
 *   硬件把两行串行读出来（同一个 PRAM 读口），在合成级缓存图一那一行、与图二逐拍合成，
 *   掩码按"合成后整张图"给（图二不透明的字节用图二的颜色，否则用图一的）。
 *   ★ w/h 是【目标矩形】，两张图共用；两张素材都得 ≥ 这个尺寸 */
void CI_gpu2d_combine(u32 base_addr, u32 base_stride, u32 base_flags,
                      u32 fx_addr,   u32 fx_stride,   u32 fx_flags,
                      u32 dst_addr,  u32 w, u32 h)
{
    (void)ci_op(GPU2D_FID_BLIT, ci_wh(w, h), dst_addr);
    (void)ci_args_full(base_addr, base_flags, base_stride, 1u);   /* 图一：后面还有一张 */
    (void)ci_args     (fx_addr,   fx_flags,   fx_stride);         /* 图二：这条是结束词 */
}

/* 合成（坐标版）：只有目的要换算地址 */
void CI_gpu2d_combineRect(u32 base_addr, u32 base_stride, u32 base_flags,
                          u32 fx_addr,   u32 fx_stride,   u32 fx_flags,
                          u32 dst_fb, u32 dx, u32 dy, u32 w, u32 h)
{
    u32 dst = dst_fb
            + dy * ci_line_bytes()
            + dx * LINEBUFFER_PIXEL_BYTES;

    CI_gpu2d_combine(base_addr, base_stride, base_flags,
                     fx_addr, fx_stride, fx_flags,
                     dst, w, h);
}

void CI_gpu2d_fence(void)
{
    (void)ci_op(GPU2D_FID_FENCE, 0u, 0u);   /* FENCE: 宽高/地址都不用 */
}

/*   —— 图片合成 + 色调（颜色合成）：第 2 层不给素材图，改给一个颜色 ARGB ——
 *   命令还是 3 条 CI，区别只在那一条"特效层"的 ARGS：
 *       inputs_0        = 颜色 ARGB（A 可以不用，A=0）
 *       flags[2:0]      = 3（纯色层）| flags[3]=1，再加需要的 ck/镜像位
 *       param[23:21]    = 色调模式：1 插值染色 / 2 加光（饱和）/ 3 乘系数 / 4 正片叠底
 *       param[20]       = 0（特效层在上）
 *       param[19:12]    = 强度（0~255，浓度）
 *       param[11:0]     = 0（纯色层不读素材，没有行跨距）
 *   见 docs/spec/硬件_加速器.md §1.3.1 / §5.6 */
void CI_gpu2d_combineTint(u32 base_addr, u32 base_stride, u32 base_flags,
                          u32 argb, u32 strength, u32 tone,
                          u32 dst_addr, u32 w, u32 h)
{
    /*   ★★ ck 位（flags[4]）必须【开着】：命令级 ck 开关取自最后一格（就是这一格），
     *      而"到底写不写"由【下层那张图】的 ck 掩码说话（纯色层自己不带掩码）。
     *      不开 ck 的话出口掩码恒 1 ⇒ 整块矩形（连精灵四周的透明处）都被染上色，
     *      症状是"闪红闪出一个红方块、把背景擦掉"。*/
    u32 fx_flags = GPU2D_MODE_REPLACE | GPU2D_MODE_SPRITE | GPU2D_MODE_CKEY_EN | 3u;
    u32 fx_param = ((tone & 7u) << 21) | ((strength & 0xFFu) << 12);

    (void)ci_op(GPU2D_FID_BLIT, ci_wh(w, h), dst_addr);
    (void)ci_args_full(base_addr, base_flags, base_stride, 1u);   /* 底图：后面还有一层 */
    (void)ci_args_full(argb, fx_flags, fx_param, 0u);             /* 纯色层（结束词） */
}

/* 合成 + 色调的坐标版（只有目的要换算地址） */
void CI_gpu2d_combineTintRect(u32 base_addr, u32 base_stride, u32 base_flags,
                              u32 argb, u32 strength, u32 tone,
                              u32 dst_fb, u32 dx, u32 dy, u32 w, u32 h)
{
    u32 dst = dst_fb
            + dy * ci_line_bytes()
            + dx * LINEBUFFER_PIXEL_BYTES;

    CI_gpu2d_combineTint(base_addr, base_stride, base_flags, argb, strength, tone, dst, w, h);
}
