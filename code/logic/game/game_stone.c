//==============================================================================
//  game_stone.c —— 房间里的可交互石头（阻挡人物 / 弹幕，不可破坏）
//------------------------------------------------------------------------------
//  规则：
//    · 每间房【第一次进】随机刷 3~5 块；之后按 room 记住位置，重进不重随机；
//    · 落点避开当前房间还没捡的金币 / 网；
//    · 一定保证【门与门之间可以走通】：刷完后在竞技场 32 px 网格上 BFS，
//      所有开着的门对应的内侧格必须互相可达，否则撤掉这块石头；
//    · 中间 3×3 格子留给玩家/道具/Boss，不落石；
//    · 石头挡住玩家肚子；玩家/boss 的弹幕撞上就消失；石头本身无 HP、不可破坏。
//  画法：Z = ACTOR_Z(底边 y)，和人物同一把深度尺子，站在石头后面会被挡。
//==============================================================================
#include "game.h"
#include "CI_gpu2d.h"

#define STONE_SIZE 32
/*  ★ 竞技场尺寸随房间变（普通 23×13 / Boss 46×24）⇒ 网格按最大世界建表，
 *    循环仍用当前 SG_AW/SG_AH。  */
#define SG_MAX_AW 46                                /* Boss 房竞技场最大 46×24 格 */
#define SG_MAX_AH 24
#define SG_AW ((ARENA_X1 - ARENA_X0) / STONE_SIZE)
#define SG_AH ((ARENA_Y1 - ARENA_Y0) / STONE_SIZE)
#define SG_CX (SG_AW / 2)                            /* 门中心 / 房间中心格 */
#define SG_CY (SG_AH / 2)

static u32 s_seed;
static unsigned char s_stone_done[ROOM_COUNT];   /* 每间房只随机生成一次 */

/*  当前房间的石头占用表：32 px 格对齐，碰撞只查 1 个格 / 身体覆盖的几格。
 *  s_grid_room 与当前房间不一致时重建；换房/重刷后置 -1。  */
/*  网格只存 0/1 ⇒ 按位压，46×24 = 1104 格只用 138 B/张。  */
#define SG_CELLS (SG_MAX_AW * SG_MAX_AH)
#define SG_BITS  ((SG_CELLS + 7) / 8)
#define SG_IDX(i, j) ((j) * SG_MAX_AW + (i))
#define SG_GET(a, i) (((a)[(i) >> 3] >> ((i) & 7)) & 1u)
#define SG_SET(a, i) ((a)[(i) >> 3] |= (unsigned char)(1u << ((i) & 7)))

static unsigned char s_stone_grid[SG_BITS];
static signed char   s_grid_room = -1;

/*  BFS 工作区：放静态区，不占 4 KB 栈（换了 Boss 大房间后网格变大，
 *  原来那套"局部数组"会把默认 4 KB 栈顶爆）。  */
static unsigned char s_bfs_block[SG_BITS];
static unsigned char s_bfs_seen[SG_BITS];
static short         s_bfs_q[SG_MAX_AW * SG_MAX_AH];

static u32 stone_rand(void)
{
    if (s_seed == 0u) s_seed = game_ms() | 1u;
    s_seed = s_seed * 1664525u + 1013904223u;
    return s_seed >> 16;
}

static int cell_blocked(int i, int j)
{
    int k;
    signed char room = (signed char)game_room_id();

    /* 四个门的内侧格 */
    if ((i == SG_CX && j == 0) || (i == SG_CX && j == SG_AH - 1) ||
        (i == 0 && j == SG_CY) || (i == SG_AW - 1 && j == SG_CY))
        return 1;

    /* 中间 3×3：玩家出生点 / 道具 / Boss */
    if (i >= SG_CX - 1 && i <= SG_CX + 1 && j >= SG_CY - 1 && j <= SG_CY + 1)
        return 1;

    for (k = 0; k < N_STONES; ++k) {
        const stone_t *s = &g.stones[k];

        if (!s->active || s->room != room) continue;    /* 别的房间的石头不算 */
        if ((s->x - ARENA_X0) / STONE_SIZE == i &&
            (s->y - ARENA_Y0) / STONE_SIZE == j) return 1;
    }
    return 0;
}

