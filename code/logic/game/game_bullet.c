//==============================================================================
//  game_bullet.c —— 子弹池：生成 / 五种弹道 / 判定 / 绘制
//------------------------------------------------------------------------------
//  干什么      :
//    · game_bullet_spawn()     : 通用生成（位置 + 速度 + 弹道 + 贴图 + 敌我）
//    · game_bullet_fire_enemy(): 朝向版（直线），弹幕雨 / 简单图案用它
//    · game_bullets_step()     : 按弹道推进 → 判命中 → 判越界
//    · game_bullets_submit()   : 把每颗弹登记进渲染队列（z = 底边 y，和角色/Boss 同一把尺子）
//  上一级是谁  : game/game_main.c（每帧）、game_player.c / game_boss.c（生成）
//  接口契约    :
//    · 固定 N_BULLETS 槽的池子，不开动态内存；其中 PLAYER_RESERVE 槽留给玩家
//    · ★ 判定只用【子弹中心点】× 【脚到眼睛的身体矩形】，不用整张贴图包围盒
//    · 素材只有"朝右"一套，四个朝向靠 镜像 / ROT90 / ROT90+MY 拼出来
//  为什么这么做:
//    · 直线弹幕太单调 ⇒ 弹道做成"速度 + 模式"两件套，图案只负责给参数
//    · 判定用整张包围盒会让"擦着边就受伤" ⇒ 用身体矩形 + 子弹中心
//  参考        : docs/spec/硬件_加速器.md §1.6.1（ROT90）/ 软件_驱动与API.md §8.1
//==============================================================================
#include "game.h"
#include "CI_gpu2d.h"

#ifndef GAME_BULLET_FAST_DRAW
#define GAME_BULLET_FAST_DRAW 1
#endif
#if GAME_BULLET_FAST_DRAW && defined(__GNUC__)
#define BULLET_DRAW_OPT __attribute__((optimize("O3")))
#else
#define BULLET_DRAW_OPT
#endif
#ifndef GAME_BULLET_FAST_SIM
#define GAME_BULLET_FAST_SIM 1
#endif
#if GAME_BULLET_FAST_SIM && defined(__GNUC__)
#define BULLET_SIM_OPT __attribute__((optimize("O3")))
#else
#define BULLET_SIM_OPT
#endif

/*  池子管理：分配时不扫描整池。
 *    · s_alive / s_enemy_alive 是权威计数，生成/释放时维护；
 *    · s_free 是空闲槽栈，生成 O(1)，释放时压回去。  */
static int s_alive;
static int s_enemy_alive;
static int s_split_alive;
static unsigned short s_chili_ids[PLAYER_RESERVE];
static int s_chili_n;
#if GAME_BOSS_SKILL_DIAG
static u32 s_enemy_spawn_rejects;
u32 game_enemy_spawn_rejects(void) { return s_enemy_spawn_rejects; }
#endif
static int s_free_n;
static unsigned short s_free_head;
#if GAME_BULLET_DIRTY_BOUNDS
/* Sprite kind (0=A, 1=B, 2=C) and axis (bit 0=90-degree rotation) for the
 * pixels retained in each framebuffer. Stored at draw time so a reused slot
 * cannot make the old footprint depend on its new bullet type or direction. */
static const unsigned char s_dirty_bounds[6][4] = {
    {4, 4, 24, 25}, {3, 4, 26, 24}, /* A: horizontal / vertical */
    {7, 8, 18, 21}, {7, 8, 18, 21}, /* B never rotates */
    {1, 6, 30, 21}, {5, 1, 22, 30}  /* C: horizontal / vertical */
};
#endif
static u32 s_draw_alpha;
static u32 s_draw_tint_enemy;
static u32 s_draw_tint_player;

static int magic_owned(void);

static int uranus_enemy_slow_active(void)
{
    return game_item_has(ITEM_URANUS) && g.boss_slow_until &&
           (int)(game_ms() - g.boss_slow_until) < 0;
}

static int bullet_alloc(void)
{
    int i;
    if (s_free_n <= 0) return -1;
    i = s_free_head;
    s_free_head = g.bullets[i].age;
    --s_free_n;
    return i;
}

static void bullet_untrack_chili(int i)
{
    int k;

    for (k = 0; k < s_chili_n; ++k) {
        if (s_chili_ids[k] == (unsigned short)i) {
            s_chili_ids[k] = s_chili_ids[--s_chili_n];
            break;
        }
    }
}

/* An inactive slot lends its age field to the free list. Active ages remain
 * independent; allocation initializes age before the bullet becomes active. */
static void bullet_free_tail(int first)
{
    int i;
    s_free_n = N_BULLETS - first;
    s_free_head = s_free_n ? (unsigned short)(N_BULLETS - 1) : 65535u;
    for (i = first; i < N_BULLETS; ++i)
        g.bullets[i].age = i == first ? 65535u : (unsigned short)(i - 1);
}

static void bullet_release(int i)
{
    bullet_t *b = &g.bullets[i];

    if (!b->active) return;
    b->active = 0;
    --s_alive;
    if (b->enemy) --s_enemy_alive;
    else {
        if (b->split_color) --s_split_alive;
        if (b->type == BULLET_CHILI) bullet_untrack_chili(i);
    }
    b->age = s_free_head;
    s_free_head = (unsigned short)i;
    ++s_free_n;
}

/* 朝向 → 翻转位（朝右是原图）。辣椒那张美术要求"不转"，单独一套。 */
static const unsigned char dir_flip[4]       = { 0u, GPU2D_FLIP_X, GPU2D_ROT90, GPU2D_ROT90 | GPU2D_FLIP_Y };
static const unsigned char dir_flip_noRot[4] = { 0u, GPU2D_FLIP_X, 0u, 0u };

