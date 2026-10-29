//==============================================================================
//  game_main.c —— 主循环：一帧四步
//------------------------------------------------------------------------------
//  干什么      :
//      frame_begin(now)          清脏表、记命令数基线、算 FPS
//      game_logic_run(keys, aim, now) 按 CLINT 累积 60 Hz 逻辑步，保留不足一步的余数
//      game_draw(now, buf)       元素各自 push 进渲染队列（带 z）→ 排序后下发
//      frame_end(now, buf)       交帧 + 统计 + 每秒探针（+ 压测推进）
//  上一级是谁  : src/main.c（DEMO_RUN_SEL == 2 时调 game_demo_run()）
//  接口契约    :
//    · 双缓冲画 front ^ 1；三缓冲只画既未显示、也未排队的空闲块
//    · 双缓冲复用前必须 FENCE + 等真帧边界，否则会写到正在显示的缓冲
//    · 绘制顺序 = z 序：场景(0) → 实体(1000+底边 y) → 护盾(3000) → HUD(5000+) → GAME OVER(6000)
//  为什么这么做:
//    · 硬件没有 z 缓冲 ⇒ 后发的命令盖前面的（画家算法）
//    · 每个元素自己带 z（render_list.c 统一排序）⇒ 加元素不用改排序逻辑，
//      实体按"底边 y"排就得到俯视视角的前后遮挡（越靠下越近）
//    · 每块缓冲各记一份"上次画在哪"⇒ 双缓冲怎么交替都不会擦错地方
//    · 压测（PERF_TEST_MODE）用运行时 if 而不是 #if：少一层预处理噪声，
//      而且这段代码永远参与编译，不会"只在某个模式才编到、平时有错也发现不了"
//  参考        : docs/spec/软件_驱动与API.md §6.3
//==============================================================================
#include "game.h"
#include "game_schedule.h"
#include "CI_gpu2d.h"
#include "user_gpu2d.h"
#include "user_lineBuffer.h"
#include "IRQ_frame.h"
#include "bsp.h"

static const u32 g_fb[GAME_FB_COUNT] = { FB_C, FB_D
#if GAME_TRIPLE_BUFFER
    , FB_THIRD
#endif
};

static unsigned char s_static_room_key[GAME_FB_COUNT];
static int s_static_force;
#if GAME_BOSS_SKILL_DIAG
static int s_dense_diag_active, s_dense_diag_frames, s_dense_diag_gap_max;
static int s_dense_diag_bad, s_dense_diag_min_enemy, s_dense_diag_min_visible;
static int s_dense_diag_max_visible, s_dense_diag_cmd_max;
static int s_dense_diag_start_enemy, s_dense_diag_start_visible, s_dense_diag_start_total;
static u32 s_dense_diag_start_rejected;
static u32 s_dense_diag_last_ms, s_dense_diag_last_irq, s_dense_diag_elapsed;
static u32 s_dense_diag_cmd_sum;
static u32 s_dense_diag_draw_sum, s_dense_diag_fence_sum;
static u32 s_dense_diag_draw_max, s_dense_diag_fence_max;
static u32 s_dense_diag_draw_n, s_dense_diag_fence_n;
static u32 s_dense_diag_update_sum, s_dense_diag_update_max;
static u32 s_dense_diag_present_sum, s_dense_diag_present_max;
static u32 s_dense_diag_sim_steps, s_dense_diag_sim_zero, s_dense_diag_sim_multi;
static u32 s_dense_diag_discarded_ticks;
static gpu2d_stats_t s_dense_diag_hw_start;

void game_dense_diag_start(void)
{
    s_dense_diag_active = 1;
    s_dense_diag_frames = 0;
    s_dense_diag_gap_max = 0;
    s_dense_diag_bad = 0;
    s_dense_diag_min_enemy = ENEMY_CAP;
    s_dense_diag_min_visible = ENEMY_CAP;
    s_dense_diag_max_visible = 0;
    s_dense_diag_start_enemy = game_enemy_bullets_alive();
    s_dense_diag_start_visible = game_enemy_bullets_visible();
    s_dense_diag_start_total = game_bullets_alive();
    s_dense_diag_start_rejected = game_enemy_spawn_rejects();
    s_dense_diag_cmd_max = 0;
    s_dense_diag_elapsed = 0;
    s_dense_diag_cmd_sum = 0;
    s_dense_diag_last_ms = 0;
    s_dense_diag_last_irq = 0;
}

static void game_dense_diag_frame(void)
{
    u32 now, irq, dt, di;
    int enemy, visible;
    if (!s_dense_diag_active) return;
    now = game_ms();
    irq = IRQ_frame_count();
    if (s_dense_diag_last_ms == 0u) {
        s_dense_diag_last_ms = now;
        s_dense_diag_last_irq = irq;
        s_dense_diag_draw_sum = s_dense_diag_fence_sum = 0u;
        s_dense_diag_draw_max = s_dense_diag_fence_max = 0u;
        s_dense_diag_draw_n = s_dense_diag_fence_n = 0u;
        s_dense_diag_update_sum = s_dense_diag_update_max = 0u;
        s_dense_diag_present_sum = s_dense_diag_present_max = 0u;
        s_dense_diag_sim_steps = s_dense_diag_sim_zero = s_dense_diag_sim_multi = 0u;
        s_dense_diag_discarded_ticks = 0u;
        gpu2d_getStats(&s_dense_diag_hw_start);
        return;
    }
    dt = now - s_dense_diag_last_ms;
    di = irq - s_dense_diag_last_irq;
    s_dense_diag_last_ms = now;
    s_dense_diag_last_irq = irq;
    s_dense_diag_elapsed += dt;
    s_dense_diag_cmd_sum += (u32)g.cmd_frame;
    if (g.cmd_frame > s_dense_diag_cmd_max) s_dense_diag_cmd_max = g.cmd_frame;
    if ((int)dt > s_dense_diag_gap_max) s_dense_diag_gap_max = (int)dt;
    if (dt > 18u || di > 1u) ++s_dense_diag_bad;
    enemy = game_enemy_bullets_alive();
    if (enemy < s_dense_diag_min_enemy) s_dense_diag_min_enemy = enemy;
    visible = game_enemy_bullets_visible();
    if (visible < s_dense_diag_min_visible) s_dense_diag_min_visible = visible;
    if (visible > s_dense_diag_max_visible) s_dense_diag_max_visible = visible;
    if (++s_dense_diag_frames == 120) {
        gpu2d_stats_t hw_end;
        u32 ticks_per_us = (u32)(BSP_CLINT_HZ / 1000000u);
        gpu2d_getStats(&hw_end);
        s_dense_diag_active = 0;
        bsp_printf("DENSE RINGS: enemy=%d visible=%d total=%d rejected=%d\r\n",
                   s_dense_diag_start_enemy, s_dense_diag_start_visible,
                   s_dense_diag_start_total, (int)s_dense_diag_start_rejected);
        bsp_printf("DENSE LOAD: 120 frames=%d ms gap_max=%d bad=%d enemy_min=%d visible_min/max=%d/%d cmd_avg/max=%d/%d\r\n",
                   (int)s_dense_diag_elapsed, s_dense_diag_gap_max,
                   s_dense_diag_bad, s_dense_diag_min_enemy,
                   s_dense_diag_min_visible, s_dense_diag_max_visible,
                   (int)(s_dense_diag_cmd_sum / 120u), s_dense_diag_cmd_max);
        bsp_printf("DENSE COST: draw_avg/max=%d/%d us fence_avg/max=%d/%d us gpu_rd/wr/busy=%d/%d/%d per frame\r\n",
                   (int)(s_dense_diag_draw_sum / (s_dense_diag_draw_n ? s_dense_diag_draw_n : 1u) / ticks_per_us),
                   (int)(s_dense_diag_draw_max / ticks_per_us),
                   (int)(s_dense_diag_fence_sum / (s_dense_diag_fence_n ? s_dense_diag_fence_n : 1u) / ticks_per_us),
                   (int)(s_dense_diag_fence_max / ticks_per_us),
                   (int)((hw_end.rd_beats - s_dense_diag_hw_start.rd_beats) / 120u),
                   (int)((hw_end.wr_beats - s_dense_diag_hw_start.wr_beats) / 120u),
                   (int)((hw_end.busy_cyc - s_dense_diag_hw_start.busy_cyc) / 120u));
        bsp_printf("DENSE SCHED: steps=%d hz_x100=%d zero/multi=%d/%d dropped_us=%d update_avg/max=%d/%d us present_avg/max=%d/%d us buffers=%d\r\n",
                   (int)s_dense_diag_sim_steps,
                   (int)(s_dense_diag_sim_steps * 100000u / (s_dense_diag_elapsed ? s_dense_diag_elapsed : 1u)),
                   (int)s_dense_diag_sim_zero, (int)s_dense_diag_sim_multi,
                   (int)(s_dense_diag_discarded_ticks / ticks_per_us),
                   (int)(s_dense_diag_update_sum / 120u / ticks_per_us),
                   (int)(s_dense_diag_update_max / ticks_per_us),
                   (int)(s_dense_diag_present_sum / 120u / ticks_per_us),
                   (int)(s_dense_diag_present_max / ticks_per_us), GAME_FB_COUNT);
    }
}
#endif

void game_static_draw_reset(void)
{
    int b;
    for (b = 0; b < GAME_FB_COUNT; ++b) s_static_room_key[b] = 0;
    s_static_force = 1;
}

int game_static_draw_force(void) { return s_static_force; }

#if GAME_TRIPLE_BUFFER
/* 显示基址寄存器在帧边界锁存。在对应 IRQ 到来前，
 * 正在显示的缓冲和已排队等待显示的缓冲都归 HDMI 所有，游戏不能写入。 */
static int s_pending_fb = -1;
static u32 s_pending_irq;

static void triple_poll_pending(void)
{
    if (s_pending_fb >= 0 && IRQ_frame_count() != s_pending_irq) {
        g.front = s_pending_fb;
        s_pending_fb = -1;
    }
}

static int triple_retire_pending(void)
{
    triple_poll_pending();
    if (s_pending_fb < 0) return 1;
    /* 保留提交时的 IRQ 基准。若等待过程中重新取快照，
     * 可能错过紧接在上一次轮询之后到达的帧边界。 */
    if (!game_wait_display_after(s_pending_irq)) return 0;
    g.front = s_pending_fb;
    s_pending_fb = -1;
    return 1;
}

static int triple_free_buffer(void)
{
    int n;
    triple_poll_pending();
    for (n = 1; n < GAME_FB_COUNT; ++n) {
        int b = (g.front + n) % GAME_FB_COUNT;
        if (b != s_pending_fb) return b;
    }
    return -1;
}
#endif

