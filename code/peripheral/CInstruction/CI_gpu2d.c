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

/* 第 1 条 CI: 操作类型 + 源地址 + 目的地址 */
static u32 ci_op(u32 fid3, u32 src, u32 dst)
{
    if (fid3 == GPU2D_FID_FILL) return (u32)cfu_type_R(0x1, 0x0, src, dst);
    if (fid3 == GPU2D_FID_BLIT) return (u32)cfu_type_R(0x2, 0x0, src, dst);
    return (u32)cfu_type_R(0x3, 0x0, src, dst);                 /* FENCE */
}

/* 第 2 条 CI: ARGS。
 *   flags 低 3 位 = 合成模式; flags[3] = 源类型; flags[4] = 颜色键控使能;
 *   val = 颜色(或素材源跨距) */
static u32 ci_args(u32 wh, u32 flags, u32 val)
{
    u32 word1 = (((u32)flags & 0x7Fu) << 24)                   /* more 恒 0 */
              | ((u32)val  & 0x00FFFFFFu);

    return (u32)cfu_type_R(0x7, 0x0, wh, word1);
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
    (void)ci_op(GPU2D_FID_FILL, 0u, dst_addr);
    (void)ci_args(((w & 0xFFFFu) << 16) | (h & 0xFFFFu),
                  GPU2D_MODE_FILL, rgb);
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
    (void)ci_op(GPU2D_FID_BLIT, src_addr, dst_addr);
    (void)ci_args(((w & 0xFFFFu) << 16) | (h & 0xFFFFu),
                  GPU2D_MODE_REPLACE, 0u);
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
    (void)ci_op(GPU2D_FID_BLIT, asset_base, dst_addr);
    (void)ci_args(((w & 0xFFFFu) << 16) | (h & 0xFFFFu),
                  GPU2D_MODE_REPLACE | GPU2D_MODE_SPRITE, stride);
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
    (void)ci_op(GPU2D_FID_BLIT, asset_base, dst_addr);
    (void)ci_args(((w & 0xFFFFu) << 16) | (h & 0xFFFFu),
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
    (void)ci_op(GPU2D_FID_BLIT, asset_base, dst_addr);
    (void)ci_args(((w & 0xFFFFu) << 16) | (h & 0xFFFFu),
                  GPU2D_MODE_ALPHA | GPU2D_MODE_SPRITE
                  | (extra & GPU2D_MODE_CKEY_EN)
                  | ((extra << 5) & 0x60u),
                  stride);
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
    u32 dst = dst_fb
            + dy * ci_line_bytes()
            + dx * LINEBUFFER_PIXEL_BYTES;

    (void)ci_op(GPU2D_FID_BLIT, asset_base, dst);
    (void)ci_args(((w & 0xFFFFu) << 16) | (h & 0xFFFFu),
                  GPU2D_MODE_REPLACE | GPU2D_MODE_SPRITE | ((flip & 0x3u) << 5),
                  stride);
}

void CI_gpu2d_spriteFlipCK(u32 asset_base, u32 stride, u32 dst_fb,
                           u32 dx, u32 dy, u32 w, u32 h, u32 flip)
{
    u32 dst = dst_fb
            + dy * ci_line_bytes()
            + dx * LINEBUFFER_PIXEL_BYTES;

    (void)ci_op(GPU2D_FID_BLIT, asset_base, dst);
    (void)ci_args(((w & 0xFFFFu) << 16) | (h & 0xFFFFu),
                  GPU2D_MODE_REPLACE | GPU2D_MODE_SPRITE | GPU2D_MODE_CKEY_EN
                  | ((flip & 0x3u) << 5),
                  stride);
}

void CI_gpu2d_fence(void)
{
    (void)ci_op(GPU2D_FID_FENCE, 0u, 0u);
}
