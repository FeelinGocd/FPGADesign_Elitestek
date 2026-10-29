/* Diagnostic only: mode 1 owns a fixed visible grid; mode 2 observes the
 * unmodified fight/update path. Both use silent windows and display IRQs. */
#include "game.h"
#if GAME_CAPACITY_AUDIT
#include "IRQ_frame.h"
#include "render_list.h"
#include "bsp.h"

#if GAME_CAPACITY_AUDIT == 1
#if GAME_CAPACITY_START < 1 || GAME_CAPACITY_STOP > ENEMY_CAP || GAME_CAPACITY_START > GAME_CAPACITY_STOP || GAME_CAPACITY_STEP < 1
#error Invalid capacity audit range
#endif
#elif GAME_CAPACITY_AUDIT == 2
#if ENEMY_CAP != 500 || !GAME_CAPACITY_BOSS
#error Real-fight capacity audit requires a 500-enemy Boss configuration
#endif
#if !GAME_BOSS_DEBUG || GAME_TEST_DAMAGE != 0
#error Real-fight capacity audit requires GAME_BOSS_DEBUG=1 and GAME_TEST_DAMAGE=0
#endif
#else
#error Capacity audit mode must be 1 or 2
#endif
#if GAME_CAPACITY_FRAMES < 1
#error Capacity audit windows must contain at least one frame
#endif

#define CAP_WARMUP 30
#define CAP_WINDOWS 3
#define CAP_HIST_BINS 160
#define CAP_BIN_US 200u
#define CAP_BUDGET_US 13333u
static int armed, running, done, level, window, warmup, frames;
static int raw_ok, budget_ok, best_raw, best_budget;
static u32 epoch, last_ms, last_irq, last_drops, start_tick, work_us;
static u32 elapsed, work_sum, work_max, missing, irq_zero, irq_max, drops;
static u32 cam_frames, visible_min, cmd_max;
#if GAME_CAPACITY_AUDIT == 2
static u32 enemy_min, enemy_max, visible_max, dense_frames, phase_bad;

/* Count enemy pixels in the actual rendered viewport, excluding player
 * shots. No synthetic-fill or SKILL_DIAG helper is needed in this mode. */
static int visible_enemies(void)
{
    int i, count = 0;
    for (i = 0; i < N_BULLETS; ++i) {
        const bullet_t *b = &g.bullets[i];
        if (!b->active || !b->enemy) continue;
        if (b->x >= g.cam_x && b->x + BW <= g.cam_x + SCR_W &&
            b->y >= g.cam_y && b->y + BH <= g.cam_y + SCR_H) ++count;
    }
    return count;
}
#endif
static int last_cam_x, last_cam_y;
static unsigned short histogram[CAP_HIST_BINS];
static u32 mark_ticks[3], part_us[4], part_sum[4];

static void reset_window(void)
{
    int i;
    frames = 0; elapsed = work_sum = work_max = missing = irq_zero = 0;
    irq_max = drops = cam_frames = cmd_max = 0; visible_min = N_BULLETS;
#if GAME_CAPACITY_AUDIT == 2
    enemy_min = ENEMY_CAP; enemy_max = visible_max = dense_frames = phase_bad = 0;
#endif
    for (i = 0; i < CAP_HIST_BINS; ++i) histogram[i] = 0;
    for (i = 0; i < 4; ++i) part_sum[i] = 0;
    warmup = CAP_WARMUP;
}

void game_boss_stress_arm(void)
{
    armed = 1; running = done = 0;
#if GAME_CAPACITY_AUDIT == 2
    level = ENEMY_CAP;
#else
    level = GAME_CAPACITY_START;
#endif
    window = best_raw = best_budget = 0;
    raw_ok = budget_ok = 1;
}

int game_boss_stress_step(void)
{
    if (armed && (!GAME_CAPACITY_BOSS || g.phase == PHASE_FIGHT)) {
        armed = 0; running = 1; epoch = game_ms();
#if GAME_CAPACITY_AUDIT == 2
        /* Keep normal automatic fire, while zero test damage and
         * invincibility keep the selected real phase alive for all windows. */
        g.test_mode = 1;
        reset_window();
        bsp_printf("CAPSTART scene=%d mode=2 phase=%d start=%d stop=%d step=0 frames=%d windows=%d\r\n",
                   GAME_CAPACITY_BOSS, GAME_CAPACITY_PHASE, level, level,
                   GAME_CAPACITY_FRAMES, CAP_WINDOWS);
#else
        game_bullets_stress_fill(level); reset_window();
        bsp_printf("CAPSTART scene=%d start=%d stop=%d step=%d frames=%d windows=%d\r\n",
                   GAME_CAPACITY_BOSS, level, GAME_CAPACITY_STOP,
                   GAME_CAPACITY_STEP, GAME_CAPACITY_FRAMES, CAP_WINDOWS);
#endif
    }
#if GAME_CAPACITY_AUDIT == 2
    return 0; /* Observation only: game_update_step must run every subsystem. */
#else
    if (running) g.fire_tick = 0; /* player movement runs, automatic shots do not */
    return running;
#endif
}

