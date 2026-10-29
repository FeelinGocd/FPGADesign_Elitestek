//==============================================================================
//  game_schedule.h —— 60 Hz 固定步长逻辑时钟
//------------------------------------------------------------------------------
//  干什么      : 把 CLINT 原始 tick 换算成固定的 60 Hz 逻辑步，并限制追帧数量。
//  上一级是谁  : game_main.c、game_state.c
//  接口契约    :
//    · game_clock_feed() 只累计时间并返回因卡顿被丢弃的 tick；
//    · game_clock_take() 每成功返回 1 一次，调用方执行一步模型更新；
//    · 余数保存在 period_phase 中，避免长期漂移；
//    · 32 位 tick 回绕由无符号减法自然处理。
//  为什么这样做:
//    · 绘制与显示仍按实际帧率运行，玩法和碰撞保持确定的 60 Hz；
//    · 断点或长停顿时最多追 4 步，避免一次补过多帧造成连锁卡顿。
//==============================================================================
#ifndef GAME_SCHEDULE_H
#define GAME_SCHEDULE_H

#include <stdint.h>

#define GAME_LOGIC_HZ       60u
#define GAME_LOGIC_MAX_STEPS 4u

/* 使用 CLINT 原始 tick 保留亚毫秒相位。
 * period_phase 保存不能整除的余数，保证 60 步累计恰好消耗 tick_hz 个 tick。 */
typedef struct {
    uint32_t last_tick;      /* 上一次读取的 CLINT tick */
    uint32_t pending_ticks;  /* 尚未消费的逻辑时间 */
    uint32_t period_phase;   /* 60 分频周期内的余数相位 */
} game_clock_t;

static inline void game_clock_reset(game_clock_t *clock, uint32_t tick)
{
    clock->last_tick = tick;
    clock->pending_ticks = 0u;
    clock->period_phase = GAME_LOGIC_HZ - 1u;
}

/* 限制断点或长停顿后的追帧量，并返回被丢弃的 tick。
 * 诊断代码据此区分“有界恢复”和“稳定运行”。 */
static inline uint32_t game_clock_feed(game_clock_t *clock, uint32_t tick,
                                       uint32_t tick_hz)
{
    uint32_t elapsed = tick - clock->last_tick; /* also handles the 32-bit wrap */
    uint32_t limit = ((tick_hz + GAME_LOGIC_HZ - 1u) / GAME_LOGIC_HZ)
                   * GAME_LOGIC_MAX_STEPS;
    uint32_t discarded = 0u;
    clock->last_tick = tick;
    if (elapsed > limit - clock->pending_ticks) {
        discarded = elapsed - (limit - clock->pending_ticks);
        clock->pending_ticks = limit;
    } else {
        clock->pending_ticks += elapsed;
    }
    return discarded;
}

static inline int game_clock_take(game_clock_t *clock, uint32_t tick_hz)
{
    uint32_t period = tick_hz / GAME_LOGIC_HZ;
    uint32_t phase = clock->period_phase + tick_hz % GAME_LOGIC_HZ;
    if (phase >= GAME_LOGIC_HZ) {
        ++period;
        phase -= GAME_LOGIC_HZ;
    }
    if (clock->pending_ticks < period) return 0;
    clock->pending_ticks -= period;
    clock->period_phase = phase;
    return 1;
}

#endif
