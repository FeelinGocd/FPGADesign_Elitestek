///////////////////////////////////////////////////////////////////////////////////
//  Copyright (c) 2026
//  SPDX license identifier: MIT
//  Full license header bsp/efinix/EfxSapphireSoc/include/LICENSE.MD
///////////////////////////////////////////////////////////////////////////////////

/*******************************************************************************
*
* @file user_lineBuffer.c
*
* @brief 显示通道 lineBuffer 驱动实现(对外 API 见 user_lineBuffer.h)。
*
* RTL: 总线层 APB0_GPU_2D.v + GPU2D_APBHandler.v, 功能层 lineBuffer/lineBuffer.v。
* 地址: APB0 窗口 0xf8100000 的槽 2 内 0x2040 ~ 0x206F;
*       同槽 0x2000 ~ 0x201F 是加速器观测寄存器(user_gpu2d.h), 由 PADDR[6] 分。
*
******************************************************************************/

#include "user_lineBuffer.h"
#include "io.h"             /* 带来 soc.h 里的 IO_APB_SLAVE_0_INPUT */
#include "vexriscv.h"       /* data_cache_invalidate_all: 写完帧缓存要刷 cache */

/*  软件缓存的行跨距：绘制热路径不再每发一条命令都读一次 APB STATUS。
 *  lineBuffer_setSplit() 是唯一入口，切全屏/分屏时同步更新。  */
static u32 s_line_bytes = LINEBUFFER_LINE_BYTES;

/*------------------------------------------------------------------------------
 * 基地址 = APB Slave 0 窗口(soc.h, 0xf8100000) + 0x2040。
 * 和 RTL 必须一致, 改一处要改两处: 槽 2 = PADDR[15:12]==4'b0010, 显示子块 = PADDR[6]==1。
 *----------------------------------------------------------------------------*/
#ifdef IO_APB_SLAVE_0_INPUT
    #define LINEBUFFER_BASE     (IO_APB_SLAVE_0_INPUT + 0x2040u)
#else
    #error "APB Slave 0 is disabled, please enable it in IP Manager!!!"
#endif

/*------------------------------------------------------------------------------
 * 寄存器映射(偏移见下面每个成员的注释, 地址表见 user_lineBuffer.h)
 *
 *   顺序 = 偏移顺序, 一个字都不能改/删/插空位 —— 结构体靠偏移量访问硬件。
 *   volatile 不能省: 每次访问都必须真去总线, 不许编译器缓存或合并。
 *   !! 以后在 0x2C 之后加寄存器, 必须先补一个 volatile u32 reserved 占位再往后加。
 *----------------------------------------------------------------------------*/
typedef struct {
    volatile u32 id;                /* 0x00  RO */
    volatile u32 framea_base_acc;   /* 0x04  RW */
    volatile u32 framea_base_cpu;   /* 0x08  RW */
    volatile u32 frameb_base_acc;   /* 0x0C  RW */
    volatile u32 frameb_base_cpu;   /* 0x10  RW */
    volatile u32 framec_full_base;  /* 0x14  RW */
    volatile u32 framed_full_base;  /* 0x18  RW */
    volatile u32 status;            /* 0x1C  RW  bit0=ENABLE bit1=SPLIT */
    volatile u32 fps_acc;           /* 0x20  RO */
    volatile u32 fps_cpu;           /* 0x24  RO */
    volatile u32 done_cpu;          /* 0x28  RW1S */
    volatile u32 done_gpu2d;          /* 0x2C  RW1S */
} LINEBUFFERTYPE;

/* 把基地址当成 LINEBUFFERTYPE 结构体的首地址 */
#define LINEBUFFER          ((LINEBUFFERTYPE *)LINEBUFFER_BASE)


/* ============================== API 实现 ============================== */

/** @brief 读外设 ID, 确认这个 APB 外设活着。 */
u32 lineBuffer_getId(void){
    return LINEBUFFER->id;
}