/*----------------------------------------------------------------------------*/
static int bullet_asset(const bullet_t *b)
{
    /*  ★ 敌方（Boss）没有专属素材：预染的 bullet_type_A_enemy 已从素材目录删除
     *     （2026-10-22，不再进 PRAM），敌方弹一律走 A_base + 硬件 MODE=4 实时 tint
     *     （见 bullet_draw_one）。玩家的 B/C 弹走各自素材；C_Uranus 只给玩家。  */
    if (b->enemy)                 return ASSET_bullet_type_A_base;
    if (b->type == BULLET_CHILI)  return ASSET_bullet_type_B_chili_NOTROT90;
    if (b->type == BULLET_URANUS) return ASSET_bullet_type_C_Uranus;
    return ASSET_bullet_type_A_base;
}

/*  贴图朝向由速度推：竖向为主用 ROT90，横向为主用镜像。  */
static u32 bullet_flip(const bullet_t *b, int asset)
{
    int ax = b->vx < 0 ? -b->vx : b->vx;
    int ay = b->vy < 0 ? -b->vy : b->vy;
    int dir;

    if (ay > ax) dir = (b->vy >= 0) ? DOWN : UP;
    else         dir = (b->vx < 0) ? LEFT : RIGHT;

    return (asset == ASSET_bullet_type_B_chili_NOTROT90) ? dir_flip_noRot[dir]
                                                        : dir_flip[dir];
}

/*----------------------------------------------------------------------------*/
/*  MagicCard：所有玩家弹幕淡紫 + 追踪最近的小怪/Boss；追踪距离由射程存活时间决定。  */
static int magic_owned(void)
{
    return (g.item_mask & (1u << ITEM_MAGICCARD)) != 0u;
}