static int prepare_room_transition(void)
{
#if GAME_TRIPLE_BUFFER
    if(!game_room_can_overlap_transition()) return triple_retire_pending();
#endif
    return 1;
}

static dirty_map_t g_dirty;                     /* 一帧的脏区位图（格子坐标） */
static int  g_alive;                            /* 本帧在场子弹数 */
static int  g_hud_need;                         /* 本帧要不要重画 HUD 文字 */
static int  g_toast_on;                         /* 本帧要不要画拾取字幕 */
static int  g_toast_fade;                       /* 同上（拾取字幕） */
static int  g_bossbar_fade;                     /* 血条消失后还要擦几帧（它不在面板行里） */
static int  g_hud_full;                         /* 相机移动过：本帧整块 HUD 强制全画一遍 */

/*  ★ HUD 数字快照（2026-10-23）
 *    双缓冲是【隔帧】画的：若每帧都取实时值，两块缓冲会显示两个不同的数，
 *    屏幕上看到的就是"数字来回跳 + 闪烁"（相机一动、HUD 每帧重画时最明显）。
 *    每轮缓冲才更新一次快照，双/三缓冲都使用同一组快照。  */
static int      s_hv_fps, s_hv_bullets, s_hv_cmd, s_hv_coins;
static unsigned s_hv_tick;

static void hud_vals_update(int fps, int bullets, int cmd, int coins)
{
    if ((s_hv_tick++ % GAME_FB_COUNT) != 0u) return;
    s_hv_fps     = fps;
    s_hv_bullets = bullets;
    s_hv_cmd     = cmd;
    s_hv_coins   = coins;
}

/*  分段计时（CLINT 原始 tick → us）。默认开；只想省代码/不想看日志时改 0。  */
#ifndef GAME_PROFILE
#define GAME_PROFILE 1
#endif
#ifndef GAME_FIXED_UPDATE
#define GAME_FIXED_UPDATE 1
#endif
#if GAME_FIXED_UPDATE
static game_clock_t s_sim_clock;
static void game_logic_reset(void)
{
    game_clock_reset(&s_sim_clock, clint_getTimeLow(BSP_CLINT));
}
#endif

static int game_logic_run(u32 keys, u32 aim_keys, u32 now)
{
    int steps = 0;
#if GAME_BOSS_SKILL_DIAG
    u32 update_t0 = clint_getTimeLow(BSP_CLINT);
#endif
#if GAME_FIXED_UPDATE
    u32 discarded = game_clock_feed(&s_sim_clock, clint_getTimeLow(BSP_CLINT), BSP_CLINT_HZ);
#if GAME_BOSS_SKILL_DIAG
    if (s_dense_diag_active) s_dense_diag_discarded_ticks += discarded;
#else
    (void)discarded;
#endif
    while (steps < (int)GAME_LOGIC_MAX_STEPS && game_clock_take(&s_sim_clock, BSP_CLINT_HZ)) {
        u32 step_now = now - s_sim_clock.pending_ticks / (BSP_CLINT_HZ / 1000u);
        game_update_step(keys, aim_keys, step_now);
        game_room_step();
        ++steps;
        if (game_room_pending() || game_room_rebuild_pending()) break;
    }
#else
    game_update_step(keys, aim_keys, now);
    game_room_step();
    steps = 1;
#endif
#if GAME_BOSS_SKILL_DIAG
    if (s_dense_diag_active) {
        u32 dt = clint_getTimeLow(BSP_CLINT) - update_t0;
        s_dense_diag_update_sum += dt;
        if (dt > s_dense_diag_update_max) s_dense_diag_update_max = dt;
        s_dense_diag_sim_steps += (u32)steps;
        if (steps == 0) ++s_dense_diag_sim_zero;
        if (steps > 1) ++s_dense_diag_sim_multi;
    }
#endif
    return steps;
}
#if GAME_PROFILE
#if GAME_BOSS_STAGE_TEST
typedef struct {
    u32 frames, elapsed, enemy_peak, core_peak, rain_peak, mobs_peak;
    u32 cmd_peak, gap_max, slow25, draw_max, fence_max;
} stage_profile_t;
static stage_profile_t s_stage_profile[BOSS_PHASE_COUNT+2];
static u32 s_stage_last, s_stage_draw, s_stage_fence;
static int s_stage_id = -1, s_stage_skip, s_stage_done;

static void boss_stage_profile_frame(void)
{
    u32 now = game_ms(), gap;
    int i, enemy, rain = 0, core = 0, mobs;
    int id=g.boss_locked ? BOSS_PHASE_COUNT+g.boss_phase-1 : g.boss_phase;
    stage_profile_t *v;
    if (g.phase == PHASE_FIGHT && g.boss_phase == 0 && g.boss_tick == 0 && !g.boss_locked) {
        for (i = 0; i < BOSS_PHASE_COUNT+2; ++i) s_stage_profile[i] = (stage_profile_t){0};
        s_stage_done = 0; s_stage_last = 0; s_stage_id = -1;
    }
    if (g.phase != PHASE_FIGHT || (!g.boss_locked && !g.boss_tick)) {
        s_stage_last = 0;
        if (!s_stage_done && g.boss_hp == 0 && s_stage_profile[2].frames) {
            u32 div = BSP_CLINT_HZ / 1000000u;
            for (i = 0; i < BOSS_PHASE_COUNT+2; ++i) {
                v = &s_stage_profile[i];
                bsp_printf("%s LOAD: phase=%d frames=%d elapsed_ms=%d enemy_peak=%d core_peak=%d rain_peak=%d mobs_peak=%d cmd_max=%d gap_max_ms=%d slow25=%d draw_max_us=%d fence_max_us=%d\r\n",
                           i<BOSS_PHASE_COUNT ? "PHASE" : "INTERMISSION",
                           i<BOSS_PHASE_COUNT ? i+1 : i-BOSS_PHASE_COUNT+1,(int)v->frames,(int)v->elapsed,(int)v->enemy_peak,
                           (int)v->core_peak,(int)v->rain_peak,(int)v->mobs_peak,
                           (int)v->cmd_peak,(int)v->gap_max,(int)v->slow25,
                           (int)(v->draw_max/div),(int)(v->fence_max/div));
            }
            bsp_printf("NAV LOAD: build_max_us=%d\r\n",(int)game_mobs_nav_max_us());
            s_stage_done = 1;
        }
        return;
    }
    if (s_stage_done) return;
    if (s_stage_id != id) { s_stage_id = id; s_stage_skip = 3; }
    if (s_stage_skip > 0 || !s_stage_last) {
        if (s_stage_skip > 0) --s_stage_skip;
        s_stage_last = now; return;
    }
    gap = now - s_stage_last; s_stage_last = now;
    v = &s_stage_profile[id]; ++v->frames; v->elapsed += gap;
    for (i = 0; i < N_BULLETS; ++i) if (g.bullets[i].active && g.bullets[i].enemy) {
        if (g.bullets[i].mode == MOTION_ACCEL) ++rain; else ++core;
    }
    enemy = rain + core; mobs = game_mobs_alive();
    if ((u32)enemy > v->enemy_peak) v->enemy_peak = (u32)enemy;
    if ((u32)rain > v->rain_peak) v->rain_peak = (u32)rain;
    if ((u32)core > v->core_peak) v->core_peak = (u32)core;
    if ((u32)mobs > v->mobs_peak) v->mobs_peak = (u32)mobs;
    if ((u32)g.cmd_frame > v->cmd_peak) v->cmd_peak = (u32)g.cmd_frame;
    if (gap > v->gap_max) v->gap_max = gap;
    if (gap >= 25u) ++v->slow25;
    if (s_stage_draw > v->draw_max) v->draw_max = s_stage_draw;
    if (s_stage_fence > v->fence_max) v->fence_max = s_stage_fence;
}
#endif
static u32 s_pf_begin, s_pf_update, s_pf_draw, s_pf_fence, s_pf_present;
static u32 s_pf_draw_prepare, s_pf_draw_submit, s_pf_draw_execute;
static u32 s_pf_history_merge, s_pf_history_build;
static u32 s_pf_begin_max, s_pf_update_max, s_pf_draw_max;
static u32 s_pf_fence_max, s_pf_present_max;
static u32 s_pf_frames;
static u32 s_pf_gap_sum, s_pf_gap_max, s_pf_last_frame;
static u32 s_pf_gap_25, s_pf_gap_33;
static u32 s_pf_irq_last, s_pf_irq_start, s_pf_irq_ticks, s_pf_irq_max, s_pf_irq_late;
static u32 s_pf_sim_steps;
static gpu2d_stats_t s_gpu_prev;
static int s_gpu_prev_valid;
#if GAME_BOSS_DEBUG && GAME_BOSS_SKILL_DIAG
#define BOSS_SKILL_DIAG_MAX 16
typedef struct {
    u32 frames, cmd_sum, cmd_max, bullet_max, dirty_max;
    u32 draw_max, fence_max, gap_max, slow25, cam_frames, pause_frames;
    u32 irq_repeats, irq_delta_max, sim0, sim2;
    u32 spawn_rejects, elapsed_ms, enemy_max, visible_max, dense_frames;
    u32 dense_ms, dense_gap_max;
    u32 work_sum, work_max, dense_work_max, dense_visible_max;
} boss_skill_diag_t;
static boss_skill_diag_t s_skill_diag[BOSS_SKILL_DIAG_MAX];
static u32 s_skill_gap, s_skill_draw, s_skill_fence, s_skill_reject_prev;
static u32 s_skill_irq_last;
static u32 s_skill_work_start, s_skill_work;
static int s_skill_steps;
static int s_skill_cam, s_skill_last = -1, s_skill_warmup = 3, s_skill_done;
static int s_skill_cycle;

static u32 boss_skill_irq_mark(void)
{
#if GAME_TRIPLE_BUFFER
    return s_pending_irq;
#else
    return IRQ_frame_count();
#endif
}

