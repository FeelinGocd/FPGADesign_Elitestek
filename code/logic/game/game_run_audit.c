//==============================================================================
//  game_run_audit.c —— 普通 Boss 战的只读运行审计
//------------------------------------------------------------------------------
//  干什么      : 在 RAM 中记录各 Boss 阶段的帧耗时、IRQ 间隔和敌弹峰值。
//  启用条件    : GAME_RUN_AUDIT != 0；默认关闭。
//  接口契约    : 只观察游戏状态，不修改相机、伤害、弹幕发射或串口归属。
//  数据读取    : 结构定义见 game_run_audit.h，读取时应检查 sequence 一致性。
//==============================================================================
#include "game.h"
#if GAME_RUN_AUDIT
#include "game_run_audit.h"
#include "game_schedule.h"
#include "IRQ_frame.h"
#include "render_list.h"
#include "bsp.h"

_Static_assert(sizeof(game_run_audit_bucket_t) == 44u, "run-audit bucket layout");
_Static_assert(sizeof(game_run_audit_t) == 244u, "run-audit RAM layout");

volatile game_run_audit_t g_game_run_audit __attribute__((used, externally_visible)) = {
    .magic = GAME_RUN_AUDIT_MAGIC,
    .version = GAME_RUN_AUDIT_VERSION,
    .hz = BSP_CLINT_HZ,
    .struct_size = sizeof(game_run_audit_t),
    .bucket_count = GAME_RUN_AUDIT_BUCKETS
};

static u32 start_tick, work_us, begin_drawn, begin_drops;
static u32 last_irq, warmup;
static int begun, ended, have_baseline;
static int last_room = -1, last_bucket = -1, last_cam_x, last_cam_y;
#if GAME_RUN_DETAIL
volatile game_run_detail_t g_game_run_detail __attribute__((used, externally_visible));
static u32 part_tick, part_us[RUN_PARTS];
static u32 begin_alpha, begin_player;

void game_run_audit_mark(unsigned part)
{
    u32 tick;
    if (!begun || part >= RUN_PARTS) return;
    tick = clint_getTimeLow(BSP_CLINT);
    part_us[part] = (tick - part_tick) / (u32)(BSP_CLINT_HZ / 1000000u);
    part_tick = tick;
}
#endif

void game_run_audit_rebase(void)
{
    last_room = last_bucket = -1;
    warmup = 0;
    have_baseline = begun = ended = 0;
}

static int fight_bucket(void)
{
    if (g.phase != PHASE_FIGHT || g.boss_hp <= 0 || !game_world_is_boss()) return -1;
    if (g.boss_locked) {
        if (g.boss_phase == BOSS_PHASE_RAIN) return GAME_RUN_INTERMISSION1;
        if (g.boss_phase == BOSS_PHASE_COMBINED) return GAME_RUN_INTERMISSION2;
        return -1;
    }
    if (g.boss_phase < BOSS_PHASE_CORE || g.boss_phase > BOSS_PHASE_COMBINED) return -1;
    return g.boss_phase * 2;
}

void game_run_audit_begin(void)
{
    begun = ended = 0;
    if (fight_bucket() < 0) return;
    start_tick = clint_getTimeLow(BSP_CLINT);
#if GAME_RUN_DETAIL
    part_tick = start_tick;
    game_bullet_draw_stats(&begin_alpha, &begin_drawn, &begin_player);
#else
    game_bullet_draw_stats(0, &begin_drawn, 0);
#endif
    begin_drops = rl_dropped();
    begun = 1;
}

void game_run_audit_end(void)
{
    if (!begun) return;
    game_run_audit_mark(RUN_FENCE);
    work_us = (clint_getTimeLow(BSP_CLINT) - start_tick) /
              (u32)(BSP_CLINT_HZ / 1000000u);
    ended = 1;
}