void game_capacity_camera(u32 now)
{
    u32 t, p;
    int mx, my;
    if (!running || done) return;
    mx = game_world_w() - SCR_W; my = game_world_h() - SCR_H;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    t = (now - epoch) % 6000u;
    p = t < 500u ? 0u : t < 3000u ? t - 500u : t < 3500u ? 2500u : 6000u - t;
    g.cam_x = (int)(p * (u32)mx / 2500u);
    g.cam_y = (int)(p * (u32)my / 2500u);
}

void game_capacity_work_begin(void)
{
#if GAME_CAPACITY_AUDIT == 2
    if (done) return;
#endif
    start_tick = clint_getTimeLow(BSP_CLINT);
    mark_ticks[0] = mark_ticks[1] = mark_ticks[2] = start_tick;
}

void game_capacity_mark(unsigned phase)
{
#if GAME_CAPACITY_AUDIT == 2
    if (done) return;
#endif
    if (phase < 3u) mark_ticks[phase] = clint_getTimeLow(BSP_CLINT);
}

void game_capacity_work_end(void)
{
#if GAME_CAPACITY_AUDIT == 2
    if (done) return;
#endif
    u32 end = clint_getTimeLow(BSP_CLINT);
    u32 div = (u32)(BSP_CLINT_HZ / 1000000u);
    work_us = (end - start_tick) / div;
    part_us[0] = (mark_ticks[0] - start_tick) / div;
    part_us[1] = (mark_ticks[1] - mark_ticks[0]) / div;
    part_us[2] = (mark_ticks[2] - mark_ticks[1]) / div;
    part_us[3] = (end - mark_ticks[2]) / div;
}

