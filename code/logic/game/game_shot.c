//==============================================================================
//  game_shot.c —— 通用弹幕发射器（和"谁在打"完全解耦）
//------------------------------------------------------------------------------
//  干什么      :
//    · 挂一个 shot_t 在身上，给它一套 move_set_t（招式表）+ 炮口坐标，
//      它就按表放弹幕。Boss 挂着它、小怪以后也挂着它。
//    · 敌弹上限、招式间清场、关卡/残血加速都在这里统一处理。
//  上一级是谁  : game/game_boss.c（Boss 的发射器）、以后的小怪
//  接口契约    :
//    · 宿主每帧做两件事：更新 s->mx/my（炮口）、s->tx/ty（瞄准点，aim 图案用）
//    · 之后调 game_shot_step(s) 即可；不想让它打就 game_shot_stop(s)
//  为什么这么做:
//    · 招式、节奏、图案全是数据 ⇒ 加弹幕不用改这个文件
//    · 发射器和宿主分离 ⇒ 小怪用同一套图案，不用复制代码
//==============================================================================
#include "game.h"
#include "CI_gpu2d.h"
#if GAME_BOSS_SKILL_DIAG
#include "bsp.h"
#endif

/*----------------------------------------------------------------------------*/
/*  朝 angle 方向发一发（angle 是 1/64 圈的格号）  */
static void shot_angle(const shot_t *s, int angle, int speed,
                       int motion, int mx, int my)
{
    int a = angle & (ANGLE_STEPS - 1);
    /*  ★ 速度放大到 1/8 像素：整数速度时"接近水平"的方向只能取 0° 或 14°，
     *    远距离必然从头顶或脚底飞过去 —— 自机狙就是被这个坑掉的。  */
    int vx8 = (g_sin_tab[(a + ANGLE_STEPS / 4) & 63] * speed * 8) >> 6;
    int vy8 = (g_sin_tab[a] * speed * 8) >> 6;

    game_bullet_spawn8(mx - BW / 2, my - BH / 2, vx8, vy8,
                       motion, s->type, s->enemy);
}

/*----------------------------------------------------------------------------*/
/*  从炮口指向瞄准点的角度（64 格，用最大点积搜，不算 atan）  */
static int angle_to_target(const shot_t *s)
{
    int dx = s->tx - s->mx;
    int dy = s->ty - s->my;
    int best = 0, best_dot = -0x7fffffff, a;

    for (a = 0; a < ANGLE_STEPS; ++a) {
        int dot = g_sin_tab[(a + ANGLE_STEPS / 4) & 63] * dx
                + g_sin_tab[a] * dy;

        if (dot > best_dot) { best_dot = dot; best = a; }
    }
    return best;
}

/*----------------------------------------------------------------------------*/
static const pattern_t *cur_pattern(const shot_t *s)
{
    const move_t *mv = &s->set->moves[s->move_idx];
    int p = mv->pattern;

    if (p < 0 || p >= s->set->n_patterns) p = 0;
    return &s->set->patterns[p];
}

static int shot_count(const shot_t *s, const pattern_t *p)
{
    if (s->set == g_boss_def.moves)
        return (p->count * game_enemy_bullet_limit() + ENEMY_CAP-1) / ENEMY_CAP;
    return p->count;
}

/*  难度缩放：关卡越高、宿主越急（Boss 残血），间隔和休息越短  */
static int eff_gap(const shot_t *s, const pattern_t *p)
{
    int v = (int)p->gap - (g.stage - 1) - s->frenzy;
    int minimum = s->set == g_boss_def.moves ? 1 : 2;
    return v < minimum ? minimum : v;
}

static int eff_rest(const shot_t *s, const pattern_t *p)
{
#if GAME_BOSS_PACING_TEST
    if (s->set == g_boss_def.moves) return 2;
#endif
    int v = (int)p->rest - (g.stage - 1) * 3 - s->frenzy * 14;
    return v < 16 ? 16 : v;
}

static int inter_volley_gap(const shot_t *s, const pattern_t *p)
{
#if GAME_BOSS_PACING_TEST
    if (s->set == g_boss_def.moves) return 2;
#else
    (void)s;
#endif
    if (p->motion == MOTION_PULSE) return (int)p->gap;
    return s->set == g_boss_def.moves ? 3 : INTER_VOLLEY_GAP;
}

/*----------------------------------------------------------------------------*/
static void start_volley(shot_t *s);

/*  开始一个招式  */
static void start_move(shot_t *s, int idx)
{
    const move_t *mv;
    const pattern_t *p;

    if (idx < 0 || idx >= s->set->n_moves) idx = 0;
    mv = &s->set->moves[idx];
    p  = &s->set->patterns[mv->pattern];

    s->move_idx    = idx;
    s->move_times  = (mv->times > 1) ? mv->times : 1;
    s->rep_left    = p->repeat ? p->repeat : 1;
    s->move_base   = p->base;
    s->tick        = 0u;
    start_volley(s);
}

/*  开一轮：起始角 = 招式起始角（或瞄准角）+ 已发轮数 × spin  */
static void start_volley(shot_t *s)
{
    const pattern_t *p = cur_pattern(s);
    int base = s->move_base;

    if (p->aim) {
        /*  自机狙：整轮以"指向目标的角度"为中心
         *  扇形（count × step）从中心往两边摊开，所以退半步  */
        base = angle_to_target(s) - (shot_count(s,p) - 1) * (int)p->step / 2;
    }

    s->burst_base = (base + (p->repeat - s->rep_left) * p->spin) & (ANGLE_STEPS - 1);
    s->burst_idx  = 0;
    s->burst_left = shot_count(s,p);
    s->burst_wait = 1;
    --s->rep_left;
}

