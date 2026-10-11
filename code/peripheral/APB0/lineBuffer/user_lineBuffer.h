///////////////////////////////////////////////////////////////////////////////////
//  Copyright (c) 2026
//  SPDX license identifier: MIT
//  Full license header bsp/efinix/EfxSapphireSoc/include/LICENSE.MD
///////////////////////////////////////////////////////////////////////////////////

/*******************************************************************************
*
* @file user_lineBuffer.h
*
* @brief 显示通道 lineBuffer 外设 —— 对外 API(实现见 user_lineBuffer.c)。
*
* DDR 里放几块帧缓存, FPGA 自己按行取数 -> 跨时钟双 bank 缓冲 -> 24bit 像素
* -> 真 1080p60 TMDS 到 HDMI。CPU 只管: 定基址 / 交帧(写 DONE) / 开使能 / 看帧率。
*
* 地址: APB0 窗口 0xf8100000 的槽 2 内【显示子块】0x2040 ~ 0x206F(显示侧 PADDR[6]=1):
*   0x2040 ID(RO 恒 0x4C423031) | 0x2044~0x2050 A/B 半幅基址 x4 | 0x2054/0x2058 C/D 全屏基址
*   0x205C STATUS | 0x2060 FPS_ACC | 0x2064 FPS_CPU | 0x2068 DONE_CPU | 0x206C DONE_ACC
*   0x2070 IRQ_FRAME_EVT | 0x2074 IRQ_FRAME_EN | 0x2078 CKEY_COLOR(颜色键控的键色)
*   同槽 0x2000 ~ 0x201F 是加速器观测寄存器(user_gpu2d.h)。
*
* 帧缓存格式(最容易写错的地方): 1920 x 1080, 每像素 3 字节, 行距固定 5760, 行间无 padding。
*   字节顺序是【B, G, R】—— 地址最低那个字节是"蓝"(RTL: TMDS 把 [23:16] 当红)。
*   红蓝反了、或者整体偏几列, 基本都是这里写错了。
*
* DONE: 置位靠写 1(或加速器硬件置位), 清零【只有 handler 在帧边界翻页后自己做, 软件不许清】。
*   写 DONE = 这块画完了, 下个帧边界切过去显示; 静态图不写 DONE 就永远显示当前那一块。
 *   ★ 0x206C 这一位: 地址表里写作 DONE_ACC, RTL 里信号叫 done_gpu2d(和 done_cpu 对称);
 *     下面 API 跟着 RTL 叫 doneGpu2d。名字不同, 指的是同一个寄存器。
*
******************************************************************************/

#pragma once

#include "type.h"

/** @brief ID 寄存器的期望值, "LB01" 的 ASCII; 读到它说明整条 APB 通路是通的。 */
#define LINEBUFFER_ID_VALUE     0x4C423031u

/*------------------------------------------------------------------------------
 * STATUS 寄存器位定义
 *----------------------------------------------------------------------------*/
#define LINEBUFFER_ST_ENABLE    0x00000001u     /* bit0 使能(不锁存, 写 0 立即停) */
#define LINEBUFFER_ST_SPLIT     0x00000002u     /* bit1 分屏(帧边界才生效) */

/*------------------------------------------------------------------------------
 * 帧缓存几何(和 RTL 里写死的 5760/1080 必须一致)
 *----------------------------------------------------------------------------*/
#define LINEBUFFER_WIDTH        1920u
#define LINEBUFFER_HEIGHT       1080u
#define LINEBUFFER_LINE_BYTES   5760u                                   /* 1920 * 3 */
#define LINEBUFFER_PIXEL_BYTES  3u
#define LINEBUFFER_FRAME_BYTES  (LINEBUFFER_LINE_BYTES * LINEBUFFER_HEIGHT)  /* 6220800 */
/* 分屏半幅: 每幅 960 列, 行距 2880 字节, 一块约 2.97MB */
#define LINEBUFFER_HALF_WIDTH       960u
#define LINEBUFFER_HALF_LINE_BYTES  2880u                               /* 960 * 3 */
#define LINEBUFFER_HALF_FRAME_BYTES (LINEBUFFER_HALF_LINE_BYTES * LINEBUFFER_HEIGHT)

/** @brief 读外设 ID, 确认这个 APB 外设活着; 正常恒为 LINEBUFFER_ID_VALUE。 */
u32  lineBuffer_getId(void);

/** @brief 设置全屏模式两块缓冲基址: base_c = 上电默认显示的那块, base_d = 画好了等切过去的。
 *  全屏(SPLIT=0)只用 C/D; A/B 那四个半幅基址用不到。
 */
void lineBuffer_setFullBase(u32 base_c, u32 base_d);