static int int_sqrt(int v)
{
    int r = 0, bit = 1 << 30;              /* 4^15，覆盖 int 范围 */

    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

static int bullet_find_target(const bullet_t *b, int *tx, int *ty, int *kind, int *idx)
{
    int i, best = 0x7FFFFFFF;
    int bx = b->x + BW / 2;
    int by = b->y + BH / 2;
    int found = 0;

    for (i = 0; i < N_MOBS; ++i) {
        const mob_t *m = &g.mobs[i];
        int dx, dy, d2;

        if (!m->active) continue;
        dx = (m->x + 16) - bx;
        dy = (m->y + 16) - by;
        d2 = dx * dx + dy * dy;
        if (d2 < best) {
            best = d2;
            *tx = m->x + 16;
            *ty = m->y + 16;
            *kind = 1;
            *idx = i;
            found = 1;
        }
    }

    if (g.phase == PHASE_FIGHT && g.boss_hp > 0 && !g.boss_locked) {
        int dx = (g.boss_x + BOSS_W / 2) - bx;
        int dy = (g.boss_y + BOSS_H / 2) - by;
        int d2 = dx * dx + dy * dy;

        if (d2 < best) {
            *tx = g.boss_x + BOSS_W / 2;
            *ty = g.boss_y + BOSS_H / 2;
            *kind = 2;
            *idx = 0;
            found = 1;
        }
    }
    return found;
}

/*  生成时锁定一次目标，飞行中不再换目标；目标死了就直接飞直线。  */
static void bullet_lock_target(bullet_t *b)
{
    int tx, ty, kind = 0, idx = 0;

    if (b->enemy || !magic_owned()) return;
    if (bullet_find_target(b, &tx, &ty, &kind, &idx)) {
        b->home_kind = (signed char)kind;
        b->home_idx = (signed char)idx;
    }
}

static void bullet_magic_homing(bullet_t *b)
{
    int tx, ty, dx, dy, d2, dist, sp8, ax, ay;
    int want_vx, want_vy, old_vx, old_vy, nd;

    if (b->home_kind == 1) {
        const mob_t *m;

        if ((unsigned)b->home_idx >= N_MOBS) return;
        m = &g.mobs[b->home_idx];
        if (!m->active) return;
        tx = m->x + 16;
        ty = m->y + 16;
    } else if (b->home_kind == 2) {
        if (g.phase != PHASE_FIGHT || g.boss_hp <= 0) return;
        tx = g.boss_x + BOSS_W / 2;
        ty = g.boss_y + BOSS_H / 2;
    } else {
        return;
    }

    dx = tx - (b->x + BW / 2);
    dy = ty - (b->y + BH / 2);
    d2 = dx * dx + dy * dy;
    if (d2 <= 36) return;                   /* 已经很近：不拐，免得过冲后掉头 */

    /* 目标在身后就不追：避免弹幕“往反方向拐”。 */
    if (b->vx8 * dx + b->vy8 * dy <= 0) return;

    dist = int_sqrt(d2);
    if (dist < 1) dist = 1;

    ax = b->vx8 < 0 ? -b->vx8 : b->vx8;
    ay = b->vy8 < 0 ? -b->vy8 : b->vy8;
    sp8 = ax > ay ? ax : ay;
    if (sp8 == 0) sp8 = 8 * 4;

    /* 目标方向；每帧只把速度向它插值 1/4，弹道是弧线，不是瞬间拐弯。 */
    want_vx = (dx * sp8) / dist;
    want_vy = (dy * sp8) / dist;
    old_vx = b->vx8;
    old_vy = b->vy8;
    b->vx8 += (want_vx - b->vx8) / 4;
    b->vy8 += (want_vy - b->vy8) / 4;
    if (want_vx != old_vx && b->vx8 == old_vx) b->vx8 += (want_vx > old_vx) ? 1 : -1;
    if (want_vy != old_vy && b->vy8 == old_vy) b->vy8 += (want_vy > old_vy) ? 1 : -1;

    /* 插值后把速度长度拉回原速度，避免越转越慢。 */
    nd = int_sqrt(b->vx8 * b->vx8 + b->vy8 * b->vy8);
    if (nd > 0) {
        b->vx8 = (b->vx8 * sp8) / nd;
        b->vy8 = (b->vy8 * sp8) / nd;
    }

    b->vx = b->vx8 / 8;
    b->vy = b->vy8 / 8;
    b->use8 = 1;
}

/*----------------------------------------------------------------------------*/
/*  一个点是否落在矩形里（子弹判定统一走这里）  */
static int pt_in(int px, int py, int x, int y, int w, int h)
{
    return px >= x && px < x + w && py >= y && py < y + h;
}

/*----------------------------------------------------------------------------*/
int game_bullet_spawn(int x, int y, int vx, int vy, int mode, int type, int enemy)
{
    int i;
    bullet_t *b;

    /* 敌弹占用达到上限后保留 PLAYER_RESERVE 槽给玩家。 */
    if (enemy) {
        if (s_enemy_alive >= ENEMY_CAP) {
#if GAME_BOSS_SKILL_DIAG
            ++s_enemy_spawn_rejects;
#endif
            return -1;
        }
    }

    /* Player reserve is also the upper bound for active flame projectiles;
     * keeping their indices compact avoids scanning all 612 slots per enemy. */
    if (!enemy && type == BULLET_CHILI && s_chili_n >= PLAYER_RESERVE)
        return -1;

    i = bullet_alloc();
    if (i < 0) {
#if GAME_BOSS_SKILL_DIAG
        if (enemy) ++s_enemy_spawn_rejects;
#endif
        return -1;
    }
    b = &g.bullets[i];
    b->x = x;
    b->y = y;
    b->vx = vx;
    b->vy = vy;
    b->mode = mode;
    b->age = 0;
    b->base_x = x;
    b->type = type;
    b->dist = 0;
    /*  寿命：
     *    · 普通自机弹：距离 = range × 32 px；
     *    · 追踪自机弹（MagicCard）：时间 = range × 0.8 s = range × 48 帧；
     *    · 敌方弹仍按固定帧数。  */
    if (enemy) {
        b->range = 0;
        b->life  = BOSS_BULLET_LIFE;
    } else if (type == BULLET_CHILI) {
        b->range = 0;
        b->life  = CHILI_FLAME_LIFE_FRAMES;
    } else if (magic_owned()) {
        b->range = 0;
        b->life  = (unsigned short)(g.stats.range * RANGE_FRAMES_PER_UNIT);
    } else {
        b->range = (unsigned short)(g.stats.range * RANGE_PX_PER_UNIT);
        b->life  = 0;
    }
    b->enemy = enemy;
    if (enemy && uranus_enemy_slow_active()) {
        if (b->vx > 0) b->vx = (b->vx + 1) / 2;
        else if (b->vx < 0) b->vx = -((-b->vx + 1) / 2);
        if (b->vy > 0) b->vy = (b->vy + 1) / 2;
        else if (b->vy < 0) b->vy = -((-b->vy + 1) / 2);
    }
    b->home_kind = 0;
    b->home_idx = 0;
    b->chili_blocks = (!enemy && type == BULLET_CHILI) ? CHILI_FLAME_BLOCKS : 0u;
    b->split_color = 0;
    b->use8 = 0;
    b->vx8 = 0; b->vy8 = 0; b->rx = 0; b->ry = 0;
    b->active = 1;
    bullet_lock_target(b);
    ++s_alive;
    if (enemy) ++s_enemy_alive;
    else if (type == BULLET_CHILI) s_chili_ids[s_chili_n++] = (unsigned short)i;
    return i;
}

/*----------------------------------------------------------------------------*/
/*  亚像素版：先按整数速度出生，再把速度换成 1/8 精度 —— 角度可以细 8 倍  */
int game_bullet_spawn8(int x, int y, int vx8, int vy8, int mode, int type, int enemy)
{
    int i = game_bullet_spawn(x, y, vx8 / 8, vy8 / 8, mode, type, enemy);

    if (i < 0) return -1;
    g.bullets[i].vx8 = vx8;
    g.bullets[i].vy8 = vy8;
    if (enemy && uranus_enemy_slow_active()) {
        g.bullets[i].vx8 /= 2;
        g.bullets[i].vy8 /= 2;
        g.bullets[i].vx = g.bullets[i].vx8 / 8;
        g.bullets[i].vy = g.bullets[i].vy8 / 8;
    }
    g.bullets[i].use8 = 1;
    return i;
}

/*----------------------------------------------------------------------------*/
/*  朝向版：从 (x,y) 朝 dir 直线飞 —— 简单图案和弹幕雨用它  */
void game_bullet_fire_enemy(int x, int y, int dir)
{
    if (dir == RIGHT)      game_bullet_spawn(x, y,  ENEMY_BULLET_SPEED, 0, MOTION_LINE, BULLET_URANUS, 1);
    else if (dir == LEFT)  game_bullet_spawn(x, y, -ENEMY_BULLET_SPEED, 0, MOTION_LINE, BULLET_URANUS, 1);
    else if (dir == DOWN)  game_bullet_spawn(x, y, 0,  ENEMY_BULLET_SPEED, MOTION_LINE, BULLET_URANUS, 1);
    else                   game_bullet_spawn(x, y, 0, -ENEMY_BULLET_SPEED, MOTION_LINE, BULLET_URANUS, 1);
}

/*----------------------------------------------------------------------------*/
/*  压测：撞墙/命中都挪回网格里，维持恒定的在飞数量。  */
static void bullet_recycle(bullet_t *b, int i)
{
    if (!b->enemy && b->split_color) {
        --s_split_alive;
        b->split_color = 0;
    }
    if (!b->enemy && b->type == BULLET_CHILI) bullet_untrack_chili(i);
    b->x = 16 + (i % 24) * 40;
    b->y = 40 + (i / 24) * 80;
    b->vx = (i % 3) - 1;
    b->vy = ((i / 3) % 3) - 1;
    if (b->vx == 0 && b->vy == 0) b->vy = 1;
    b->mode = MOTION_LINE;
    b->age = 0;
    b->base_x = b->x;
    b->dist = 0;
    if (b->enemy) {
        b->range = 0;
        b->life = BOSS_BULLET_LIFE;
    } else if (b->type == BULLET_CHILI) {
        b->range = 0;
        b->life = CHILI_FLAME_LIFE_FRAMES;
    } else if (magic_owned()) {
        b->range = 0;
        b->life = (unsigned short)(g.stats.range * RANGE_FRAMES_PER_UNIT);
    } else {
        b->range = (unsigned short)(g.stats.range * RANGE_PX_PER_UNIT);
        b->life = 0;
    }
    b->home_kind = 0;
    b->home_idx = 0;
    b->chili_blocks = (!b->enemy && b->type == BULLET_CHILI) ? CHILI_FLAME_BLOCKS : 0u;
    b->use8 = 0;
    b->vx8 = 0;
    b->vy8 = 0;
    b->rx = 0;
    b->ry = 0;
}

/* A player projectile crossing a TriPrism is consumed and becomes a four-way
 * colored fan. Split projectiles carry split_color and therefore cannot split
 * again when multiple prisms overlap the same path. */
static int triprism_try_split(int i, u32 now, int cx, int cy)
{
    bullet_t *b = &g.bullets[i];
    static const signed char fan[GAME_TRIPRISM_SPLIT_COUNT] = { -12, -4, 4, 12 };
    int angle, speed8, color;
    int type, vx8, vy8;

    if (!b->active || b->enemy || b->split_color) return 0;
    if (!game_triprism_hit_test(cx, cy, now)) return 0;
    if (s_split_alive + GAME_TRIPRISM_SPLIT_COUNT > GAME_TRIPRISM_SPLIT_LIMIT)
        return 0;
    if (game_perf_pinned()) return 0;

    vx8 = b->use8 ? b->vx8 : b->vx * 8;
    vy8 = b->use8 ? b->vy8 : b->vy * 8;
    if ((vx8 < 0 ? -vx8 : vx8) >= (vy8 < 0 ? -vy8 : vy8))
        angle = vx8 < 0 ? 32 : 0;
    else
        angle = vy8 < 0 ? 48 : 16;
    speed8 = int_sqrt(vx8 * vx8 + vy8 * vy8);
    if (speed8 < 8) speed8 = 8;
    if (speed8 > 96) speed8 = 96;
    type = b->type;

    bullet_release(i);
    for (color = 0; color < GAME_TRIPRISM_SPLIT_COUNT; ++color) {
        int a = (angle + fan[color]) & (ANGLE_STEPS - 1);
        int sx8 = (g_sin_tab[(a + ANGLE_STEPS / 4) & 63] * speed8) >> 6;
        int sy8 = (g_sin_tab[a] * speed8) >> 6;
        int ni = game_bullet_spawn8(cx - BW / 2, cy - BH / 2,
                                    sx8, sy8, MOTION_LINE, type, 0);

        if (ni < 0) continue;
        g.bullets[ni].split_color = (unsigned char)(color + 1);
        ++s_split_alive;
    }
    return 1;
}

/*----------------------------------------------------------------------------*/
/*  按弹道推进  */
static BULLET_SIM_OPT void bullet_move(bullet_t *b)
{
    switch (b->mode) {
    case MOTION_SINE:
        /* 蛇形：竖直下落，横向按 sin 摆动（vx = 振幅，vy = 下落速度） */
        ++b->age;
        b->y += b->vy;
        b->x = b->base_x + ((g_sin_tab[(b->age * 3) & 63] * b->vx) >> 6);
        break;

    case MOTION_ACCEL: {
        /*  加速：沿【有速度的那个轴】越走越快（雨滴感）。
         *  ★ 四个方向都支持：从上/下/左/右边缘往对面砸。  */
        int a = (int)(b->age >> 3);

        if (b->vx > 0)      b->x += b->vx + a;
        else if (b->vx < 0) b->x += b->vx - a;
        else if (b->vy > 0) b->y += b->vy + a;
        else if (b->vy < 0) b->y += b->vy - a;
        ++b->age;
        break;
    }

    case MOTION_SPIRAL:
        /* 螺旋：vx = 半径（像素/帧），vy = 每帧转几格（1 格 = 5.6°） */
        b->age = (b->age + b->vy) & 63;
        b->x += (g_sin_tab[(b->age + 16) & 63] * b->vx) >> 6;
        b->y += (g_sin_tab[b->age] * b->vx) >> 6;
        break;

    case MOTION_PULSE: {
        /* One 64-step cycle: slow from full speed to half, then accelerate. */
        static const unsigned char scale8[8] = { 8, 7, 6, 5, 4, 5, 6, 7 };
        int scale = scale8[(b->age >> 3) & 7u];
        b->rx += (b->vx8 * scale) / 8;
        b->ry += (b->vy8 * scale) / 8;
        b->x += b->rx >> 3;
        b->y += b->ry >> 3;
        b->rx -= (b->rx >> 3) * 8;
        b->ry -= (b->ry >> 3) * 8;
        ++b->age;
        break;
    }

    case MOTION_HOME:
        /* 追踪：每 8 帧重新对准玩家，但【只走上下左右】四个方向 */
        if ((b->age & 7) == 0) {
            int tx = (g.x + PW / 2) - b->x;
            int ty = (g.y + PH / 2) - b->y;
            int ax = tx < 0 ? -tx : tx;
            int ay = ty < 0 ? -ty : ty;

            if (ax >= ay) {
                b->vx = (tx < 0) ? -ENEMY_BULLET_SPEED : ENEMY_BULLET_SPEED;
                b->vy = 0;
            } else {
                b->vy = (ty < 0) ? -ENEMY_BULLET_SPEED : ENEMY_BULLET_SPEED;
                b->vx = 0;
            }
        }
        b->x += b->vx;
        b->y += b->vy;
        ++b->age;
        break;

    default:
        if (b->use8) {
            b->rx += b->vx8;
            b->ry += b->vy8;
            b->x += b->rx >> 3;
            b->y += b->ry >> 3;
            b->rx -= (b->rx >> 3) * 8;
            b->ry -= (b->ry >> 3) * 8;
        } else {
            b->x += b->vx;
            b->y += b->vy;
        }
        break;
    }

    /*  累计飞行距离（1/8 像素，曼哈顿，省一次开方）：射程判定用  */
    b->dist += b->use8 ? ((b->vx8 < 0 ? -b->vx8 : b->vx8) + (b->vy8 < 0 ? -b->vy8 : b->vy8))
                       : 8 * ((b->vx < 0 ? -b->vx : b->vx) + (b->vy < 0 ? -b->vy : b->vy));
}

/* A Chili flame is a player projectile with a small, local shield. It absorbs
 * one enemy projectile at a time and disappears after the third interception.
 * The scan is only entered for enemy bullets while Chili is owned, preserving
 * the normal enemy-cap path when the item is absent. */
static int chili_block_enemy(int enemy_i, int cx, int cy)
{
    int i;

    if (!game_item_has(ITEM_CHILI) || s_chili_n <= 0) return 0;
    for (i = 0; i < s_chili_n; ++i) {
        int flame_i = s_chili_ids[i];
        bullet_t *f = &g.bullets[flame_i];
        int fx, fy;

        if (!f->active || f->chili_blocks == 0u) continue;
        fx = f->x + BW / 2;
        fy = f->y + BH / 2;
        if (cx < fx - BW || cx >= fx + BW || cy < fy - BH || cy >= fy + BH)
            continue;
        bullet_release(enemy_i);
        if (--f->chili_blocks == 0u) bullet_release(flame_i);
        return 1;
    }
    return 0;
}

/*----------------------------------------------------------------------------*/
BULLET_SIM_OPT void game_bullets_step(u32 now)
{
    int i;
    int stone_collision = game_stone_has_blockers();

    for (i = 0; i < N_BULLETS; ++i) {
        bullet_t *b;
        int cx, cy;

        if (!g.bullets[i].active) continue;
        b = &g.bullets[i];

        /* 追踪弹有寿命：玩家一直躲的话它会在天上待到池子满 */
        if (b->mode == MOTION_HOME && b->age > 600) { bullet_release(i); continue; }

        if (!b->enemy && magic_owned()) bullet_magic_homing(b);

        bullet_move(b);

        /*  ★ 寿命（射程 / 存活帧）：
         *    自机弹按【距离】熄火 —— 不用飞到屏幕外才消失，打出去有"射程"感；
         *    敌方弹按【时间】退休 —— 免得追踪/加速弹在场上赖着不走把池子占满。  */
        if ((b->range > 0 && b->dist >= b->range * 8) ||
            (b->life > 0 && --b->life <= 0)) {
            if (game_perf_pinned()) bullet_recycle(b, i);
            else bullet_release(i);
            continue;
        }

        cx = b->x + BW / 2;             /* 判定点 = 子弹中心 */
        cy = b->y + BH / 2;

        if (b->enemy && chili_block_enemy(i, cx, cy)) continue;
        if (!b->enemy && triprism_try_split(i, now, cx, cy)) continue;

        /*  可交互石头：玩家弹 / Boss 弹撞上都直接消失；石头本身不可破坏  */
        if (stone_collision && game_stone_hit_at(cx, cy)) {
            if (game_perf_pinned()) bullet_recycle(b, i);
            else bullet_release(i);
            continue;
        }

        if (b->enemy) {
            /* 打到角色的【脚到眼睛】矩形（头不判，脚参与） */
            if (pt_in(cx, cy, g.x + PB_OFF_X, g.y + PB_OFF_Y, PB_W, PB_H)) {
                /*  扣血/死亡/无敌全交给 game_vital；这里只管"这一发打中了"  */
                if (game_vital_hit(now) != 0) g.hurt_until = now + HURT_MS;
                if (game_perf_pinned()) bullet_recycle(b, i);
                else { bullet_release(i); continue; }
            }
        } else {
            /* 打到 Boss：锁血时判定框换成护盾（子弹打在玻璃上碎掉，不掉血） */
            /* 锁血时判定框换成整块玻璃罩（子弹打在罩子上碎掉、不掉血） */
            int hit = (g.phase != PHASE_FIGHT) ? 0
                    : g.boss_locked
                ? pt_in(cx, cy, g.boss_x, g.boss_y, BOSS_W, BOSS_H)
                : pt_in(cx, cy, g.boss_x + BB_OFF_X, g.boss_y + BB_OFF_Y, BB_W, BB_H);

            if (hit) {
                game_boss_hit(now);
                if (game_perf_pinned()) bullet_recycle(b, i);
                else bullet_release(i);
                continue;
            }

            /*  没打中 Boss：再看当前房间的 slime  */
            if (game_mobs_try_hit_ex(cx, cy, b->type)) {
                if (game_perf_pinned()) bullet_recycle(b, i);
                else bullet_release(i);
                continue;
            }
        }

        if (b->x < 0 || b->x + BW > game_world_w() ||
            b->y < 0 || b->y + BH > game_world_h()) {
            if (game_perf_pinned()) bullet_recycle(b, i);
            else bullet_release(i);
        }
    }
}

/*----------------------------------------------------------------------------*/
/* History belongs to the framebuffer, not to the current bullet occupants. */
void game_bullets_clear_history(int buf)
{
    if ((unsigned)buf >= GAME_FB_COUNT) return;
#if !GAME_BULLET_TILE_HISTORY
    {
        int i;
        for (i = 0; i < N_BULLETS; ++i) g.old_bullets[buf][i].active = 0;
    }
#else
    dirty_clear(g.old_bullet_tiles[buf]);
#endif
}

#if GAME_BULLET_TILE_HISTORY
/* Match game_mark_dirty's clipping and C integer-division semantics, but only
 * write the saved bitmap. Updating the current frame's dirty queue here would
 * corrupt its already-completed restore/HUD decisions. */
static inline __attribute__((always_inline)) void bullet_history_rect(dirty_map_t tiles, int x, int y, int w, int h)
{
    int tx0 = x / TILE, ty0 = y / TILE;
    int tx1 = (x + w - 1) / TILE, ty1 = (y + h - 1) / TILE;
    int tx, ty;

    if (tx0 < 0) tx0 = 0;
    if (ty0 < 0) ty0 = 0;
    if (tx1 >= GRID_W) tx1 = GRID_W - 1;
    if (ty1 >= GRID_H) ty1 = GRID_H - 1;
    for (ty = ty0; ty <= ty1; ++ty)
        for (tx = tx0; tx <= tx1; ++tx)
            dirty_set(tiles, tx, ty);
}
#endif

/* Called after the sprite command is queued. Tile history then overlaps GPU
 * execution and records only sprites actually drawn into this framebuffer. */
BULLET_DRAW_OPT void game_bullets_record_history(int buf, int i)
{
#if GAME_BULLET_TILE_HISTORY
    const bullet_t *b;
    if ((unsigned)buf >= GAME_FB_COUNT || (unsigned)i >= N_BULLETS) return;
    b = &g.bullets[i];
    if (!b->active) return;
#if GAME_BULLET_DIRTY_BOUNDS
    {
        int ai = bullet_asset(b);
        int ax = b->vx < 0 ? -b->vx : b->vx;
        int ay = b->vy < 0 ? -b->vy : b->vy;
        int kind = ai == ASSET_bullet_type_C_Uranus ? 2 :
                   ai == ASSET_bullet_type_B_chili_NOTROT90 ? 1 : 0;
        const unsigned char *r;
        kind = kind * 2 + (ay > ax && kind != 1);
        r = s_dirty_bounds[kind];
        bullet_history_rect(g.old_bullet_tiles[buf], b->x + r[0], b->y + r[1], r[2], r[3]);
    }
#else
    bullet_history_rect(g.old_bullet_tiles[buf], b->x, b->y, BW, BH);
#endif
#else
    (void)buf; (void)i;
#endif
}

/*  记下"这块缓冲里每颗弹这次画在哪"（脏区用）；每帧只调一次  */
void game_bullets_sync(int buf)
{
#if GAME_BULLET_TILE_HISTORY
    game_bullets_clear_history(buf);
#else
    int i;
    for (i = 0; i < N_BULLETS; ++i) {
        g.old_bullets[buf][i].active = g.bullets[i].active;
        g.old_bullets[buf][i].x = g.bullets[i].x;
        g.old_bullets[buf][i].y = g.bullets[i].y;
        if (g.bullets[i].active) {
#if GAME_BULLET_DIRTY_BOUNDS
            const bullet_t *b = &g.bullets[i];
            int ai = bullet_asset(b);
            int ax = b->vx < 0 ? -b->vx : b->vx;
            int ay = b->vy < 0 ? -b->vy : b->vy;
            int kind = ai == ASSET_bullet_type_C_Uranus ? 2 :
                       ai == ASSET_bullet_type_B_chili_NOTROT90 ? 1 : 0;
            kind = kind * 2 + (ay > ax && kind != 1);
            g.old_bullets[buf][i].kind = (unsigned)kind;
#endif
        }
    }
#endif
}

void game_bullets_mark_old(dirty_map_t dirty, int buf)
{
    int i;
#if !GAME_BULLET_TILE_HISTORY
    for (i = 0; i < N_BULLETS; ++i) {
        const old_t *b = &g.old_bullets[buf][i];
        if (!b->active) continue;
#if GAME_BULLET_DIRTY_BOUNDS
        {
            const unsigned char *r = s_dirty_bounds[b->kind];
            game_mark_dirty(dirty, b->x + r[0], b->y + r[1], r[2], r[3]);
        }
#else
        game_mark_dirty(dirty, b->x, b->y, BW, BH);
#endif
    }
#else
#if GAME_BULLET_MERGE_TILES
    game_dirty_merge_tiles(dirty, g.old_bullet_tiles[buf]);
#else
    for (i = 0; i < DIRTY_BYTES; ++i) {
        unsigned bits = g.old_bullet_tiles[buf][i];
        int bit;
        for (bit = 0; bits; ++bit, bits >>= 1) {
            if (bits & 1u) {
                int cell = i * 8 + bit;
                game_mark_dirty(dirty, (cell % GRID_W) * TILE,
                                (cell / GRID_W) * TILE, TILE, TILE);
            }
        }
    }
#endif
#endif
}

/*----------------------------------------------------------------------------*/
/*  一颗弹的画法：由渲染队列排好序之后按槽号调用  */
static BULLET_DRAW_OPT void bullet_draw_one(u32 fb, u32 now, int buf, int i)
{
    const bullet_t *b = &g.bullets[i];
    int ai = bullet_asset(b);
    u32 flip = bullet_flip(b, ai);
    u32 src = g_assets[ai].base_word;
    u32 dx = (u32)b->x, dy = (u32)b->y, w = BW, h = BH;
#if GAME_BULLET_ALPHA_CROP
    {
        u32 sx = 4u, sy = 4u;
        w = 24u; h = 25u;
        if (ai == ASSET_bullet_type_C_Uranus) { sx = 0u; sy = 6u; w = 32u; h = 21u; }
        else if (ai == ASSET_bullet_type_B_chili_NOTROT90) { sy = 8u; h = 21u; }
        src += sy * g_assets[ai].stride_word + sx / 4u;
        /* Hardware ROT90 transposes source axes. MX/MY flip output x/y;
         * compensate crop origins in the same coordinates as CmdExec. */
        if (flip & GPU2D_ROT90) {
            dx += (flip & GPU2D_FLIP_X) ? BH - sy - h : sy;
            dy += (flip & GPU2D_FLIP_Y) ? BW - sx - w : sx;
        } else {
            dx += (flip & GPU2D_FLIP_X) ? BW - sx - w : sx;
            dy += (flip & GPU2D_FLIP_Y) ? BH - sy - h : sy;
        }
    }
#endif

    (void)now; (void)buf;

    /*  ★ Boss 弹幕走硬件 MODE=4：A_base + spriteAlphaTintRect 实时红 tint；
     *     base 的 α=128 软边保留。玩家 B/C 弹走普通 alpha。
     *     ★ bullet_type_A_enemy 已删除，敌方弹的素材就是 bullet_asset() 返回的
     *       A_base（ai），这里不再另外写死一个 ASSET_ 名字。  */
    if (b->enemy) {
#if GAME_BOSS_BULLET_SWEEP && GAME_BOSS_SWEEP_OPAQUE == 1
        CI_gpu2d_spriteFlipCK(g_assets[ai].base_word,
                              g_assets[ai].stride_word,
                              fb, (u32)b->x, (u32)b->y, BW, BH,
                              flip);
#elif (GAME_BOSS_BULLET_SWEEP && GAME_BOSS_SWEEP_OPAQUE == 2) || GAME_BULLET_FAST_CKEY
        CI_gpu2d_spriteTintCKRect(g_assets[ai].base_word,
                                  g_assets[ai].stride_word,
                                  fb, (u32)b->x, (u32)b->y, BW, BH,
                                  0x00FF2020u, 170u, 1u,
                                  flip);
#elif GAME_BULLET_TINT
        ++s_draw_tint_enemy;
        CI_gpu2d_spriteAlphaTintRect(
            src,
            g_assets[ai].stride_word,
            fb, dx, dy, w, h,
            0x00FF2020u,   /* 柔和红：= 旧预染弹 bullet_type_A_enemy 的配色 */
            170u,          /* 浓度 170/255：保留 base 的明暗层次，不会糊成一片纯红 */
            1u,            /* 插值染色 */
            flip);
    #else
        ++s_draw_alpha;
        CI_gpu2d_spriteAlphaRect(src, g_assets[ai].stride_word,
            fb, dx, dy, w, h, flip);
#endif
    } else if (b->split_color) {
        static const u32 split_rgb[GAME_TRIPRISM_SPLIT_COUNT] = {
            0x00FF4848u, 0x00FFD040u, 0x0048E868u, 0x004888FFu
        };
        unsigned color = b->split_color - 1u;

        if (color >= GAME_TRIPRISM_SPLIT_COUNT) color = 0u;
        ++s_draw_tint_player;
        CI_gpu2d_spriteAlphaTintRect(
            src, g_assets[ai].stride_word,
            fb, dx, dy, w, h,
            split_rgb[color],
            190u,
            1u,
            flip);
    } else if (magic_owned()) {
        ++s_draw_tint_player;
        /*  MagicCard：所有玩家弹幕统一淡紫色。  */
        CI_gpu2d_spriteAlphaTintRect(
            src, g_assets[ai].stride_word,
            fb, dx, dy, w, h,
            0x00A050D0u,   /* 更深的紫 */
            160u,
            1u,
            flip);
    } else {
        ++s_draw_alpha;
        CI_gpu2d_spriteAlphaRect(src, g_assets[ai].stride_word,
            fb, dx, dy, w, h, flip);
    }
#if GAME_BULLET_TILE_HISTORY
    game_bullets_record_history(buf, i);
#endif
}

/*----------------------------------------------------------------------------*/
/*  把每颗在场、且整块在屏内的弹登记进渲染队列。
 *  z = Z_ACTOR + 底边 y ⇒ 弹和角色 / Boss / 道具用同一条深度尺子互相遮挡：
 *  弹飞到人物脚下方就盖住人物，飞到人物头上就被人物盖住（俯视视角的透视）。  */
void game_bullets_submit(void)
{
    int i;
    int draw_id = -1;

    for (i = 0; i < N_BULLETS; ++i) {
        const bullet_t *b = &g.bullets[i];

        if (!b->active) continue;
        if (!game_visible_world(b->x, b->y, BW, BH)) continue;   /* 屏外不占队列 */

        if (draw_id < 0) draw_id = rl_register_draw(bullet_draw_one);
        rl_push_id(ACTOR_Z(b->y + BH), draw_id, i);
    }
}

/*----------------------------------------------------------------------------*/
int game_bullets_alive(void)
{
    return s_alive;
}

void game_bullet_draw_stats(u32 *alpha, u32 *tint_enemy, u32 *tint_player)
{
    if (alpha) *alpha = s_draw_alpha;
    if (tint_enemy) *tint_enemy = s_draw_tint_enemy;
    if (tint_player) *tint_player = s_draw_tint_player;
}

/*----------------------------------------------------------------------------*/
/*  只数敌方弹 —— "上一招清完了没有"要用它（玩家自己的弹一直在飞）  */
int game_enemy_bullets_alive(void)
{
    return s_enemy_alive;
}

#if GAME_BOSS_SKILL_DIAG
int game_enemy_bullets_visible(void)
{
    int i, n = 0;
    for (i = 0; i < N_BULLETS; ++i) {
        const bullet_t *b = &g.bullets[i];
        if (!b->active || !b->enemy) continue;
        if (b->x < g.cam_x || b->x + BW > g.cam_x + SCR_W ||
            b->y < g.cam_y || b->y + BH > g.cam_y + SCR_H) continue;
        ++n;
    }
    return n;
}
#endif

/*----------------------------------------------------------------------------*/
void game_enemy_bullets_clear(void)
{
    int i;
    for (i = 0; i < N_BULLETS; ++i)
        if (g.bullets[i].active && g.bullets[i].enemy) bullet_release(i);
}

void game_bullets_clear(void)
{
    int i;

    /* GAME OVER 保留倒计时，但旧弹幕不再移动、碰撞或重绘。 */
    for (i = 0; i < N_BULLETS; ++i) g.bullets[i].active = 0;
    s_alive = 0;
    s_enemy_alive = 0;
    s_split_alive = 0;
    s_chili_n = 0;
    bullet_free_tail(0);
}

/*----------------------------------------------------------------------------*/
void game_bullets_perf_fill(int count)
{
    int i, n = count;

    if (n < 0) n = 0;
    if (n > N_BULLETS) n = N_BULLETS;

    game_bullets_clear();
    for (i = 0; i < n; ++i) {
        bullet_t *b = &g.bullets[i];

        b->x = 16 + (i % 24) * 40;
        b->y = 40 + (i / 24) * 80;
        b->vx = 0;
        b->vy = 0;
        b->mode = MOTION_LINE;
        b->age = 0;
        b->base_x = b->x;
        b->type = BULLET_URANUS;
        b->enemy = (i & 1) ? 1 : 0;
        b->chili_blocks = 0u;
        b->split_color = 0;
        b->home_kind = 0;
        b->home_idx = 0;
        b->dist = 0;
        b->range = b->enemy ? 0 : (unsigned short)(g.stats.range * RANGE_PX_PER_UNIT);
        b->life = b->enemy ? BOSS_BULLET_LIFE : 0;
        b->use8 = 0;
        b->vx8 = 0;
        b->vy8 = 0;
        b->rx = 0;
        b->ry = 0;
        if (i & 1) b->vy = ENEMY_BULLET_SPEED;
        else       b->vx = -PLAYER_BULLET_SPEED;
        b->active = 1;
        ++s_alive;
        if (b->enemy) ++s_enemy_alive;
    }

    /* active 是 0..n-1，空闲槽从 n 开始 */
    bullet_free_tail(n);
}

#if GAME_BOSS_BULLET_SWEEP
/* Sweep all physical slots, including the 32 normally reserved for player
 * shots. Keep every bullet visible and moving for a constant render load. */
void game_bullets_stress_fill(int count)
{
    int i;
#if GAME_CAPACITY_AUDIT
    int cols;
#endif
    if (count < 0) count = 0;
    if (count > N_BULLETS) count = N_BULLETS;
    game_bullets_clear();
#if GAME_CAPACITY_AUDIT
    cols = (count + 19) / 20;
#endif
    for (i = 0; i < count; ++i) {
        bullet_t *b = &g.bullets[i];
#if GAME_CAPACITY_AUDIT
        b->x = g.cam_x + 24 + (cols > 1 ? (i % cols) * (SCR_W - BW - 48) / (cols - 1) : 0);
        b->y = g.cam_y + 56 + (i / cols) * (SCR_H - BH - 80) / 19;
#else
        b->x = g.cam_x + 48 + (i % 16) * 72;
        b->y = g.cam_y + 72 + (i / 16) * 36;
#endif
        b->vx = (i & 1) ? 2 : -2;
        b->vy = (i & 2) ? 1 : -1;
        b->mode = MOTION_LINE;
        b->type = BULLET_A;
        b->enemy = 1;
        b->chili_blocks = 0;
        b->split_color = 0;
        b->use8 = 0;
        b->age = 0;
        b->life = 0;
        b->range = 0;
        b->active = 1;
    }
    s_alive = count;
    s_enemy_alive = count;
    bullet_free_tail(count);
}

void game_bullets_stress_step(void)
{
    int i;
    int x0 = g.cam_x + 24, x1 = g.cam_x + SCR_W - BW - 24;
    int y0 = g.cam_y + 56, y1 = g.cam_y + SCR_H - BH - 24;
    for (i = 0; i < N_BULLETS; ++i) {
        bullet_t *b = &g.bullets[i];
        if (!b->active) continue;
        b->x += b->vx;
        b->y += b->vy;
        if (b->x < x0) { b->x = x0; if (b->vx < 0) b->vx = -b->vx; }
        if (b->x > x1) { b->x = x1; if (b->vx > 0) b->vx = -b->vx; }
        if (b->y < y0) { b->y = y0; if (b->vy < 0) b->vy = -b->vy; }
        if (b->y > y1) { b->y = y1; if (b->vy > 0) b->vy = -b->vy; }
    }
}

int game_bullets_stress_visible(void)
{
    int i, visible = 0;
    for (i = 0; i < N_BULLETS; ++i) {
        const bullet_t *b = &g.bullets[i];
        if (!b->active) continue;
        /* Require the whole sprite inside both the camera and world, not
         * merely an intersection as game_visible_world() does. */
        if (b->x < g.cam_x || b->x + BW > g.cam_x + SCR_W ||
            b->y < g.cam_y || b->y + BH > g.cam_y + SCR_H) continue;
        if (b->x < 0 || b->y < 0 ||
            b->x + BW > game_world_w() || b->y + BH > game_world_h()) continue;
        ++visible;
    }
    return visible;
}
#endif