static void boss_skill_diag_frame(void)
{
    int i, idx = g.shot.move_idx;
    u32 div = (u32)(BSP_CLINT_HZ / 1000000u);
    boss_skill_diag_t *v;

    if (g.phase != PHASE_FIGHT) {
        if (s_skill_last >= 0) {
            for (i = 0; i < BOSS_SKILL_DIAG_MAX; ++i)
                s_skill_diag[i] = (boss_skill_diag_t){0};
            s_skill_last = -1;
            s_skill_warmup = 3;
            s_skill_done = 0;
            s_skill_cycle = 0;
            s_skill_irq_last = 0u;
        }
        s_skill_reject_prev = game_enemy_spawn_rejects();
        return;
    }
    if (s_skill_done || idx < 0 || idx >= g_boss_move_n ||
        idx >= BOSS_SKILL_DIAG_MAX) return;
    if (s_skill_warmup > 0) {
        --s_skill_warmup;
        s_skill_last = idx;
        s_skill_reject_prev = game_enemy_spawn_rejects();
        s_skill_irq_last = boss_skill_irq_mark();
        return;
    }
    if (idx == 0 && s_skill_last == g_boss_move_n - 1 &&
        ++s_skill_cycle >= GAME_BOSS_SKILL_CYCLES) {
        if (div == 0u) div = 1u;
        for (i = 0; i < g_boss_move_n; ++i) {
            v = &s_skill_diag[i];
            bsp_printf("SKILL LOAD: id=%d frames=%d cmd_avg/max=%d/%d bullets_max=%d enemy_max=%d visible_max=%d dense_frames=%d dense_ms=%d dense_gap_max_ms=%d elapsed_ms=%d dirty_max=%d draw_max_us=%d fence_max_us=%d gap_max_ms=%d slow25=%d cam=%d pause=%d rejects=%d irq_repeat=%d irq_delta_max=%d sim0/2=%d/%d work_avg/max_us=%d/%d dense_work_max_us=%d dense_visible_max=%d\r\n",
                       i, (int)v->frames,
                       (int)(v->frames ? v->cmd_sum / v->frames : 0u),
                       (int)v->cmd_max, (int)v->bullet_max, (int)v->enemy_max,
                       (int)v->visible_max, (int)v->dense_frames, (int)v->dense_ms,
                       (int)v->dense_gap_max, (int)v->elapsed_ms,
                       (int)v->dirty_max,
                       (int)(v->draw_max / div), (int)(v->fence_max / div),
                       (int)v->gap_max, (int)v->slow25,
                       (int)v->cam_frames, (int)v->pause_frames,
                       (int)v->spawn_rejects, (int)v->irq_repeats,
                       (int)v->irq_delta_max, (int)v->sim0, (int)v->sim2,
                       (int)(v->frames ? v->work_sum / v->frames : 0u),
                       (int)(v->work_max / div), (int)(v->dense_work_max / div),
                       (int)v->dense_visible_max);
        }
        s_skill_done = 1;
        return;
    }
    s_skill_last = idx;
    v = &s_skill_diag[idx];
    v->spawn_rejects += game_enemy_spawn_rejects() - s_skill_reject_prev;
    s_skill_reject_prev = game_enemy_spawn_rejects();
    ++v->frames;
    v->work_sum += s_skill_work / (div ? div : 1u);
    if (s_skill_work > v->work_max) v->work_max = s_skill_work;
    v->elapsed_ms += s_skill_gap;
    {
        u32 enemy = (u32)game_enemy_bullets_alive();
        u32 visible = (u32)game_enemy_bullets_visible();
        if (enemy > v->enemy_max) v->enemy_max = enemy;
        if (visible > v->visible_max) v->visible_max = visible;
        if (enemy >= (u32)(ENEMY_CAP - 8)) {
            ++v->dense_frames;
            v->dense_ms += s_skill_gap;
            if (s_skill_gap > v->dense_gap_max) v->dense_gap_max = s_skill_gap;
            if (s_skill_work > v->dense_work_max) v->dense_work_max = s_skill_work;
            if (visible > v->dense_visible_max) v->dense_visible_max = visible;
        }
    }
    v->cmd_sum += (u32)g.cmd_frame;
    if ((u32)g.cmd_frame > v->cmd_max) v->cmd_max = (u32)g.cmd_frame;
    if ((u32)g_alive > v->bullet_max) v->bullet_max = (u32)g_alive;
    if (game_restore_dirty_cells() > v->dirty_max)
        v->dirty_max = game_restore_dirty_cells();
    if (s_skill_draw > v->draw_max) v->draw_max = s_skill_draw;
    if (s_skill_fence > v->fence_max) v->fence_max = s_skill_fence;
    if (s_skill_gap > v->gap_max) v->gap_max = s_skill_gap;
    if (s_skill_gap >= 25u) ++v->slow25;
    if (s_skill_cam) ++v->cam_frames;
    if (g.shot.burst_left == 0) ++v->pause_frames;
    if (s_skill_irq_last != 0u) {
        u32 delta = boss_skill_irq_mark() - s_skill_irq_last;
        if (delta > 1u) v->irq_repeats += delta - 1u;
        if (delta > v->irq_delta_max) v->irq_delta_max = delta;
    }
    s_skill_irq_last = boss_skill_irq_mark();
    if (s_skill_steps == 0) ++v->sim0;
    if (s_skill_steps >= 2) ++v->sim2;
}
#endif
#if GAME_BOSS_DEBUG && GAME_BOSS_AUTO_DIAG
typedef struct {
    u32 frames, cmd_sum, cmd_max, dirty_max;
    u32 draw_max, fence_max, gap_max, slow25;
    u32 step0, step2, irq_repeats, irq_delta_max, irq_late;
    u32 elapsed, draw_sum, fence_sum, restore_sum, restore_max;
    u32 bullets_min, bullets_max;
} boss_cam_diag_t;
static boss_cam_diag_t s_camdiag[2][2]; /* [camera moved][at least 32 bullets] */
static u32 s_camdiag_start, s_camdiag_gap, s_camdiag_draw, s_camdiag_fence;
static u32 s_camdiag_irq_last;
static int s_camdiag_moved, s_camdiag_skip, s_camdiag_steps, s_camdiag_late;
static int s_camdiag_room = -1;

static void boss_cam_diag_frame(u32 now)
{
    boss_cam_diag_t *v;
    int moving, heavy, m, h;
    u32 div = (u32)(BSP_CLINT_HZ / 1000000u);

    if (GAME_BOSS_AUTO_DIAG == 1 && g.phase != PHASE_FIGHT) {
        s_camdiag_start = 0u;
        s_camdiag_irq_last = 0u;
        return;
    }
    if (s_camdiag_room != game_room_id()) {
        s_camdiag_room = game_room_id();
        for (m = 0; m < 2; ++m) for (h = 0; h < 2; ++h)
            s_camdiag[m][h] = (boss_cam_diag_t){0};
        s_camdiag_start = 0u;
        s_camdiag_irq_last = 0u;
        s_camdiag_skip = 3;
    }
    if (s_camdiag_skip > 0) {
        --s_camdiag_skip;
#if GAME_TRIPLE_BUFFER
        s_camdiag_irq_last = s_pending_irq;
#else
        s_camdiag_irq_last = IRQ_frame_count();
#endif
        return;
    }
    if (s_camdiag_start == 0u) s_camdiag_start = now;
    moving = s_camdiag_moved != 0;
    heavy = g_alive >= 32;
    v = &s_camdiag[moving][heavy];
    if (v->frames == 0u || (u32)g_alive < v->bullets_min) v->bullets_min = (u32)g_alive;
    if ((u32)g_alive > v->bullets_max) v->bullets_max = (u32)g_alive;
    ++v->frames;
    v->elapsed += s_camdiag_gap;
    v->draw_sum += s_camdiag_draw;
    v->fence_sum += s_camdiag_fence;
    v->restore_sum += game_restore_dirty_cmds();
    if (game_restore_dirty_cmds() > v->restore_max)
        v->restore_max = game_restore_dirty_cmds();
    v->cmd_sum += (u32)g.cmd_frame;
    if ((u32)g.cmd_frame > v->cmd_max) v->cmd_max = (u32)g.cmd_frame;
    if (game_restore_dirty_cells() > v->dirty_max)
        v->dirty_max = game_restore_dirty_cells();
    if (s_camdiag_draw > v->draw_max) v->draw_max = s_camdiag_draw;
    if (s_camdiag_fence > v->fence_max) v->fence_max = s_camdiag_fence;
    if (s_camdiag_gap > v->gap_max) v->gap_max = s_camdiag_gap;
    if (s_camdiag_gap >= 25u) ++v->slow25;
    if (s_camdiag_steps == 0) ++v->step0;
    if (s_camdiag_steps >= 2) ++v->step2;
    if (s_camdiag_late) ++v->irq_late;
    {
#if GAME_TRIPLE_BUFFER
        u32 irq_now = s_pending_irq;
#else
        u32 irq_now = IRQ_frame_count();
#endif
        if (s_camdiag_irq_last != 0u) {
            u32 delta = irq_now - s_camdiag_irq_last;
            if (delta > 1u) v->irq_repeats += delta - 1u;
            if (delta > v->irq_delta_max) v->irq_delta_max = delta;
        }
        s_camdiag_irq_last = irq_now;
    }
    if ((u32)(now - s_camdiag_start) < 15000u) return;

    if (div == 0u) div = 1u;
    for (m = 0; m < 2; ++m) for (h = 0; h < 2; ++h) {
        v = &s_camdiag[m][h];
        if (v->frames == 0u) continue;
        bsp_printf("CAMLOAD move=%d heavy=%d n=%d cmd_avg/max=%d/%d dirty_max=%d draw_max_us=%d fence_max_us=%d gap_max_ms=%d slow25=%d sim0/2=%d/%d irq_repeat=%d irq_delta_max=%d irq_late=%d elapsed_ms=%d draw_avg_us=%d fence_avg_us=%d restore_avg/max=%d/%d layout=%d flip=%d x/y=%d/%d cam_x/y=%d/%d bullets_min/max=%d/%d\r\n",
                   m, h, (int)v->frames,
                   (int)(v->frames ? v->cmd_sum / v->frames : 0u),
                   (int)v->cmd_max, (int)v->dirty_max,
                   (int)(v->draw_max / div), (int)(v->fence_max / div),
                   (int)v->gap_max, (int)v->slow25,
                   (int)v->step0, (int)v->step2, (int)v->irq_repeats,
                   (int)v->irq_delta_max, (int)v->irq_late,
                   (int)v->elapsed, (int)(v->draw_sum / v->frames / div),
                   (int)(v->fence_sum / v->frames / div),
                   (int)(v->restore_sum / v->frames), (int)v->restore_max,
                   game_room_layout(), game_room_flip(), g.x, g.y, g.cam_x, g.cam_y,
                   (int)v->bullets_min, (int)v->bullets_max);
    }
    for (m = 0; m < 2; ++m)
        for (h = 0; h < 2; ++h) s_camdiag[m][h] = (boss_cam_diag_t){0};
    s_camdiag_start = 0u;
    s_camdiag_irq_last = 0u;
    s_camdiag_skip = 3; /* Exclude the UART print from the next sample. */
}
#endif
#if GAME_BOSS_DEBUG
static u32 s_diag_frames, s_diag_gap_max, s_diag_irq_max, s_diag_irq_ticks;
static u32 s_diag_sim_steps, s_diag_fence_max, s_diag_gap_25, s_diag_gap_33;

