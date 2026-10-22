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

/*----------------------------------------------------------------------------*/
/*  朝 angle 方向发一发（angle 是 1/64 圈的格号）  */
static void shot_angle(const shot_t *s, const pattern_t *p, int angle, int speed)
{
    int a = angle & (ANGLE_STEPS - 1);
    /*  ★ 速度放大到 1/8 像素：整数速度时"接近水平"的方向只能取 0° 或 14°，
     *    远距离必然从头顶或脚底飞过去 —— 自机狙就是被这个坑掉的。  */
    int vx8 = (g_sin_tab[(a + ANGLE_STEPS / 4) & 63] * speed * 8) >> 6;
    int vy8 = (g_sin_tab[a] * speed * 8) >> 6;

    game_bullet_spawn8(s->mx - BW / 2, s->my - BH / 2, vx8, vy8,
                       MOTION_LINE, s->type, s->enemy);
    (void)p;
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

/*  难度缩放：关卡越高、宿主越急（Boss 残血），间隔和休息越短  */
static int eff_gap(const shot_t *s, const pattern_t *p)
{
    int v = (int)p->gap - (g.stage - 1) - s->frenzy;
    return v < 2 ? 2 : v;
}

static int eff_rest(const shot_t *s, const pattern_t *p)
{
    int v = (int)p->rest - (g.stage - 1) * 3 - s->frenzy * 14;
    return v < 16 ? 16 : v;
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
        base = angle_to_target(s) - ((int)p->count - 1) * (int)p->step / 2;
    }

    s->burst_base = (base + (p->repeat - s->rep_left) * p->spin) & (ANGLE_STEPS - 1);
    s->burst_idx  = 0;
    s->burst_left = p->count;
    s->burst_wait = 1;
    --s->rep_left;
}

/*  这一轮的下一发  */
static void burst_next(shot_t *s)
{
    const pattern_t *p = cur_pattern(s);
    int a = s->burst_base + s->burst_idx * p->step;

    shot_angle(s, p, a, p->speed);
    if (p->twin)                                    /* 前后对射 */
        shot_angle(s, p, a + ANGLE_STEPS / 2, p->speed2 ? p->speed2 : p->speed);

    ++s->burst_idx;
    --s->burst_left;
    s->burst_wait = eff_gap(s, p);
}

/*============================================================================*/
void game_shot_init(shot_t *s, const move_set_t *set, int enemy, int type)
{
    int i;

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
    (void)i;
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
        if (++s->tick >= INTER_VOLLEY_GAP) {
            s->tick = 0u;
            start_volley(s);
        }
        return;
    }

    /*  招式表说这一招要连播 ⇒ 接着转下一轮  */
    if (--s->move_times > 0) {
        p = cur_pattern(s);
        if (++s->tick >= INTER_VOLLEY_GAP) {
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
    if (game_enemy_bullets_alive() > WAVE_CLEAR_MAX) return;

    start_move(s, (s->move_idx + 1) % s->set->n_moves);
}
