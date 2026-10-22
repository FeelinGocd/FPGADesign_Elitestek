//==============================================================================
//  game_coin.c —— 房间清理奖励金币（按房间持久）
//------------------------------------------------------------------------------
//  干什么      :
//    · 房间清完由 game_mobs.c 调 game_coins_spawn(n)（Boss 现在掉饰品，不掉金币）。
//    · 每条金币记录带 room：离开房间不消失，回来还在原来的位置；
//      只有【当前房间】的金币可捡、可画、可标脏。
//    · 生成时避开石头：石头挡路，金币落在石头里就永远捡不到。
//  上一级是谁  : game/game_main.c（每帧 step / sync / submit）、game_mob.c（生成）
//  为什么单独一个文件:
//    · 金币是"掉落物"，和 slime / 子弹 / 道具的生命周期都不同，分开更好改。
//==============================================================================
#include "game.h"
#include "CI_gpu2d.h"

#define COIN_W 24
#define COIN_H 24
#define COIN_SPREAD 70          /* 中心 ±70 px 内散开 */
#define COIN_PLACE_TRIES 24     /* 落点避开石头的重试次数 */

static u32 s_seed;

static u32 coin_rand(void)
{
    if (s_seed == 0u) s_seed = game_ms() | 1u;
    s_seed = s_seed * 1664525u + 1013904223u;
    return s_seed >> 16;
}

static int coin_hits_stone(int x, int y)
{
    return game_stone_overlap(x, y, COIN_W, COIN_H);
}

/*  整局复位（新一局 / 新一轮）：active、房间号、old 全清，随后地板会整屏重铺。  */
void game_coins_reset(void)
{
    int i, b;

    for (i = 0; i < N_COINS; ++i) {
        g.coin_items[i].active = 0;
        g.coin_items[i].room   = -1;
    }
    for (b = 0; b < 2; ++b)
        for (i = 0; i < N_COINS; ++i) g.old_coin[b][i].active = 0;
}

/*  每帧把 old_coin[buf] 对齐到当前金币 —— 和 game_bullets_sync() 同一个节奏：
 *  帧首脏区收集用【上一帧】的 old_coin 擦旧位置，擦完这里再更新成这一帧的位置。
 *  ★ 只认【当前房间】：换房时上一间房的金币 active 还在，但 old_coin 要归 0，
 *    两块缓冲各擦一次后就不会再画到新房间里。
 *  ★ 没有它：金币被捡走后 old_coin 永远停在 1，那一小块会被每帧标脏。  */
void game_coins_sync(int buf)
{
    int i;
    signed char room = (signed char)game_room_id();

    for (i = 0; i < N_COINS; ++i) {
        const coin_t *c = &g.coin_items[i];

        g.old_coin[buf][i].active = c->active && (c->room == room);
        g.old_coin[buf][i].x = c->x;
        g.old_coin[buf][i].y = c->y;
    }
}

void game_coins_spawn(int count)
{
    int i, n = 0;
    int cx = (ARENA_X0 + ARENA_X1) / 2 - COIN_W / 2;
    int cy = (ARENA_Y0 + ARENA_Y1) / 2 - COIN_H / 2;
    signed char room = (signed char)game_room_id();

    if (count <= 0) return;

    for (i = 0; i < N_COINS && n < count; ++i) {
        coin_t *c = &g.coin_items[i];
        int x, y, tries;

        if (c->active) continue;

        /*  先随机散开；落在石头里就重掷。中心 3×3 是石头禁区，兜底放中心。  */
        for (tries = 0; tries < COIN_PLACE_TRIES; ++tries) {
            x = cx + (int)(coin_rand() % (2 * COIN_SPREAD + 1)) - COIN_SPREAD;
            y = cy + (int)(coin_rand() % (2 * COIN_SPREAD + 1)) - COIN_SPREAD;
            if (x < ARENA_X0) x = ARENA_X0;
            if (x > ARENA_X1 - COIN_W) x = ARENA_X1 - COIN_W;
            if (y < ARENA_Y0) y = ARENA_Y0;
            if (y > ARENA_Y1 - COIN_H) y = ARENA_Y1 - COIN_H;
            if (!coin_hits_stone(x, y)) break;
        }
        if (tries >= COIN_PLACE_TRIES) {
            x = cx;
            y = cy;
        }

        c->active = 1;
        c->room   = room;
        c->x      = (short)x;
        c->y      = (short)y;
        ++n;
    }
}

/*  矩形是否压住【当前房间】还没捡的金币 —— 石头生成时避让用。
 *  和 game_stone_overlap() 是一对：金币避石头、石头也避金币，
 *  这样重进房间时石头重新随机，也不会把旧金币埋掉。  */
int game_coin_overlap(int x, int y, int w, int h)
{
    int i;
    signed char room = (signed char)game_room_id();

    for (i = 0; i < N_COINS; ++i) {
        const coin_t *c = &g.coin_items[i];

        if (!c->active || c->room != room) continue;
        if (x < c->x + COIN_W && x + w > c->x &&
            y < c->y + COIN_H && y + h > c->y) return 1;
    }
    return 0;
}

void game_coins_step(u32 now)
{
    int i;
    int px = g.x + PB_OFF_X + PB_W / 2;
    int py = g.y + PB_OFF_Y + PB_H / 2;
    signed char room = (signed char)game_room_id();

    (void)now;

    for (i = 0; i < N_COINS; ++i) {
        coin_t *c = &g.coin_items[i];

        if (!c->active) continue;
        if (c->room != room) continue;              /* 别的房间的金币不在这捡 */
        if (px >= c->x && px < c->x + COIN_W &&
            py >= c->y && py < c->y + COIN_H) {
            c->active = 0;
            c->room   = -1;
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
    (void)buf;
    if (!c->active || c->room != (signed char)game_room_id()) return;

    CI_gpu2d_spriteAlphaRect(a->base_word, a->stride_word, fb,
                             (u32)c->x, (u32)c->y, COIN_W, COIN_H, 0u);
}

void game_coins_submit(void)
{
    int i;
    signed char room = (signed char)game_room_id();

    for (i = 0; i < N_COINS; ++i) {
        const coin_t *c = &g.coin_items[i];

        if (!c->active || c->room != room) continue;
        if (!game_visible(c->x, c->y, COIN_W, COIN_H)) continue;
        rl_push(ACTOR_Z(c->y + COIN_H), coin_draw_one, i);
    }
}