void game_debug_frame_report(void)
{
    bsp_printf("FRAME DIAG: frames=%d irq=%d irq_max=%d sim=%d gap_max=%d slow25=%d slow33=%d fence_max_us=%d bullets=%d\r\n",
               (int)s_diag_frames, (int)s_diag_irq_ticks, (int)s_diag_irq_max,
               (int)s_diag_sim_steps, (int)s_diag_gap_max,
               (int)s_diag_gap_25, (int)s_diag_gap_33,
               (int)(s_diag_fence_max / (u32)(BSP_CLINT_HZ / 1000000u)),
               game_bullets_alive());
    s_diag_frames = s_diag_gap_max = s_diag_irq_max = s_diag_irq_ticks = 0u;
    s_diag_sim_steps = s_diag_fence_max = s_diag_gap_25 = s_diag_gap_33 = 0u;
}
#endif

static u32 prof_now(void)
{
    return (u32)clint_getTime(BSP_CLINT);
}

static void prof_add(u32 *sum, u32 *max, u32 dt)
{
    *sum += dt;
    if (dt > *max) *max = dt;
}

#if GAME_BOSS_STAGE_TEST == 2
/* 手动性能剖析不能在游戏循环里等待 UART 发送完成，先写入内存缓冲区。 */
static char s_trace_tx[512];
static unsigned s_trace_len,s_trace_pos,s_trace_drop,s_trace_cam;
static void trace_text(const char *s)
{
    while(*s && s_trace_len<sizeof(s_trace_tx)-2) s_trace_tx[s_trace_len++]=*s++;
}
static void trace_field(const char *key,u32 value)
{
    char digits[10];int n=0;
    trace_text(" ");trace_text(key);trace_text("=");
    do { digits[n++]=(char)('0'+value%10u);value/=10u; } while(value);
    while(n && s_trace_len<sizeof(s_trace_tx)-2) s_trace_tx[s_trace_len++]=digits[--n];
}
static void trace_flush(void)
{
    u32 free=uart_writeAvailability(BSP_UART_TERMINAL),n=0;
    while(s_trace_pos<s_trace_len && n<free && n<32u) {
        uart_write(BSP_UART_TERMINAL,(u32)(unsigned char)s_trace_tx[s_trace_pos++]);++n;
    }
}
static void trace_report(void)
{
    u32 n=s_pf_frames,div=BSP_CLINT_HZ/1000000u;
    gpu2d_stats_t hw;
    if(g.phase!=PHASE_FIGHT || !n) { s_trace_cam=0;return; }
    if(s_trace_pos!=s_trace_len) { ++s_trace_drop;return; }
    s_trace_len=s_trace_pos=0;trace_text("FRAME1S");
    trace_field("n",n);trace_field("irq",s_pf_irq_ticks);
    trace_field("irqmax",s_pf_irq_max);trace_field("late",s_pf_irq_late);
    trace_field("gap",s_pf_gap_max);trace_field("slow25",s_pf_gap_25);
    trace_field("sim",s_pf_sim_steps);trace_field("cam",s_trace_cam);
    trace_field("upd",s_pf_update/n/div);trace_field("umax",s_pf_update_max/div);
    trace_field("draw",s_pf_draw/n/div);trace_field("dmax",s_pf_draw_max/div);
    trace_field("fence",s_pf_fence/n/div);trace_field("fmax",s_pf_fence_max/div);
    trace_field("present",s_pf_present/n/div);trace_field("pmax",s_pf_present_max/div);
    trace_field("prep",s_pf_draw_prepare/n/div);
    trace_field("submit",s_pf_draw_submit/n/div);trace_field("exec",s_pf_draw_execute/n/div);
    trace_field("enemy",(u32)game_enemy_bullets_alive());
    trace_field("mobs",(u32)game_mobs_alive());trace_field("cmd",(u32)g.cmd_frame);
    trace_field("dirty",game_restore_dirty_cells());trace_field("restore",game_restore_dirty_cmds());
    trace_field("phase",(u32)g.boss_phase+1u);trace_field("lock",(u32)g.boss_locked);
    trace_field("drop",s_trace_drop);
    gpu2d_getStats(&hw);
    if(s_gpu_prev_valid) {
        trace_field("rd",(hw.rd_beats-s_gpu_prev.rd_beats)/n);
        trace_field("wr",(hw.wr_beats-s_gpu_prev.wr_beats)/n);
        trace_field("busy",(hw.busy_cyc-s_gpu_prev.busy_cyc)/n);
    }
    s_gpu_prev=hw;s_gpu_prev_valid=1;
    s_trace_tx[s_trace_len++]='\r';s_trace_tx[s_trace_len++]='\n';
    s_trace_cam=0;
}
#endif

static void prof_report(void)
{
#if GAME_UART_TELEMETRY
    u32 div = (u32)(BSP_CLINT_HZ / 1000000u);
    gpu2d_stats_t hw;

    if (div == 0u) div = 1u;
#endif
    if (s_pf_frames == 0u) return;
#if GAME_BOSS_STAGE_TEST == 2
    trace_report();
#endif

#if GAME_BOSS_DEBUG
    s_diag_frames += s_pf_frames;
    if (s_pf_gap_max > s_diag_gap_max) s_diag_gap_max = s_pf_gap_max;
    if (s_pf_irq_max > s_diag_irq_max) s_diag_irq_max = s_pf_irq_max;
    s_diag_irq_ticks += s_pf_irq_ticks;
    s_diag_sim_steps += s_pf_sim_steps;
    if (s_pf_fence_max > s_diag_fence_max) s_diag_fence_max = s_pf_fence_max;
    s_diag_gap_25 += s_pf_gap_25;
    s_diag_gap_33 += s_pf_gap_33;
#endif

#if GAME_UART_TELEMETRY
    bsp_printf("PROFILE 1s us avg/max: begin=%d/%d update=%d/%d draw=%d/%d fence=%d/%d present=%d/%d gap_ms=%d/%d\r\n",
               (int)(s_pf_begin / s_pf_frames / div),   (int)(s_pf_begin_max / div),
               (int)(s_pf_update / s_pf_frames / div),  (int)(s_pf_update_max / div),
               (int)(s_pf_draw / s_pf_frames / div),    (int)(s_pf_draw_max / div),
               (int)(s_pf_fence / s_pf_frames / div),   (int)(s_pf_fence_max / div),
               (int)(s_pf_present / s_pf_frames / div), (int)(s_pf_present_max / div),
               (int)(s_pf_gap_sum / s_pf_frames), (int)s_pf_gap_max);
    bsp_printf("DRAW detail us avg: prepare=%d submit=%d execute=%d history_merge=%d history_build=%d\r\n",
               (int)(s_pf_draw_prepare / s_pf_frames / div),
               (int)(s_pf_draw_submit / s_pf_frames / div),
               (int)(s_pf_draw_execute / s_pf_frames / div),
               (int)(s_pf_history_merge / s_pf_frames / div),
               (int)(s_pf_history_build / s_pf_frames / div));
    bsp_printf("IRQ timing: ticks/frame=%d max=%d late_before_present=%d/%d\r\n",
               (int)(s_pf_irq_ticks / s_pf_frames), (int)s_pf_irq_max,
               (int)s_pf_irq_late, (int)s_pf_frames);
    bsp_printf("SIM timing: steps=%d render_frames=%d\r\n",
               (int)s_pf_sim_steps, (int)s_pf_frames);

    gpu2d_getStats(&hw);
    if (s_gpu_prev_valid)
        bsp_printf("GPU 1s beats/frame: rd=%d wr=%d busy_cyc=%d cmds=%d\r\n",
                   (int)((hw.rd_beats - s_gpu_prev.rd_beats) / s_pf_frames),
                   (int)((hw.wr_beats - s_gpu_prev.wr_beats) / s_pf_frames),
                   (int)((hw.busy_cyc - s_gpu_prev.busy_cyc) / s_pf_frames),
                   (int)((hw.cmd_done - s_gpu_prev.cmd_done) / s_pf_frames));
    s_gpu_prev = hw;
    s_gpu_prev_valid = 1;
#endif

    s_pf_begin = s_pf_update = s_pf_draw = s_pf_fence = s_pf_present = 0u;
    s_pf_draw_prepare = s_pf_draw_submit = s_pf_draw_execute = 0u;
    s_pf_history_merge = s_pf_history_build = 0u;
    s_pf_begin_max = s_pf_update_max = s_pf_draw_max = 0u;
    s_pf_fence_max = s_pf_present_max = 0u;
    s_pf_gap_sum = s_pf_gap_max = 0u;
    s_pf_gap_25 = s_pf_gap_33 = 0u;
    s_pf_irq_ticks = s_pf_irq_max = s_pf_irq_late = 0u;
    s_pf_sim_steps = 0u;
    s_pf_frames = 0u;
}
#endif


/*----------------------------------------------------------------------------*/
/*  ① 帧首  */
static int fps_from_counts(u32 frames, u32 elapsed_ms)
{
    if (elapsed_ms == 0u) return 0;
    return (int)((frames * 1000u + elapsed_ms / 2u) / elapsed_ms);
}

static void frame_begin(u32 now)
{
#if GAME_PROFILE && GAME_BOSS_STAGE_TEST == 2
    trace_flush();
#endif
    dirty_clear(g_dirty);
    game_dirty_begin();                 /* 同时清空脏格队列（mark 时只登记 0→1） */

    g.cmd_before = gpu2d_getCmdDone();

    if (g.fps_epoch == 0u) g.fps_epoch = now;
#if GAME_PROFILE
    s_pf_irq_start = IRQ_frame_count();
    if (s_pf_last_frame != 0u) {
        u32 gap = now - s_pf_last_frame;
#if GAME_BOSS_DEBUG && GAME_BOSS_SKILL_DIAG
        s_skill_gap = gap;
#endif
#if GAME_BOSS_DEBUG && GAME_BOSS_AUTO_DIAG
        s_camdiag_gap = gap;
#endif
        s_pf_gap_sum += gap;
        if (gap > s_pf_gap_max) s_pf_gap_max = gap;
        if (gap >= 25u) ++s_pf_gap_25;
        if (gap >= 33u) ++s_pf_gap_33;
    }
    s_pf_last_frame = now;
#endif
    ++g.fps_frames;
    if ((u32)(now - g.fps_epoch) >= 1000u) {
        g.fps = fps_from_counts(g.fps_frames, now - g.fps_epoch);
        g.fps_frames = 0u;
        g.fps_epoch = now;
    }
}

