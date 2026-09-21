///////////////////////////////////////////////////////////////////////////////////
//  Copyright (c) 2026
//  SPDX license identifier: MIT
//  Full license header bsp/efinix/EfxSapphireSoc/include/LICENSE.MD
///////////////////////////////////////////////////////////////////////////////////

/*******************************************************************************
*
* @file user_lineBuffer.c
*
* @brief 用户 APB "显示通道 lineBuffer"外设驱动的实现。
*
* 对应 RTL:
*     RTL/Peripherial/APB0/APB0_Interconnect.v           (槽 1 = PSEL_lineBuffer)
*     RTL/Peripherial/APB0/APB0_Decoder.v                (PADDR[15:12] == 4'b0001)
*     RTL/Peripherial/APB0/lineBuffer/APB0_lineBuffer.v  (总线层: 寄存器)
*     RTL/Peripherial/LineBuffer/bufferAPBHandler.v      (寄存器 -> 有效信号)
*     RTL/Peripherial/LineBuffer/lineBuffer.v            (功能层整条显示通路)
*
* 地址: APB0 窗口 0xf8100000, 译码到槽 1 (槽内偏移 0x1000 ~ 0x1FFF)
*
******************************************************************************/

#include "user_lineBuffer.h"
#include "io.h"             /* 带来 soc.h 里的 IO_APB_SLAVE_0_INPUT */
#include "vexriscv.h"       /* data_cache_invalidate_all: 写完帧缓存要刷 cache */

/*------------------------------------------------------------------------------
 * 基地址
 *
 *   APB Slave 0 的窗口基址由易灵思按 SoC 配置生成在 soc.h 里 (0xf8100000)。
 *   槽 1 的基址 = 窗口基址 + 0x1000 (和 RTL 的 APB0_Decoder.v 里
 *   "PADDR[15:12] == 4'b0001" 必须一致, 改一处就要改另一处)。
 *----------------------------------------------------------------------------*/
#ifdef IO_APB_SLAVE_0_INPUT
    #define LINEBUFFER_BASE     (IO_APB_SLAVE_0_INPUT + 0x1000u)
#else
    #error "APB Slave 0 is disabled, please enable it in IP Manager!!!"
#endif

/*------------------------------------------------------------------------------
 * 寄存器映射
 *
 *   成员顺序 = 地址偏移顺序。一个字都不能改、不能删、不能插空位 ——
 *   结构体是靠偏移量访问硬件的, 顺序错了读写的就是别的寄存器。
 *
 *   槽内偏移  成员              读写   说明
 *   ---------------------------------------------------------------------------
 *   0x00      id                RO     恒为 0x4C423031 ("LB01")
 *   0x04      framea_base_acc   RW     缓冲 A·左半(加速器)
 *   0x08      framea_base_cpu   RW     缓冲 A·右半(CPU)
 *   0x0C      frameb_base_acc   RW     缓冲 B·左半
 *   0x10      frameb_base_cpu   RW     缓冲 B·右半
 *   0x14      framec_full_base  RW     全屏 C
 *   0x18      framed_full_base  RW     全屏 D
 *   0x1C      status            RW     bit0=ENABLE bit1=SPLIT
 *   0x20      fps_acc           RO     加速器帧率
 *   0x24      fps_cpu           RO     CPU 帧率
 *   0x28      done_cpu          RW1S   写 1 置位
 *   0x2C      done_acc          RW1S   写 1 置位
 *
 *   volatile 不能省: 每次访问都必须真去总线, 不允许编译器缓存或合并。
 *
 *   !! 以后要在 0x2C 之后加寄存器, 必须先补一个占位成员, 不能直接往后追加:
 *          volatile u32 reserved;      // 0x30 占位
 *          volatile u32 new_reg;       // 0x34 新寄存器
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
    volatile u32 done_acc;          /* 0x2C  RW1S */
} LINEBUFFERTYPE;

/* 把基地址当成 LINEBUFFERTYPE 结构体的首地址 */
#define LINEBUFFER          ((LINEBUFFERTYPE *)LINEBUFFER_BASE)


/*==============================================================================
 * API 实现
 *============================================================================*/

/*******************************************************************************
*
* @brief 读外设 ID, 用来确认这个 APB 外设确实活着。
*
******************************************************************************/
u32 lineBuffer_getId(void){
    return LINEBUFFER->id;
}