/*  这一轮的下一发  */
static void burst_next(shot_t *s)
{
    const pattern_t *p = cur_pattern(s);
    int a = s->burst_base + s->burst_idx * p->step;

    int budget = s->enemy ? game_enemy_bullet_limit() - game_enemy_bullets_alive()
                          : game_bullet_slots() - game_bullets_alive();
    if (s->enemy && s->set == g_boss_def.moves)
        budget = game_boss_shot_limit() - game_enemy_bullets_alive();
    int free_slots = game_bullet_slots() - game_bullets_alive();
    if (budget > free_slots) budget = free_slots;
    if (budget < 0) budget = 0;

    if (p->motion == MOTION_PULSE) {
        int i, emit = shot_count(s,p), mx = s->mx, my = s->my;
        if (s->set == g_boss_def.moves) {
            mx = g.boss_x + BOSS_W / 2;
            my = g.boss_y + BOSS_H / 2;
        }
        if (emit > budget) emit = budget;
        for (i = 0; i < emit; ++i)
            shot_angle(s, s->burst_base + i * ANGLE_STEPS / emit,
                       p->speed, MOTION_PULSE, mx, my);
#if GAME_BOSS_SKILL_DIAG == 1
        if (s->set == g_boss_def.moves && s->move_idx == 0 && s->rep_left == 0)
            game_dense_diag_start();
#endif
        s->burst_idx = p->count;
        s->burst_left = 0;
    } else {
        int layer, twin, layers = p->layers ? p->layers : 1;
        if (s->set == g_boss_def.moves)
            layers = (layers * game_enemy_bullet_limit() + ENEMY_CAP-1) / ENEMY_CAP;
        for (layer = 0; layer < layers && budget > 0; ++layer) {
            /* 平行通道和 1/4 像素速度阶梯让各层保持分离，
             * 同时保留图案原有的瞄准、扇形、旋转和双向弹臂。 */
            int offset = layers > 1 ? (2 * layer - layers + 1) : 0;
            for (twin = 0; twin <= (p->twin != 0) && budget > 0; ++twin) {
                int angle = (a + twin * (ANGLE_STEPS / 2)) & 63;
                int speed = twin && p->speed2 ? p->speed2 : p->speed;
                int speed8 = speed * 8 + offset;
                int dx = g_sin_tab[(angle + 16) & 63];
                int dy = g_sin_tab[angle];
                game_bullet_spawn8(s->mx - BW / 2 - dy * offset * 3 / 64,
                                   s->my - BH / 2 + dx * offset * 3 / 64,
                                   (dx * speed8) >> 6, (dy * speed8) >> 6,
                                   MOTION_LINE, s->type, s->enemy);
                --budget;
            }
        }
        ++s->burst_idx;
        --s->burst_left;
    }
    s->burst_wait = eff_gap(s, p);
}

/*============================================================================*/
void game_shot_init(shot_t *s, const move_set_t *set, int enemy, int type)
{
    s->active = 0;
    s->enemy = enemy;
    s->type = type;
    s->set = set;
    s->mx = 0; s->my = 0;
    s->tx = 0; s->ty = 0;
    s->frenzy = 0;
    s->move_idx = 0; s->move_times = 0;
    s->burst_left = 0; s->burst_wait = 0; s->burst_idx = 0; s->burst_base = 0;
    s->rep_left = 0; s->move_base = 0;
    s->tick = 0u;
}

void game_shot_stop(shot_t *s)
{
    s->active = 0;
    s->burst_left = 0;
    s->rep_left = 0;
    s->move_times = 0;
    s->tick = 0u;
}

/*  从第 move_idx 条招式开始打  */
void game_shot_begin(shot_t *s, int move_idx)
{
    if (!s->set || s->set->n_moves <= 0) return;
    s->active = 1;
    start_move(s, move_idx);
}

/*----------------------------------------------------------------------------*/
void game_shot_step(shot_t *s)
{
    const pattern_t *p;

    if (!s->active) return;

    if (s->burst_left > 0) {                    /* 这一轮还没发完 */
        if (--s->burst_wait <= 0) burst_next(s);
        return;
    }

    /*  这一轮发完：同一招还有轮（repeat 没走完）⇒ 短间隔后接着转  */
    if (s->rep_left > 0) {
        p = cur_pattern(s);
        if (++s->tick >= (u32)inter_volley_gap(s, p)) {
            s->tick = 0u;
            start_volley(s);
        }
        return;
    }

    /*  招式表说这一招要连播 ⇒ 接着转下一轮  */
    if (--s->move_times > 0) {
        p = cur_pattern(s);
        if (++s->tick >= (u32)inter_volley_gap(s, p)) {
            s->tick = 0u;
            s->move_base = (s->move_base + p->spin * (p->repeat ? p->repeat : 1))
                         & (ANGLE_STEPS - 1);
            s->rep_left = p->repeat ? p->repeat : 1;
            start_volley(s);
        }
        return;
    }

    /*  整招结束：先歇够【并且】等场上的弹清得差不多，再出下一招  */
    p = cur_pattern(s);
    if (s->tick < (u32)eff_rest(s, p)) {
        ++s->tick;
        return;
    }
#if GAME_BOSS_PACING_TEST
    if (s->set != g_boss_def.moves && game_enemy_bullets_alive() > WAVE_CLEAR_MAX) return;
#else
    if (game_enemy_bullets_alive() > WAVE_CLEAR_MAX &&
        !(s->set == g_boss_def.moves && g.boss_phase == BOSS_PHASE_COMBINED)) return;
#endif

    start_move(s, (s->move_idx + 1) % s->set->n_moves);
}