/*----------------------------------------------------------------------------*/
/*  ② 更新模型  */
/*----------------------------------------------------------------------------*/
/*  更新逻辑在 game_update.c；换房检测由主循环在更新之后调 game_room_step() */

/*==============================================================================
 *  渲染层号系统（★ 硬件没有 z 缓冲，层序 = 命令下发顺序，后画的盖前面的）
 *------------------------------------------------------------------------------
 *  每个元素提交时自己带一个 z（见 game.h 的 Z_* / ACTOR_Z）：
 *    · 静态层：场景 0 / 护盾 3000 / HUD 5000+ / GAME OVER 6000
 *    · 实体层：Z_ACTOR + 底边 y ⇒ 越靠下越近，自动盖住上面的东西
 *  绘制 = 全部 rl_push 进队列 → 按 (z, 提交顺序) 排序 → 依次下发命令。
 *  加一种新元素只要在自己的 submit() 里 push 一句，不用再动这里的排序逻辑。
 *============================================================================*/

/*----------------------------------------------------------------------------*/
/*  场景层：把这一帧的脏区恢复成地砖。
 *  ★ 必须最下面：旧精灵就是在这里被擦掉的（先整块擦、再按 z 重画）。  */
static void scene_restore(u32 fb, u32 now, int buf, int arg)
{
    (void)now; (void)buf; (void)arg;
    if (game_perf_keep_bg()) {
        if (game_floor_needs_prepare(fb)) {
            /* 换房失效已经覆盖旧实体和 HUD。准备整个当前视口会清除这些痕迹；
             * 再恢复旧脏区会把同一区域重画两次。
             * 屏幕外旧像素继续保持失效，直到重新进入可见范围。 */
            game_floor_prepare_frame(fb);
        } else {
            game_restore_dirty_view(fb, g_dirty);
        }
    }
}

/*----------------------------------------------------------------------------*/
/*  HUD：每一块单独一个 z（game.h 的 Z_HUD_*），从下往上依次登记。
 *  ★ 这里只管"画"，擦除 / 标脏已经在帧首的脏区里做完了。  */
static void hud_toast(u32 fb, u32 now, int buf, int arg)
{
    (void)now; (void)buf; (void)arg;
    game_hud_toast(fb, game_item_desc(g.toast_type));
}

static void hud_bossbar(u32 fb, u32 now, int buf, int arg)
{
    (void)now; (void)buf; (void)arg;
    game_hud_boss_bar(fb, g.boss_hp, g_boss_def.hp);
}

static void exit_draw(u32 fb, u32 now, int buf, int arg)
{
    (void)now; (void)buf; (void)arg;
    game_boss_draw_exit(fb);
}

static void hud_panel(u32 fb, u32 now, int buf, int arg)
{
    (void)now; (void)arg;
    /*  ★ full 只用于"相机移动 / 整屏重铺"；瓦片擦除波及的相邻行由
     *    game_hud_prepare_overlap() 精确补进 s_draw，不整面板重画。 */
    game_hud_frame(fb, buf, s_hv_fps, s_hv_cmd, s_hv_bullets, s_hv_coins,
                   game_hud_need_full || g_hud_full);
}

static void hud_vital(u32 fb, u32 now, int buf, int arg)
{
    (void)buf; (void)arg;
    game_vital_draw(fb, now, game_hud_need_full || g_hud_full || g_hud_need);
}

static void hud_pool(u32 fb, u32 now, int buf, int arg)
{
    (void)now; (void)arg;
    game_item_draw_pool(fb, buf, game_hud_need_full || g_hud_full);
}

static void hud_minimap(u32 fb, u32 now, int buf, int arg)
{
    (void)now; (void)buf; (void)arg;
    game_minimap_draw(fb);
}

static void hud_gameover(u32 fb, u32 now, int buf, int arg)
{
    int left = (int)(g.gameover_until - now);
    int secs = (left + 999) / 1000;

    (void)buf; (void)arg;
    if (secs < 0) secs = 0;
    game_hud_gameover(fb, g.score, secs);
}

/*  这一帧要画哪些 HUD 元素；登记顺序 = z 从下到上  */
static void hud_submit(void)
{
    if (g_toast_on)
        rl_push(Z_HUD_TOAST, hud_toast, 0);
    if (g.phase == PHASE_FIGHT)
        rl_push(Z_HUD_BOSSBAR, hud_bossbar, 0);     /* 没开战就不显示血条 */
    if (g_hud_need)
        rl_push(Z_HUD_PANEL, hud_panel, 0);
    rl_push(Z_HUD_VITAL, hud_vital, 0);             /* 心 + X 命数 + 死亡飘心 */
    rl_push(Z_HUD_MINIMAP, hud_minimap, 0);         /* 右上角小地图（道具池已下移 64） */
    rl_push(Z_HUD_POOL, hud_pool, 0);               /* 右侧道具池 */

    /*  ★ GAME OVER 窗口放最上面（Z_OVERLAY）：不能再被飘心 / 道具池压出洞  */
    if (g.phase == PHASE_GAMEOVER)
        rl_push(Z_OVERLAY, hud_gameover, 0);
}

/*----------------------------------------------------------------------------*/
/*  相机移动后：把【旧相机位置】的 HUD 区域标脏，让脏区恢复把旧字擦掉。
 *  HUD 在屏幕上的位置固定，但它在 framebuffer 里的世界坐标 = cam + 屏幕坐标。  */
/* 只有不透明 HUD 控件可以直接覆盖旧像素的相交部分。
 * 透明字模和道具图标必须擦除完整的旧覆盖范围。 */
static void mark_exposed_rect(int ox, int oy, int w, int h, int nx, int ny)
{
    int ix0 = ox > nx ? ox : nx;
    int iy0 = oy > ny ? oy : ny;
    int ix1 = ox + w < nx + w ? ox + w : nx + w;
    int iy1 = oy + h < ny + h ? oy + h : ny + h;

    if (ix0 >= ix1 || iy0 >= iy1) {
        game_mark_dirty(g_dirty, ox, oy, w, h);
        return;
    }
    if (iy0 > oy) game_mark_dirty(g_dirty, ox, oy, w, iy0 - oy);
    if (iy1 < oy + h) game_mark_dirty(g_dirty, ox, iy1, w, oy + h - iy1);
    if (ix0 > ox) game_mark_dirty(g_dirty, ox, iy0, ix0 - ox, iy1 - iy0);
    if (ix1 < ox + w) game_mark_dirty(g_dirty, ix1, iy0, ox + w - ix1, iy1 - iy0);
}

static void mark_camera_exposed(dirty_map_t dirty,
                                int old_x, int old_y, int new_x, int new_y)
{
    if (new_x > old_x)
        game_mark_dirty(dirty, old_x + SCR_W, new_y, new_x - old_x, SCR_H);
    else if (new_x < old_x)
        game_mark_dirty(dirty, new_x, new_y, old_x - new_x, SCR_H);
    if (new_y > old_y)
        game_mark_dirty(dirty, new_x, old_y + SCR_H, SCR_W, new_y - old_y);
    else if (new_y < old_y)
        game_mark_dirty(dirty, new_x, new_y, SCR_W, old_y - new_y);
}

static void mark_hud_rects_at(int camx, int camy)
{
    int x, y, w, h;

    /* 左侧面板（标签 + 数值 + 心/命数） */
    game_hud_mark_at(g_dirty, camx, camy);

    /* 顶部 BOSS 名称与血条 */
    if (g.phase == PHASE_FIGHT || g_bossbar_fade > 0)
        game_mark_dirty(g_dirty, camx + HUD_BOSS_X, camy + 0,
                        HUD_BOSS_W, HUD_BOSS_H);

    /* 右上角小地图 */
    game_minimap_rect(&x, &y, &w, &h);
    mark_exposed_rect(camx + x, camy + y, w, h,
                      g.cam_x + x, g.cam_y + y);

    /* 右侧道具池 */
    game_item_pool_rect(&x, &y, &w, &h);
    game_mark_dirty(g_dirty, camx + x, camy + y, w, h);

    /* 拾取字幕（显示期间 + 消失后的 3 帧擦除期） */
    if (g.toast_until != 0u || g_toast_fade > 0)
        game_mark_dirty(g_dirty, camx + HUD_TOAST_X, camy + HUD_TOAST_Y,
                        HUD_TOAST_W, HUD_TOAST_H);

    /* GAME OVER 窗口：它每帧都画，但相机一动旧窗口也得擦掉 */
    if (g.phase == PHASE_GAMEOVER)
        game_mark_dirty(g_dirty, camx + GAMEOVER_X, camy + GAMEOVER_Y,
                        GAMEOVER_W, GAMEOVER_H);
}

/* 与换房失效共用已有实体旧位置矩形，避免重复维护一套记录。 */
static void mark_old_actors(dirty_map_t dirty, int buf)
{
    int i;
    if (g.old_boss[buf].active)
        game_mark_dirty(dirty, g.old_boss[buf].x, g.old_boss[buf].y, BOSS_W, BOSS_H);
    if (g.old_player[buf].active)
        /* 往上多标 ITEM_HELD_DY 那一截：拾取时道具是举在头顶的 */
        game_mark_dirty(dirty, g.old_player[buf].x,
                        g.old_player[buf].y + ITEM_HELD_DY, PW, PH - ITEM_HELD_DY);
    if (g.old_item[buf].active)
        game_mark_dirty(dirty, g.old_item[buf].x,
                        g.old_item[buf].y + ITEM_ICON_DY - ITEM_FLOAT_AMP - 2,
                        ITEM_W, ITEM_H - ITEM_ICON_DY + ITEM_FLOAT_AMP + 2);
    for (i = 0; i < GAME_TRIPRISM_MAX; ++i)
        if (g.old_triprism[buf][i].active)
            game_mark_dirty(dirty, g.old_triprism[buf][i].x,
                            g.old_triprism[buf][i].y,
                            GAME_TRIPRISM_SIZE, GAME_TRIPRISM_SIZE);
#if GAME_PROFILE
    {
        u32 history_t0 = prof_now();
        game_bullets_mark_old(dirty, buf);
        s_pf_history_merge += prof_now() - history_t0;
    }
#else
    game_bullets_mark_old(dirty, buf);
#endif
    for (i = 0; i < N_MOBS; ++i)
        if (g.old_mob[buf][i].active)
            game_mark_dirty(dirty, g.old_mob[buf][i].x, g.old_mob[buf][i].y - 8, 32, 40);
    for (i = 0; i < N_COINS; ++i)
        if (g.old_coin[buf][i].active)
            game_mark_dirty(dirty, g.old_coin[buf][i].x, g.old_coin[buf][i].y, 24, 24);
}

