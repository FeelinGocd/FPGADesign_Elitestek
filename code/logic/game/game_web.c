//==============================================================================
//  game_web.c —— 普通房间里的蜘蛛网（站上去减速）
//------------------------------------------------------------------------------
//  干什么      :
//    · 普通房（START / EMPTY）随机生成 1~2 张网；特殊房（ITEM / BOSS / SHOP）不生成。
//    · 玩家肚子中心落在网矩形内 → 移动速度降到 45%（离开立即恢复）。
//    · 网画在场景层 Z_SCENE+2，位于地板之上、人物/金币之下。
//  上一级是谁  : game/game_main.c（每帧 submit）、game/game_state.c（进房生成）
//==============================================================================
#include "game.h"
#include "CI_gpu2d.h"

#define WEB_SIZE 32

static u32 s_seed;

static u32 web_rand(void)
{
    if (s_seed == 0u) s_seed = game_ms() | 1u;
    s_seed = s_seed * 1664525u + 1013904223u;
    return s_seed >> 16;
}

void game_web_reset(void)
{
    int i;

    /*  ★ 只清 active，不清 old_web：换房后帧首还要用 old_web 把上一间房的网擦掉。  */
    for (i = 0; i < N_WEBS; ++i) g.webs[i].active = 0;
}

void game_web_spawn(int count)
{
    int i, n = 0;

    if (count <= 0) return;

    for (i = 0; i < N_WEBS && n < count; ++i) {
        web_t *w = &g.webs[i];

        if (w->active) continue;
        w->active = 1;
        w->x = (short)(ARENA_X0 + (int)(web_rand() % (u32)(ARENA_X1 - ARENA_X0 - WEB_SIZE)));
        w->y = (short)(ARENA_Y0 + (int)(web_rand() % (u32)(ARENA_Y1 - ARENA_Y0 - WEB_SIZE)));
        ++n;
    }
}

int game_web_slow_at(int x, int y)
{
    int i;

    for (i = 0; i < N_WEBS; ++i) {
        const web_t *w = &g.webs[i];

        if (!w->active) continue;
        if (x >= w->x && x < w->x + WEB_SIZE &&
            y >= w->y && y < w->y + WEB_SIZE) return 1;
    }
    return 0;
}

/*----------------------------------------------------------------------------*/
static void web_draw_one(u32 fb, u32 now, int buf, int i)
{
    web_t *w = &g.webs[i];
    const asset_desc_t *a = &g_assets[ASSET_background_interact_web];

    (void)now;
    g.old_web[buf][i].active = w->active;
    g.old_web[buf][i].x = w->x;
    g.old_web[buf][i].y = w->y;
    if (!w->active) return;

    CI_gpu2d_spriteAlphaRect(a->base_word, a->stride_word, fb,
                             (u32)w->x, (u32)w->y, WEB_SIZE, WEB_SIZE, 0u);
}

void game_web_submit(void)
{
    int i;

    for (i = 0; i < N_WEBS; ++i) {
        const web_t *w = &g.webs[i];

        if (!w->active) continue;
        if (!game_visible(w->x, w->y, WEB_SIZE, WEB_SIZE)) continue;
        rl_push(Z_SCENE + 2, web_draw_one, i);
    }
}
