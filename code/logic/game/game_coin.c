//==============================================================================
//  game_coin.c —— 房间清理奖励金币（0~5 个/房）
//------------------------------------------------------------------------------
//  干什么      :
//    · 房间清完由 game_mobs.c / game_boss.c 调 game_coins_spawn(n)。
//    · 金币散在房间中心附近；玩家肚子碰到 +1，COINS 行由 game_hud 画。
//  上一级是谁  : game/game_main.c（每帧 step/submit）、game_mob.c / game_boss.c（生成）
//  为什么单独一个文件:
//    · 金币是"掉落物"，和 slime / 子弹 / 道具的生命周期都不同，分开更好改。
//==============================================================================
#include "game.h"
#include "CI_gpu2d.h"

#define COIN_W 24
#define COIN_H 24
#define COIN_SPREAD 70          /* 中心 ±70 px 内散开 */

static u32 s_seed;

static u32 coin_rand(void)
{
    if (s_seed == 0u) s_seed = game_ms() | 1u;
    s_seed = s_seed * 1664525u + 1013904223u;
    return s_seed >> 16;
}

void game_coins_reset(void)
{
    int i, b;

    for (i = 0; i < N_COINS; ++i) g.coin_items[i].active = 0;
    for (b = 0; b < 2; ++b)
        for (i = 0; i < N_COINS; ++i) g.old_coin[b][i].active = 0;
}

void game_coins_spawn(int count)
{
    int i, n = 0;
    int cx = (ARENA_X0 + ARENA_X1) / 2 - COIN_W / 2;
    int cy = (ARENA_Y0 + ARENA_Y1) / 2 - COIN_H / 2;

    if (count <= 0) return;

    for (i = 0; i < N_COINS && n < count; ++i) {
        coin_t *c = &g.coin_items[i];

        if (c->active) continue;
        c->active = 1;
        c->x = (short)(cx + (int)(coin_rand() % (2 * COIN_SPREAD + 1)) - COIN_SPREAD);
        c->y = (short)(cy + (int)(coin_rand() % (2 * COIN_SPREAD + 1)) - COIN_SPREAD);
        if (c->x < ARENA_X0) c->x = ARENA_X0;
        if (c->x > ARENA_X1 - COIN_W) c->x = ARENA_X1 - COIN_W;
        if (c->y < ARENA_Y0) c->y = ARENA_Y0;
        if (c->y > ARENA_Y1 - COIN_H) c->y = ARENA_Y1 - COIN_H;
        ++n;
    }
}

void game_coins_step(u32 now)
{
    int i;
    int px = g.x + PB_OFF_X + PB_W / 2;
    int py = g.y + PB_OFF_Y + PB_H / 2;

    (void)now;

    for (i = 0; i < N_COINS; ++i) {
        coin_t *c = &g.coin_items[i];

        if (!c->active) continue;
        if (px >= c->x && px < c->x + COIN_W &&
            py >= c->y && py < c->y + COIN_H) {
            c->active = 0;
            ++g.coins;
        }
    }
}

/*----------------------------------------------------------------------------*/
static void coin_draw_one(u32 fb, u32 now, int buf, int i)
{
    coin_t *c = &g.coin_items[i];
    const asset_desc_t *a = &g_assets[ASSET_SpecialEffect_coin];

    (void)now;
    g.old_coin[buf][i].active = c->active;
    g.old_coin[buf][i].x = c->x;
    g.old_coin[buf][i].y = c->y;
    if (!c->active) return;

    CI_gpu2d_spriteAlphaRect(a->base_word, a->stride_word, fb,
                             (u32)c->x, (u32)c->y, COIN_W, COIN_H, 0u);
}

void game_coins_submit(void)
{
    int i;

    for (i = 0; i < N_COINS; ++i) {
        const coin_t *c = &g.coin_items[i];

        if (!c->active) continue;
        if (!game_visible(c->x, c->y, COIN_W, COIN_H)) continue;
        rl_push(ACTOR_Z(c->y + COIN_H), coin_draw_one, i);
    }
}