/* 主循环选出既不属于显示、也不属于待显示请求的缓冲后，
 * 才能开始独立的地形恢复工作。 */
static void game_preupdate_restore(int buf)
{
#if GAME_BG_PREUPDATE_RESTORE && GAME_BG_COPY && !GAME_BG_EXACT_RECT
    if ((unsigned)buf >= GAME_FB_COUNT || game_perf_frozen() ||
        !game_perf_keep_bg() || game_floor_needs_prepare(g_fb[buf])) return;
#if GAME_TRIPLE_BUFFER
    triple_poll_pending();
    if (buf == g.front || buf == s_pending_fb) return;
#else
    if (buf == g.front) return;
#endif
    mark_old_actors(g_dirty, buf);
    game_shop_mark_old(g_dirty, buf);
    game_restore_preupdate(g_fb[buf], g_dirty);
#else
    (void)buf;
#endif
}

static void mark_layout_props(dirty_map_t dirty, int layout, int flip)
{
    const room_layout_t *pl = game_layout(layout);
    int k;
    if (!pl) return;
    for (k = 0; k < (int)pl->n_objs; ++k) {
        int kind = pl->objs[k].kind;
        if (kind == OBJ_WEB || kind == OBJ_STONE || kind == OBJ_BOX || kind == OBJ_CRATE) {
            int x, y;
            game_layout_obj_cell(pl, &pl->objs[k], flip, &x, &y);
            game_mark_dirty(dirty, x * TILE, y * TILE, TILE, TILE);
        }
    }
}

void game_capture_floor_damage(void)
{
    int buf, x, y, w, h, tx, ty;
    for (buf = 0; buf < GAME_FB_COUNT; ++buf) {
        /* 复用本帧脏区网格，不额外维护一份逐缓冲像素缓存。 */
        dirty_clear(g_dirty);
        game_dirty_begin();
        mark_old_actors(g_dirty, buf);
        game_bombs_prepare(g_dirty, buf);
        game_shop_mark_old(g_dirty, buf);
        mark_layout_props(g_dirty, game_room_layout(), game_room_flip());
        game_mark_doors_dirty(g_dirty);
        x = g.hud_cam_x[buf]; y = g.hud_cam_y[buf];
        game_hud_mark_at(g_dirty, x, y);
        game_mark_dirty(g_dirty, x + HUD_BOSS_X, y, HUD_BOSS_W, HUD_BOSS_H);
        game_mark_dirty(g_dirty, x + HUD_TOAST_X, y + HUD_TOAST_Y,
                        HUD_TOAST_W, HUD_TOAST_H);
        if (g.phase == PHASE_GAMEOVER)
            game_mark_dirty(g_dirty, x + GAMEOVER_X, y + GAMEOVER_Y, GAMEOVER_W, GAMEOVER_H);
        game_minimap_rect(&tx, &ty, &w, &h);
        game_mark_dirty(g_dirty, x + tx, y + ty, w, h);
        game_item_pool_rect(&tx, &ty, &w, &h);
        game_mark_dirty(g_dirty, x + tx, y + ty, w, h);
        game_mark_dirty(g_dirty, g.exit_x, g.exit_y, 32, 32);
        game_floor_damage(buf, g_dirty);
    }
}

/*----------------------------------------------------------------------------*/
/*  ③ 绘制  */
#if (GAME_PROFILE && GAME_BOSS_DEBUG && GAME_BOSS_AUTO_DIAG == 2) || GAME_BOSS_CAMERA_PAN
/* 纯渲染扫视：即使测试生成点被 L 形墙或道具挡住，也遍历每个视口。
 * 正式发布构建不会启用。 */
static void room_render_pan(u32 now)
{
    static int room = -1;
    static u32 epoch;
    u32 t, progress;
    int mx = game_world_w() - SCR_W, my = game_world_h() - SCR_H;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (room != game_room_id()) { room = game_room_id(); epoch = now; }
#if GAME_BOSS_CAMERA_PAN
    t = (now - epoch) % 6000u;
    progress = t < 500u ? 0u : t < 3000u ? t - 500u :
               t < 3500u ? 2500u : 6000u - t;
    g.cam_x = (int)(progress * (u32)mx / 2500u);
    g.cam_y = (int)(progress * (u32)my / 2500u);
#else
    t = (now - epoch) % 24000u;
    if (t < 2000u || t >= 22000u) progress = 0u;
    else if (t < 10000u) progress = t - 2000u;
    else if (t < 14000u) progress = 8000u;
    else progress = 22000u - t;
    g.cam_x = (int)(progress * (u32)mx / 8000u);
    g.cam_y = (int)(progress * (u32)my / 8000u);
#endif
}
#endif