/*******************************************************************************
*
* @brief 设置全屏模式的两块显示缓冲基址(C / D)。
*
******************************************************************************/
void lineBuffer_setFullBase(u32 base_c, u32 base_d){
    LINEBUFFER->framec_full_base = base_c;
    LINEBUFFER->framed_full_base = base_d;
}

/*******************************************************************************
*
* @brief 设置分屏模式的两块显示缓冲基址(A / B), 每块都是"左半 + 右半"。
*
******************************************************************************/
void lineBuffer_setSplitBase(u32 a_acc, u32 a_cpu, u32 b_acc, u32 b_cpu){
    LINEBUFFER->framea_base_acc = a_acc;
    LINEBUFFER->framea_base_cpu = a_cpu;
    LINEBUFFER->frameb_base_acc = b_acc;
    LINEBUFFER->frameb_base_cpu = b_cpu;
}

/*******************************************************************************
*
* @brief 开/关显示通道。
*
* @note ENABLE 在 RTL 里不锁存, 写 0 立刻停取数; 但 STATUS 是寄存器的值,
*       读回来就是刚写进去的那个值。
*
******************************************************************************/
void lineBuffer_enable(u32 en){
    u32 s = LINEBUFFER->status;

    if(en)
        s |=  LINEBUFFER_ST_ENABLE;
    else
        s &= ~LINEBUFFER_ST_ENABLE;

    LINEBUFFER->status = s;
}

/*******************************************************************************
*
* @brief 请求切换全屏/分屏模式(帧边界生效)。
*
******************************************************************************/
void lineBuffer_setSplit(u32 split){
    u32 s = LINEBUFFER->status;

    if(split)
        s |=  LINEBUFFER_ST_SPLIT;
    else
        s &= ~LINEBUFFER_ST_SPLIT;

    LINEBUFFER->status = s;
}

/*******************************************************************************
*
* @brief 读 STATUS 原值。
*
******************************************************************************/
u32 lineBuffer_getStatus(void){
    return LINEBUFFER->status;
}

/*******************************************************************************
*
* @brief 显示通道是否已使能。
*
******************************************************************************/
u32 lineBuffer_isEnabled(void){
    return (LINEBUFFER->status & LINEBUFFER_ST_ENABLE) ? 1u : 0u;
}

/*******************************************************************************
*
* @brief 当前请求的模式是否为分屏。
*
******************************************************************************/
u32 lineBuffer_isSplit(void){
    return (LINEBUFFER->status & LINEBUFFER_ST_SPLIT) ? 1u : 0u;
}

/*******************************************************************************
*
* @brief 读加速器侧帧率。
*
******************************************************************************/
u32 lineBuffer_getFpsAcc(void){
    return LINEBUFFER->fps_acc;
}

/*******************************************************************************
*
* @brief 读 CPU 侧帧率。
*
******************************************************************************/
u32 lineBuffer_getFpsCpu(void){
    return LINEBUFFER->fps_cpu;
}

/*******************************************************************************
*
* @brief 交帧(加速器侧): 写 1 置位 DONE_ACC。
*
* @note 这是 W1S(写 1 置位): 只能置位, 清零由 RTL 的 handler 在帧边界翻页后
*       发 done_clr 完成。所以这里千万不要"读-改-写", 直接写 1 就行。
*
******************************************************************************/
void lineBuffer_doneAcc(void){
    LINEBUFFER->done_acc = 1u;
}

/*******************************************************************************
*
* @brief 交帧(CPU 侧): 写 1 置位 DONE_CPU。
*
******************************************************************************/
void lineBuffer_doneCpu(void){
    LINEBUFFER->done_cpu = 1u;
}

/*******************************************************************************
*
* @brief 读 DONE_ACC 的当前值。
*
******************************************************************************/
u32 lineBuffer_getDoneAcc(void){
    return LINEBUFFER->done_acc & 1u;
}