/** @brief 读/写全屏模式两块显示缓冲的基址(C / D)。 */
u32 lineBuffer_getFullBaseC(void){ return LINEBUFFER->framec_full_base; }
u32 lineBuffer_getFullBaseD(void){ return LINEBUFFER->framed_full_base; }

void lineBuffer_setFullBase(u32 base_c, u32 base_d){
    LINEBUFFER->framec_full_base = base_c;
    LINEBUFFER->framed_full_base = base_d;
}

/** @brief 设置分屏模式两块缓冲的基址(A / B), 每块 = 左半(加速器) + 右半(CPU)。 */
void lineBuffer_setSplitBase(u32 a_acc, u32 a_cpu, u32 b_acc, u32 b_cpu){
    LINEBUFFER->framea_base_acc = a_acc;
    LINEBUFFER->framea_base_cpu = a_cpu;
    LINEBUFFER->frameb_base_acc = b_acc;
    LINEBUFFER->frameb_base_cpu = b_cpu;
}

/** @brief 开/关显示通道。ENABLE 不锁存(写 0 立刻停取数), STATUS 读回的是刚写进去的值。 */
void lineBuffer_enable(u32 en){
    u32 s = LINEBUFFER->status;
    if(en)
        s |=  LINEBUFFER_ST_ENABLE;
    else
        s &= ~LINEBUFFER_ST_ENABLE;

    LINEBUFFER->status = s;
}

/** @brief 请求切换全屏/分屏模式(帧边界才生效)。 */
void lineBuffer_setSplit(u32 split){
    u32 s = LINEBUFFER->status;

    if(split)
        s |=  LINEBUFFER_ST_SPLIT;
    else
        s &= ~LINEBUFFER_ST_SPLIT;

    LINEBUFFER->status = s;
    s_line_bytes = split ? LINEBUFFER_HALF_LINE_BYTES : LINEBUFFER_LINE_BYTES;
}

/** @brief 读 STATUS 原值。 */
u32 lineBuffer_getStatus(void){
    return LINEBUFFER->status;
}

/** @brief 显示通道是否已使能。 */
u32 lineBuffer_isEnabled(void){
    return (LINEBUFFER->status & LINEBUFFER_ST_ENABLE) ? 1u : 0u;
}

/** @brief 当前请求的模式是否为分屏。 */
u32 lineBuffer_isSplit(void){
    return (LINEBUFFER->status & LINEBUFFER_ST_SPLIT) ? 1u : 0u;
}

/** @brief 当前显示模式的一行字节数（全屏 5760 / 分屏半幅 2880）。 */
u32 lineBuffer_lineBytes(void){
    return s_line_bytes;
}

/** @brief 读加速器侧帧率。 */
u32 lineBuffer_getFpsAcc(void){
    return LINEBUFFER->fps_acc;
}

/** @brief 读 CPU 侧帧率。 */
u32 lineBuffer_getFpsCpu(void){
    return LINEBUFFER->fps_cpu;
}

/** @brief 交帧(GPU2D 侧): 写 1 置位 0x206C(RTL 里信号叫 done_gpu2d)。
 *        W1S 只能置位, 清零由 RTL 在帧边界翻页后做 —— 所以别"读-改-写", 直接写 1。
 */
void lineBuffer_doneGpu2d(void){
    LINEBUFFER->done_gpu2d = 1u;
}

/** @brief 交帧(CPU 侧): 写 1 置位 DONE_CPU。 */
void lineBuffer_doneCpu(void){
    LINEBUFFER->done_cpu = 1u;
}

/** @brief 读 0x206C 当前值(1 = 还挂着没被消费)。 */
u32 lineBuffer_getDoneGpu2d(void){
    return LINEBUFFER->done_gpu2d & 1u;
}

/** @brief 把一块帧缓存灌成纯色。 */
void lineBuffer_fillSolid(u32 base, u32 lines, u8 r, u8 g, u8 b){
    u8 *p = (u8 *)base;
    u32 x, y;

    if(lines > LINEBUFFER_HEIGHT)
        lines = LINEBUFFER_HEIGHT;

    for(y = 0; y < lines; y++){
        u8 *row = p + y * LINEBUFFER_LINE_BYTES;

        for(x = 0; x < LINEBUFFER_WIDTH; x++){
            u8 *q = row + x * LINEBUFFER_PIXEL_BYTES;

            q[0] = b;
            q[1] = g;
            q[2] = r;
        }
    }

    data_cache_invalidate_all();
}