static void game_draw(u32 now, int buf)
{
#if GAME_CAPACITY_AUDIT
    game_capacity_mark(0);
#endif
    u32 fb = g_fb[buf];
    int cam_moved, prop_dirty;
#if (GAME_PROFILE && GAME_BOSS_DEBUG && GAME_BOSS_AUTO_DIAG == 2) || GAME_BOSS_CAMERA_PAN
    room_render_pan(now);
#endif
#if GAME_BOSS_SKILL_DIAG
    u32 dense_draw_t0 = clint_getTimeLow(BSP_CLINT);
#endif
#if GAME_PROFILE
    u32 draw_t0 = prof_now(), draw_t1;
#endif

    /* 这里消费一次缓冲中的换房标脏计数，后面的两项处理必须共用同一次结果。 */
    prop_dirty = game_room_prop_dirty();
    {
        unsigned char key = (unsigned char)(game_room_id() + 1);
#if GAME_BG_PREUPDATE_RESTORE
        if (s_static_room_key[buf] != key || game_floor_needs_prepare(fb) ||
            g.door_dirty > 0 || prop_dirty)
            game_restore_preupdate_cancel();
#endif
        s_static_force = s_static_room_key[buf] != key || game_floor_needs_prepare(fb);
        s_static_room_key[buf] = key;
    }

    /*  ★ 这两个是"本帧"标志：必须在帧首清掉。
     *    g_hud_need 以前只置不清 ⇒ 一旦有过一次刷新就永远为 1，
     *    连带动不动就 game_vital_invalidate()，把血量行每帧擦一遍。 */
    g_hud_need = 0;
    if (game_floor_needs_prepare(fb)) {
        /* 另一块缓冲轮到绘制时再修复，不在换房后做阻塞式整屏拷贝。
         * HUD 在地形恢复完成后重新绘制。 */
        g_hud_full = 1;
        game_hud_need_full = 1;
    }

    /*  ★ 相机移动：先擦【这块缓冲上次画 HUD 时】的位置，再在 new cam 处重画。
     *    HUD 是"屏幕固定"的，但画进 framebuffer 时必须跟着相机走。
     *    ★ 必须按 buf 各记一份：双缓冲下这块缓冲上次画可能是两帧前、相机更旧。  */
    cam_moved = (g.cam_x != g.hud_cam_x[buf]) || (g.cam_y != g.hud_cam_y[buf]);
#if GAME_PROFILE && GAME_BOSS_STAGE_TEST == 2
    if(cam_moved) ++s_trace_cam;
#endif
#if GAME_PROFILE && GAME_BOSS_DEBUG && GAME_BOSS_SKILL_DIAG
    s_skill_cam = cam_moved;
#endif
#if GAME_PROFILE && GAME_BOSS_DEBUG && GAME_BOSS_AUTO_DIAG
    s_camdiag_moved = cam_moved;
#endif
    if (cam_moved) {
        mark_camera_exposed(g_dirty, g.hud_cam_x[buf], g.hud_cam_y[buf],
                           g.cam_x, g.cam_y);
        mark_hud_rects_at(g.hud_cam_x[buf], g.hud_cam_y[buf]);
        g_hud_need = 1;
        g_hud_full = 1;                 /* 整块面板全画，不留"只画变了的那行"的缝 */
        game_hud_need_full = 1;
        /* 下面会完整重画当前缓冲；若此处同时让两块缓冲失效，
         * 相机停止后还会把所有行再次标脏。 */
    }

    /*  ★ 换房后的两帧：把 4 扇门的两格标脏（两块缓冲各重画一次）。
     *    地图没变时 game_room_commit 不再整屏重铺，门开/关的变化就靠这里更新。  */
    if (g.door_dirty > 0) {
        game_mark_doors_dirty(g_dirty);
        --g.door_dirty;
    }

    /*  exit：触发/换关后擦两帧；不在 Boss 房时也要把旧 exit 擦掉  */
    if (g.exit_dirty > 0) {
        game_mark_dirty(g_dirty, g.exit_x, g.exit_y, 32, 32);
        --g.exit_dirty;
    } else if (g.exit_active && game_room_id() != g.exit_room) {
        game_mark_dirty(g_dirty, g.exit_x, g.exit_y, 32, 32);
    }

    /*  商店：商品按 room 持久；先擦这块缓冲上次画的位置  */
    if (!GAME_BG_PREUPDATE_RESTORE || !game_restore_preupdate_active(fb)) {
        game_shop_mark_old(g_dirty, buf);
        /* 地形或背景失效后，需要重新合并旧实体历史。 */
        mark_old_actors(g_dirty, buf);
    }
    /*  蜘蛛网已经没有实例（位置来自蓝图）：只在换房后的两帧里，
     *  把【旧房间】的网所在格补标脏，否则上一个房间的网会留在缓冲里。  */
    if (prop_dirty)
        mark_layout_props(g_dirty, game_room_prev_layout(), game_room_prev_flip());
    {
        int ex, ey, k;

        for (k = 0; game_stones_erase_at(k, &ex, &ey); ++k)
            game_mark_dirty(g_dirty, ex, ey, 32, 32);
    }

    /* HUD 的文字：只有"数字变了"或"有精灵压在它那块"时才重画并标脏。
       平时一条命令都不花 —— 之前每帧都标 32 个格子，把帧率压下去了（移动发卡）。 */
    g_alive = game_bullets_alive();
    hud_vals_update(g.fps, g_alive, g.cmd_frame, g.coins);

    /*  ★ HUD 擦除 = 按【整瓦片】恢复场景。相机的 cam_y 不一定是 32 的倍数，
     *    所以"只标脏一行 / 只补画一行"会把相邻行（STAGE、DMG ...）的像素
     *    擦掉一截却没人补画 ⇒ 大房间里左侧面板持续闪烁、缺字。
     *    顺序很重要：必须等 game_vital_prepare() / game_item_prepare() /
     *    飘心标脏都做完，再按瓦片重查一次，把被波及的相邻行补进 s_draw。  */
    {
        int hud_changed = game_hud_prepare(g_dirty, buf,
                                           s_hv_fps, s_hv_cmd, s_hv_bullets, s_hv_coins);

        game_vital_prepare(g_dirty, buf);
        game_item_prepare(g_dirty, buf);
        game_bombs_prepare(g_dirty, buf);
        game_vital_mark_dirty(g_dirty);     /* 飘心也会压到面板上：先标脏再查重叠 */

        /*  ★ 第二次检查：vital / 飘心 / 实体的脏瓦片可能跨到相邻 HUD 行，
         *    补画这些行（只置重画位，不重复标脏）。  */
        if (game_hud_prepare_overlap(g_dirty, buf))
            hud_changed = 1;

        game_hud_need_full = 0;
        g_hud_need = g_hud_need || hud_changed || g_hud_full;
        /* 面板被实体压到时，hud_vital 会重画生命行，
         * 但不丢弃两块缓冲缓存的 HP/命数，也不使下一帧继续脏。 */
    }

    /*  ★ 临时文字（拾取字幕 / WIN）的擦除规则：
     *    显示期间每帧标脏；【结束后还要再标 3 帧】。
     *    为什么？双缓冲下每帧只画一块，一帧只擦得掉一块 ——
     *    只擦一帧的话，另一块缓冲里的字会留下来，两块交替显示就成了"疯狂闪"。  */

    /* 拾取字幕 */
    {
        g_toast_on = (g.toast_until != 0u && (int)(now - g.toast_until) < 0);
        if (g.toast_until != 0u && !g_toast_on) {
            g.toast_until = 0u;
            g_toast_fade = 3;                   /* 之后 3 帧继续擦 */
        }
        if (g_toast_on) {
            /* 始终恢复整块固定字幕带。下一次拾取文案可能更短，
             * 只标当前字模会残留上一次文案的尾部。 */
            game_mark_hud_dirty(g_dirty, HUD_TOAST_X, HUD_TOAST_Y,
                                HUD_TOAST_W, HUD_TOAST_H);
            /* 字幕的脏区会按【整瓦片】擦，连带血条所在那行 ⇒ 一起标脏，血条随后重画 */
            game_mark_hud_dirty(g_dirty, HUD_HP_X, HUD_HP_Y, HUD_HP_W, HUD_HP_H);
        } else if (g_toast_fade > 0) {
            game_mark_hud_dirty(g_dirty, HUD_TOAST_X, HUD_TOAST_Y,
                                HUD_TOAST_W, HUD_TOAST_H);
            --g_toast_fade;
        }
    }

    /*  Boss 血条：离开 Boss 房 / 击破后要擦掉 ——
     *   它是画在面板外面（y = 0..44）的，不在面板行的脏区里；
     *   和临时文字一样，消失后再擦 3 帧（双缓冲两块都要擦到）。  */
    {
        int bar_on = (g.phase == PHASE_FIGHT);

        if (bar_on) {
            g_bossbar_fade = 3;
        } else if (g_bossbar_fade > 0) {
            game_mark_hud_dirty(g_dirty, HUD_BOSS_X, 0, HUD_BOSS_W, HUD_BOSS_H);
            --g_bossbar_fade;
        }
    }

#if GAME_CAPACITY_AUDIT
    game_capacity_mark(1);
#endif
#if GAME_BG_EARLY_RESTORE
    /* 脏区网格已经完整。先下发地形命令，再处理历史复制、队列构建和排序，
     * 让 GPU 与这段 CPU 工作重叠。实体/HUD 命令仍按 FIFO 紧跟地形之后；
     * s_static_force 保留恢复前的失效决策。 */
    scene_restore(fb, now, buf, 0);
#endif

    /*  ★ 帧首：把"这块缓冲里已经过期"的 HUD 行 / 血量行 / 道具池标脏，
     *    让下面的脏区恢复统一擦掉 —— 擦除只在帧首做，绘制层不再贴地砖，
     *    否则会把走到面板上的人物一起盖掉。
     *    （飘心的标脏已提前到上面的 HUD 重叠检查之前。）  */
#if GAME_PROFILE
    {
        u32 history_t0 = prof_now();
        game_bullets_sync(buf);
        s_pf_history_build += prof_now() - history_t0;
    }
#else
    game_bullets_sync(buf);             /* 脏区用：记下这一帧每颗弹画在哪 */
#endif
    game_mobs_sync(buf);
    game_coins_sync(buf);               /* 金币同样：擦完旧位置再把 old_coin 对齐到这一帧 */
    game_shop_sync(buf);                /* 商店商品：房间 + 位置的 old 对齐 */

    /*  ★ 先提交、再统一排序绘制：z 小的在下、z 大的在上（同 z 保持提交顺序）。
     *    实体的 z 在各自的 submit() 里 = Z_ACTOR + 底边 y ⇒ 自然的透视遮挡。
     *  ★ HUD 必须【最先】入队：rl_push 满员时丢弃后来的元素，而 HUD 的 z 最大、
     *    入队早晚不影响绘制顺序（排序只看 z）。以前 HUD 最后入队 ⇒ Boss 房弹幕一多
     *    队列就满、先丢 HUD ⇒ 表现正是"字母/血量闪烁、数字变 00"。  */
#if GAME_PROFILE
    draw_t1 = prof_now();
    s_pf_draw_prepare += draw_t1 - draw_t0;
    draw_t0 = draw_t1;
#endif
    rl_begin();
    hud_submit();                         /* z 最大：先入队，永远不会被丢弃 */
#if !GAME_BG_EARLY_RESTORE
    rl_push(Z_SCENE, scene_restore, 0);   /* 脏区恢复（擦旧精灵）必须最先 */
#endif
    if (g.exit_active && game_room_id() == g.exit_room)
        rl_push(Z_SCENE + 5, exit_draw, 0);  /* exit 在地板上、人物下面 */
    game_shop_submit();                   /* 商店商品也在地板上、人物下面 */
    game_player_submit();                 /* 同深度时玩家在实体里最靠后 ⇒ 先提交 */
    game_triprism_submit();
    game_bullets_submit();
    game_boss_submit();                   /* 本体 + 锁血护盾（Z_SHIELD） */
    game_mobs_submit();
    game_coins_submit();
    game_web_submit(buf);
    game_stones_submit(buf);
    game_item_submit();
    game_bombs_submit();

#if GAME_PROFILE
    draw_t1 = prof_now();
    s_pf_draw_submit += draw_t1 - draw_t0;
    draw_t0 = draw_t1;
#endif
    rl_draw_all(fb, now, buf);
#if GAME_PROFILE
    s_pf_draw_execute += prof_now() - draw_t0;
#endif

    game_stones_erase_tick();          /* 待擦队列：这一帧已经标过了，ttl-- */

    g_hud_full = 0;
    g.hud_cam_x[buf] = g.cam_x;
    g.hud_cam_y[buf] = g.cam_y;
#if GAME_BOSS_SKILL_DIAG
    if (s_dense_diag_active) {
        u32 dt = clint_getTimeLow(BSP_CLINT) - dense_draw_t0;
        s_dense_diag_draw_sum += dt;
        if (dt > s_dense_diag_draw_max) s_dense_diag_draw_max = dt;
        ++s_dense_diag_draw_n;
    }
#endif
}