/*******************************************************************************
*
* @brief 往一块帧缓存里灌上板验证图案。
*
* @note 字节约顺序是 B, G, R(见 user_lineBuffer.h 的说明)。
*       这里按字节写: 3 字节步长不是 4 的倍数, 不能用 u32* 逐像素写,
*       否则会踩掉相邻像素。
*
******************************************************************************/
void lineBuffer_fillTestPattern(u32 base, u32 lines){
    u8 *p = (u8 *)base;
    u32 x, y;

    if(lines > LINEBUFFER_HEIGHT)
        lines = LINEBUFFER_HEIGHT;

    for(y = 0; y < lines; y++){
        u8 *row = p + y * LINEBUFFER_LINE_BYTES;

        for(x = 0; x < LINEBUFFER_WIDTH; x++){
            u8 *q = row + x * LINEBUFFER_PIXEL_BYTES;

            if(y == 0){
                q[0] = 0xFF;                    /* B  第 0 行整行纯白: 看行对齐 */
                q[1] = 0xFF;                    /* G */
                q[2] = 0xFF;                    /* R */
            } else {
                q[0] = 0x20;                                        /* B  固定 */
                q[1] = (u8)((y * 255u) / (lines ? lines : 1u));     /* G  随行变(看纵向/行距) */
                q[2] = (u8)(x >> 3);                                /* R  随列变(看横向/像素序) */
            }
        }
    }

    /* ★ 写完必须刷一次 cache: 不刷的话数据可能还压在 CPU cache 里,
     *   FPGA 从 DDR 读到的是旧内容(现象: 屏上是上一次的图或花屏)。
     *   如果换了 BSP 版本这句编译不过, 用对应的 cache flush 接口替换即可。 */
    data_cache_invalidate_all();
}

/*******************************************************************************
*
* @brief 排错专用: 灌"坐标标记"图案(黑底 + 四角块 + 十字中线 + 顶行白线)。
*
******************************************************************************/
void lineBuffer_fillMarkerPattern(u32 base){
    u8 *p = (u8 *)base;
    u32 x, y;

    for(y = 0; y < LINEBUFFER_HEIGHT; y++){
        u8 *row = p + y * LINEBUFFER_LINE_BYTES;

        for(x = 0; x < LINEBUFFER_WIDTH; x++){
            u8 *q = row + x * LINEBUFFER_PIXEL_BYTES;
            u8 b = 0x00, g = 0x00, r = 0x00;

            /* 四个角块 120x120 */
            if((x < 120u) && (y < 120u)){
                r = 0xFF;                                       /* 左上 红 */
            } else if((x >= (LINEBUFFER_WIDTH - 120u)) && (y < 120u)){
                g = 0xFF;                                       /* 右上 绿 */
            } else if((x < 120u) && (y >= (LINEBUFFER_HEIGHT - 120u))){
                b = 0xFF;                                       /* 左下 蓝 */
            } else if((x >= (LINEBUFFER_WIDTH - 120u)) && (y >= (LINEBUFFER_HEIGHT - 120u))){
                r = 0xFF; g = 0xFF; b = 0xFF;                   /* 右下 白 */
            }

            /* 中线(后写, 覆盖角块) */
            if((x == 959u) || (x == 960u)){
                r = 0xFF; g = 0xFF; b = 0x00;                   /* 黄竖线 */
            }
            if((y == 539u) || (y == 540u)){
                r = 0xFF; g = 0x00; b = 0xFF;                   /* 洋红横线 */
            }
            if(y == 0u){
                r = 0xFF; g = 0xFF; b = 0xFF;                   /* 顶行白线(行对齐标记) */
            }

            q[0] = b;
            q[1] = g;
            q[2] = r;
        }
    }

    data_cache_invalidate_all();
}