/** @brief 往半幅缓冲里灌纯色(960 列, 行距 2880), 两端各画一条白边。 */
void lineBuffer_fillHalf(u32 base, u32 lines, u8 r, u8 g, u8 b){
    u8 *p = (u8 *)base;
    u32 x, y;

    if(lines > LINEBUFFER_HEIGHT)
        lines = LINEBUFFER_HEIGHT;

    for(y = 0; y < lines; y++){
        u8 *row = p + y * LINEBUFFER_HALF_LINE_BYTES;

        for(x = 0; x < LINEBUFFER_HALF_WIDTH; x++){
            u8 *q = row + x * LINEBUFFER_PIXEL_BYTES;

            if((x == 0u) || (x == (LINEBUFFER_HALF_WIDTH - 1u))){
                q[0] = 0xFF; q[1] = 0xFF; q[2] = 0xFF;      /* 半幅两端: 白边 */
            } else {
                q[0] = b;
                q[1] = g;
                q[2] = r;
            }
        }
    }

    data_cache_invalidate_all();
}

/** @brief 读回帧缓存某个像素(按 0xRRGGBB 拼), 方便和屏上颜色对照。 */
u32 lineBuffer_readPixel(u32 base, u32 x, u32 y){
    const u8 *q;

    if((x >= LINEBUFFER_WIDTH) || (y >= LINEBUFFER_HEIGHT))
        return 0xFFFFFFFFu;

    q = (const u8 *)base + y * LINEBUFFER_LINE_BYTES + x * LINEBUFFER_PIXEL_BYTES;

    data_cache_invalidate_all();        /* 先丢 cache 才读得到 DDR 真实内容 */

    return ((u32)q[2] << 16) | ((u32)q[1] << 8) | (u32)q[0];
}

/*----------------------------------------------------------------------------*/
/* 帧起始中断: 显示子块槽 12/13 = 0x2070 / 0x2074                                */
/*   ★ 用槽位索引硬访问: 新加的寄存器要等重新综合之后 Efinity 生成的 BSP 才会有   */
/*     对应成员名, 先用裸地址不影响功能                                          */
/*----------------------------------------------------------------------------*/
/* 帧起始中断: 显示子块槽 12/13 = 0x2070 / 0x2074                                */
/*----------------------------------------------------------------------------*/
void lineBuffer_IRQ_frameClear(void)
{
    ((volatile u32 *)LINEBUFFER_BASE)[12] = 1u;      /* W1C: 写 1 清事件位 */
}

u32 lineBuffer_IRQ_frameEvt(void)
{
    return ((volatile u32 *)LINEBUFFER_BASE)[12] & 1u;
}

void lineBuffer_IRQ_frameEnable(u32 en)
{
    ((volatile u32 *)LINEBUFFER_BASE)[13] = en ? 1u : 0u;
}

/*----------------------------------------------------------------------------*/
/* 颜色键控(ck): 显示子块槽 14 = 0x2078                                          */
/*   ★ 和 IRQ 那两个一样用【槽位索引】硬访问: 重新综合之前 Efinity 生成的 BSP 里  */
/*     还没有这个成员名, 先用裸地址, 不影响功能                                   */
/*   ★ 复位默认白(0xFFFFFF): 软件不写它 = 老行为(透明像素是白)                     */
/*----------------------------------------------------------------------------*/
void lineBuffer_setCkeyColor(u32 rgb)
{
    ((volatile u32 *)LINEBUFFER_BASE)[14] = rgb & 0x00FFFFFFu;
}

u32 lineBuffer_getCkeyColor(void)
{
    return ((volatile u32 *)LINEBUFFER_BASE)[14] & 0x00FFFFFFu;
}

