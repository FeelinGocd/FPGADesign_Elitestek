#include "game.h"
#include "IRQ_frame.h"
#include "bsp.h"
#include "render_list.h"
#include "user_gpu2d.h"

#if GAME_BOSS_BULLET_SWEEP && !GAME_CAPACITY_AUDIT
#if !GAME_BOSS_DEBUG
#error GAME_BOSS_BULLET_SWEEP requires GAME_BOSS_DEBUG=1
#endif
#if GAME_BOSS_SWEEP_START < 16 || GAME_BOSS_SWEEP_START > GAME_BOSS_SWEEP_HOLD_LEVEL || GAME_BOSS_SWEEP_HOLD_LEVEL > N_BULLETS
#error Sweep start/hold levels must be ordered inside the bullet pool
#endif

/* 120 measured frames per level; the first 30 frames after changing levels
 * are ignored so framebuffer history and the single UART report settle. */
#define SWEEP_WARMUP 30
#define SWEEP_FRAMES 120
#define SWEEP_STEP 16
#define SWEEP_FINE_STEP 4

static int s_armed, s_running, s_holding, s_level, s_best, s_best_once;
static int s_warmup, s_frames, s_bad, s_gap_max, s_irq_max, s_visible_min;
static int s_repeat, s_level_all_pass, s_cmd_max;
static int s_hold_started, s_hold_reported, s_hold_move_frames, s_hold_player_frames;
static int s_hold_cam_x, s_hold_cam_y, s_hold_player_x, s_hold_player_y;
static u32 s_last_ms, s_last_irq, s_elapsed, s_cmd_sum, s_drops;
static u32 s_last_drops;
static gpu2d_stats_t s_hw_start;

void game_boss_stress_arm(void)
{
    s_armed = 1;
    s_running = 0;
    s_holding = 0;
    s_best = 0;
    s_best_once = 0;
}

/* Return 1 while the synthetic load owns the Boss fight. */
int game_boss_stress_step(void)
{
    if (!s_armed && s_running && g.phase != PHASE_FIGHT) {
        s_running = 0;
        s_holding = 0;
        game_bullets_clear();
        return 0;
    }
    if (s_armed && g.phase == PHASE_FIGHT) {
        s_armed = 0;
        s_running = 1;
        s_holding = 0;
        s_level = GAME_BOSS_SWEEP_START;
        s_warmup = SWEEP_WARMUP;
        s_frames = 0;
        s_repeat = 0;
        s_level_all_pass = 1;
        s_last_ms = 0;
        game_bullets_stress_fill(s_level);
        bsp_printf("BULLET SWEEP: start=%d hold=%d pool=%d, %d frames/window\r\n",
                   s_level, GAME_BOSS_SWEEP_HOLD_LEVEL, N_BULLETS, SWEEP_FRAMES);
    }
    return s_running;
}