/*******************************************************************************
*
* @brief 排错专用: 灌"标尺"图案(竖彩色线 + 白横线), 用来量映射。
*
******************************************************************************/
void lineBuffer_fillRulerPattern(u32 base){
    /* 8 根竖线的颜色, 顺序 B,G,R —— 依次对应 x = 0,256,...,1792 */
    static const u8 vcol[8][3] = {
        { 0xFF, 0xFF, 0xFF },       /* x=0     白 */
        { 0x00, 0x00, 0xFF },       /* x=256   红 */
        { 0x00, 0xFF, 0x00 },       /* x=512   绿 */
        { 0xFF, 0x00, 0x00 },       /* x=768   蓝 */
        { 0x00, 0xFF, 0xFF },       /* x=1024  黄 */
        { 0xFF, 0xFF, 0x00 },       /* x=1280  青 */
        { 0xFF, 0x00, 0xFF },       /* x=1536  品红 */
        { 0x00, 0x80, 0xFF },       /* x=1792  橙 */
    };
    /* 5 根横线的行号 */
    static const u32 hrow[5] = { 0u, 270u, 540u, 810u, 1077u };

    u8 *p = (u8 *)base;
    u32 x, y, k;

    for(y = 0; y < LINEBUFFER_HEIGHT; y++){
        u8 *row = p + y * LINEBUFFER_LINE_BYTES;

        for(x = 0; x < LINEBUFFER_WIDTH; x++){
            u8 *q = row + x * LINEBUFFER_PIXEL_BYTES;

            q[0] = 0x00;
            q[1] = 0x00;
            q[2] = 0x00;
        }

        /* 白横线(4 行宽) */
        for(k = 0; k < 5u; k++){
            if((y >= hrow[k]) && (y < (hrow[k] + 4u))){
                for(x = 0; x < LINEBUFFER_WIDTH; x++){
                    u8 *q = row + x * LINEBUFFER_PIXEL_BYTES;

                    q[0] = 0xFF;
                    q[1] = 0xFF;
                    q[2] = 0xFF;
                }
            }
        }
    }

    /* 竖线最后画(4 像素宽), 盖过横线, 这样竖线位置最好认 */
    for(k = 0; k < 8u; k++){
        for(y = 0; y < LINEBUFFER_HEIGHT; y++){
            u8 *row = p + y * LINEBUFFER_LINE_BYTES;

            for(x = k * 256u; x < (k * 256u + 4u); x++){
                u8 *q = row + x * LINEBUFFER_PIXEL_BYTES;

                q[0] = vcol[k][0];
                q[1] = vcol[k][1];
                q[2] = vcol[k][2];
            }
        }
    }

    data_cache_invalidate_all();
}

/*******************************************************************************
*
* @brief 把一块帧缓存灌成纯色。
*
******************************************************************************/
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

/*******************************************************************************
*
* @brief 往一块半幅缓冲里灌纯色(960 列, 行距 2880), 两端各画一条白边。
*
******************************************************************************/
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

/*******************************************************************************
*
* @brief 把帧缓存里的某个像素读回来(按 0xRRGGBB 拼, 方便打印对照)。
*
******************************************************************************/
u32 lineBuffer_readPixel(u32 base, u32 x, u32 y){
    const u8 *q;

    if((x >= LINEBUFFER_WIDTH) || (y >= LINEBUFFER_HEIGHT))
        return 0xFFFFFFFFu;

    q = (const u8 *)base + y * LINEBUFFER_LINE_BYTES + x * LINEBUFFER_PIXEL_BYTES;

    data_cache_invalidate_all();        /* 先丢 cache, 读到的才是 DDR 里的真实内容 */

    return ((u32)q[2] << 16) | ((u32)q[1] << 8) | (u32)q[0];
}

/*******************************************************************************
*
* @brief 往一块帧缓存里灌 EBU 8 竖彩条。
*
* @note 表里的顺序是【B, G, R】(内存里的字节顺序, 见 user_lineBuffer.h)。
*
******************************************************************************/
void lineBuffer_fillColorBars(u32 base, u32 lines){
    /* EBU 顺序: 白 黄 青 绿 品红 红 蓝 黑 —— 每项都是 {B, G, R} */
    static const u8 bar[8][3] = {
        { 0xFF, 0xFF, 0xFF },       /* 白 */
        { 0x00, 0xFF, 0xFF },       /* 黄 */
        { 0xFF, 0xFF, 0x00 },       /* 青 */
        { 0x00, 0xFF, 0x00 },       /* 绿 */
        { 0xFF, 0x00, 0xFF },       /* 品红 */
        { 0x00, 0x00, 0xFF },       /* 红 */
        { 0xFF, 0x00, 0x00 },       /* 蓝 */
        { 0x00, 0x00, 0x00 }        /* 黑 */
    };
    u8 *p = (u8 *)base;
    u32 b, x, y;

    if(lines > LINEBUFFER_HEIGHT)
        lines = LINEBUFFER_HEIGHT;

    for(y = 0; y < lines; y++){
        u8 *row = p + y * LINEBUFFER_LINE_BYTES;

        /* 按条循环, 不做除法: 每条 1920/8 = 240 列 */
        for(b = 0; b < 8u; b++){
            for(x = b * 240u; x < (b + 1u) * 240u; x++){
                u8 *q = row + x * LINEBUFFER_PIXEL_BYTES;

                q[0] = bar[b][0];
                q[1] = bar[b][1];
                q[2] = bar[b][2];
            }
        }
    }

    data_cache_invalidate_all();
}