void game_boss_stress_frame(u32 ignored)
{
    u32 now, irq, di, bin, p99, cumulative, target;
    int visible, stable, budget, i;
    (void)ignored;
    if (!running || done) return;
    now = game_ms(); irq = IRQ_frame_count();
    if (warmup) {
        --warmup; last_ms = now; last_irq = irq; last_drops = rl_dropped();
        last_cam_x = g.cam_x; last_cam_y = g.cam_y;
        return;
    }
    elapsed += now - last_ms; last_ms = now;
    di = irq - last_irq; last_irq = irq;
    if (!di) ++irq_zero;
    if (di > 1u) missing += di - 1u;
    if (di > irq_max) irq_max = di;
    drops += rl_dropped() - last_drops; last_drops = rl_dropped();
#if GAME_CAPACITY_AUDIT == 2
    {
        u32 enemies = (u32)game_enemy_bullets_alive();
        if (enemies < enemy_min) enemy_min = enemies;
        if (enemies > enemy_max) enemy_max = enemies;
        if (enemies >= (u32)(ENEMY_CAP - 8)) ++dense_frames;
        if (g.phase != PHASE_FIGHT || g.boss_phase != GAME_CAPACITY_PHASE) ++phase_bad;
    }
    visible = visible_enemies();
    if ((u32)visible > visible_max) visible_max = (u32)visible;
#else
    visible = game_bullets_stress_visible();
#endif
    if ((u32)visible < visible_min) visible_min = (u32)visible;
    if (g.cam_x != last_cam_x || g.cam_y != last_cam_y) ++cam_frames;
    last_cam_x = g.cam_x; last_cam_y = g.cam_y;
    work_sum += work_us;
    for (i = 0; i < 4; ++i) part_sum[i] += part_us[i];
    if (work_us > work_max) work_max = work_us;
    bin = work_us / CAP_BIN_US;
    if (bin >= CAP_HIST_BINS) bin = CAP_HIST_BINS - 1;
    ++histogram[bin];
    if ((u32)g.cmd_frame > cmd_max) cmd_max = (u32)g.cmd_frame;
    if (++frames < GAME_CAPACITY_FRAMES) return;

    target = ((u32)frames * 99u + 99u) / 100u;
    cumulative = p99 = 0;
    for (i = 0; i < CAP_HIST_BINS; ++i) {
        cumulative += histogram[i];
        if (cumulative >= target) { p99 = (u32)(i + 1) * CAP_BIN_US; break; }
    }
    if (i == CAP_HIST_BINS - 1) p99 = work_max;
#if GAME_CAPACITY_AUDIT == 2
    stable = !missing && !irq_zero && !drops && !phase_bad &&
             enemy_max >= (u32)(ENEMY_CAP - 8) && cam_frames * 3u >= (u32)frames;
#else
    stable = !missing && !irq_zero && !drops && visible_min == (u32)level &&
             cam_frames >= (u32)frames / 3u;
#endif
    budget = stable && work_max <= CAP_BUDGET_US;
    if (!stable) raw_ok = 0;
    if (!budget) budget_ok = 0;
    /* Printing happens after the measured window. The next 30 frames are
     * discarded, including UART FIFO drain and buffer-history transition. */
#if GAME_CAPACITY_AUDIT == 2
    bsp_printf("CAP scene=%d mode=2 phase=%d n=%d win=%d frames=%d ms=%d enemy_min=%d enemy_max=%d visible_min=%d visible_max=%d dense_frames=%d cam=%d avg_us=%d p99_us=%d max_us=%d miss=%d irq0=%d irq_max=%d drops=%d phase_bad=%d cmd_max=%d stable=%d budget=%d logic_us=%d prep_us=%d submit_us=%d fence_us=%d\r\n",
               GAME_CAPACITY_BOSS, GAME_CAPACITY_PHASE, level, window + 1, frames, (int)elapsed,
               (int)enemy_min, (int)enemy_max, (int)visible_min, (int)visible_max,
               (int)dense_frames, (int)cam_frames, (int)(work_sum / (u32)frames),
               (int)p99, (int)work_max, (int)missing, (int)irq_zero,
               (int)irq_max, (int)drops, (int)phase_bad, (int)cmd_max, stable, budget,
               (int)(part_sum[0] / (u32)frames), (int)(part_sum[1] / (u32)frames),
               (int)(part_sum[2] / (u32)frames), (int)(part_sum[3] / (u32)frames));
#else
    bsp_printf("CAP scene=%d n=%d win=%d frames=%d ms=%d visible=%d cam=%d avg_us=%d p99_us=%d max_us=%d miss=%d irq0=%d irq_max=%d drops=%d cmd_max=%d stable=%d budget=%d logic_us=%d prep_us=%d submit_us=%d fence_us=%d\r\n",
               GAME_CAPACITY_BOSS, level, window + 1, frames, (int)elapsed,
               (int)visible_min, (int)cam_frames, (int)(work_sum / (u32)frames),
               (int)p99, (int)work_max, (int)missing, (int)irq_zero,
               (int)irq_max, (int)drops, (int)cmd_max, stable, budget,
               (int)(part_sum[0] / (u32)frames), (int)(part_sum[1] / (u32)frames),
               (int)(part_sum[2] / (u32)frames), (int)(part_sum[3] / (u32)frames));
#endif
    if (++window < CAP_WINDOWS) { reset_window(); return; }
    if (raw_ok) best_raw = level;
    if (budget_ok) best_budget = level;
    if (GAME_CAPACITY_AUDIT == 2 || level >= GAME_CAPACITY_STOP) {
        done = 1;
#if GAME_CAPACITY_AUDIT == 2
        bsp_printf("CAPDONE scene=%d mode=2 phase=%d raw=%d reserved=%d budget_us=%d pool=%d\r\n",
                   GAME_CAPACITY_BOSS, GAME_CAPACITY_PHASE, best_raw, best_budget,
                   (int)CAP_BUDGET_US, N_BULLETS);
#else
        bsp_printf("CAPDONE scene=%d raw=%d reserved=%d budget_us=%d pool=%d\r\n",
                   GAME_CAPACITY_BOSS, best_raw, best_budget, (int)CAP_BUDGET_US, N_BULLETS);
#endif
        return;
    }
#if GAME_CAPACITY_AUDIT == 1
    level += GAME_CAPACITY_STEP;
    if (level > GAME_CAPACITY_STOP) level = GAME_CAPACITY_STOP;
    window = 0; raw_ok = budget_ok = 1;
    game_bullets_stress_fill(level); reset_window();
#endif
}
#endif
