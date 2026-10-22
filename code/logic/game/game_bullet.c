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
//    · 固定 256 槽的池子，不开动态内存；其中 32 槽永远留给玩家（PLAYER_RESERVE）
//    · ★ 判定只用【子弹中心点】× 【脚到眼睛的身体矩形】，不用整张贴图包围盒
//    · 素材只有"朝右"一套，四个朝向靠 镜像 / ROT90 / ROT90+MY 拼出来
//  为什么这么做:
//    · 直线弹幕太单调 ⇒ 弹道做成"速度 + 模式"两件套，图案只负责给参数
//    · 判定用整张包围盒会让"擦着边就受伤" ⇒ 用身体矩形 + 子弹中心
//  参考        : docs/spec/硬件_加速器.md §1.6.1（ROT90）/ 软件_驱动与API.md §8.1
//==============================================================================
#include "game.h"
#include "CI_gpu2d.h"

/*  池子管理：不扫 256 槽。
 *    · s_alive / s_enemy_alive 是权威计数，生成/释放时维护；
 *    · s_free 是空闲槽栈，生成 O(1)，释放时压回去。  */
static int s_alive;
static int s_enemy_alive;
static int s_free_n;
static unsigned char s_free[N_BULLETS];

static int bullet_alloc(void)
{
    if (s_free_n <= 0) return -1;
    return (int)s_free[--s_free_n];
}

static void bullet_release(int i)
{
    bullet_t *b = &g.bullets[i];

    if (!b->active) return;
    b->active = 0;
    --s_alive;
    if (b->enemy) --s_enemy_alive;
    if (s_free_n < N_BULLETS) s_free[s_free_n++] = (unsigned char)i;
}

/* 朝向 → 翻转位（朝右是原图）。辣椒那张美术要求"不转"，单独一套。 */
static const u32 dir_flip[4]       = { 0u, GPU2D_FLIP_X, GPU2D_ROT90, GPU2D_ROT90 | GPU2D_FLIP_Y };
static const u32 dir_flip_noRot[4] = { 0u, GPU2D_FLIP_X, 0u, 0u };

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

    /* 敌弹上限：既给玩家留槽（不哑火），也压住每帧命令数（一颗弹≈3 条命令）。
       关卡越往后上限越高，但封顶 160 —— 再多帧率就压不住了。 */
    if (enemy) {
        int cap = ENEMY_CAP + (g.stage - 1) * 10;

        if (cap > 160) cap = 160;
        if (s_alive >= cap) return -1;
    }

    i = bullet_alloc();
    if (i < 0) return -1;
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
    /*  寿命：自机弹按【距离】（射程），敌方弹按【时间】（帧数）。
     *  两种都给上了，谁先到谁先消失。  */
    b->range = enemy ? 0 : PLAYER_BULLET_RANGE;
    b->life  = enemy ? BOSS_BULLET_LIFE : 0;
    b->enemy = enemy;
    b->use8 = 0;
    b->vx8 = 0; b->vy8 = 0; b->rx = 0; b->ry = 0;
    b->active = 1;
    ++s_alive;
    if (enemy) ++s_enemy_alive;
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
    b->x = 16 + (i % 24) * 40;
    b->y = 40 + (i / 24) * 80;
    b->vx = (i % 3) - 1;
    b->vy = ((i / 3) % 3) - 1;
    if (b->vx == 0 && b->vy == 0) b->vy = 1;
    b->mode = MOTION_LINE;
    b->age = 0;
    b->base_x = b->x;
    b->dist = 0;
    b->range = b->enemy ? 0 : PLAYER_BULLET_RANGE;
    b->life = b->enemy ? BOSS_BULLET_LIFE : 0;
    b->use8 = 0;
    b->vx8 = 0;
    b->vy8 = 0;
    b->rx = 0;
    b->ry = 0;
}

/*----------------------------------------------------------------------------*/
/*  按弹道推进  */
static void bullet_move(bullet_t *b)
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

