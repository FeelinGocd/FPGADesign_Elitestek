///////////////////////////////////////////////////////////////////////////////////
//  Copyright (c) 2026
//  SPDX license identifier: MIT
///////////////////////////////////////////////////////////////////////////////////

/*******************************************************************************
*
* @file user_gpu2d.c
*
* @brief 2D 加速器(GPU_2D)只读观测寄存器的驱动实现(说明见 user_gpu2d.h)。
*
* RTL: APB0_GPU_2D.v(总线层 + 寄存器格子) + GPU2D_APBHandler.v(把事件数成值)。
*
******************************************************************************/

#include "user_gpu2d.h"
#include "io.h"             /* 带来 soc.h 里的 IO_APB_SLAVE_0_INPUT */
#include "bsp.h"            /* bsp_uDelay: 超时轮询的时间粒度 */
#include "user_lineBuffer.h"  /* 大块绘制纪律要用 lineBuffer_enable */

/*------------------------------------------------------------------------------
 * 基地址 = APB Slave 0 窗口(soc.h, 0xf8100000) + 0x2000(槽 2)。
 * 和 RTL 必须一致, 改一处要改两处: 槽 2 = PADDR[15:12]==4'b0010。
 *----------------------------------------------------------------------------*/
#ifdef IO_APB_SLAVE_0_INPUT
    #define GPU2D_BASE          (IO_APB_SLAVE_0_INPUT + 0x2000u)
#else
    #error "APB Slave 0 is disabled, please enable it in IP Manager!!!"
#endif

/*------------------------------------------------------------------------------
 * 寄存器映射(偏移和含义见下面每个成员的注释)
 *
 *   顺序 = 偏移顺序, 一个字都不能改/删/插空位 —— 结构体靠偏移量访问硬件。
 *   volatile 不能省: 每次访问都必须真去总线, 不许编译器缓存或合并。
 *   全是只读: 写没有意义(硬件忽略), 也没有清零位。
 *   !! 以后在 0x18 之后加寄存器, 必须先补一个 volatile u32 reserved 占位再往后加。
 *----------------------------------------------------------------------------*/
typedef struct {
    volatile u32 id;            /* 0x00  RO */
    volatile u32 status;        /* 0x04  RO  bit0 = BUSY */
    volatile u32 cmd_done;      /* 0x08  RO */
    volatile u32 frame_done;    /* 0x0C  RO */
    volatile u32 wr_beats;      /* 0x10  RO */
    volatile u32 rd_beats;      /* 0x14  RO */
    volatile u32 busy_cyc;      /* 0x18  RO */
} GPU2DTYPE;

#define GPU2D               ((GPU2DTYPE *)GPU2D_BASE)

/*------------------------------------------------------------------------------
 * 观测读数
 *----------------------------------------------------------------------------*/

/** @brief 读 ID —— 确认 APB 这条槽是通的。 */
u32 gpu2d_getId(void)
{
    return GPU2D->id;
}

/** @brief 现在忙不忙。 */
u32 gpu2d_isBusy(void)
{
    return (GPU2D->status & GPU2D_ST_BUSY) ? 1u : 0u;
}

/** @brief 已完成命令数(只增不减)。 */
u32 gpu2d_getCmdDone(void)
{
    return GPU2D->cmd_done;
}

/** @brief 已完成帧数(= FENCE 完成次数)。 */
u32 gpu2d_getFrameDone(void)
{
    return GPU2D->frame_done;
}

/** @brief 一次读全(顺序无所谓, 但一起读出来的那一组更整齐)。 */
void gpu2d_getStats(gpu2d_stats_t *s)
{
    if (s == 0) return;

    s->cmd_done   = GPU2D->cmd_done;
    s->frame_done = GPU2D->frame_done;
    s->wr_beats   = GPU2D->wr_beats;
    s->rd_beats   = GPU2D->rd_beats;
    s->busy_cyc   = GPU2D->busy_cyc;
    s->busy       = (GPU2D->status & GPU2D_ST_BUSY) ? 1u : 0u;
}

/*------------------------------------------------------------------------------
 * 等待
 *
 *   判据是"计数 >= 目标", 不是"计数 != 快照" —— 计数器只增不减, 写成 != 快照
 *   的话只完成一部分也会被判成全部完成(踩过: 连发 20 条完成 1 条就打印"全完成")。
 *   必须带超时: 加速器卡住时死等会让整块板子变哑, 串口什么都不打。
 *----------------------------------------------------------------------------*/

/** @brief 等到 命令完成数 >= target。 */
int gpu2d_waitCmdDone(u32 target, u32 timeout_us)
{
    u32 waited = 0u;

    /* 计数只增不减, 所以"还差几条"用有符号差值判断, 天然不怕回绕 */
    while ((int)(target - gpu2d_getCmdDone()) > 0) {
        if (waited >= timeout_us) return -1;
        bsp_uDelay(1u);                 /* 1 us 粒度让出, 别把总线占死 */
        waited++;
    }
    return 0;
}

/** @brief 等到"再完成至少一条命令"—— 等价于 waitCmdDone(before + 1)。 */
int gpu2d_waitDone(u32 before, u32 timeout_us)
{
    return gpu2d_waitCmdDone(before + 1u, timeout_us);
}

/** @brief 大块绘制纪律第一步: 关显示, 让显示取数流水线排空(为什么见 user_gpu2d.h)。 */
void gpu2d_exclusive_begin(void)
{
    lineBuffer_enable(0);            /* 关显示: 取数停止, 流水线排空 */
}

/** @brief 大块绘制收尾: 等画完, 再放开显示(取数从帧边界重新对齐)。 */
int gpu2d_exclusive_end(u32 target)
{
    int r = gpu2d_waitCmdDone(target, 500000);   /* 等这一批真的画完 */
    lineBuffer_enable(1);                        /* 再放开显示 */
    return r;
}

/** @brief 等到 帧完成数 >= before + 1(= 一整帧画完了)。 */
int gpu2d_waitFrame(u32 before, u32 timeout_us)
{
    u32 target = before + 1u;
    u32 waited = 0u;

    while ((int)(target - gpu2d_getFrameDone()) > 0) {
        if (waited >= timeout_us) return -1;
        bsp_uDelay(1u);
        waited++;
    }
    return 0;
}