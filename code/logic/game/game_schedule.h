#ifndef GAME_SCHEDULE_H
#define GAME_SCHEDULE_H

#include <stdint.h>

#define GAME_LOGIC_HZ       60u
#define GAME_LOGIC_MAX_STEPS 4u

/* Raw CLINT ticks preserve sub-millisecond phase. The fractional period makes
 * 60 steps consume exactly tick_hz ticks, even when tick_hz is not /60. */
typedef struct {
    uint32_t last_tick;
    uint32_t pending_ticks;
    uint32_t period_phase;
} game_clock_t;

static inline void game_clock_reset(game_clock_t *clock, uint32_t tick)
{
    clock->last_tick = tick;
    clock->pending_ticks = 0u;
    clock->period_phase = GAME_LOGIC_HZ - 1u;
}

/* Bound catch-up after a breakpoint or long stall. Return discarded ticks so
 * a diagnostic can distinguish bounded recovery from a steady simulation. */
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