/** @brief 回读全屏两块缓冲的基址寄存器(0x14 / 0x18)。
 *  只写不读的话, 地址译码写错这类问题永远发现不了 —— 显示会去读另一块内存。
 */
u32  lineBuffer_getFullBaseC(void);

u32  lineBuffer_getFullBaseD(void);

/** @brief 设置分屏模式两块缓冲的基址(A / B), 每块 = 左半(加速器画的) + 右半(CPU 画的)。 */
void lineBuffer_setSplitBase(u32 a_acc, u32 a_cpu, u32 b_acc, u32 b_cpu);

/** @brief 开/关显示通道(en: 1 开, 0 关)。ENABLE 不锁存, 写 0 立刻停取数;
 *  建议上电先关, 图灌完再开, 免得先看到半张图。
 */
void lineBuffer_enable(u32 en);

/** @brief 请求切换全屏(split=0, 一行取 1 段)/分屏(1, 一行取左右 2 段)。
 *  模式在【帧边界】才生效, 一帧内取数几何一致(防撕裂); 写 SPLIT 那一拍 RTL 会把两个 DONE 丢掉。
 */
void lineBuffer_setSplit(u32 split);

/** @brief 读当前 STATUS(bit0 = ENABLE, bit1 = SPLIT 待生效值)。 */
u32  lineBuffer_getStatus(void);

/** @brief 显示通道是否已使能。 */
u32  lineBuffer_isEnabled(void);

/** @brief 当前请求的模式是否为分屏。 */
u32  lineBuffer_isSplit(void);

/** @brief 读加速器侧帧率(帧/秒)。没写 DONE 之前恒为 0 —— 统计的是交帧事件, 不是刷新率。 */
u32  lineBuffer_getFpsAcc(void);

/** @brief 读 CPU 侧帧率。 */
u32  lineBuffer_getFpsCpu(void);

/** @brief 交帧(GPU2D 侧): 写 1 置位 0x206C(地址表里叫 DONE_ACC)。真正翻页在下个帧边界, 翻完 handler 自己清;
 *  软件不要清, 也不要读回来判断(读回来只是当前还挂不挂着)。
 */
void lineBuffer_doneGpu2d(void);

/** @brief 交帧(CPU 侧): 写 1 置位 DONE_CPU。 */
void lineBuffer_doneCpu(void);

/** @brief 读 0x206C 当前值(1 = 还挂着, 0 = 已被 handler 在帧边界消费掉)。
 *  排错用: 写完 lineBuffer_doneGpu2d() 立刻读回, 能分清"根本没写进去"和"写了没被消费"。
 */
u32  lineBuffer_getDoneGpu2d(void);

/** @brief 把一块帧缓存灌成纯色(排错用: 让 C 和 D 内容完全不同, 屏上出现 D 就说明翻页了)。 */
void lineBuffer_fillSolid(u32 base, u32 lines, u8 r, u8 g, u8 b);

/** @brief 往【半幅】缓冲里灌纯色(分屏用: 960 列, 行距 2880), 第 0 列和最后一列强制白边。
 *  分屏时左半的右白边和右半的左白边应紧挨着拼在屏幕正中, 中缝错开/重叠一眼可见。
 */
void lineBuffer_fillHalf(u32 base, u32 lines, u8 r, u8 g, u8 b);

/** @brief 读回帧缓存某个像素(先刷 cache, 保证读的是 DDR 里的真实内容)。
 * @param x 列 0..1919, y 行 0..1079
 * @return 按 0xRRGGBB 拼好的值, 方便和屏上颜色对照 —— 它证明 DDR 里存的到底是什么。
 */
u32  lineBuffer_readPixel(u32 base, u32 x, u32 y);

/* 颜色键控(ck)的键色(显示子块 0x2078, 复位默认 0x00FFFFFF = 白)
 *   画精灵/贴图时, "等于这个颜色的像素【不写】" —— 那些位置保持帧缓存里原有的内容
 *   (背景、参考线), 所以精灵外面不会再有白框。
 *   ★ 配套约定(pic2pram.py 保证): 透明像素一律落成调色板第 0 项 = 纯白;
 *     素材【不透明区】里不能有纯白, 否则那部分也会被抠掉。
 *   ★ 只想用默认白的话, 不用调这两个函数。 */
void lineBuffer_setCkeyColor(u32 rgb);      /* rgb = 0xRRGGBB */
u32  lineBuffer_getCkeyColor(void);

/* 帧起始中断(显示子块槽 12/13 = 0x2070/0x2074)
 *   电平触发: ISR 里第一件事必须 lineBuffer_IRQ_frameClear(), 否则会一直重进 */
void lineBuffer_IRQ_frameClear(void);
u32  lineBuffer_IRQ_frameEvt(void);
void lineBuffer_IRQ_frameEnable(u32 en);
