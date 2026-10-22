//==============================================================================
//  game_stone.c —— 房间里的可交互石头（阻挡人物 / 弹幕，不可破坏）
//------------------------------------------------------------------------------
//  规则：
//    · 每个房间随机刷 3~5 块（"中等"数量）；
//    · 一定保证【门与门之间可以走通】：刷完后在竞技场 32 px 网格上 BFS，
//      所有开着的门对应的内侧格必须互相可达，否则撤掉这块石头；
//    · 中间 3×3 格子留给玩家/道具/Boss，不落石；
//    · 石头挡住玩家肚子；玩家/boss 的弹幕撞上就消失；石头本身无 HP、不可破坏。
//  画法：Z = ACTOR_Z(底边 y)，和人物同一把深度尺子，站在石头后面会被挡。
//==============================================================================
#include "game.h"
#include "CI_gpu2d.h"

#define STONE_SIZE 32
#define SG_AW ((ARENA_X1 - ARENA_X0) / STONE_SIZE)   /* 23 */
#define SG_AH ((ARENA_Y1 - ARENA_Y0) / STONE_SIZE)   /* 13 */
#define SG_CX 11                                     /* 门中心 / 房间中心格 */
#define SG_CY 6

static u32 s_seed;

static u32 stone_rand(void)
{
    if (s_seed == 0u) s_seed = game_ms() | 1u;
    s_seed = s_seed * 1664525u + 1013904223u;
    return s_seed >> 16;
}

static int cell_blocked(int i, int j)
{
    int k;

    /* 四个门的内侧格 */
    if ((i == SG_CX && j == 0) || (i == SG_CX && j == SG_AH - 1) ||
        (i == 0 && j == SG_CY) || (i == SG_AW - 1 && j == SG_CY))
        return 1;

    /* 中间 3×3：玩家出生点 / 道具 / Boss */
    if (i >= SG_CX - 1 && i <= SG_CX + 1 && j >= SG_CY - 1 && j <= SG_CY + 1)
        return 1;

    for (k = 0; k < N_STONES; ++k) {
        const stone_t *s = &g.stones[k];

        if (!s->active) continue;
        if ((s->x - ARENA_X0) / STONE_SIZE == i &&
            (s->y - ARENA_Y0) / STONE_SIZE == j) return 1;
    }
    return 0;
}

/*  32 px 网格 BFS：所有开着的门内侧格必须互相连通  */
static int stone_bfs_ok(void)
{
    unsigned char block[SG_AH][SG_AW];
    unsigned char seen[SG_AH][SG_AW];
    short q[SG_AW * SG_AH];
    int starts[4][2];
    int ns = 0, head = 0, tail = 0;
    int k, i, j, d;
    static const int dx[4] = { 0, 1, 0, -1 };
    static const int dy[4] = { -1, 0, 1, 0 };

    for (j = 0; j < SG_AH; ++j) {
        for (i = 0; i < SG_AW; ++i) {
            block[j][i] = 0;
            seen[j][i] = 0;
        }
    }
    for (k = 0; k < N_STONES; ++k) {
        const stone_t *s = &g.stones[k];

        if (!s->active) continue;
        i = (s->x - ARENA_X0) / STONE_SIZE;
        j = (s->y - ARENA_Y0) / STONE_SIZE;
        if (i >= 0 && i < SG_AW && j >= 0 && j < SG_AH) block[j][i] = 1;
    }

    if (game_room_door_exists(0)) { starts[ns][0] = SG_CX;        starts[ns][1] = 0;        ++ns; }
    if (game_room_door_exists(1)) { starts[ns][0] = SG_AW - 1;    starts[ns][1] = SG_CY;    ++ns; }
    if (game_room_door_exists(2)) { starts[ns][0] = SG_CX;        starts[ns][1] = SG_AH - 1; ++ns; }
    if (game_room_door_exists(3)) { starts[ns][0] = 0;            starts[ns][1] = SG_CY;    ++ns; }

    if (ns <= 1) return 1;                          /* 只有一扇门，不需要门门连通 */

    q[tail++] = (short)(starts[0][1] * SG_AW + starts[0][0]);
    seen[starts[0][1]][starts[0][0]] = 1;
    while (head < tail) {
        int c = q[head++];
        int x = c % SG_AW;
        int y = c / SG_AW;

        for (d = 0; d < 4; ++d) {
            int nx = x + dx[d];
            int ny = y + dy[d];

            if (nx < 0 || nx >= SG_AW || ny < 0 || ny >= SG_AH) continue;
            if (block[ny][nx] || seen[ny][nx]) continue;
            seen[ny][nx] = 1;
            q[tail++] = (short)(ny * SG_AW + nx);
        }
    }

    for (k = 1; k < ns; ++k)
        if (!seen[starts[k][1]][starts[k][0]]) return 0;

    /*  单门房间（特殊房）还要保证从门能走到房间中心（玩家/道具/Boss）  */
    if (!seen[SG_CY][SG_CX]) return 0;
    return 1;
}