void game_run_audit_frame(void)
{
    int bucket = fight_bucket();
    int room;
    u32 irq, drawn, drop_delta, delta, old_sum, new_sum;
    int enemies;
    volatile game_run_audit_bucket_t *entry;
    if (bucket < 0) {
        /* 离开战斗时只重建节拍基线，不产生新的日志。 */
        last_room = last_bucket = -1;
        warmup = 0; have_baseline = begun = ended = 0;
        return;
    }
    room = game_room_id();
    irq = IRQ_frame_count();
    if (room != last_room || bucket != last_bucket) {
        last_room = room; last_bucket = bucket;
        warmup = GAME_RUN_AUDIT_WARMUP;
        have_baseline = 0;
    }
    if (warmup) {
        --warmup;
    } else if (begun && ended) {
        game_bullet_draw_stats(0, &drawn, 0);
        drawn -= begin_drawn; /* unsigned arithmetic also handles counter wrap */
        drop_delta = rl_dropped() - begin_drops;
        delta = have_baseline ? irq - last_irq : 0u;
        enemies = game_enemy_bullets_alive();
#if GAME_RUN_DETAIL
        {
            unsigned i, bin = work_us / 2000u;
            u32 alpha, player_drawn, split_drawn;
            int players = game_bullets_alive() - enemies;
            int moved = have_baseline && (g.cam_x != last_cam_x || g.cam_y != last_cam_y);
            game_bullet_draw_stats(&alpha, 0, &player_drawn);
            alpha -= begin_alpha;
            player_drawn -= begin_player;
            /* Without MagicCard, only actual TriPrism split draws use player tint. */
            split_drawn = (g.item_mask & (1u << ITEM_MAGICCARD)) ? 0u : player_drawn;
#if GAME_BULLET_TINT
            player_drawn += alpha;
#else
            /* Alpha-only enemies share this counter, so do not label them players. */
            player_drawn = 0u;
#endif
            if (players > 0 && (u32)players > g_game_run_detail.player_peak)
                g_game_run_detail.player_peak = (u32)players;
            if (player_drawn > g_game_run_detail.drawn_player_peak)
                g_game_run_detail.drawn_player_peak = player_drawn;
            if (split_drawn > g_game_run_detail.drawn_split_peak)
                g_game_run_detail.drawn_split_peak = split_drawn;
            if (drawn + player_drawn > g_game_run_detail.total_drawn_peak)
                g_game_run_detail.total_drawn_peak = drawn + player_drawn;
            ++g_game_run_detail.frames;
            if (work_us > 1000000u / GAME_LOGIC_HZ) ++g_game_run_detail.over_budget;
            if (enemies >= game_enemy_bullet_limit() - 8) {
                ++g_game_run_detail.full_frames;
                if (delta > 1u) ++g_game_run_detail.full_gaps;
            }
            if (drawn >= (u32)(game_enemy_bullet_limit() - 8)) {
                ++g_game_run_detail.full_drawn_frames;
                if (delta > 1u) ++g_game_run_detail.full_drawn_gaps;
                if (split_drawn) {
                    ++g_game_run_detail.full_with_split_frames;
                    if (delta > 1u) ++g_game_run_detail.full_with_split_gaps;
                    if (split_drawn > g_game_run_detail.full_split_peak)
                        g_game_run_detail.full_split_peak = split_drawn;
                }
            }
            ++g_game_run_detail.histogram[bin < 16u ? bin : 15u];
            for (i = 0; i < RUN_PARTS; ++i) {
                g_game_run_detail.sum_us[i] += part_us[i];
                if (part_us[i] > g_game_run_detail.max_us[i])
                    g_game_run_detail.max_us[i] = part_us[i];
                if (work_us > g_game_run_detail.worst_work_us)
                    g_game_run_detail.worst_us[i] = part_us[i];
            }
            if (work_us > g_game_run_detail.worst_work_us) {
                g_game_run_detail.worst_work_us = work_us;
                g_game_run_detail.worst_enemy = enemies;
                g_game_run_detail.worst_drawn = drawn;
                g_game_run_detail.worst_cam = moved;
                g_game_run_detail.worst_cmd = g.cmd_frame;
            }
        }
#endif
        entry = &g_game_run_audit.buckets[bucket];
        ++g_game_run_audit.sequence;
        ++entry->frames;
        old_sum = entry->work_sum_us_lo;
        new_sum = old_sum + work_us;
        entry->work_sum_us_lo = new_sum;
        if (new_sum < old_sum) ++entry->work_sum_us_hi;
        if (work_us > entry->work_max_us) entry->work_max_us = work_us;
        if (delta > 1u) ++entry->submit_irq_gap_count;
        if (delta > entry->submit_irq_delta_max) entry->submit_irq_delta_max = delta;
        if (have_baseline && (g.cam_x != last_cam_x || g.cam_y != last_cam_y))
            ++entry->camera_moved_frames;
        if (enemies > 0 && (u32)enemies > entry->enemy_peak) entry->enemy_peak = (u32)enemies;
        if (drawn > entry->drawn_enemy_peak) entry->drawn_enemy_peak = drawn;
        entry->queue_drops += drop_delta;
        if (g.cmd_frame > 0 && (u32)g.cmd_frame > entry->cmd_max) entry->cmd_max = (u32)g.cmd_frame;
        ++g_game_run_audit.sequence;
    }
    last_irq = irq; have_baseline = 1;
    last_cam_x = g.cam_x; last_cam_y = g.cam_y;
    begun = ended = 0;
}
#endif