void game_boss_stress_frame(u32 now)
{
    u32 irq;
    u32 dt, di;
    if (!s_running) return;
    (void)now;
    now = game_ms();
    irq = IRQ_frame_count();
    if (s_holding) {
        int moved = g.cam_x != s_hold_cam_x || g.cam_y != s_hold_cam_y;
        int player_moved = g.x != s_hold_player_x || g.y != s_hold_player_y;
        int visible;
        s_hold_cam_x = g.cam_x;
        s_hold_cam_y = g.cam_y;
        s_hold_player_x = g.x;
        s_hold_player_y = g.y;
        if (s_hold_reported) return;
        if (!s_hold_started) {
            if (!moved && !player_moved) return;
            s_hold_started = 1;
            s_frames = 0;
            s_elapsed = 0;
            s_bad = 0;
            s_gap_max = 0;
            s_irq_max = 0;
            s_cmd_sum = 0;
            s_cmd_max = 0;
            s_visible_min = N_BULLETS;
            s_hold_move_frames = 0;
            s_hold_player_frames = 0;
            s_last_ms = now;
            s_last_irq = irq;
            gpu2d_getStats(&s_hw_start);
            return;
        }
        dt = now - s_last_ms;
        di = irq - s_last_irq;
        s_last_ms = now;
        s_last_irq = irq;
        s_elapsed += dt;
        s_cmd_sum += (u32)g.cmd_frame;
        if (g.cmd_frame > s_cmd_max) s_cmd_max = g.cmd_frame;
        if ((int)dt > s_gap_max) s_gap_max = (int)dt;
        if ((int)di > s_irq_max) s_irq_max = (int)di;
        visible = game_bullets_stress_visible();
        if (visible < s_visible_min) s_visible_min = visible;
        if (moved) ++s_hold_move_frames;
        if (player_moved) ++s_hold_player_frames;
        if (dt > 18u || di > 1u || visible < s_level) ++s_bad;
        if (++s_frames >= SWEEP_FRAMES) {
            gpu2d_stats_t hw_end;
            gpu2d_getStats(&hw_end);
            s_hold_reported = 1;
            bsp_printf("BULLET HOLD: n=%d visible_min=%d player/cam_move=%d/%d, 120 frames=%d ms, gap_max=%d, irq_max=%d, bad=%d, cmd_avg/max=%d/%d, gpu_rd/wr/busy=%d/%d/%d\r\n",
                       s_level, s_visible_min, s_hold_player_frames, s_hold_move_frames,
                       (int)s_elapsed, s_gap_max, s_irq_max, s_bad,
                       (int)(s_cmd_sum / SWEEP_FRAMES), s_cmd_max,
                       (int)((hw_end.rd_beats - s_hw_start.rd_beats) / SWEEP_FRAMES),
                       (int)((hw_end.wr_beats - s_hw_start.wr_beats) / SWEEP_FRAMES),
                       (int)((hw_end.busy_cyc - s_hw_start.busy_cyc) / SWEEP_FRAMES));
        }
        return;
    }
    if (s_warmup > 0) {
        --s_warmup;
        s_last_ms = now;
        s_last_irq = irq;
        if (s_warmup == 0) {
            s_frames = 0;
            s_bad = 0;
            s_gap_max = 0;
            s_irq_max = 0;
            s_elapsed = 0;
            s_cmd_sum = 0;
            s_cmd_max = 0;
            s_visible_min = N_BULLETS;
            s_drops = 0;
            s_last_drops = rl_dropped();
            gpu2d_getStats(&s_hw_start);
        }
        return;
    }
    dt = now - s_last_ms;
    di = irq - s_last_irq;
    s_last_ms = now;
    s_last_irq = irq;
    s_elapsed += dt;
    s_cmd_sum += (u32)g.cmd_frame;
    if (g.cmd_frame > s_cmd_max) s_cmd_max = g.cmd_frame;
    s_drops += rl_dropped() - s_last_drops;
    s_last_drops = rl_dropped();
    if ((int)dt > s_gap_max) s_gap_max = (int)dt;
    if ((int)di > s_irq_max) s_irq_max = (int)di;
    {
        int visible = game_bullets_stress_visible();
        if (visible < s_visible_min) s_visible_min = visible;
        if (dt > 18u || di > 1u || game_bullets_alive() != s_level ||
            visible < s_level || s_drops) ++s_bad;
    }
    if (++s_frames < SWEEP_FRAMES) return;

    {
        int pass = s_bad == 0 && s_elapsed <= 2010u;
        gpu2d_stats_t hw_end;
        gpu2d_getStats(&hw_end);
        if (pass) s_best_once = s_level;
        if (!pass) s_level_all_pass = 0;
        bsp_printf("BULLET SWEEP: n=%d visible_min=%d run=%d %s, 120 frames=%d ms, max_gap=%d ms, irq_delta_max=%d, bad=%d, cmd_avg/max=%d/%d, drop=%d, gpu_rd/wr/busy=%d/%d/%d\r\n",
                   s_level, s_visible_min, s_repeat + 1, pass ? "PASS" : "FAIL", (int)s_elapsed,
                   s_gap_max, s_irq_max, s_bad, (int)(s_cmd_sum / SWEEP_FRAMES),
                   s_cmd_max, (int)s_drops,
                   (int)((hw_end.rd_beats - s_hw_start.rd_beats) / SWEEP_FRAMES),
                   (int)((hw_end.wr_beats - s_hw_start.wr_beats) / SWEEP_FRAMES),
                   (int)((hw_end.busy_cyc - s_hw_start.busy_cyc) / SWEEP_FRAMES));
    }
    if (s_level >= 180 && s_level <= 240 && ++s_repeat < 3) {
        s_warmup = SWEEP_WARMUP;
        return;
    }
    if (s_level_all_pass && s_level <= 240) s_best = s_level;
    if (s_level >= GAME_BOSS_SWEEP_HOLD_LEVEL) {
        bsp_printf("BULLET SWEEP DONE: highest 3-window pass=%d, highest single-window=%d, pool=%d, normal enemy cap=%d\r\n",
                   s_best, s_best_once, N_BULLETS, ENEMY_CAP);
        s_holding = 1;
        s_hold_started = 0;
        s_hold_reported = 0;
        s_hold_cam_x = g.cam_x;
        s_hold_cam_y = g.cam_y;
        s_hold_player_x = g.x;
        s_hold_player_y = g.y;
        bsp_printf("BULLET SWEEP HOLD: %d fully visible bullets stay on screen; PAD and camera active\r\n",
                   game_bullets_stress_visible());
        return;
    }
    s_level += s_level < 160 ? SWEEP_STEP :
               s_level < 188 ? SWEEP_FINE_STEP :
               s_level < 192 ? 1 :
               s_level < 224 ? SWEEP_FINE_STEP : SWEEP_STEP;
    if (s_level > GAME_BOSS_SWEEP_HOLD_LEVEL)
        s_level = GAME_BOSS_SWEEP_HOLD_LEVEL;
    s_repeat = 0;
    s_level_all_pass = 1;
    game_bullets_stress_fill(s_level);
    s_warmup = SWEEP_WARMUP;
}
#endif
