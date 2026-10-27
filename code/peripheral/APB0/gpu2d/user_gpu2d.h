///////////////////////////////////////////////////////////////////////////////////
//  Copyright (c) 2026
//  SPDX license identifier: MIT
///////////////////////////////////////////////////////////////////////////////////

/*******************************************************************************
*
* @file user_gpu2d.h
*
* @brief 2D 加速器(GPU_2D)观测寄存器 —— 对外 API(实现见 user_gpu2d.c)。
*
* 地址: APB0 窗口 0xf8100000 的槽 2 内【观测子块】0x2000 ~ 0x2018(观测侧 PADDR[6]=0):
*   0x2000 ID | 0x2004 STATUS | 0x2008 CMD_DONE | 0x200C FRAME_DONE
*   0x2010 WR_BEATS | 0x2014 RD_BEATS | 0x2018 BUSY_CYC, 全是只读。
*   同槽 0x2040 ~ 0x2078 是显示通道寄存器(user_lineBuffer.h)。
*
* 绘图命令走【CI】(peripheral/CInstruction/CI_gpu2d.h, 有顺序保证);
* 本文件走【APB】, 只用来配置/看状态 —— 没有顺序保证, 绝不能拿它发命令。
* 计数器只增不减(没有清零位), 要"等某条命令画完"就记快照再轮询(见 gpu2d_waitCmdDone)。
*
******************************************************************************/

#pragma once

#include "type.h"

/** 硬件对 GPU2D_ID 回的固定标识, "GP2D" 的 ASCII */
#define GPU2D_ID_VALUE      0x47503244u

/** STATUS 的位 */
#define GPU2D_ST_BUSY       0x00000001u     /* bit0 = 有命令没做完 */

/** 一拍 AXI = 多少字节(算带宽用: 带宽 = 拍数 * 16 / 时间) */
#define GPU2D_BEAT_BYTES    16u

/** @brief 一次读全的观测值(几个计数器分几次读会不同步, 一起读更整齐)。 */
typedef struct {
    u32 cmd_done;       /* 已完成命令数 */
    u32 frame_done;     /* 已完成帧数(FENCE 完成次数) */
    u32 wr_beats;       /* 已写出的 AXI 拍数 */
    u32 rd_beats;       /* 已读回的 AXI 拍数 */
    u32 busy_cyc;       /* 忙的周期数 */
    u32 busy;           /* 当前忙不忙(0/1) */
} gpu2d_stats_t;

/** @brief 确认 APB 通路是否打通: 读 ID 寄存器。
 * @return 正常恒为 GPU2D_ID_VALUE; 读到别的值说明槽号/译码有问题, 别去怀疑"命令发错了"。
 */
u32  gpu2d_getId(void);

/** @brief 现在还有没有命令没做完: 1 = 忙, 0 = 空。 */
u32  gpu2d_isBusy(void);

/** @brief 已经做完了几条命令(只增不减, 不因为复位以外的原因清零)。 */
u32  gpu2d_getCmdDone(void);

/** @brief 已经交了几帧(= FENCE 完成次数); 和显示侧 FPS_ACC 数的是同一个事件。 */
u32  gpu2d_getFrameDone(void);

/** @brief 一次读全观测值。 */
void gpu2d_getStats(gpu2d_stats_t *s);

/** @brief 等到命令完成数【到达】target(判据是 计数 >= target)。
 * @param target     目标计数 = 发命令前的快照 + 这次一共要等几条
 * @param timeout_us 超时(微秒); @return 0 = 到了, -1 = 超时(加速器可能卡住了)
 *
 * 判据必须是 ">= target", 不能是 "!= 快照": 否则连发 20 条只要完成 1 条就会被判成
 * "全完成", 之后读到的 WR_BEATS/BUSY_CYC 全是半截数(踩过一次, 记在这里)。
 * 用法: u32 before = gpu2d_getCmdDone(); 连发 N 条(走 CI); gpu2d_waitCmdDone(before + N, 3000000);
 * 一定要带超时 —— 加速器卡住时死等会让整块板子变哑, 串口什么都不打。
 */
int  gpu2d_waitCmdDone(u32 target, u32 timeout_us);

/** @brief 大块绘制纪律: 关显示 -> 画 -> 等画完 -> 再开显示。
 * 为什么: 显示取数是"现用现取"(片上只缓存两行), DDR 被大量写请求占住时会被饿到;
 * 一饿, 读侧就比行边界落后, 而且【自己追不回来】=> 之后整幅画面永久错位
 * (踩过: 横向错开约 520 像素)。关显示时流水线排空, 再使能时取数从帧边界干净起步。
 * 用法(整屏铺底/整屏重画): u32 t = gpu2d_getCmdDone(); gpu2d_exclusive_begin();
 * 连发这批命令; gpu2d_exclusive_end(t + N)。脏区小更新【不用】关显示: 量小饿不到。
 */
void gpu2d_exclusive_begin(void);

/** @brief 大块绘制收尾: 等这一批命令真的画完, 再放开显示。
 * @param target 目标命令完成数(= 发命令前快照 + 命令条数)
 * @return 0 = 正常, -1 = 超时 —— 超时也会放开显示, 不会把屏幕永远关着。
 */
int  gpu2d_exclusive_end(u32 target);

/** @brief 等到某条命令真的画完(等价于 gpu2d_waitCmdDone(before + 1))。
 * @param before     发命令【之前】读的 gpu2d_getCmdDone() 快照
 * @param timeout_us 超时(微秒); @return 0 = 等到了, -1 = 超时。
 * 用法: u32 before = gpu2d_getCmdDone(); CI_gpu2d_blitRect(...);
 *       if (gpu2d_waitDone(before, 200000) != 0) { 报错 }
 */
int  gpu2d_waitDone(u32 before, u32 timeout_us);

/** @brief 等到画完【一整帧】(FENCE 完成次数 +1)。
 * @param before 发这一帧命令之前读的 gpu2d_getFrameDone() 快照; timeout_us 超时(微秒)
 * 一帧通常是"一堆绘制命令 + 末尾一条 FENCE", 所以要等帧数不是命令数。
 * 不急着轮询也可以: FENCE 完成会硬件拉 acc_frame_done, 显示侧在帧边界自动翻页。
 */
int  gpu2d_waitFrame(u32 before, u32 timeout_us);