/*  32 px 网格 BFS：所有开着的门内侧格必须互相连通  */
static int stone_bfs_ok(void)
{
    short *q = s_bfs_q;
    int starts[4][2];
    int ns = 0, head = 0, tail = 0;
    int k, i, j, d, b;
    static const int dx[4] = { 0, 1, 0, -1 };
    static const int dy[4] = { -1, 0, 1, 0 };

    for (b = 0; b < SG_BITS; ++b) {
        s_bfs_block[b] = 0;
        s_bfs_seen[b] = 0;
    }
    for (k = 0; k < N_STONES; ++k) {
        const stone_t *s = &g.stones[k];

        if (!s->active || s->room != (signed char)game_room_id()) continue;
        i = (s->x - ARENA_X0) / STONE_SIZE;
        j = (s->y - ARENA_Y0) / STONE_SIZE;
        if (i >= 0 && i < SG_AW && j >= 0 && j < SG_AH)
            SG_SET(s_bfs_block, SG_IDX(i, j));
    }

    if (game_room_door_exists(0)) { starts[ns][0] = SG_CX;        starts[ns][1] = 0;        ++ns; }
    if (game_room_door_exists(1)) { starts[ns][0] = SG_AW - 1;    starts[ns][1] = SG_CY;    ++ns; }
    if (game_room_door_exists(2)) { starts[ns][0] = SG_CX;        starts[ns][1] = SG_AH - 1; ++ns; }
    if (game_room_door_exists(3)) { starts[ns][0] = 0;            starts[ns][1] = SG_CY;    ++ns; }

    if (ns <= 1) return 1;                          /* 只有一扇门，不需要门门连通 */

    q[tail++] = (short)(starts[0][1] * SG_AW + starts[0][0]);
    SG_SET(s_bfs_seen, SG_IDX(starts[0][0], starts[0][1]));
    while (head < tail) {
        int c = q[head++];
        int x = c % SG_AW;
        int y = c / SG_AW;

        for (d = 0; d < 4; ++d) {
            int nx = x + dx[d];
            int ny = y + dy[d];

            if (nx < 0 || nx >= SG_AW || ny < 0 || ny >= SG_AH) continue;
            if (SG_GET(s_bfs_block, SG_IDX(nx, ny)) ||
                SG_GET(s_bfs_seen, SG_IDX(nx, ny))) continue;
            SG_SET(s_bfs_seen, SG_IDX(nx, ny));
            q[tail++] = (short)(ny * SG_AW + nx);
        }
    }

    for (k = 1; k < ns; ++k)
        if (!SG_GET(s_bfs_seen, SG_IDX(starts[k][0], starts[k][1]))) return 0;

    /*  单门房间（特殊房）还要保证从门能走到房间中心（玩家/道具/Boss）  */
    if (!SG_GET(s_bfs_seen, SG_IDX(SG_CX, SG_CY))) return 0;
    return 1;
}

/*  重建当前房间的石头占用表（只在换房后第一次碰撞/绘制查询时做一次）  */
static void stone_grid_build(void)
{
    int i, j, k;
    signed char room = (signed char)game_room_id();

    for (i = 0; i < SG_BITS; ++i) s_stone_grid[i] = 0;

    for (k = 0; k < N_STONES; ++k) {
        const stone_t *s = &g.stones[k];
        int gx, gy;

        if (!s->active || s->room != room) continue;
        gx = (s->x - ARENA_X0) / STONE_SIZE;
        gy = (s->y - ARENA_Y0) / STONE_SIZE;
        if (gx >= 0 && gx < SG_AW && gy >= 0 && gy < SG_AH)
            SG_SET(s_stone_grid, SG_IDX(gx, gy));
    }
    s_grid_room = room;
}

void game_stones_reset(void)
{
    int i, b;

    for (i = 0; i < N_STONES; ++i) {
        g.stones[i].active = 0;
        g.stones[i].room   = -1;
    }
    for (b = 0; b < 2; ++b)
        for (i = 0; i < N_STONES; ++i) g.old_stone[b][i].active = 0;
    for (i = 0; i < ROOM_COUNT; ++i) s_stone_done[i] = 0;
    s_grid_room = -1;
}

void game_stones_spawn(int count)
{
    int n = 0, attempt;
    int room = game_room_id();

    if (count <= 0) return;
    if (room < 0 || room >= ROOM_COUNT) return;
    if (s_stone_done[room]) return;             /* 这间房已经生成过，不重随机 */
    s_stone_done[room] = 1;

    for (attempt = 0; attempt < 300 && n < count; ++attempt) {
        int i = (int)(stone_rand() % (u32)SG_AW);
        int j = (int)(stone_rand() % (u32)SG_AH);
        int slot, x, y;
        stone_t *s;

        if (cell_blocked(i, j)) continue;

        x = ARENA_X0 + i * STONE_SIZE;
        y = ARENA_Y0 + j * STONE_SIZE;
        /*  别把当前房间的金币 / 网埋进石头里  */
        if (game_coin_overlap(x, y, STONE_SIZE, STONE_SIZE)) continue;
        if (game_web_overlap(x, y, STONE_SIZE, STONE_SIZE)) continue;

        for (slot = 0; slot < N_STONES; ++slot)
            if (!g.stones[slot].active) break;  /* 别的房间的石头占着槽 */
        if (slot >= N_STONES) break;
        s = &g.stones[slot];

        s->active = 1;
        s->room   = (signed char)room;
        s->x      = (short)x;
        s->y      = (short)y;
        ++n;

        if (!stone_bfs_ok()) {                  /* 这块挡了门/中心 ⇒ 撤掉重来 */
            s->active = 0;
            s->room   = -1;
            --n;
        }
    }
    s_grid_room = -1;                           /* 下一帧碰撞查询时重建 */
}

