//==============================================================================
//  IRQ_frame.c —— 显示"帧起始"中断
//------------------------------------------------------------------------------
//  这一版先只做【计数】: 中断每来一次 +1, 主循环/日志读它, 用来确认
//  "中断确实每个显示帧来一次、而且和软件自己数的帧数一致"。
//  确认无误之后, 再把"移动方块 + 发这一帧的命令 + FENCE"搬进 IRQ_frame_isr()。
//==============================================================================
#include <stdint.h>
#include "bsp.h"
#include "riscv.h"
#include "plic.h"
#include "IRQ_frame.h"
#include "user_lineBuffer.h"

void crash(void);                         /* main.c 里定义 */
void trap_entry(void);                    /* common/trap.S 里定义 */

static volatile u32 g_IRQ_frame_cnt;      /* 收到多少次 */
static volatile u32 g_IRQ_frame_last;     /* 上一次中断的时刻(CPU 周期) */
static volatile u32 g_IRQ_frame_maxgap;   /* 两次中断之间最长间隔 = 看有没有漏帧 */

/*----------------------------------------------------------------------------*/
/* 中断服务: 显示端帧边界                                                        */
/*----------------------------------------------------------------------------*/
void IRQ_frame_isr(void)
{
    u32 now = clint_getTimeLow(BSP_CLINT);
    u32 gap = now - g_IRQ_frame_last;

    /* ★★ 第一件事: 清事件位。电平触发, 不清就永远保持高 -> 会一直重进中断 */
    lineBuffer_IRQ_frameClear();

    g_IRQ_frame_last = now;
    if (gap > g_IRQ_frame_maxgap) g_IRQ_frame_maxgap = gap;
    g_IRQ_frame_cnt++;

    /* 以后这一帧的绘制就放这儿:
     *   移动方块 -> frame_draw() -> CI_gpu2d_fence()
     * 注意: 打印、自检这些慢活【不能】放进来。 */
}

/*----------------------------------------------------------------------------*/
/* 陷阱总入口(中断 + 异常), trap.S 里 call trap                                  */
/*----------------------------------------------------------------------------*/
void trap(void)
{
    int32_t mcause = csr_read(mcause);

    if (mcause < 0 && ((mcause & 0xF) == CAUSE_MACHINE_EXTERNAL)) {
        u32 claim;

        /* 把所有挂着的中断都处理掉(claim 到 0 表示没有了) */
        while ((claim = plic_claim(BSP_PLIC, BSP_PLIC_CPU_0)) != 0u) {
            if (claim == SYSTEM_PLIC_USER_INTERRUPT_A_INTERRUPT)
                IRQ_frame_isr();
            /* 别的源(如果以后加了)在这里分支 */
            plic_release(BSP_PLIC, BSP_PLIC_CPU_0, claim);          /* ★ 不释放就再不来 */
        }
    } else {
        /* 异常。最常见的两个:
         *   CAUSE_MACHINE_LOAD_ACCESS / STORE_ACCESS + mtval = 出错的地址
         *   -> 多半是软件碰了【当前 bitstream 上不存在】的寄存器(回 PSLVERROR)
         * 一定要打印出来, 不然只看到"板子起不来", 完全查不出是哪一步。 */
        bsp_printf("\r\n*** 异常 *** mcause=0x%08x mtval=0x%08x mepc=0x%08x\r\n",
                   (u32)mcause, (u32)csr_read(mtval), (u32)csr_read(mepc));
        bsp_printf("  若是 5/7(读写访问错误): 查是不是访问了当前 bitstream 里没有的寄存器\r\n");
        crash();
    }
}

/* 只装陷阱入口: 不碰任何新寄存器 => 旧 bitstream 上也能安全调用。
 * 早点调用它, 后面的总线错误才会被打印出来而不是把 CPU 打到重启。 */
void IRQ_trapInstall(void)
{
    csr_write(mtvec, trap_entry);
}

/*----------------------------------------------------------------------------*/
/* 初始化: 开这一路中断                                                          */
/*----------------------------------------------------------------------------*/
void IRQ_frame_init(void)
{
    IRQ_trapInstall();                              /* ★ 先装入口: 万一后面踩到总线错误, 能打印出来 */

    g_IRQ_frame_last  = clint_getTimeLow(BSP_CLINT);  /* 免得第一次算出一个假的最大间隔 */
    g_IRQ_frame_maxgap = 0u;
    lineBuffer_IRQ_frameClear();                    /* 先把可能挂着的旧事件清掉 */
    lineBuffer_IRQ_frameEnable(1);                  /* 使能硬件侧的事件 -> 中断 */

    plic_set_threshold(BSP_PLIC, BSP_PLIC_CPU_0, 0);            /* CPU0 接受优先级 > 0 */
    plic_set_enable(BSP_PLIC, BSP_PLIC_CPU_0, SYSTEM_PLIC_USER_INTERRUPT_A_INTERRUPT, 1);
    plic_set_priority(BSP_PLIC, SYSTEM_PLIC_USER_INTERRUPT_A_INTERRUPT, 1);

    csr_write(mtvec, trap_entry);                               /* 陷阱入口 = common/trap.S */
    csr_set(mie, MIE_MEIE);                                     /* 开机器外部中断 */
    csr_write(mstatus, csr_read(mstatus) | MSTATUS_MPP | MSTATUS_MIE);   /* 开全局中断 */
}

u32 IRQ_frame_count(void)  { return g_IRQ_frame_cnt; }
u32 IRQ_frame_maxGap(void) { return g_IRQ_frame_maxgap; }