/*----------------------------------------------------------------------------*/
void game_bullets_step(u32 now)
{
    int i;

    for (i = 0; i < N_BULLETS; ++i) {
        bullet_t *b;
        int cx, cy;

        if (!g.bullets[i].active) continue;
        b = &g.bullets[i];

        /* 追踪弹有寿命：玩家一直躲的话它会在天上待到池子满 */
        if (b->mode == MOTION_HOME && b->age > 600) { bullet_release(i); continue; }

        bullet_move(b);

        /*  ★ 寿命（射程 / 存活帧）：
         *    自机弹按【距离】熄火 —— 不用飞到屏幕外才消失，打出去有"射程"感；
         *    敌方弹按【时间】退休 —— 免得追踪/加速弹在场上赖着不走把池子占满。  */
        if (b->range > 0 && b->dist >= b->range * 8) { bullet_release(i); continue; }
        if (b->life > 0 && --b->life <= 0)           { bullet_release(i); continue; }

        cx = b->x + BW / 2;             /* 判定点 = 子弹中心 */
        cy = b->y + BH / 2;

        /*  可交互石头：玩家弹 / Boss 弹撞上都直接消失；石头本身不可破坏  */
        if (game_stone_hit_at(cx, cy)) {
            bullet_release(i);
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
                bullet_release(i);
                continue;
            }

            /*  没打中 Boss：再看当前房间的 slime  */
            if (game_mobs_try_hit(cx, cy)) {
                bullet_release(i);
                continue;
            }
        }

        if (b->x < 0 || b->x + BW > SCR_W || b->y < 0 || b->y + BH > SCR_H) {
            if (game_perf_pinned()) bullet_recycle(b, i);
            else bullet_release(i);
        }
    }
}

/*----------------------------------------------------------------------------*/
/*  记下"这块缓冲里每颗弹这次画在哪"（脏区用）；每帧只调一次  */
void game_bullets_sync(int buf)
{
    int i;

    for (i = 0; i < N_BULLETS; ++i) {
        g.old_bullets[buf][i].active = g.bullets[i].active;
        g.old_bullets[buf][i].x = g.bullets[i].x;
        g.old_bullets[buf][i].y = g.bullets[i].y;
    }
}

/*----------------------------------------------------------------------------*/
/*  一颗弹的画法：由渲染队列排好序之后按槽号调用  */
static void bullet_draw_one(u32 fb, u32 now, int buf, int i)
{
    const bullet_t *b = &g.bullets[i];
    int ai = bullet_asset(b);

    (void)now; (void)buf;

    /*  ★ Boss 弹幕走硬件 MODE=4：A_base + spriteAlphaTintRect 实时红 tint；
     *     base 的 α=128 软边保留。玩家 B/C 弹走普通 alpha。
     *     ★ bullet_type_A_enemy 已删除，敌方弹的素材就是 bullet_asset() 返回的
     *       A_base（ai），这里不再另外写死一个 ASSET_ 名字。  */
    if (b->enemy) {
        CI_gpu2d_spriteAlphaTintRect(
            g_assets[ai].base_word,
            g_assets[ai].stride_word,
            fb, (u32)b->x, (u32)b->y, BW, BH,
            0x00FF2020u,   /* 柔和红：= 旧预染弹 bullet_type_A_enemy 的配色 */
            170u,          /* 浓度 170/255：保留 base 的明暗层次，不会糊成一片纯红 */
            1u,            /* 插值染色 */
            bullet_flip(b, ai));
    } else {
        CI_gpu2d_spriteAlphaRect(g_assets[ai].base_word, g_assets[ai].stride_word,
            fb, (u32)b->x, (u32)b->y, BW, BH, bullet_flip(b, ai));
    }
}

/*----------------------------------------------------------------------------*/
/*  把每颗在场、且整块在屏内的弹登记进渲染队列。
 *  z = Z_ACTOR + 底边 y ⇒ 弹和角色 / Boss / 道具用同一条深度尺子互相遮挡：
 *  弹飞到人物脚下方就盖住人物，飞到人物头上就被人物盖住（俯视视角的透视）。  */
void game_bullets_submit(void)
{
    int i;

    for (i = 0; i < N_BULLETS; ++i) {
        const bullet_t *b = &g.bullets[i];

        if (!b->active) continue;
        if (!game_visible(b->x, b->y, BW, BH)) continue;   /* 屏外不占队列 */

        rl_push(ACTOR_Z(b->y + BH), bullet_draw_one, i);
    }
}

/*----------------------------------------------------------------------------*/
int game_bullets_alive(void)
{
    return s_alive;
}

/*----------------------------------------------------------------------------*/
/*  只数敌方弹 —— "上一招清完了没有"要用它（玩家自己的弹一直在飞）  */
int game_enemy_bullets_alive(void)
{
    return s_enemy_alive;
}

/*----------------------------------------------------------------------------*/
void game_bullets_clear(void)
{
    int i;

    /* GAME OVER 保留倒计时，但旧弹幕不再移动、碰撞或重绘。 */
    for (i = 0; i < N_BULLETS; ++i) g.bullets[i].active = 0;
    s_alive = 0;
    s_enemy_alive = 0;
    s_free_n = 0;
    for (i = 0; i < N_BULLETS; ++i) s_free[s_free_n++] = (unsigned char)i;
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
        b->dist = 0;
        b->range = b->enemy ? 0 : PLAYER_BULLET_RANGE;
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
    s_free_n = 0;
    for (i = n; i < N_BULLETS; ++i) s_free[s_free_n++] = (unsigned char)i;
}
