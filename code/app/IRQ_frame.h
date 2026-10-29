//==============================================================================
//  IRQ_frame.h —— 显示"帧起始"中断(让 CPU 在硬件帧边界那一刻被叫醒)
//------------------------------------------------------------------------------
//  硬件侧: APB0_gpu2d 里加了 IRQ_FRAME_EVT(粘滞位) + IRQ_FRAME_EN(使能),
//          电平 = 粘滞位 & 使能 -> 顶层 userInterruptA -> PLIC 中断号 16。
//  电平触发 => ISR 里【第一件事】必须清事件位, 不清会一直重进中断。
//==============================================================================
#pragma once

#include "type.h"

/* ★★ 必须先把【新 bitstream】烧进去, 这个开关才能置 1 ★★
 *   0x2070/0x2074 是两个新加的寄存器, 旧 bitstream 里不存在;
 *   旧硬件收到这两个地址会回 PSLVERROR(总线错误) => CPU 异常。
 *   所以: 新 bitstream + 新 app 才置 1; 只下了新 app 一定保持 0, 否则板子起不来。 */
#define IRQ_FRAME_USE   1

void IRQ_trapInstall(void);      /* 只装陷阱入口(mtvec), 不碰任何新寄存器 —— 可以早点调 */
void IRQ_frame_init(void);       /* 开这一路中断(PLIC + CSR); 需要新 bitstream */
u32  IRQ_frame_count(void);      /* 从初始化到现在收到过多少次"帧起始"中断 */
u32  IRQ_frame_maxGap(void);     /* 两次中断之间最长间隔(CPU 周期数) —— 看有没有漏帧 */
void IRQ_frame_isr(void);        /* 中断服务: 只清事件并计数；绘制留在主循环 */
void trap(void);                 /* 陷阱总入口(中断 + 异常), 由 common/trap.S 调用(名字由 trap.S 定) */