void game_stones_reset(void)
{
    int i;

    /*  只清 active，保留 old_stone 让换房后的帧首把上一间房的石头擦掉  */
    for (i = 0; i < N_STONES; ++i) g.stones[i].active = 0;
}

void game_stones_spawn(int count)
{
    int n = 0, attempt;

    if (count <= 0) return;

    for (attempt = 0; attempt < 300 && n < count; ++attempt) {
        int i = (int)(stone_rand() % (u32)SG_AW);
        int j = (int)(stone_rand() % (u32)SG_AH);
        stone_t *s;

        if (cell_blocked(i, j)) continue;

        s = &g.stones[n];
        s->active = 1;
        s->x = (short)(ARENA_X0 + i * STONE_SIZE);
        s->y = (short)(ARENA_Y0 + j * STONE_SIZE);
        ++n;

        if (!stone_bfs_ok()) {
            s->active = 0;
            --n;
        }
    }
}

int game_stone_block(int px, int py)
{
    int bx = px + PB_OFF_X;
    int by = py + PB_OFF_Y;
    int i;

    for (i = 0; i < N_STONES; ++i) {
        const stone_t *s = &g.stones[i];

        if (!s->active) continue;
        if (bx < s->x + STONE_SIZE && bx + PB_W > s->x &&
            by < s->y + STONE_SIZE && by + PB_H > s->y) return 1;
    }
    return 0;
}

int game_stone_hit_at(int x, int y)
{
    int i;

    for (i = 0; i < N_STONES; ++i) {
        const stone_t *s = &g.stones[i];

        if (!s->active) continue;
        if (x >= s->x && x < s->x + STONE_SIZE &&
            y >= s->y && y < s->y + STONE_SIZE) return 1;
    }
    return 0;
}

/*----------------------------------------------------------------------------*/
static void stone_draw_one(u32 fb, u32 now, int buf, int i)
{
    stone_t *s = &g.stones[i];
    const asset_desc_t *a = &g_assets[ASSET_background_interact_stone];

    (void)now;
    g.old_stone[buf][i].active = s->active;
    g.old_stone[buf][i].x = s->x;
    g.old_stone[buf][i].y = s->y;
    if (!s->active) return;

    CI_gpu2d_spriteAlphaRect(a->base_word, a->stride_word, fb,
                             (u32)s->x, (u32)s->y, STONE_SIZE, STONE_SIZE, 0u);
}

void game_stones_submit(void)
{
    int i;

    for (i = 0; i < N_STONES; ++i) {
        const stone_t *s = &g.stones[i];

        if (!s->active) continue;
        if (!game_visible(s->x, s->y, STONE_SIZE, STONE_SIZE)) continue;
        rl_push(ACTOR_Z(s->y + STONE_SIZE), stone_draw_one, i);
    }
}
