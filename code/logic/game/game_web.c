//==============================================================================
//  game_web.c —— 房间里的蜘蛛网（站上去减速，按房间持久）
//------------------------------------------------------------------------------
//  干什么      :
//    · 普通房（START / EMPTY）第一次进房随机生成 1~2 张网；特殊房不生成。
//    · 每条网记录带 room：离开房间不消失，回来还在原位；只有当前房间的网
//      参与减速 / 绘制 / 脏区。
//    · 玩家肚子中心落在网矩形内 → 移动速度降到 45%（离开立即恢复）。
//  上一级是谁  : game/game_main.c（每帧 sync / submit）、game/game_state.c（进房生成）
//==============================================================================
#include "game.h"
#include "CI_gpu2d.h"

#define WEB_SIZE 32

static u32 s_seed;
static unsigned char s_web_done[ROOM_COUNT];    /* 每间房只随机生成一次 */

static u32 web_rand(void)
{
    if (s_seed == 0u) s_seed = game_ms() | 1u;
    s_seed = s_seed * 1664525u + 1013904223u;
    return s_seed >> 16;
}

void game_web_reset(void)
{
    int i, b;

    for (i = 0; i < N_WEBS; ++i) {
        g.webs[i].active = 0;
        g.webs[i].room   = -1;
    }
    for (b = 0; b < 2; ++b)
        for (i = 0; i < N_WEBS; ++i) g.old_web[b][i].active = 0;
    for (i = 0; i < ROOM_COUNT; ++i) s_web_done[i] = 0;
}

void game_web_spawn(int count)
{
    int i, n = 0;
    int room = game_room_id();

    if (count <= 0) return;
    if (room < 0 || room >= ROOM_COUNT) return;
    if (s_web_done[room]) return;               /* 这间房已经生成过，不重随机 */
    s_web_done[room] = 1;

    for (i = 0; i < N_WEBS && n < count; ++i) {
        web_t *w = &g.webs[i];

        if (w->active) continue;                /* 别的房间的网占着这个槽 */
        w->active = 1;
        w->room   = (signed char)room;
        w->x = (short)(ARENA_X0 + (int)(web_rand() % (u32)(ARENA_X1 - ARENA_X0 - WEB_SIZE)));
        w->y = (short)(ARENA_Y0 + (int)(web_rand() % (u32)(ARENA_Y1 - ARENA_Y0 - WEB_SIZE)));
        ++n;
    }
}

int game_web_slow_at(int x, int y)
{
    int i;
    signed char room = (signed char)game_room_id();

    for (i = 0; i < N_WEBS; ++i) {
        const web_t *w = &g.webs[i];

        if (!w->active || w->room != room) continue;
        if (x >= w->x && x < w->x + WEB_SIZE &&
            y >= w->y && y < w->y + WEB_SIZE) return 1;
    }
    return 0;
}

/*  矩形是否压住当前房间的网 —— 石头落点避让用（别把网埋进石头里）  */
int game_web_overlap(int x, int y, int w, int h)
{
    int i;
    signed char room = (signed char)game_room_id();

    for (i = 0; i < N_WEBS; ++i) {
        const web_t *ww = &g.webs[i];

        if (!ww->active || ww->room != room) continue;
        if (x < ww->x + WEB_SIZE && x + w > ww->x &&
            y < ww->y + WEB_SIZE && y + h > ww->y) return 1;
    }
    return 0;
}

/*  每帧把 old_web[buf] 对齐到当前房间的网（节奏同 game_bullets_sync）  */
void game_web_sync(int buf)
{
    int i;
    signed char room = (signed char)game_room_id();

    for (i = 0; i < N_WEBS; ++i) {
        const web_t *w = &g.webs[i];

        g.old_web[buf][i].active = w->active && (w->room == room);
        g.old_web[buf][i].x = w->x;
        g.old_web[buf][i].y = w->y;
    }
}

/*----------------------------------------------------------------------------*/
static void web_draw_one(u32 fb, u32 now, int buf, int i)
{
    web_t *w = &g.webs[i];
    const asset_desc_t *a = &g_assets[ASSET_background_interact_web];

    (void)now;
    (void)buf;
    if (!w->active || w->room != (signed char)game_room_id()) return;

    CI_gpu2d_spriteAlphaRect(a->base_word, a->stride_word, fb,
                             (u32)w->x, (u32)w->y, WEB_SIZE, WEB_SIZE, 0u);
}

void game_web_submit(void)
{
    int i;
    signed char room = (signed char)game_room_id();

    for (i = 0; i < N_WEBS; ++i) {
        const web_t *w = &g.webs[i];

        if (!w->active || w->room != room) continue;
        if (!game_visible(w->x, w->y, WEB_SIZE, WEB_SIZE)) continue;
        rl_push(Z_SCENE + 2, web_draw_one, i);
    }
}