static void stone_grid_ensure(void)
{
    if (s_grid_room != (signed char)game_room_id()) stone_grid_build();
}

/*  石头按“相对上下”只挡迎面的那条边：
 *    · 石头在上、人物在下 → 用脚底（向上走时脚先碰到石头下沿）；
 *    · 反之石头在下、人物在上 → 用最上一排（眼睛，向下走时先碰到石头上沿）。
 *  这样人物上半身可以叠进石头，不会因为整条身体矩形擦到石头就被卡住。  */
int game_stone_block(int px, int py)
{
    int i;
    signed char room = (signed char)game_room_id();
    int pcy = py + PB_OFF_Y + PB_H / 2;

    for (i = 0; i < N_STONES; ++i) {
        const stone_t *s = &g.stones[i];
        int ex = px + PB_OFF_X;
        int ey, eh;

        if (!s->active || s->room != room) continue;

        if (s->y + STONE_SIZE / 2 < pcy) {
            /* 石头在上：脚底 */
            ey = py + PB_OFF_Y + PB_H - PB_FOOT_H;
            eh = PB_FOOT_H;
        } else {
            /* 石头在下：最上一排（眼睛） */
            ey = py + PB_OFF_Y;
            eh = PB_TOP_H;
        }

        if (ex < s->x + STONE_SIZE && ex + PB_W > s->x &&
            ey < s->y + STONE_SIZE && ey + eh > s->y) return 1;
    }
    return 0;
}

int game_stone_hit_at(int x, int y)
{
    int gx, gy;

    if (x < ARENA_X0 || x >= ARENA_X1 ||
        y < ARENA_Y0 || y >= ARENA_Y1) return 0;

    gx = (x - ARENA_X0) / STONE_SIZE;
    gy = (y - ARENA_Y0) / STONE_SIZE;
    stone_grid_ensure();
    return SG_GET(s_stone_grid, SG_IDX(gx, gy)) != 0u;
}

/*  矩形 (x,y,w,h) 和当前房间的石头相交？—— 金币落点避让用。
 *  石头占满 32 px 格，所以只要矩形覆盖到任一标记格就相交。  */
int game_stone_overlap(int x, int y, int w, int h)
{
    int x0 = x, x1 = x + w - 1;
    int y0 = y, y1 = y + h - 1;
    int i0, i1, j0, j1, i, j;

    if (w <= 0 || h <= 0) return 0;
    if (x1 < ARENA_X0 || x0 >= ARENA_X1 ||
        y1 < ARENA_Y0 || y0 >= ARENA_Y1) return 0;

    if (x0 < ARENA_X0) x0 = ARENA_X0;
    if (y0 < ARENA_Y0) y0 = ARENA_Y0;
    if (x1 >= ARENA_X1) x1 = ARENA_X1 - 1;
    if (y1 >= ARENA_Y1) y1 = ARENA_Y1 - 1;

    i0 = (x0 - ARENA_X0) / STONE_SIZE;
    i1 = (x1 - ARENA_X0) / STONE_SIZE;
    j0 = (y0 - ARENA_Y0) / STONE_SIZE;
    j1 = (y1 - ARENA_Y0) / STONE_SIZE;

    stone_grid_ensure();
    for (j = j0; j <= j1; ++j)
        for (i = i0; i <= i1; ++i)
            if (SG_GET(s_stone_grid, SG_IDX(i, j))) return 1;
    return 0;
}

/*  每帧把 old_stone[buf] 对齐到当前房间的石头（节奏同 game_bullets_sync）  */
void game_stones_sync(int buf)
{
    int i;
    signed char room = (signed char)game_room_id();

    for (i = 0; i < N_STONES; ++i) {
        const stone_t *s = &g.stones[i];

        g.old_stone[buf][i].active = s->active && (s->room == room);
        g.old_stone[buf][i].x = s->x;
        g.old_stone[buf][i].y = s->y;
    }
}

/*----------------------------------------------------------------------------*/
static void stone_draw_one(u32 fb, u32 now, int buf, int i)
{
    stone_t *s = &g.stones[i];
    const asset_desc_t *a = &g_assets[ASSET_background_interact_stone];

    (void)now;
    (void)buf;
    if (!s->active || s->room != (signed char)game_room_id()) return;

    CI_gpu2d_spriteAlphaRect(a->base_word, a->stride_word, fb,
                             (u32)s->x, (u32)s->y, STONE_SIZE, STONE_SIZE, 0u);
}

void game_stones_submit(void)
{
    int i;
    signed char room = (signed char)game_room_id();

    for (i = 0; i < N_STONES; ++i) {
        const stone_t *s = &g.stones[i];

        if (!s->active || s->room != room) continue;
        if (!game_visible_world(s->x, s->y, STONE_SIZE, STONE_SIZE)) continue;
        rl_push(ACTOR_Z(s->y + STONE_SIZE), stone_draw_one, i);
    }
}