/*----------------------------------------------------------------------------*/
/*  ④ 帧尾：交帧 → 统计 → 探针  */
static int frame_end(u32 now, int buf)
{
#if GAME_PROFILE
    u32 t0 = prof_now();
#endif
#if GAME_BOSS_SKILL_DIAG
    u32 dense_fence_t0 = s_dense_diag_active ? (u32)clint_getTime(BSP_CLINT) : 0u;
#endif

    if (!game_finish_gpu()) return 0;
#if GAME_RUN_AUDIT
    game_run_audit_end();
#endif
#if GAME_CAPACITY_AUDIT
    game_capacity_work_end();
#endif
#if GAME_BOSS_SKILL_DIAG
    if (s_dense_diag_active) {
        u32 dt = (u32)clint_getTime(BSP_CLINT) - dense_fence_t0;
        s_dense_diag_fence_sum += dt;
        if (dt > s_dense_diag_fence_max) s_dense_diag_fence_max = dt;
        ++s_dense_diag_fence_n;
    }
    dense_fence_t0 = clint_getTimeLow(BSP_CLINT);
#endif
#if GAME_PROFILE
    {
        u32 t1 = prof_now();
#if GAME_BOSS_DEBUG && GAME_BOSS_SKILL_DIAG
        s_skill_work = t1 - s_skill_work_start;
#endif
#if GAME_BOSS_STAGE_TEST
        s_stage_fence = t1 - t0;
#endif
#if GAME_BOSS_DEBUG && GAME_BOSS_SKILL_DIAG
        s_skill_fence = t1 - t0;
#endif
#if GAME_BOSS_DEBUG && GAME_BOSS_AUTO_DIAG
        s_camdiag_fence = t1 - t0;
#endif
        prof_add(&s_pf_fence, &s_pf_fence_max, t1 - t0);
        t0 = t1;
    }
#endif
#if GAME_PROFILE
    if (IRQ_frame_count() != s_pf_irq_start) ++s_pf_irq_late;
#if GAME_BOSS_DEBUG && GAME_BOSS_AUTO_DIAG
    s_camdiag_late = (IRQ_frame_count() != s_pf_irq_start);
#endif
#endif
#if GAME_TRIPLE_BUFFER
    if (!triple_retire_pending()) return 0;
    lineBuffer_setFullBase(game_view_fb(g_fb[buf]), game_view_fb(g_fb[buf]));
    s_pending_fb = buf;
    /* 两次 APB 写完成后才读取 IRQ 基准。若写入之间发生中断，
     * 不能让旧的可见缓冲被提前判定为可写。 */
    s_pending_irq = IRQ_frame_count();
#else
    if (!game_present(g_fb[buf])) return 0;
#endif
#if GAME_BOSS_SKILL_DIAG
    if (s_dense_diag_active) {
        u32 dt = clint_getTimeLow(BSP_CLINT) - dense_fence_t0;
        s_dense_diag_present_sum += dt;
        if (dt > s_dense_diag_present_max) s_dense_diag_present_max = dt;
    }
#endif
#if GAME_PROFILE
    {
        u32 t1 = prof_now();
        u32 irq_now = IRQ_frame_count();
        if (s_pf_irq_last != 0u) {
            u32 ticks = irq_now - s_pf_irq_last;
            s_pf_irq_ticks += ticks;
            if (ticks > s_pf_irq_max) s_pf_irq_max = ticks;
        }
        s_pf_irq_last = irq_now;
        prof_add(&s_pf_present, &s_pf_present_max, t1 - t0);
        ++s_pf_frames;
    }
#endif

    g.cmd_frame = (int)(gpu2d_getCmdDone() - g.cmd_before);
#if GAME_RUN_AUDIT
    game_run_audit_frame();
#endif
#if GAME_BOSS_SKILL_DIAG
    game_dense_diag_frame();
#endif
#if GAME_PROFILE && GAME_BOSS_STAGE_TEST
    boss_stage_profile_frame();
#endif
#if GAME_PROFILE && GAME_BOSS_DEBUG && GAME_BOSS_SKILL_DIAG
    boss_skill_diag_frame();
#endif
#if GAME_PROFILE && GAME_BOSS_DEBUG && GAME_BOSS_AUTO_DIAG
    boss_cam_diag_frame(now);
#endif

    /* 每秒分别统计峰值子弹数和峰值命令数；空房间也能看到真实命令峰值。 */
    ++g.probe_frames;
    ++g.probe_frame_no;
    if (g.probe_epoch == 0u) g.probe_epoch = now;
    if (g_alive > g.probe_peak) {
        g.probe_peak = g_alive;
        g.probe_peak_frame = (int)g.probe_frame_no;
    }
    if (g.cmd_frame > g.probe_peak_cmd) g.probe_peak_cmd = g.cmd_frame;
    if ((u32)(now - g.probe_epoch) >= 1000u) {
#if GAME_UART_TELEMETRY
        u32 dt = now - g.probe_epoch;
        /*  ★ 只能用 %d / %x / %X / %s / %c：这个 BSP 的 bsp_printf 不认 %u，
         *    遇到 %u 会一路扫到字符串末尾，把后面的 "\r\n" 一起吃掉（日志就不换行了）。  */
        {
#if GAME_PROFILE
            u32 ba, be, bp;
            game_bullet_draw_stats(&ba, &be, &bp);
            bsp_printf("PROBE 1s: peak bullets=%d at frame=%d, fps=%d, frames=%d, cmd/max=%d, restore=%d/%d(d/c=%d/%d), bullet_draw=%d/%d/%d, px=%d py=%d hp=%d life=%d, z-drop=%d\r\n",
                       g.probe_peak, g.probe_peak_frame,
                       (int)((g.probe_frames * 1000u) / (dt ? dt : 1u)),
                       (int)g.probe_frames, g.probe_peak_cmd,
                       (int)game_restore_dirty_cells(),
                       (int)game_restore_dirty_cmds(),
                       (int)game_restore_direct_frames(),
                       (int)game_restore_copy_frames(),
                       (int)ba, (int)be, (int)bp,
                       g.x, g.y, game_vital_hp(), game_vital_lives(),
                       (int)rl_dropped());
#else
            bsp_printf("PROBE 1s: peak bullets=%d at frame=%d, fps=%d, frames=%d, cmd/max=%d, restore=%d/%d, px=%d py=%d hp=%d life=%d, z-drop=%d\r\n",
                       g.probe_peak, g.probe_peak_frame,
                       (int)((g.probe_frames * 1000u) / (dt ? dt : 1u)),
                       (int)g.probe_frames, g.probe_peak_cmd,
                       (int)game_restore_dirty_cells(),
                       (int)game_restore_dirty_cmds(),
                       g.x, g.y, game_vital_hp(), game_vital_lives(),
                       (int)rl_dropped());
#endif
        }
#endif
#if GAME_PROFILE
        prof_report();
#endif
        g.probe_epoch = now;
        g.probe_frames = 0u;
        g.probe_peak = 0;
        g.probe_peak_frame = (int)g.probe_frame_no;
        g.probe_peak_cmd = 0;
    }

#if GAME_BOSS_BULLET_SWEEP
    game_boss_stress_frame(now);
#endif
    return game_perf_tick(now);
}

/*----------------------------------------------------------------------------*/
void game_demo_run(void)
{
    bsp_printf("\r\nGAME: WASD=move/facing, hold numpad/arrow=aim+fire\r\n");
    bsp_printf("BOARD: KEY0..3=move/turn, auto fire only when an enemy is ahead\r\n");
    bsp_printf("GAME CONFIG: buffers=%d pool=%d enemy_cap=%d rings=%d logic_hz=%d tint=%d restore_early=%d\r\n",
               GAME_FB_COUNT, N_BULLETS, ENEMY_CAP, GAME_BOSS_DENSE_RINGS,
               GAME_FIXED_UPDATE ? (int)GAME_LOGIC_HZ : 0, GAME_BULLET_TINT,
               GAME_BG_EARLY_RESTORE);
    bsp_printf("DEBUG CMDS: R = restart, Y = spawn item+base, E = bomb (type + Enter)\r\n");
    bsp_printf("TEST MODE: T = toggle invincibility and %d damage (GamePad / type + Enter)\r\n", GAME_TEST_DAMAGE);
#if GAME_BOSS_SHORTCUT
    bsp_printf("BOSS SHORTCUT: 1 = jump to boss room\r\n");
#endif
#if GAME_BOSS_DEBUG
    bsp_printf("BOSS DEBUG: player damage disabled\r\n");
#if GAME_BOSS_BULLET_SWEEP
    bsp_printf("BOSS TEST: 1 starts automatic moving-bullet 60Hz sweep\r\n");
#endif
#if GAME_PROFILE
    bsp_printf("BOSS TEST: /stat prints accumulated frame timing, then resets it\r\n");
#endif
#endif
    if (PERF_TEST_MODE == 1)
        bsp_printf("PERF TEST: fixed model, bullet levels 16/32/48/64/80/96\r\n");
    else if (PERF_TEST_MODE == 2)
        bsp_printf("PERF TEST: %d moving bullets, background restore ON\r\n", PERF_TEST_BULLETS);
    else if (PERF_TEST_MODE == 3)
        bsp_printf("PERF TEST: %d moving bullets, background restore OFF\r\n", PERF_TEST_BULLETS);

    if (!game_init()) return;
#if GAME_CAPACITY_AUDIT
    if (GAME_CAPACITY_BOSS) game_room_debug_boss();
    game_boss_stress_arm();
#endif
#if GAME_TRIPLE_BUFFER
    s_pending_fb = -1;
#endif
#if GAME_FIXED_UPDATE
    game_logic_reset();
#endif

    for (;;) {
        u32 keys = game_keys_read();
        u32 aim_keys = game_aim_read();
        u32 now  = game_ms();
        int back;

#if GAME_TRIPLE_BUFFER
        back = triple_free_buffer();
        if (back < 0) return;
#else
        back = g.front ^ 1;
#endif

        /* R 键按一下就重开（输入层锁存过，快按也不会丢） */
        if (game_take_restart() ||
            (g.phase == PHASE_GAMEOVER && (int)(now - g.gameover_until) >= 0)) {
#if GAME_TRIPLE_BUFFER
            if (!triple_retire_pending()) return;
#endif
            if (!game_restart()) return;
#if GAME_FIXED_UPDATE
            game_logic_reset();
#endif
            continue;                       /* 这一帧已重开，别再用旧状态画 */
        }

#if GAME_BOSS_SHORTCUT
        if (game_take_boss_jump()) {
            game_room_debug_boss();
#if GAME_BOSS_BULLET_SWEEP
            game_boss_stress_arm();
#endif
        }
#endif
#if GAME_BOSS_DEBUG && GAME_PROFILE
        if (game_take_debug_stats()) game_debug_frame_report();
#endif

        /* 换房流程独占后台缓冲。必须先完成当前换房，
         * 再接受下一个排队请求或恢复普通模型/渲染更新。 */
        if (game_room_rebuild_pending()) {
#if GAME_TRIPLE_BUFFER
            if (!triple_retire_pending()) return;
#endif
            if (!game_room_finish_rebuild()) return;
#if GAME_FIXED_UPDATE
            game_logic_reset();
#endif
            continue;
        }
        if (game_room_pending()) {
            if (!prepare_room_transition()) return;
            if (!game_room_commit()) return;
#if GAME_FIXED_UPDATE
            game_logic_reset();
#endif
            continue;
        }
#if GAME_RUN_AUDIT
        game_run_audit_begin();
#endif
#if GAME_CAPACITY_AUDIT
        game_capacity_work_begin();
#endif
#if GAME_PROFILE
        {
            u32 t0 = prof_now();
#if GAME_BOSS_DEBUG && GAME_BOSS_SKILL_DIAG
            s_skill_work_start = t0;
#endif

            frame_begin(now);
            game_preupdate_restore(back);
            {
                u32 t1 = prof_now();
                prof_add(&s_pf_begin, &s_pf_begin_max, t1 - t0);
                t0 = t1;
            }
            {
                int steps = game_logic_run(keys, aim_keys, now);
                s_pf_sim_steps += (u32)steps;
#if GAME_BOSS_DEBUG && GAME_BOSS_SKILL_DIAG
                s_skill_steps = steps;
#endif
#if GAME_BOSS_DEBUG && GAME_BOSS_AUTO_DIAG
                s_camdiag_steps = steps;
#endif
            }
            {
                u32 t1 = prof_now();
                prof_add(&s_pf_update, &s_pf_update_max, t1 - t0);
                t0 = t1;
            }
            game_draw(now, back);
            {
                u32 t1 = prof_now();
#if GAME_BOSS_STAGE_TEST
                s_stage_draw = t1 - t0;
#endif
#if GAME_BOSS_DEBUG && GAME_BOSS_SKILL_DIAG
                s_skill_draw = t1 - t0;
#endif
#if GAME_BOSS_DEBUG && GAME_BOSS_AUTO_DIAG
                s_camdiag_draw = t1 - t0;
#endif
                prof_add(&s_pf_draw, &s_pf_draw_max, t1 - t0);
            }
        }
#else
        frame_begin(now);
        game_preupdate_restore(back);
        game_logic_run(keys, aim_keys, now);
        game_draw(now, back);
#endif
#if GAME_CAPACITY_AUDIT
        game_capacity_mark(2);
#endif
        if (!frame_end(now, back)) return;

#if !GAME_TRIPLE_BUFFER
        g.front = back;
#endif
    }
}
