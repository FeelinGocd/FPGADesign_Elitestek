//==============================================================================
//  game_room.c —— 房间系统：网格生成 / 看邻居类型渲染门 / 走进门触发 / 切换
//------------------------------------------------------------------------------
//  干什么      :
//    · 在 7×7 网格上随机生成一片连通房间（不再硬编码房间表），起点固定在正中
//    · 每间房渲染门时直接看 4 个方向邻居格的类型：
//        - 邻居格没有房间 -> 墙（不画门）
//        - 邻居是普通房   -> normal 门
//        - 邻居是特殊房   -> 邻居的特殊门
//        - 自己是特殊房   -> 本房间所有门都用本房间的特殊门
//    · 特殊房 = 非普通房（ITEM / SHOP / BOSS）；普通房 = START / EMPTY
//    · 生成规则：道具房 1、Boss 房 1；特殊房之间不直连、空间 8 邻域不相邻；
//      普通房可以和特殊房相邻/连通，也可以同时接多间特殊房
//    · 触发：玩家【肚子中心】走进门洞；进新房间落在门内侧 8 px
//  上一级是谁  : game/game_main.c（每帧 game_room_step / 待切换时 commit）
//  坐标        : 房间 id = y * ROOM_GRID_W + x
//==============================================================================
#include "game.h"
#include "game_layout_data.h"
#include "bsp.h"
#include "CI_gpu2d.h"
#include "user_gpu2d.h"

/* Keep transition measurements available in IDE builds without custom flags. */
#ifndef GAME_PROFILE
#define GAME_PROFILE 1
#endif

/*  ★ L 房总开关：整份文件只在这里判一次。
 *    以前 6 处 #ifdef LAY_map_L 散在生成/布局两段里，布局改名或删掉时
 *    会"静默关掉 L 生成"，编译一个字都不报（2026-10-24 踩过）。
 *    现在要停用 L 只改这里：改成 0。  */
#ifdef LAY_map_L
#  define HAS_L_ROOM 1
#else
#  define HAS_L_ROOM 0
#endif

/*  一格房间：类型 + 网格坐标。没有房间的格子 type = ROOM_NONE。  */
/*==============================================================================
 *  房间实例：一间房在【运行时】的全部数据。
 *  ★ 和"房间布局"（game_layout.c 的 room_layout_t）分开，两者职责不同：
 *      room_layout_t = 蓝图（只读）：这间房长什么样，对象位置只存在蓝图里
 *      room_t        = 实例（可写）：这间房现在是什么类型、用哪张蓝图、
 *                      道具拿没拿、内容生没生成、箱子破没破、奖励发没发
 *  以前这些状态散在 5 个平行数组里（s_used / s_spawned / s_respawn /
 *  s_reward / s_cleared），加一种状态就要加一个数组 + 一对读写函数；
 *  现在全部收进这一条记录，加状态 = 加一个位标志。
 *============================================================================*/
enum {
    R_USED    = 1u << 0,   /* 道具房：这一轮的道具已经拿过 */
    R_SPAWNED = 1u << 1,   /* 内容（道具/Boss/怪）这一轮已经生成过 */
    R_RESPAWN = 1u << 2,   /* 死亡卡：这间房下次进入要重新刷怪 */
    R_CLEARED = 1u << 3    /* 房间已经清过（奖励只发一次） */
};

typedef struct {
    signed char   type;        /* ROOM_NONE / START / EMPTY / ITEM / SHOP / BOSS */
    signed char   gx, gy;
    signed char   owner;       /* -1 = 自己；>=0 = 被 2×2 房间覆盖时的 anchor */
    unsigned char w, h;        /* 构造：普通 1×1；Boss 2×2 的 anchor 填 2/2 */
    signed char   layout;      /* 蓝图下标；-1 = 没有固定布局（走随机兜底） */
    unsigned char flags;       /* R_* 位 */
    unsigned char reward;      /* 清房奖励：0..5 枚金币 */
    unsigned char flip;        /* 异形房镜像：bit0 左右翻、bit1 上下翻（L 房的 4 种形状） */
} room_t;

static room_t s_rooms[ROOM_COUNT];

/*  多格房统一折算：覆盖格 → 锚点格；1×1 房返回自己。 -1 = 无效。
 *  ★ 形状和 1×1 走同一套数据的唯一入口：判邻居、BFS、回字形都先过这里。  */
static int room_canon(int c)
{
    if (c < 0 || c >= ROOM_COUNT) return -1;
    return (s_rooms[c].owner >= 0) ? (int)s_rooms[c].owner : c;
}

/*  房间实例的状态位读写（内部用；对外还是 game_room_is_used() 那套 API）  */
static int  room_flag(int id, unsigned char f) { return (s_rooms[id].flags & f) != 0; }
static void room_set(int id, unsigned char f)  { s_rooms[id].flags |= f; }
static void room_clr(int id, unsigned char f)  { s_rooms[id].flags = (unsigned char)(s_rooms[id].flags & ~f); }

/*  换房时记下"旧蓝图"，好把旧房间的静态道具从两块缓冲里擦掉。
 *  静态道具不再有实例、也不逐帧标脏，只在换房后的 2 帧（两块缓冲各一次）补标。  */
static signed char   s_prev_layout = -1;
static unsigned char s_prev_flip;
static unsigned char s_prop_dirty;

/*  房内 interact "已经被破坏"：只记 (房间, 格子 x, y)。
 *  ★ 位置和种类都从房间布局表查 —— 布局表是唯一的位置来源，
 *    所以这里不记对象序号（序号会随布局重新导出而变），也不记种类。
 *    (x, y) 是布局坐标（0..44 / 0..34），一个格子最多一个对象。  */
static struct { unsigned char room, x, y; } s_broken[MAX_BROKEN];
static int s_broken_n;

int game_room_cell_gone(int room, int x, int y)
{
    int i;

    if (room < 0 || room >= ROOM_COUNT || x < 0 || y < 0) return 0;
    for (i = 0; i < s_broken_n; ++i)
        if (s_broken[i].room == (unsigned char)room &&
            s_broken[i].x == (unsigned char)x &&
            s_broken[i].y == (unsigned char)y) return 1;
    return 0;
}

void game_room_mark_cell_gone(int room, int x, int y)
{
    int i;

    if (room < 0 || room >= ROOM_COUNT || x < 0 || x > 255 || y < 0 || y > 255) return;
    if (game_room_cell_gone(room, x, y)) return;

    if (s_broken_n >= MAX_BROKEN) {
        /*  满了：丢掉最早的一条（那几块会在重进房间时长回来，正常碰不到）  */
        for (i = 1; i < s_broken_n; ++i) s_broken[i - 1] = s_broken[i];
        --s_broken_n;
    }
    s_broken[s_broken_n].room = (unsigned char)room;
    s_broken[s_broken_n].x    = (unsigned char)x;
    s_broken[s_broken_n].y    = (unsigned char)y;
    ++s_broken_n;
}

void game_room_clear_broken(void)
{
    s_broken_n = 0;
}

int game_room_prev_layout(void) { return (int)s_prev_layout; }
int game_room_prev_flip(void) { return (int)s_prev_flip; }

int game_room_prop_dirty(void)
{
    if (s_prop_dirty == 0u) return 0;
    --s_prop_dirty;
    return 1;
}

static int s_start_room;            /* 本轮起点房间的格子编号 */
static int s_boss_anchor = -1;      /* Boss 2×2 的左上角（规范房间 id） */
#if GAME_BOSS_SHORTCUT
static int s_debug_boss_entry;
#endif
static int s_boss_entry = -1;       /* 生成时选中的 Boss 入口格（验证距离用） */

/*  当前房间里的门：地图门格 + 内侧落点 + 朝向 + 通到的房间。
 *  ★ 一间房可以有 8 扇门、同方向可以有多扇（L 形、2×2 大房）。 */
#define MAX_ROOM_DOORS 8
typedef struct {
    unsigned char dx, dy; /* D/H tile anchor; bounded by MAP_MAX_* */
    unsigned char cx, cy; /* inward spawn cell */
    unsigned char dir;    /* N/E/S/W = 0..3 */
    signed char room;     /* destination room, -1 = wall */
    signed char cell;     /* owning grid cell, -1 = none */
    unsigned char hidden;
} door_info_t;

static int s_room;
static int s_pending;
static int s_target;
static int s_entry_dir;         /* < 0 = 落在房间中心 */
static int s_entry_cell = -1;   /* 从哪一格进来（多格房同方向有多扇门时用来对位） */
static int s_lock;              /* 刚进过门 ⇒ 不重复触发 */
static unsigned char s_force_full;
/* A full transition owns the back buffer until its entry view is complete.
 * The other buffer is repaired on its next ordinary rendering turn. */
static signed char s_rebuild_buf = -1;
enum { ROOM_BUILD_TILES, ROOM_BUILD_FINISH };
static unsigned char s_rebuild_phase;
static int s_rebuild_row, s_rebuild_last;
#ifndef ROOM_BUILD_ROWS
#define ROOM_BUILD_ROWS 2
#endif
#if ROOM_BUILD_ROWS < 1 || ROOM_BUILD_ROWS > 4
#error ROOM_BUILD_ROWS must be between 1 and 4
#endif
/* Process several small batches before yielding. The millisecond clock can
 * round by 1 ms; one already-started row batch may exceed this soft budget. */
#ifndef ROOM_BUILD_BUDGET_MS
#define ROOM_BUILD_BUDGET_MS 6u
#endif
#if ROOM_BUILD_BUDGET_MS < 1 || ROOM_BUILD_BUDGET_MS > 8
#error ROOM_BUILD_BUDGET_MS must be between 1 and 8
#endif
#if GAME_PROFILE
static u32 s_rebuild_t0;
static u32 s_rebuild_cmds;
static u32 s_rebuild_frames;
static u32 s_rebuild_max_ticks;
static u32 s_rebuild_max_cmds;
static u32 s_rebuild_work_ticks;
#endif

static door_info_t s_doors[MAX_ROOM_DOORS];
static int s_door_count;

/*  已炸开的隐藏连接：记"房间对"，顺序无关。
 *  一局最多 2 间隐藏房 × 4 面 = 8 条，给 8 条足够。  */
#define MAX_HIDDEN_LINKS 8
static unsigned char s_hopen_a[MAX_HIDDEN_LINKS];
static unsigned char s_hopen_b[MAX_HIDDEN_LINKS];
static int s_hopen_n;

static void hidden_swap(int *a, int *b)
{
    int t = *a;

    *a = *b;
    *b = t;
}

int game_room_hidden_is_open(int a, int b)
{
    int i;

    if (a < 0 || b < 0) return 0;
    if (a > b) hidden_swap(&a, &b);
    for (i = 0; i < s_hopen_n; ++i)
        if ((int)s_hopen_a[i] == a && (int)s_hopen_b[i] == b) return 1;
    return 0;
}

int game_room_hidden_open(int a, int b)
{
    if (a < 0 || b < 0 || a == b) return 0;
    if (game_room_hidden_is_open(a, b)) return 0;
    if (s_hopen_n >= MAX_HIDDEN_LINKS) return 0;
    if (a > b) hidden_swap(&a, &b);
    s_hopen_a[s_hopen_n] = (unsigned char)a;
    s_hopen_b[s_hopen_n] = (unsigned char)b;
    ++s_hopen_n;
    g.door_dirty = GAME_FB_COUNT;
    return 1;
}

void game_room_hidden_reset(void)
{
    s_hopen_n = 0;
}

/*============================================================================*/
/*  网格 / 生成  */
/*============================================================================*/

static int cell_id(int x, int y)
{
    return y * ROOM_GRID_W + x;
}

/*  某房间在 dir 方向的邻居格（不检查有没有房间）  */
static int room_neighbor(int room, int dir)
{
    static const signed char dx[4] = { 0, 1, 0, -1 };
    static const signed char dy[4] = { -1, 0, 1, 0 };
    int x, y, nx, ny;

    if (room < 0 || room >= ROOM_COUNT) return -1;
    x = s_rooms[room].gx;
    y = s_rooms[room].gy;
    nx = x + dx[dir & 3];
    ny = y + dy[dir & 3];
    if (nx < 0 || nx >= ROOM_GRID_W || ny < 0 || ny >= ROOM_GRID_H) return -1;
    return cell_id(nx, ny);
}

/*  一组格子在 4 邻域上碰到的【别的房间】（按 anchor 折算、去重，最多记 cap 个）。
 *  ★ 生成期只有三处要用它：判叶子度数、判成环、判 L 放下会不会把两间房连起来。
 *    以前这三处各抄一遍同样十来行循环，改一处忘两处就是新 bug。  */
static int room_ext_neighbors(const int *cells, int n, int *out, int cap)
{
    int self = room_canon(cells[0]);
    int i, d, m = 0;

    for (i = 0; i < n; ++i)
        for (d = 0; d < 4; ++d) {
            int nb = room_neighbor(cells[i], d), c, j, dup = 0;

            if (nb < 0 || s_rooms[nb].type == ROOM_NONE) continue;
            /*  ★ 隐藏房【不在房间图里】：它的门炸开之前是墙，而且是在
             *    game_room_validate() 跑完之后才塞进来的。算成邻居会把
             *    道具房/商店房的"叶子"判死 ⇒ 启动直接 ROOM ERROR（2026-10-25 抓到）。  */
            if (s_rooms[nb].type == ROOM_HIDDEN ||
                s_rooms[nb].type == ROOM_HIDDEN_SUPER) continue;
            for (j = 0; j < n; ++j)                     /* 块内自家格子 */
                if (cells[j] == nb) dup = 1;
            if (dup) continue;
            c = room_canon(nb);
            if (c == self) continue;                    /* 自家覆盖格不算邻居 */
            for (j = 0; j < m; ++j)
                if (out[j] == c) dup = 1;
            if (dup || m >= cap) continue;
            out[m++] = c;
        }
    return m;
}

/*  某房间周围有几间房（4 邻域，按房间折算：多格房的覆盖格只算一间）  */
static int room_degree(int room)
{
    int seen[4];

    return room_ext_neighbors(&room, 1, seen, 4);
}

/*  Boss 2×2 落子：anchor 一条完整记录，另外三格写成 owner 覆盖格。
 *  ★ 生成器（room_boss_expand）和兜底图（room_fallback）共用这一份，
 *    以前两处各写一遍 12 行的双层循环。  */
static void room_fill_block(int anchor, int type, int w, int h)
{
    int dx, dy;

    for (dy = 0; dy < h; ++dy)
        for (dx = 0; dx < w; ++dx) {
            int id = anchor + dy * ROOM_GRID_W + dx;

            s_rooms[id].type = (signed char)type;
            if (id == anchor) {
                s_rooms[id].w = (unsigned char)w;
                s_rooms[id].h = (unsigned char)h;
                s_rooms[id].owner = -1;
            } else {
                s_rooms[id].w = 0;
                s_rooms[id].h = 0;
                s_rooms[id].owner = (signed char)anchor;
            }
        }
}

/*  Boss 2×2：把一个叶子 Boss 格扩成 2×2；要求另外 3 格为空，
 *  且 2×2 外围只有它原本那一个普通邻居（保证只有一扇外门）。  */
static int room_boss_expand(int boss)
{
    static const signed char ox[4] = { -1, 0, -1, 0 };
    static const signed char oy[4] = { -1, -1, 0, 0 };
    int k;

    for (k = 0; k < 4; ++k) {
        int x0 = (int)s_rooms[boss].gx + ox[k];
        int y0 = (int)s_rooms[boss].gy + oy[k];
        int dx, dy, ok = 1;
        int ext_n = 0;
        int ext[8];

        if (x0 < 0 || y0 < 0 || x0 + 1 >= ROOM_GRID_W || y0 + 1 >= ROOM_GRID_H) continue;
        for (dy = 0; dy < 2 && ok; ++dy)
            for (dx = 0; dx < 2; ++dx) {
                int id = cell_id(x0 + dx, y0 + dy);

                if (id == boss) continue;
                if (s_rooms[id].type != ROOM_NONE) { ok = 0; break; }
            }
        if (!ok) continue;

        /*  数 2×2 外围的普通邻居（去重）。  */
        for (dy = 0; dy < 2 && ok; ++dy)
            for (dx = 0; dx < 2 && ok; ++dx) {
                int cx = x0 + dx, cy = y0 + dy;
                int d;

                for (d = 0; d < 4; ++d) {
                    static const signed char nx[4] = { 0, 1, 0, -1 };
                    static const signed char ny[4] = { -1, 0, 1, 0 };
                    int xx = cx + nx[d], yy = cy + ny[d], id, j;

                    if (xx < x0 || xx > x0 + 1 || yy < y0 || yy > y0 + 1) {
                        if (xx < 0 || xx >= ROOM_GRID_W || yy < 0 || yy >= ROOM_GRID_H) continue;
                        id = cell_id(xx, yy);
                        if (s_rooms[id].type == ROOM_NONE) continue;
                        for (j = 0; j < ext_n; ++j) if (ext[j] == id) break;
                        if (j == ext_n) {
                            if (ext_n >= 8) { ok = 0; break; }
                            ext[ext_n++] = id;
                        }
                    }
                }
            }
        if (!ok || ext_n != 1) continue;

        room_fill_block(cell_id(x0, y0), ROOM_BOSS, 2, 2);
        s_boss_anchor = cell_id(x0, y0);
        s_boss_entry  = boss;
        return 1;
    }
    return 0;
}

static void room_boss_reset(void)
{
    int dx, dy;

    if (s_boss_anchor < 0) return;
    for (dy = 0; dy < 2; ++dy)
        for (dx = 0; dx < 2; ++dx) {
            int id = s_boss_anchor + dy * ROOM_GRID_W + dx;

            if (s_rooms[id].type == ROOM_BOSS) {
                s_rooms[id].type = ROOM_NONE;
                s_rooms[id].w = 1;
                s_rooms[id].h = 1;
                s_rooms[id].owner = -1;
            }
        }
    s_boss_anchor = -1;
    s_boss_entry = -1;
}

/*  这个普通特殊房是否和整个 Boss 2×2 的 8 邻域相邻。  */
static int room_boss_near(int other)
{
    int i;

    if (other < 0) return 0;
    for (i = 0; i < ROOM_COUNT; ++i) {
        int dx, dy;

        if (s_rooms[i].type != ROOM_BOSS) continue;
        dx = (int)s_rooms[i].gx - (int)s_rooms[other].gx;
        dy = (int)s_rooms[i].gy - (int)s_rooms[other].gy;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx <= 1 && dy <= 1) return 1;
    }
    return 0;
}

/*  这一类型的房间用哪张"特殊门"；普通房（START/EMPTY）返回 -1。
 *  数据在 game_layout.c 的 room_kind_t 表里，这里只做查表。  */
static int room_special_door(int type)
{
    return (int)game_kind(type)->door_asset;
}

static u32 s_rand;
static u32 s_run;

static u32 room_rand(void)
{
    s_rand = s_rand * 1664525u + 1013904223u;
    return s_rand >> 16;
}

static void room_clear(void)
{
    int i;

    for (i = 0; i < ROOM_COUNT; ++i) {
        s_rooms[i].type = ROOM_NONE;
        s_rooms[i].gx = (signed char)(i % ROOM_GRID_W);
        s_rooms[i].gy = (signed char)(i / ROOM_GRID_W);
        s_rooms[i].w = 1;
        s_rooms[i].h = 1;
        s_rooms[i].owner = -1;
        s_rooms[i].layout = -1;
        s_rooms[i].flip = 0;
    }
    s_boss_anchor = -1;
    s_boss_entry = -1;
}

/*  这一步放下会不会把两间【已经存在】的房连起来（成环 / 回廊）。
 *  ★ "回廊"不只是 2×2 四格：3×3 缺一格的 8 房环、更大的环都算。
 *    统一判据：新格子 4 邻域里已经有 ≥2 间不同的房 ⇒ 放下就成环。
 *    于是整张房间图是一棵树（每个新格子只挂在唯一一间已有房上）。  */
static int room_would_cycle(int cand)
{
    int seen[4];

    return room_ext_neighbors(&cand, 1, seen, 4) >= 2;
}

/*  全图查环：房间相邻边按"房间对"去重后做并查集，一条新边两端已连通 ⇒ 成环。
 *  ★ 同一对房间之间的多条边（L 包住缺口房那种"两个内凹门"）不算环。  */
static int room_has_cycle(void)
{
    int parent[ROOM_COUNT];
    int ea[64], eb[64], ne = 0;
    int i, d, k;

    for (i = 0; i < ROOM_COUNT; ++i) parent[i] = i;

    for (i = 0; i < ROOM_COUNT; ++i) {
        int a = room_canon(i);

        if (s_rooms[i].type == ROOM_NONE || s_rooms[i].type == ROOM_HIDDEN || a != i) continue;
        for (d = 0; d < 4; ++d) {
            int nb = room_neighbor(i, d), b, j, dup = 0;

            if (nb < 0 || s_rooms[nb].type == ROOM_NONE) continue;
            /*  隐藏房不是普通房间图的一部分（它的门是假的）  */
            if (s_rooms[nb].type == ROOM_HIDDEN) continue;
            b = room_canon(nb);
            if (b <= a) continue;
            for (j = 0; j < ne; ++j)
                if (ea[j] == a && eb[j] == b) dup = 1;
            if (dup) continue;
            if (ne >= 64) return 1;        /* 边多到装不下 ⇒ 本来就不可能是一棵树 */
            ea[ne] = a; eb[ne] = b; ++ne;
        }
    }

    for (k = 0; k < ne; ++k) {
        int ra = ea[k], rb = eb[k];

        while (parent[ra] != ra) ra = parent[ra];
        while (parent[rb] != rb) rb = parent[rb];
        if (ra == rb) return 1;                        /* 这条边把两个已连通的组接起来 */
        parent[ra] = rb;
    }
    return 0;
}

/*  从起点做 4 邻域 BFS，dist[i] = 走到第 i 间房要经过几道门。
 *  索引是【房间锚点】（多格房的覆盖格折算过去）；-1 = 不可达。  */
static void room_dist_from_start(int dist[ROOM_COUNT])
{
    int q[ROOM_COUNT];
    int head = 0, tail = 0;
    int i, d, start;

    for (i = 0; i < ROOM_COUNT; ++i) dist[i] = -1;
    if (s_start_room < 0 || s_start_room >= ROOM_COUNT) return;
    if (s_rooms[s_start_room].type == ROOM_NONE) return;

    start = room_canon(s_start_room);
    dist[start] = 0;
    q[tail++] = start;
    while (head < tail) {
        int r = q[head++];

        for (d = 0; d < 4; ++d) {
            int nb = room_neighbor(r, d), c;

            if (nb < 0 || s_rooms[nb].type == ROOM_NONE) continue;
            c = room_canon(nb);
            if (dist[c] < 0) {
                dist[c] = dist[r] + 1;
                q[tail++] = c;
            }
        }
    }
}

/*  在已生成的房间列表里挑三间做道具房 / 商店房 / Boss 房：
 *    · 三间特殊房【两两】8 邻域不能相邻（也不直连）；
 *    · **特殊房必须是叶子：只能接一间普通房**（room_degree == 1）；
 *    · **Boss 房必须放在离起点最远的叶子上**（BFS 门数最大；同距离任选）；
 *    · **Boss 房不能直接连起点房**（Boss 距离必须 ≥ 2）；
 *    · 道具房 / 商店房可以连起点。
 *  三间必须同时找到；找不到就返回 0 让外层重新生成。  */
static int room_special_near(int a, int b)
{
    int dx = (int)s_rooms[a].gx - (int)s_rooms[b].gx;
    int dy = (int)s_rooms[a].gy - (int)s_rooms[b].gy;

    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return dx <= 1 && dy <= 1;
}

static int room_try_specials(const int *rooms, int n)
{
    int dist[ROOM_COUNT];
    int leaves[ROOM_COUNT];
    int nl = 0, i, j, k, maxd = -1;

    room_dist_from_start(dist);

    for (i = 0; i < n; ++i) {
        int r = rooms[i];

        if (room_canon(r) != r) continue;           /* 多格房的覆盖格不算独立一间 */
        if (s_rooms[r].w > 1 || s_rooms[r].h > 1) continue;  /* ★ 特殊房只能是 1×1（L/Boss 不能当特殊房） */
        if (r == s_start_room) continue;
        if (s_rooms[r].type == ROOM_NONE) continue;
        if (room_degree(r) != 1) continue;          /* 必须是叶子 */
        leaves[nl++] = r;
        if (dist[r] > maxd) maxd = dist[r];
    }

    if (nl < 3 || maxd < 2) return 0;               /* 至少 3 片叶子，Boss 不挨起点 */

    /*  ★ 叶子按"离起点门数"从远到近排序：Boss 优先最远那档，
     *    最远档扩不出 2×2（3 格不空 / 外围不止一个邻居）才退到次远。
     *    实测：严格只认最远档时单次尝试成功率约 1.6%，50 次里约一半会掉进兜底图；
     *    允许退档后单次约 6.8%，100 次基本不会兜底。  */
    for (i = 1; i < nl; ++i) {
        int v = leaves[i], jj = i;

        while (jj > 0 && dist[leaves[jj - 1]] < dist[v]) {
            leaves[jj] = leaves[jj - 1];
            --jj;
        }
        leaves[jj] = v;
    }

    for (i = 0; i < nl; ++i) {
        int boss = leaves[i];

        if (dist[boss] < 2) break;                  /* Boss 不能贴起点 */

        for (j = 0; j < nl; ++j) {
            int item = leaves[j];

            if (item == boss || room_special_near(boss, item)) continue;

            for (k = 0; k < nl; ++k) {
                int shop = leaves[k];

                if (shop == boss || shop == item) continue;
                if (room_special_near(boss, shop)) continue;
                if (room_special_near(item, shop)) continue;

                if (!room_boss_expand(boss)) continue;

                if (room_boss_near(item) || room_boss_near(shop)) {
                    room_boss_reset();
                    continue;
                }

                s_rooms[item].type = ROOM_ITEM;
                s_rooms[shop].type = ROOM_SHOP;
                return 1;
            }
        }
    }
    return 0;
}

/*  一次生成尝试：随机起点 + 随机生长到 11~13 间房 + 放特殊房  */
/*  每层规模：格数随 stage 增长。第 1 层要能同时摆下
 *  1×1 若干、L×1、Boss 2×2、道具/商店/隐藏各 1。  */
static int stage_cells(void)
{
    int n = 16 + (g.stage - 1) * 2;

    if (n > 24) n = 24;      /* 25 格以上失败率明显上升（49 格里太挤），先封顶 24 */
    if (n > ROOM_COUNT) n = ROOM_COUNT;
    return n;
}

#if HAS_L_ROOM
/*  生成时就地放一间 L 房：占 2×2 里的 3 格，缺口格留空并占住。
 *  ★ flip 决定缺口在哪一格 = L 的 4 种形状。地形只存一份，读数时按 flip 翻镜像：
 *      0 = 缺口左下（原图）   1 = 右下（左右翻）
 *      2 = 左上（上下翻）     3 = 右上（180°）
 *  ★ 锚点 = 2×2 里"行优先第一间"的那格，不一定在左上角（flip 2 时在右上）。
 *  返回锚点格号；0 = 摆不下。
 *  ★ 不看缺口格外面的情况：缺口是空的还是有房都行 —— 有房时内凹那两条边会通给它。  */
static int room_place_L(int c00, int flip)
{
    int x = (int)s_rooms[c00].gx, y = (int)s_rooms[c00].gy;
    int nx = flip & 1;                  /* 缺口在 2×2 里的列 */
    int ny = 1 - ((flip >> 1) & 1);     /* 缺口在 2×2 里的行（上下翻时挪到上排） */
    int notch, anchor = -1, k, m = 0, cells[3];

    if (x + 1 >= ROOM_GRID_W || y + 1 >= ROOM_GRID_H) return 0;
    notch = cell_id(x + nx, y + ny);
    /*  ★ 缺口格本身要空着（L 就是"2×2 缺一格"），但**不占住它**：
     *    以后在这格里长出一间独立的小房间是允许的（那是两间房，不是 2×2 房）。  */
    if (s_rooms[notch].type != ROOM_NONE) return 0;

    for (k = 0; k < 4; ++k) {                   /* 行优先：0=(0,0) 1=(1,0) 2=(0,1) 3=(1,1) */
        int id = cell_id(x + (k & 1), y + ((k >> 1) & 1));

        if (id == notch) continue;
        if (s_rooms[id].type != ROOM_NONE) return 0;
        cells[m++] = id;
        if (anchor < 0) anchor = id;
    }
    if (m != 3 || anchor < 0) return 0;

    /*  ★ 成环检查：L 的 3 格对外只能碰到【正好 1 间】房（parent）。
     *    > 1 ⇒ 放下就把两间已有房连通了（回廊）；
     *    = 0 ⇒ 这 3 格谁也不挨着，成了孤岛（缺口正好落在"刚长出来的那格"时会发生，
     *           flip=2 就是这样），validate 的连通性检查会整轮作废。  */
    {
        int seen[4];

        if (room_ext_neighbors(cells, 3, seen, 4) != 1) return 0;
    }

    for (k = 0; k < 3; ++k) {
        int id = cells[k];

        s_rooms[id].type = ROOM_EMPTY;
        if (id == anchor) {
            s_rooms[id].w = 2;
            s_rooms[id].h = 2;
            s_rooms[id].owner = -1;
        } else {
            s_rooms[id].w = 0;
            s_rooms[id].h = 0;
            s_rooms[id].owner = (signed char)anchor;
        }
    }
    s_rooms[anchor].flip = (unsigned char)(flip & 3);
    return anchor;
}
#endif

static int room_try_generate(void)
{
    int start, n, guard, target, l_left;
    int rooms[ROOM_COUNT];
    signed char from_dir[ROOM_COUNT];   /* 每个格子是从哪个方向长出来的 */

    room_clear();

    /*  ★ 起点固定在网格正中：7×7 才有唯一中心。  */
    start = cell_id(ROOM_GRID_W / 2, ROOM_GRID_H / 2);
    s_start_room = start;
    s_rooms[start].type = ROOM_EMPTY;
    from_dir[start] = -1;
    rooms[0] = start;
    n = 1;

    target = stage_cells();

#if HAS_L_ROOM
    l_left = 1;                     /* 每层固定摆 1 间 L 房（第 1 层就要能看见） */
#else
    l_left = 0;
#endif

    guard = 0;
    while (n < target && guard < 4000) {
        int r = rooms[room_rand() % (u32)n];
        int d = (int)(room_rand() & 3u);
        int nb;

        ++guard;

        /*  ★ 重复回廊降权：如果这一步和上一步同方向（继续拉直走廊），
         *    只有 25% 概率接受；其余 75% 强制换方向 ⇒ 多拐弯、少重复长走廊。  */
        if (from_dir[r] == d && (room_rand() % 4u) != 0u) continue;

        nb = room_neighbor(r, d);
        if (nb < 0 || s_rooms[nb].type != ROOM_NONE) continue;

        /*  ★ 形状在生长时就定：1/3 概率优先摆 L 房（占 3 格），
         *    摆不下就退回 1×1 —— 后验改装找不到 2×2 块，L 就可能整层不出现。
         *  ★ 4 种形状只用一张地形：flip 随机，读数时翻镜像（见 room_place_L）。  */
#if HAS_L_ROOM
        if (l_left > 0 && (room_rand() % 3u) == 0u) {
            /*  ★ 4 种形状一次试完：某一种摆不下（缺口占着 / 会连成环 / 会成孤岛）
             *    就换下一种，别让这一次机会白白退回 1×1（否则整层可能一直不出 L）。  */
            int f0 = (int)(room_rand() & 3u), lf = 0, t;

            for (t = 0; t < 4; ++t) {
                lf = (f0 + t) & 3;
                if (room_place_L(nb, lf)) break;
            }

            if (t < 4) {
                int x = (int)s_rooms[nb].gx, y = (int)s_rooms[nb].gy;
                int cx, cy;

                for (cy = 0; cy < 2; ++cy)
                    for (cx = 0; cx < 2; ++cx) {
                        int id = cell_id(x + cx, y + cy);

                        if (s_rooms[id].type == ROOM_NONE) continue;   /* 缺口格 */
                        from_dir[id] = (signed char)d;
                        rooms[n++] = id;
                    }
                --l_left;
                bsp_printf("ROOM: L flip=%d at (%d,%d)\r\n", lf, x, y);
                continue;
            }
        }
#endif

        /*  ★ 不许成环（回廊）：新格子只能挂在唯一一间已存在的房上  */
        if (room_would_cycle(nb)) continue;

        s_rooms[nb].type = ROOM_EMPTY;
        from_dir[nb] = (signed char)d;
        rooms[n++] = nb;
    }
    if (n < target) return 0;
#if HAS_L_ROOM
    if (l_left > 0) return 0;       /* 这一轮没摆出 L：整轮重来（第 1 层必须能看到 L 型房） */
#endif

    s_rooms[start].type = ROOM_START;
    if (!room_try_specials(rooms, n)) return 0;
    return 1;
}

/*  极端兜底：9 格十字（7×7，起点在正中）。
 *
 *              (3,1)
 *              (3,2)
 *  (1,3)(2,3)(3,3)(4,3)(5,3)
 *              (3,4)
 *              (3,5)
 *
 *  start = (3,3)；4 片叶子：
 *    item = (1,3) 距离 2，shop = (5,3) 距离 2，boss = (3,1) 距离 2（最远档）。
 *  Boss 2×2 = (3,0)(4,0)(3,1)(4,1)，外门只有 (3,2) 一扇。
 *  三间特殊房两两不 8 邻域相邻；没有 2×2 四房块。  */
static void room_fallback(void)
{
    static const unsigned char cells[9][2] = {
        {3,1},{3,2},{3,3},{3,4},{3,5},
        {1,3},{2,3},{4,3},{5,3}
    };
    int i;

    room_clear();
    for (i = 0; i < 9; ++i)
        s_rooms[cell_id(cells[i][0], cells[i][1])].type = ROOM_EMPTY;

    s_start_room = cell_id(3, 3);
    s_rooms[s_start_room].type = ROOM_START;
    s_rooms[cell_id(1, 3)].type = ROOM_ITEM;
    s_rooms[cell_id(5, 3)].type = ROOM_SHOP;

    /*  Boss 占 2×2：(3,0)(4,0)(3,1)(4,1)；只有 (3,2) 一个外门。  */
    room_fill_block(cell_id(3, 0), ROOM_BOSS, 2, 2);
    s_boss_anchor = cell_id(3, 0);
    s_boss_entry  = s_boss_anchor;
}


/*  隐藏房：所有房间都定好之后，挑一个空格塞进去（设计边界见 docs/spec/游戏_房间.md）。
 *   · 候选 = 空格，且 4 邻域里**没有 Boss**（硬性）
 *   · adj = 4 邻域里已有房间数；**从多到少**，取最前一档里随机一个（蓄水池抽样）
 *   · 谁都不挨着（adj = 0）的格子不要 —— 那样的门永远炸不到
 *  ★ 它不参与成环/连通/格数校验：因为是在 game_room_validate() 跑完之后才放的。  */
static int room_try_make_hidden(void)
{
    int i, d, best = 0, pick = -1, ties = 0;

    for (i = 0; i < ROOM_COUNT; ++i) {
        int adj = 0, bad = 0, ids[4], nid = 0;

        if (s_rooms[i].type != ROOM_NONE) continue;
        for (d = 0; d < 4; ++d) {
            int nb = room_neighbor(i, d);

            if (nb < 0) continue;
            if (s_rooms[nb].type == ROOM_BOSS) { bad = 1; break; }
            if (s_rooms[nb].type == ROOM_NONE) continue;

            /*  ★ 同一间房不能在 4 邻域里出现两次（L 形房包住这个格子时会发生）：
             *    那样这间隐藏房会朝同一间房开【两扇】秘密门，但隐藏连接是记在
             *    "房间对"上的 ⇒ 炸开一面，另一面也跟着开（2026-10-25 上板抓到）。
             *    宁可换一格放，也不要一炸开两面墙。  */
            {
                int c = room_canon(nb), k;

                for (k = 0; k < nid; ++k)
                    if (ids[k] == c) break;
                if (k < nid) { bad = 2; break; }
                ids[nid++] = c;
            }
            ++adj;
        }
        if (bad || adj < 1) continue;
        if (adj > best) {
            best = adj;
            ties = 1;
            pick = i;
        } else if (adj == best) {
            ++ties;
            if ((room_rand() % (u32)ties) == 0u) pick = i;   /* 同档等概率 */
        }
    }
    if (pick < 0) return 0;

    s_rooms[pick].type   = ROOM_HIDDEN;
    s_rooms[pick].w      = 1;
    s_rooms[pick].h      = 1;
    s_rooms[pick].owner  = -1;
    s_rooms[pick].layout = (signed char)game_layout_pick(ROOM_HIDDEN, pick);
    s_rooms[pick].flip   = 0;
    s_rooms[pick].reward = 0;
    s_rooms[pick].flags  = 0;
    return 1;
}

static void room_layout_generate(void)
{
    int attempt, i;

    s_rand = 0x9E3779B9u ^ (u32)game_ms() ^ (++s_run * 0x85EBCA6Bu);
    for (attempt = 0; attempt < 100; ++attempt) {
        if (room_try_generate() && game_room_validate() == 0) break;
    }
    if (attempt >= 100) room_fallback();

    /*  每间房清理奖励 0..5 coin；起点房不发（0）。  */
    for (i = 0; i < ROOM_COUNT; ++i) {
        int t = (int)s_rooms[i].type;

        s_rooms[i].reward = (t == ROOM_NONE || t == ROOM_START)
                    ? 0u : (unsigned char)(room_rand() % 6u);
        s_rooms[i].flags = 0;
    }
    game_room_clear_broken();               /* 新一片房间：打碎记录清零 */

    /*  给每间房挑一张固定布局（按房间类型 + 房间 id 确定性挑选）：
     *  同一间房重进 / 死亡回退都长一样；0xFF = 这类房没有固定布局（随机兜底）。
     *  ★ 多格非 Boss 房 = L 形：固定空间以左下空缺为标准，从该空间的对象方案池选择。  */
    for (i = 0; i < ROOM_COUNT; ++i) {
        int t = (int)s_rooms[i].type;

        if (t == ROOM_NONE || s_rooms[i].w == 0) {
            s_rooms[i].layout = -1;      /* 空格 / 多格房的被覆盖格 */
            continue;
        }
#if HAS_L_ROOM
        if (t != ROOM_BOSS && s_rooms[i].w > 1 && s_rooms[i].h > 1) {
            s_rooms[i].layout = (signed char)game_layout_pick_like(LAY_map_L, t, i);
            continue;
        }
#endif
        s_rooms[i].layout = (signed char)game_layout_pick(t, i);
    }

    /*  ★ 新一片房间：隐藏连接全部封回去，再放隐藏房。
     *    放在最后是有意的 —— 上面的 validate 看不到它，所以它不参与成环/连通校验。  */
    game_room_hidden_reset();
    if (!room_try_make_hidden()) {
        bsp_printf("ROOM: hidden room not placed (grid full)\r\n");
    }
}

/*============================================================================*/
/*  门洞几何 / 进门  */
/*============================================================================*/

/*  ★ 触发判定只看【门格 D 本身】，不算"门下一格"：
 *    以前门洞是 D 格 + 朝内那一格共 2 格，人还在房间里、脚下离门一整格就传送了
 *    （上方的门最明显）。现在必须站进门槛里才算走门。
 *  ★ 普通 D 保留两格门洞；隐藏 H 只有一格，内侧格是落点地板。 */
static void door_slot(const door_info_t *door, int *x0, int *y0, int *w, int *h)
{
    *x0 = door->dx * TILE;
    *y0 = door->dy * TILE;
    if (door->hidden) {
        int hx, hy;
        game_world_hidden_cell(door->dx, door->dy, &hx, &hy);
        *x0 = hx * TILE;
        *y0 = hy * TILE;
    }
    *w  = TILE;
    *h  = TILE;
}

/*  门是不是压在两格之间的接缝上（2×2 大房的门正好在正中；L 形房的门都落在各格中间）。
 *  只有接缝上的门才允许"借用相邻那一格"去找邻居 —— 否则 L 形房会在没有邻居的那半边
 *  凭空连出一扇门（2026-10-24 抓到）。  */
static int door_on_seam(int pos, int span, int cells)
{
    int k, best = 1 << 30;

    if (cells <= 1) return 0;
    for (k = 1; k < cells; ++k) {
        int d = pos * cells - k * span;

        if (d < 0) d = -d;
        if (d < best) best = d;
    }
    return best <= cells;            /* ≤ 1 格（单位是 1/cells 格）  */
}

/*  从地图里扫出门（最多 8 扇），同时算出每扇门通到哪间房。
 *
 *  判定（L 形 / 2×2 房都适用）：
 *    1. 门"朝房间内"的那一格 = 门的归属格；
 *    2. 把房间外接框（w×h 格）等分成画布（cols×rows），归属格落在哪个格子；
 *    3. 归属格往门的方向走一格 = 邻居格；邻居格有房间就通它，没有就是墙。
 *  这样 L 形房内凹那两条边的门，都会指向被包住的那一格。  */
static void doors_build(void)
{
    static const signed char ndx[4] = { 0, 1, 0, -1 };
    static const signed char ndy[4] = { -1, 0, 1, 0 };
    int x, y, n = 0, i;
    int room = game_room_id();
    int cols = game_world_cols();
    int rows = game_world_rows();
    int gx = 0, gy = 0, bw = 1, bh = 1;

    s_door_count = 0;
    for (i = 0; i < MAX_ROOM_DOORS; ++i) {
        s_doors[i].dx = s_doors[i].dy = 0;
        s_doors[i].cx = s_doors[i].cy = 0;
        s_doors[i].dir = 0;
        s_doors[i].room = -1;
        s_doors[i].cell = -1;
        s_doors[i].hidden = 0;
    }
    if (room < 0 || room >= ROOM_COUNT) return;
    if (cols <= 0 || rows <= 0) return;

    /*  ★ 外接框 = 本房间所有格子的 min/max，**不能**拿锚点的 gx/gy 当左上角：
     *    L 的"缺口左上"（flip 2）锚点落在 2×2 的右上格。
     *    ★ 这里曾经写成"锚点 + 一个偏移"，改的时候把赋值一起删了 ⇒ gx/gy 一直是 0、
     *      bw/bh 一直是 1 ⇒ 8 扇门的归属格全算到 (0,0) ⇒ 外面那格没房间 ⇒
     *      门全部画成墙（"房间的门不在了"，2026-10-24 上板抓到）。别再省这几行。  */
    {
        int x0 = ROOM_GRID_W, y0 = ROOM_GRID_H, x1 = -1, y1 = -1;

        for (i = 0; i < ROOM_COUNT; ++i) {
            int o = (s_rooms[i].owner >= 0) ? (int)s_rooms[i].owner : i;

            if (s_rooms[i].type == ROOM_NONE || o != room) continue;
            if ((int)s_rooms[i].gx < x0) x0 = (int)s_rooms[i].gx;
            if ((int)s_rooms[i].gx > x1) x1 = (int)s_rooms[i].gx;
            if ((int)s_rooms[i].gy < y0) y0 = (int)s_rooms[i].gy;
            if ((int)s_rooms[i].gy > y1) y1 = (int)s_rooms[i].gy;
        }
        if (x1 < 0) return;                     /* 不该发生：room 是有效房间 */
        gx = x0;
        gy = y0;
        bw = x1 - x0 + 1;
        bh = y1 - y0 + 1;
    }

    /* game_world_select() already indexed all D/H cells. Reuse that list:
     * scanning the whole map again was the dominant doors phase on transitions. */
    for (i = 0; i < game_world_door_count() && n < MAX_ROOM_DOORS; ++i) {
            int dir;
            int ix, iy, cgx, cgy, nx, ny, target = -1;
            int hidden;
            char tc;

            game_world_door_at(i, &x, &y);
            tc = game_world_tile_at(x, y);
            dir = game_world_dir_at(x, y);              /* ★ 门的"朝向"，不是贴图角度 */

            if (tc != 'D' && tc != 'H') continue;       /* 'D' 两格 / 'H' 隐藏门 1 格 */

            /*  1. 朝房间内那一侧（门洞内半格）= 门格往【房间方向】走一格  */
            ix = x - ndx[dir];
            iy = y - ndy[dir];
            /*  2. 画布等分 → 归属格  */
            cgx = gx + (ix * bw) / cols;
            cgy = gy + (iy * bh) / rows;
            /*  3. 邻居格 → 房间  */
            nx = cgx + ndx[dir];
            ny = cgy + ndy[dir];
            if (nx >= 0 && nx < ROOM_GRID_W && ny >= 0 && ny < ROOM_GRID_H) {
                int cell = cell_id(nx, ny);

                if (s_rooms[cell].type != ROOM_NONE) {
                    int owner = s_rooms[cell].owner;

                    target = (owner >= 0) ? owner : cell;
                }
            }

            /*  兜底（多格房，比如 Boss 2×2）：门正好压在两格接缝上时，
             *  归属格可能落在没有邻居的那半边上。这时沿同一条边再看别的格子，
             *  有邻居就连过去，并把门的归属改成【真正接通的那一格】。
             *  ★ 只认【房间外】的格子：邻居格如果还属于本房间自己，
             *    说明那扇门朝的是自家内部（L 的缺口边就是这种），不能算门。
             *  ★★ 只有接缝上的门才允许借格：L 形房的门都在各格中间，离接缝好几格，
             *    借格会在没有邻居的那半边凭空连出一扇门（两个北门通同一间隔壁房）。  */
            if (target < 0 &&
                (door_on_seam((ndx[dir] == 0) ? ix : iy,
                              (ndx[dir] == 0) ? cols : rows,
                              (ndx[dir] == 0) ? bw : bh)
#if GAME_BOSS_1X1_TEST
                 || s_rooms[room].type == ROOM_BOSS
#endif
                )) {
                int c2;

                for (c2 = 0; c2 < ROOM_COUNT; ++c2) {
                    int o2 = s_rooms[c2].owner;
                    int rid = (o2 >= 0) ? o2 : c2;
                    int nx2, ny2, cell2;

                    if (rid != room || s_rooms[c2].type == ROOM_NONE) continue;
                    if (ndx[dir] == 0 && (int)s_rooms[c2].gy != cgy
#if GAME_BOSS_1X1_TEST
                        && s_rooms[room].type != ROOM_BOSS
#endif
                       ) continue;   /* 上下门：同一行 */
                    if (ndy[dir] == 0 && (int)s_rooms[c2].gx != cgx
#if GAME_BOSS_1X1_TEST
                        && s_rooms[room].type != ROOM_BOSS
#endif
                       ) continue;   /* 左右门：同一列 */
                    nx2 = (int)s_rooms[c2].gx + ndx[dir];
                    ny2 = (int)s_rooms[c2].gy + ndy[dir];
                    if (nx2 < 0 || nx2 >= ROOM_GRID_W || ny2 < 0 || ny2 >= ROOM_GRID_H) continue;
                    cell2 = cell_id(nx2, ny2);
                    if (s_rooms[cell2].type == ROOM_NONE) continue;
                    o2 = s_rooms[cell2].owner;
                    if (((o2 >= 0) ? o2 : cell2) == room) continue;              /* 自家格子，跳过 */
                    target = (o2 >= 0) ? o2 : cell2;
                    cgx = (int)s_rooms[c2].gx;      /* 门归属改成真正接通的那一格 */
                    cgy = (int)s_rooms[c2].gy;
                    break;
                }
            }

            /*  最终保险：指向自己 = 这不是一扇门（缺口边上那种）。  */
            if (target == room) target = -1;

            /*  ★ 隐藏连接：自己这格是 'H'，或者对面是隐藏房 —— 两头算同一条连接。
             *    没炸开之前：当墙画、不可走、不传送。  */
            hidden = (tc == 'H');
            if (target >= 0 && s_rooms[target].type == ROOM_HIDDEN) hidden = 1;
            if (target < 0) hidden = (tc == 'H');

            s_doors[n].dx = x;
            s_doors[n].dy = y;
            s_doors[n].dir = dir;
            s_doors[n].room = target;
            s_doors[n].hidden = hidden;
            s_doors[n].cell = cell_id(cgx, cgy);
            /*  ★ 入口格固定在门内侧一格（= 上面算的 ix/iy，别重复一份方向判断）：
             *    这一格被 cell_blocked() 保护，不会长石头。  */
            s_doors[n].cx = ix;
            s_doors[n].cy = iy;
            ++n;
        }

    /*  ★ 自检：一间房 8 扇门【全部】没邻居 = 外接框或朝向算错了（门会全变成墙）。
     *    正常房间至少有一扇门通到别的房，所以这条一定是 bug —— 打一行日志，
     *    省得又靠上板盯着屏幕找。  */
    {
        int alive = 0;

        for (i = 0; i < n; ++i) if (s_doors[i].room >= 0) ++alive;
        if (n > 0 && alive == 0) bsp_printf("ROOM: all doors walled (room %d)\r\n", room);
    }

    s_door_count = n;

    /* Eight per-door lines overflow the UART FIFO and can block a Boss-room
     * transition for tens of milliseconds. Enable them only for telemetry. */
#if GAME_UART_TELEMETRY
    for (i = 0; i < s_door_count; ++i) {
        int t = s_doors[i].room;
        int cur = game_room_id();
        if ((cur >= 0 && cur < ROOM_COUNT && s_rooms[cur].type == ROOM_BOSS) ||
            (t >= 0 && t < ROOM_COUNT && s_rooms[t].type == ROOM_BOSS)) {
            bsp_printf("ROOM DOOR: cur=%d type=%d cell=%d,%d dir=%d target=%d ttype=%d asset=%d hidden=%d\r\n",
                       cur, (cur >= 0 && cur < ROOM_COUNT) ? (int)s_rooms[cur].type : -1,
                       s_doors[i].dx, s_doors[i].dy, s_doors[i].dir, t,
                       (t >= 0 && t < ROOM_COUNT) ? (int)s_rooms[t].type : -1,
                       game_room_door_asset_at(s_doors[i].dx, s_doors[i].dy),
                       s_doors[i].hidden);
        }
    }
#endif
}

/*  移动边界：只要这个方向【有邻居房间】，就把那一段门洞放行。
 *  ★ 有没有怪只影响 trigger_hit() 的传送判定，不再把玩家挡在门外。  */
void game_room_door_bounds(int px, int py, int *x0, int *y0, int *x1, int *y1)
{
    int cx = px + PB_OFF_X + PB_W / 2;
    int cy = py + PB_OFF_Y + PB_H / 2;
    int i;

    for (i = 0; i < s_door_count; ++i) {
        const door_info_t *door = &s_doors[i];
        int sx, sy, sw, sh;

        /*  ★ 门洞的【可走性】只看有没有邻居房间，不看有没有怪。
         *    有怪时玩家可以走进门洞，但 trigger_hit() 用
         *    game_room_door_open() 判定"能不能传送"，所以不会换房。  */
        if (door->room < 0) continue;
        if (!game_room_door_visible(door->dx, door->dy)) continue;   /* 未开的隐藏门不挡也不放行 */
        door_slot(door, &sx, &sy, &sw, &sh);

        if (door->dir == 0) {
            if (cx >= sx && cx < sx + sw) {
                int lim = sy + sh - (PB_OFF_Y + PB_H / 2) - 1;
                if (*y0 > lim) *y0 = lim;
            }
        } else if (door->dir == 2) {
            if (cx >= sx && cx < sx + sw) {
                int lim = sy - (PB_OFF_Y + PB_H / 2) + 1;
                if (*y1 < lim) *y1 = lim;
            }
        } else if (door->dir == 3) {
            if (cy >= sy && cy < sy + sh) {
                int lim = sx + sw - (PB_OFF_X + PB_W / 2) - 1;
                if (*x0 > lim) *x0 = lim;
            }
        } else {
            if (cy >= sy && cy < sy + sh) {
                int lim = sx - (PB_OFF_X + PB_W / 2) + 1;
                if (*x1 < lim) *x1 = lim;
            }
        }
    }
}

/*  这个方向有没有门 = 邻居格有没有房间。  */
int game_room_type(void)
{
    if (s_room < 0 || s_room >= ROOM_COUNT) return ROOM_EMPTY;
    return (int)s_rooms[s_room].type;
}

/*  当前房间的固定布局下标（game_layout）；-1 = 这类房没有固定布局  */
int game_room_layout(void)
{
    int id = game_room_id();

    if (id < 0 || id >= ROOM_COUNT) return -1;
    return (int)s_rooms[id].layout;
}

/*  当前房间地形的镜像方向（0..3，见 room_place_L）；普通房恒为 0。
 *  ★ L 房的 4 种形状就靠它：地形只存一份，game_world 读数时按这个值翻坐标和角度。  */
int game_room_flip(void)
{
    int id = game_room_id();

    if (id < 0 || id >= ROOM_COUNT) return 0;
    return (int)(s_rooms[id].flip & 3);
}

/*  小地图用：当前房间在网格里的坐标 / 任意格子的房间类型（无房间 = ROOM_NONE）  */
void game_room_pos(int *gx, int *gy)
{
    int id = game_room_id();

    if (id < 0 || id >= ROOM_COUNT) {
        *gx = -1;
        *gy = -1;
        return;
    }
    *gx = (int)s_rooms[id].gx;
    *gy = (int)s_rooms[id].gy;
}

int game_room_type_at(int gx, int gy)
{
    if (gx < 0 || gx >= ROOM_GRID_W || gy < 0 || gy >= ROOM_GRID_H) return ROOM_NONE;
    return (int)s_rooms[cell_id(gx, gy)].type;
}

/*  这一格属于哪间房（多格房返回锚点格号；空格返回 -1）。小地图用。  */
int game_room_owner_at(int gx, int gy)
{
    int id;

    if (gx < 0 || gx >= ROOM_GRID_W || gy < 0 || gy >= ROOM_GRID_H) return -1;
    id = cell_id(gx, gy);
    if (s_rooms[id].type == ROOM_NONE) return -1;
    return (s_rooms[id].owner >= 0) ? (int)s_rooms[id].owner : id;
}

/*  这一格能不能站人：地板 B/K 可以；可见门及其内侧格也可以，
 *  其余（黑边、L 的缺口、墙带）都不行。异形房靠它做逐格碰撞。  */
int game_room_cell_walkable(int tx, int ty)
{
    int i;
    char tile = game_world_tile_at(tx, ty);

    if (tile == 'B' || tile == 'K') return 1;
    for (i = 0; i < s_door_count; ++i) {
        const door_info_t *d = &s_doors[i];

        if (!game_room_door_visible(d->dx, d->dy)) continue;   /* 没炸开的隐藏门 = 墙 */
        if ((d->dx == tx && d->dy == ty) || (d->cx == tx && d->cy == ty)) return 1;
    }
    return 0;
}

int game_room_rect_walkable(int x, int y, int w, int h)
{
    int x0, y0, x1, y1, tx, ty;

    if (w <= 0 || h <= 0 || x < 0 || y < 0) return 0;
    x0 = x / TILE;
    y0 = y / TILE;
    x1 = (x + w - 1) / TILE;
    y1 = (y + h - 1) / TILE;
    for (ty = y0; ty <= y1; ++ty)
        for (tx = x0; tx <= x1; ++tx)
            if (!game_room_cell_walkable(tx, ty)) return 0;
    return 1;
}

/*  当前房间占几格（普通 1×1，Boss/L 房 2×2）。  */
void game_room_span(int *w, int *h)
{
    int id = game_room_id();

    *w = 1;
    *h = 1;
    if (id < 0 || id >= ROOM_COUNT) return;
    if (s_rooms[id].w > 0) *w = (int)s_rooms[id].w;
    if (s_rooms[id].h > 0) *h = (int)s_rooms[id].h;
}

/*  按地图位置找这扇门  */
static const door_info_t *door_at(int tx, int ty)
{
    int i;

    for (i = 0; i < s_door_count; ++i)
        if (s_doors[i].dx == tx && s_doors[i].dy == ty) return &s_doors[i];
    return 0;
}

/*  这扇门通到哪间房；-1 = 外面那格没有房间（画成墙）  */
int game_room_door_target(int tx, int ty)
{
    const door_info_t *door = door_at(tx, ty);

    return door ? door->room : -1;
}

int game_room_door_exists_at(int tx, int ty)
{
    return game_room_door_target(tx, ty) >= 0;
}

/*  这扇门【画不画】：隐藏连接没炸开之前一律当墙（贴图、可走、传送都按这个走）。  */
/*  点到"格子矩形"的距离（用于炸弹冲击波判命中）。  */
static int hidden_rect_hit(int px, int py, int rx, int ry, int r)
{
    int dx = 0, dy = 0;

    if (px < rx) dx = rx - px;
    else if (px > rx + TILE - 1) dx = px - (rx + TILE - 1);
    if (py < ry) dy = ry - py;
    else if (py > ry + TILE - 1) dy = py - (ry + TILE - 1);
    return dx * dx + dy * dy <= r * r;
}

/*  炸弹命中未开的隐藏门 ⇒ 打开那一条连接。返回这次新打开几条。
 *  ★ 只认【本房间门表】里 hidden 的那些；同一条连接在两侧各有一扇门，
 *    从哪边炸都能开，开的是同一个房间对。  */
int game_room_blast_hidden(int cx, int cy, int r)
{
    int i, opened = 0;

    for (i = 0; i < s_door_count; ++i) {
        const door_info_t *d = &s_doors[i];
        int me = game_room_id();
        int sx, sy, sw, sh;

        if (d->room < 0 || !d->hidden) continue;
        if (game_room_hidden_is_open(me, d->room)) continue;
        /* Same one-cell entrance as rendering, movement and teleport.
         * cx/cy is a spawn floor cell for H, not a second door cell. */
        door_slot(d, &sx, &sy, &sw, &sh);
        if (!hidden_rect_hit(cx, cy, sx, sy, r)) continue;
        if (game_room_hidden_open(me, d->room)) {
            ++opened;
            bsp_printf("BOMB: hidden link %d <-> %d\r\n", me, (int)d->room);
        }
    }
    if (opened) (void)game_background_refresh_doors();
    return opened;
}

int game_room_door_visible(int tx, int ty)
{
    const door_info_t *d = door_at(tx, ty);

    if (!d || d->room < 0) return 0;
    if (!d->hidden) return 1;
    return game_room_hidden_is_open(game_room_id(), d->room);
}

/*  这扇门【能不能传送玩家】。
 *  ★ 门的贴图不要用这个函数判断 —— 贴图永远按 game_room_door_exists_at()
 *    渲染，有没有怪只影响传送（见 game_world.c 的 is_door）。  */
int game_room_door_open_at(int tx, int ty)
{
    int target;

    if (s_room < 0 || s_room >= ROOM_COUNT) return 0;

    target = game_room_door_target(tx, ty);
    if (target < 0 || target == game_room_id()) return 0;
    if (!game_room_door_visible(tx, ty)) return 0;      /* 隐藏连接没炸开：能看不能走 */

    /*  Boss 还活着（战斗阶段）：Boss 房的门不能传送。
     *  用 phase 而不是只看 HP：进房时先放人、再 spawn Boss。  */
    if (s_rooms[s_room].type == ROOM_BOSS &&
        g.phase == PHASE_FIGHT && g.boss_hp > 0) return 0;

    /*  当前房间还有小怪：不能传送（门贴图仍然是门）  */
    if (game_mobs_alive() > 0) return 0;

    return 1;
}

/*  这扇门用哪张贴图：
 *    · 隐藏连接【已炸开】 -> 隐藏入口（两侧都换，同一面墙）；
 *    · 自己是特殊房 -> 自己的特殊门；
 *    · 否则通到的房间是特殊房 -> 它的特殊门；
 *    · 其余 -> normal。  */
int game_room_door_asset_at(int tx, int ty)
{
    const door_info_t *d = door_at(tx, ty);
    int cur, dst, target, cur_room;

    if (s_room < 0 || s_room >= ROOM_COUNT)
        return ASSET_background_door_normal;

    /* Boss occupies four grid records. Always use its canonical anchor when
     * choosing the current-room door skin; otherwise a covered cell can be
     * treated as an ordinary room during a transition. */
    cur_room = game_room_id();
    if (cur_room < 0 || cur_room >= ROOM_COUNT)
        cur_room = s_room;

    /*  ★ 炸开的隐藏连接换成单格入口（32×32，紧贴内侧地板，
     *    位置见 game_world_hidden_cell）。门记录本身是【房间对】，
     *    所以两边看同一面墙，贴图一起换 —— 不会一边是入口、一边是普通门。  */
    if (d && d->hidden && d->room >= 0 &&
        game_room_hidden_is_open(game_room_id(), d->room))
        return ASSET_background_interact_hidden;

    cur = room_special_door((int)s_rooms[cur_room].type);
    if (cur >= 0) return cur;

    target = game_room_door_target(tx, ty);
    if (target >= 0 && target < ROOM_COUNT) {
        dst = room_special_door((int)s_rooms[target].type);
        if (dst >= 0) return dst;
    }
    return ASSET_background_door_normal;
}

/*  启动自检（由 game_init() 调用，非 0 拒绝启动）：
 *    · 起点 1、道具 1、商店 1、Boss 1；
 *    · 特殊房之间 8 邻域不相邻；
 *    · **特殊房只能接一间普通房**（叶子）；
 *    · **Boss 房不能直接连起点房**；
 *    · **Boss 房必须是离起点最远的叶子**（BFS 门数）；
 *    · 所有房间从起点连通。
 *  普通房可以同时接多间特殊房（不限制）。  */
int game_room_validate(void)
{
    int i, j, d, bad = 0;
    int start = 0, item = 0, shop = 0, boss = 0;

    for (i = 0; i < ROOM_COUNT; ++i) {
        int type = (int)s_rooms[i].type;

        if (type == ROOM_NONE) continue;
        if (type == ROOM_START) ++start;
        else if (type == ROOM_ITEM) ++item;
        else if (type == ROOM_SHOP) ++shop;
        else if (type == ROOM_BOSS &&
                 s_rooms[i].w == 2 && s_rooms[i].h == 2 &&
                 s_rooms[i].owner < 0) ++boss;   /* 一条 2×2 房间记录 */
    }
    if (start != 1) ++bad;
    if (item != 1) ++bad;
    if (boss != 1) ++bad;               /* 一条 Boss 房间记录，覆盖 4 格 */
    if (shop != 1) ++bad;

    /*  构造记录：普通房 1×1；Boss 一个 2×2 anchor + 三个 owner 覆盖格。  */
    {
        int anchors = 0, covered = 0;

        for (i = 0; i < ROOM_COUNT; ++i) {
            int t = (int)s_rooms[i].type;

            if (t == ROOM_BOSS) {
                if (s_rooms[i].w == 2 && s_rooms[i].h == 2 &&
                    s_rooms[i].owner < 0) {
                    ++anchors;
                } else if (s_rooms[i].w == 0 && s_rooms[i].h == 0 &&
                           s_rooms[i].owner >= 0 &&
                           s_rooms[s_rooms[i].owner].type == ROOM_BOSS) {
                    ++covered;
                } else {
                    ++bad;
                }
            } else if (t != ROOM_NONE) {
                if (s_rooms[i].owner >= 0) {
                    /*  多格房（L 形）的覆盖格：owner 必须指向一间存在的房  */
                    if (s_rooms[s_rooms[i].owner].type == ROOM_NONE) ++bad;
                } else if (s_rooms[i].w < 1 || s_rooms[i].h < 1 ||
                           s_rooms[i].w > 2 || s_rooms[i].h > 2) {
                    ++bad;
                }
            }
        }
        if (anchors != 1 || covered != 3) ++bad;
    }

    /*  特殊房之间 8 邻域不相邻  */
    for (i = 0; i < ROOM_COUNT; ++i) {
        if (!game_kind_special((int)s_rooms[i].type)) continue;
        for (j = i + 1; j < ROOM_COUNT; ++j) {
            int dx, dy;

            if (!game_kind_special((int)s_rooms[j].type)) continue;
            /*  Boss 2×2 内部四格是同一间房，不互相算“特殊房相邻”。  */
            if (s_rooms[i].type == ROOM_BOSS && s_rooms[j].type == ROOM_BOSS) continue;
            dx = (int)s_rooms[i].gx - (int)s_rooms[j].gx;
            dy = (int)s_rooms[i].gy - (int)s_rooms[j].gy;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            if (dx <= 1 && dy <= 1) ++bad;
        }
    }

    /*  特殊房只能接一间普通房（叶子）  */
    for (i = 0; i < ROOM_COUNT; ++i) {
        int type = (int)s_rooms[i].type;
        int one = -1;

        if (!game_kind_special(type)) continue;
        if (type == ROOM_BOSS) continue;            /* Boss 是 2×2 整间，不按普通叶子算 */
        if (room_degree(i) != 1) { ++bad; continue; }
        for (d = 0; d < 4; ++d) {
            int nb = room_neighbor(i, d);
            if (nb >= 0 && s_rooms[nb].type != ROOM_NONE) { one = nb; break; }
        }
        if (one < 0 || game_kind_special((int)s_rooms[one].type)) ++bad;
    }

    /*  Boss 房不能直接连起点房  */
    for (i = 0; i < ROOM_COUNT; ++i) {
        if (s_rooms[i].type != ROOM_BOSS) continue;
        for (d = 0; d < 4; ++d) {
            int nb = room_neighbor(i, d);
            if (nb == s_start_room) ++bad;
        }
    }

    /*  普通房可以同时接多间特殊房（每个方向看邻居类型各自渲染门），不再限制  */

    /*  回廊：2×2 四房块、3×3 缺一格的 8 房环、更大的环都不允许（房间图必须是树）。
     *  ★ 2×2 四房块本身就是一条 4 环，并查集版的 room_has_cycle() 一定查得到；
     *    以前那个 room_has_2x2() 只是把同一件事再扫一遍。  */
    if (room_has_cycle()) ++bad;

    /*  所有房间必须从起点连通  */
    {
        unsigned char seen[ROOM_COUNT];
        int stack[ROOM_COUNT], top = 0;

        for (i = 0; i < ROOM_COUNT; ++i) seen[i] = 0;
        if (s_start_room >= 0 && s_start_room < ROOM_COUNT &&
            s_rooms[s_start_room].type != ROOM_NONE) {
            seen[s_start_room] = 1;
            stack[top++] = s_start_room;
            while (top > 0) {
                int c = stack[--top];
                for (d = 0; d < 4; ++d) {
                    int nb = room_neighbor(c, d);
                    if (nb >= 0 && s_rooms[nb].type != ROOM_NONE &&
                        s_rooms[nb].type != ROOM_HIDDEN && !seen[nb]) {
                        seen[nb] = 1;
                        stack[top++] = nb;
                    }
                }
            }
        }
        for (i = 0; i < ROOM_COUNT; ++i)
            if (s_rooms[i].type != ROOM_NONE && s_rooms[i].type != ROOM_HIDDEN && !seen[i]) ++bad;
    }

    return bad;
}

/*  剧情跳转（Boss 击破后回起点）：target = 房间格子编号。  */
/*  调试传送：按房间类型找一间房（网格里第一个命中）  */
int game_room_find_type(int type)
{
    int i;

    for (i = 0; i < ROOM_COUNT; ++i)
        if (s_rooms[i].type == type) return i;
    return -1;
}

void game_room_request(int target, int entry_dir)
{
    if (target < 0 || target >= ROOM_COUNT) return;
    if (s_rooms[target].type == ROOM_NONE) return;

    s_target     = target;
    s_entry_dir  = (entry_dir < 0) ? -1 : (entry_dir & 3);
    s_entry_cell = -1;
    s_pending   = 1;
}

/*  死亡卡回退：强制整屏重铺，避免把死亡房间的怪/Boss 旧像素带到上一房间。  */
void game_room_request_full(int target, int entry_dir)
{
    if (target < 0 || target >= ROOM_COUNT) return;
    if (s_rooms[target].type == ROOM_NONE) return;

    s_force_full = 1;
    game_room_request(target, entry_dir);
}

#if GAME_BOSS_SHORTCUT
void game_room_debug_boss(void)
{
    if (s_boss_anchor < 0 || s_pending || s_rebuild_buf >= 0 || s_room == s_boss_anchor)
        return;
    s_debug_boss_entry = 1;
    game_room_request_full(s_boss_anchor, -1);
    bsp_printf("DEBUG: jump to boss room\r\n");
}
#endif

void game_room_mark_used(int room)
{
    if (room >= 0 && room < ROOM_COUNT) room_set(room, R_USED);
}

int game_room_is_used(int room)
{
    return (room >= 0 && room < ROOM_COUNT) ? room_flag(room, R_USED) : 0;
}

void game_room_mark_spawned(int room)
{
    if (room >= 0 && room < ROOM_COUNT) room_set(room, R_SPAWNED);
}

int game_room_is_spawned(int room)
{
    return (room >= 0 && room < ROOM_COUNT) ? room_flag(room, R_SPAWNED) : 0;
}

void game_room_clear_spawned(int room)
{
    if (room >= 0 && room < ROOM_COUNT) room_clr(room, R_SPAWNED);
}

void game_room_mark_respawn(int room)
{
    if (room >= 0 && room < ROOM_COUNT) room_set(room, R_RESPAWN);
}

int game_room_take_respawn(int room)
{
    if (room < 0 || room >= ROOM_COUNT) return 0;
    if (!room_flag(room, R_RESPAWN)) return 0;
    room_clr(room, R_RESPAWN);
    return 1;
}

int game_room_is_cleared(int room)
{
    return (room >= 0 && room < ROOM_COUNT) ? room_flag(room, R_CLEARED) : 1;
}

/*  房间清理奖励：第一次调用返回 0..5，之后返回 0（只发一次）  */
int game_room_take_reward(void)
{
    int id = game_room_id();

    if (id < 0 || id >= ROOM_COUNT) return 0;
    if (room_flag(id, R_CLEARED)) return 0;
    room_set(id, R_CLEARED);
    return (int)s_rooms[id].reward;
}

/*  新的一轮：重新生成一片房间，道具/Boss 重新生成  */
void game_room_new_run(void)
{
    int i;

    room_layout_generate();
    s_room = s_start_room;
    s_force_full = 1;                       /* 新一轮场景需要清理上一阶段的整屏残留 */
    s_rebuild_buf = -1;
    for (i = 0; i < ROOM_COUNT; ++i) {
        s_rooms[i].flags = 0;
    }
    g.exit_active = 0;
    g.last_room = -1;
    g.last_door = -1;
    g.exit_dirty = GAME_FB_COUNT;
    g.boss_wait_until = 0u;
    g.boss_drop_pending = 0;                /* 上一关没捡的 Boss 饰品不带进新一关 */
    g.item.active = 0;
    game_mobs_reset();
    game_coins_reset();
    game_stones_reset();
    game_shop_reset();
    game_minimap_reset();                   /* 新一层：地图从初始房间重新点亮 */
    game_minimap_refresh();
    game_vital_refill();                    /* 新一关血量补满，命数保留 */

    /*  ★ 新一层的初始房：炸弹不够就补齐到 START_BOMBS（多的留着，不没收）。
     *    隐藏门全靠炸弹，开局一颗都没有等于这层白跑。  */
    if (g.bomb_count < START_BOMBS) g.bomb_count = START_BOMBS;
}

int game_room_id(void)
{
    /*  Boss 占 2×2：四格统一用左上角做规范 id（状态/刷怪/门/回退都用它）。  */
    if (s_room >= 0 && s_room < ROOM_COUNT &&
        s_rooms[s_room].type == ROOM_BOSS) {
        if (s_rooms[s_room].owner >= 0) return s_rooms[s_room].owner;
        if (s_rooms[s_room].w == 2 && s_rooms[s_room].h == 2) return s_room;
        if (s_boss_anchor >= 0) return s_boss_anchor;
    }
    return s_room;
}

int game_room_start(void)
{
    return s_start_room;
}

int game_room_pending(void)
{
    return s_pending;
}

/*  把玩家摆到"门朝向的那一格中心点"：
 *  从哪扇门进来，就取那扇门 cx/cy 所指的格子（门内侧一格；这一格不长石头），
 *  让【身体矩形中心】正好落在该格中心 (cx*32+16, cy*32+16)。
 *  ★ 不再沿门轴贴到 ARENA 边内 8 px —— 那样不同朝向/不同贴图尺寸会偏。 */
static void place_at_door(int dir)
{
    int i, pass;

    /*  ★ 同方向可能有多扇门（L 形房、2×2 房）：
     *    第 1 轮按"进来的是哪一格"精确对位；
     *    第 2 轮退化成"通回上一间房"；
     *    第 3 轮只按方向兜底。  */
    for (pass = 0; pass < 3; ++pass)
    for (i = 0; i < s_door_count; ++i) {
        int x, y;

        if (s_doors[i].dir != dir) continue;
        if (pass == 0 && s_doors[i].cell != s_entry_cell) continue;
        if (pass == 1 && s_doors[i].room != g.last_room) continue;

        {
            /*  ★ 从门内侧那格开始【朝房间里走】，走到第一块地板 B 再站定。
             *    多格房 / L 形房的门不在竞技场边上，只靠"夹到竞技场边"会站进墙里。  */
            int ix = s_doors[i].cx;
            int iy = s_doors[i].cy;
            int sx = (dir == 1) ? -1 : ((dir == 3) ? 1 : 0);   /* 朝房间里 */
            int sy = (dir == 0) ? 1 : ((dir == 2) ? -1 : 0);
            int guard;

            for (guard = 0; guard < 4; ++guard) {
                if (game_world_tile_at(ix, iy) == 'B' || game_world_tile_at(ix, iy) == 'K') break;
                ix += sx;
                iy += sy;
            }
            x = ix * TILE + TILE / 2 - (PB_OFF_X + PB_W / 2);
            y = iy * TILE + TILE / 2 - (PB_OFF_Y + PB_H / 2);
        }

        /*  极端情况兜底：格子中心已经在场地内，正常不会触发。  */
        if (x < ARENA_X0 - PB_OFF_X) x = ARENA_X0 - PB_OFF_X;
        if (x > ARENA_X1 - PB_OFF_X - PB_W) x = ARENA_X1 - PB_OFF_X - PB_W;
        if (y < ARENA_Y0 - PB_OFF_Y) y = ARENA_Y0 - PB_OFF_Y;
        if (y > ARENA_Y1 - PB_OFF_Y - PB_H) y = ARENA_Y1 - PB_OFF_Y - PB_H;

        g.x = x;
        g.y = y;
        return;
    }
}

/*----------------------------------------------------------------------------*/
/*  开局：生成房间，回起点房，人物摆中间  */
void game_room_reset(void)
{
    int i;

    room_layout_generate();
    s_room = s_start_room;
    /* game_init()/game_restart() immediately initialize both framebuffers,
     * so the first ordinary room transition can use the same-map path. */
    s_force_full = 0;
    s_pending = 0;
#if GAME_BOSS_SHORTCUT
    s_debug_boss_entry = 0;
#endif
    s_rebuild_buf = -1;
    s_lock = 0;
    s_entry_dir = -1;
    for (i = 0; i < ROOM_COUNT; ++i) {
        s_rooms[i].flags = 0;
    }
    game_mobs_reset();
    game_coins_reset();
    game_stones_reset();
    game_shop_reset();

    game_world_select(s_room);
    doors_build();
    game_minimap_reset();                   /* 新一局：只点亮初始房间 + 邻近房间 */
    game_minimap_refresh();

    g.x = (ARENA_X0 + ARENA_X1) / 2 - PW / 2;
    g.y = (ARENA_Y0 + ARENA_Y1) / 2 - PH / 2;
    game_camera_snap();
}

/*----------------------------------------------------------------------------*/
/*  每帧：玩家肚子中心是否走进了某个门洞，且这扇门现在允许传送。  */
static int trigger_hit(int i)
{
    const door_info_t *door = &s_doors[i];
    int sx, sy, sw, sh;
    int cx = g.x + PB_OFF_X + PB_W / 2;
    int cy = g.y + PB_OFF_Y + PB_H / 2;

    if (!game_room_door_open_at(door->dx, door->dy)) return 0;

    door_slot(door, &sx, &sy, &sw, &sh);
    return cx >= sx && cx < sx + sw && cy >= sy && cy < sy + sh;
}

void game_room_step(void)
{
    int i, hit = -1;

    if (s_pending) return;

    for (i = 0; i < s_door_count; ++i)
        if (trigger_hit(i)) { hit = i; break; }

    if (hit < 0) { s_lock = 0; return; }
    if (s_lock) return;
    s_lock = 1;

    {
        int dir = s_doors[hit].dir;
        int target = s_doors[hit].room;
        int cx, cy;

        if (target < 0 || target >= ROOM_COUNT || s_rooms[target].type == ROOM_NONE) return;

        /*  目的房间里"靠近来源"的那一格 = 本房间的归属格往门的方向走一格。
         *  ★ L 形房同一个方向有两扇门，只靠 dir 会对错门，必须带上这一格。  */
        cx = s_doors[hit].cell % ROOM_GRID_W;
        cy = s_doors[hit].cell / ROOM_GRID_W;
        if (dir == 0)      --cy;
        else if (dir == 2) ++cy;
        else if (dir == 1) ++cx;
        else               --cx;
        s_entry_cell = (cx >= 0 && cx < ROOM_GRID_W &&
                        cy >= 0 && cy < ROOM_GRID_H) ? cell_id(cx, cy) : -1;

        s_target    = target;
        s_entry_dir = dir ^ 2;
        s_pending   = 1;
    }
}

/*----------------------------------------------------------------------------*/
int game_room_rebuild_pending(void)
{
    return s_rebuild_buf >= 0;
}

#if GAME_PROFILE
static int room_ticks_us(u32 ticks)
{
    u32 div = (u32)(BSP_CLINT_HZ / 1000000u);
    return (int)(ticks / (div ? div : 1u));
}

static void room_build_report(const char *stage)
{
    bsp_printf("ROOM %s: chunks=%d cmds=%d reused=%d max_cmds=%d max_work=%d us work=%d us elapsed=%d us\r\n",
               stage, (int)s_rebuild_frames, (int)s_rebuild_cmds,
               (int)game_floor_reused_cells(),
               (int)s_rebuild_max_cmds, room_ticks_us(s_rebuild_max_ticks), room_ticks_us(s_rebuild_work_ticks),
               room_ticks_us((u32)clint_getTime(BSP_CLINT) - s_rebuild_t0));
}
#endif

int game_room_finish_rebuild(void)
{
    static const u32 fb[GAME_FB_COUNT] = { FB_C, FB_D
#if GAME_TRIPLE_BUFFER
        , FB_THIRD
#endif
    };
    u32 window_t0;

    if (s_rebuild_buf < 0) return 1;
    window_t0 = game_ms();
    for (;;) {
        u32 cmd0 = gpu2d_getCmdDone(), cmds;
        int buf = (int)s_rebuild_buf;
        int row1 = s_rebuild_row + ROOM_BUILD_ROWS - 1;
#if GAME_PROFILE
        u32 work_t0 = (u32)clint_getTime(BSP_CLINT);
#endif
        if (row1 > s_rebuild_last) row1 = s_rebuild_last;

        if (s_rebuild_phase == ROOM_BUILD_TILES)
            cmds = game_draw_floor_rows(fb[buf], s_rebuild_row, row1);
        else
            cmds = game_draw_floor_finish(fb[buf]);

        /* Each small batch still waits for completed writes. Consecutive
         * batches share a work window; they no longer each consume a frame. */
        if (gpu2d_waitCmdDone(cmd0 + cmds, 3000000u) != 0) {
            bsp_printf("ROOM BUILD ERROR: GPU timeout\r\n");
            return 0;
        }
#if GAME_PROFILE
        {
            u32 work = (u32)clint_getTime(BSP_CLINT) - work_t0;
            ++s_rebuild_frames;
            s_rebuild_cmds += cmds;
            s_rebuild_work_ticks += work;
            if (work > s_rebuild_max_ticks) s_rebuild_max_ticks = work;
            if (cmds > s_rebuild_max_cmds) s_rebuild_max_cmds = cmds;
        }
#endif

        if (s_rebuild_phase == ROOM_BUILD_FINISH) {
#if GAME_BG_COPY
            {
                u32 bg0 = gpu2d_getCmdDone();
                u32 bg_cmds = game_background_build(FB_BG);
                u32 bg_target = bg0 + bg_cmds;
                if (gpu2d_waitCmdDone(bg_target, 10000000u) != 0) {
                    bsp_printf("ROOM BUILD ERROR: background build timeout base=%u cmds=%u target=%u done=%u\r\n",
                               bg0, bg_cmds, bg_target, gpu2d_getCmdDone());
                    return 0;
                }
            }
#endif
            if (!game_present(fb[buf])) return 0;
            g.front = buf;
            s_rebuild_buf = -1;
            /* Preserve each buffer's camera history. The inactive one still
             * contains its old room and will be repaired before its next draw. */
            g.hud_cam_x[buf] = g.cam_x;
            g.hud_cam_y[buf] = g.cam_y;
            game_flow_enter_room();
            g.door_dirty = GAME_FB_COUNT;
#if GAME_PROFILE
            room_build_report("BUILD");
#endif
            return 1;
        }

        s_rebuild_row = row1 + 1;
        if (s_rebuild_row > s_rebuild_last)
            s_rebuild_phase = ROOM_BUILD_FINISH;
        if ((u32)(game_ms() - window_t0) >= ROOM_BUILD_BUDGET_MS)
            return game_wait_display();
    }
}

int game_room_can_overlap_transition(void)
{
    int target=s_target;
    if(!s_pending || target<0 || target>=ROOM_COUNT) return 0;
    if(s_rooms[target].owner>=0) target=s_rooms[target].owner;
    /* Cached commits update FB_BG and model/cache metadata only. Uncached
     * paced rebuilds retain exclusive ownership of their chosen framebuffer. */
    return game_background_cached_for_layout(s_rooms[target].layout,s_rooms[target].flip);
}

int game_room_commit(void)
{
    int old_map_idx = game_world_map_index();   /* 同图判定：比池子下标，不比指针 */
    int old_flip = game_room_flip();
    int target_map_key;
    int same_layout;
#if GAME_PROFILE
    u32 t0 = (u32)clint_getTime(BSP_CLINT);
    u32 t_select, t_doors, t_flow;
#endif

    /* Damage must be captured before world selection, content resets, and
     * clearing old_* records discard the only footprint of existing pixels. */
    game_capture_floor_damage();
    game_static_draw_reset();
    /*  死亡卡：记录“从哪间房来、上一间房往死亡房间开的门”。
     *  s_entry_dir 是新房间的入口方向，旧房间对应的出口方向是 ^2。  */
    g.last_room = game_room_id();
    g.last_door = (s_entry_dir < 0) ? -1 : ((s_entry_dir ^ 2) & 3);

    s_prev_layout = s_rooms[game_room_id()].layout;   /* 旧房间：它的道具要擦掉 */
    s_prev_flip = (unsigned char)old_flip;
    s_prop_dirty  = 0u;                               /* 两块缓冲各标一次 */

    target_map_key = game_world_map_key_for_layout(s_rooms[s_target].layout);
    s_room = s_target;
    s_pending = 0;
    /*  同一张图不重选：只有换图 / 异形房（key < 0）才重建地图与门表 */
    if (target_map_key < 0 || target_map_key != old_map_idx)
        game_world_select(game_room_id());
#if GAME_PROFILE
    t_select = (u32)clint_getTime(BSP_CLINT);
#endif
    game_minimap_refresh();                 /* 进入新房间：刷新小地图 */

    same_layout = (game_world_map_index() == old_map_idx &&
                   (!game_world_terrain() || old_flip == game_room_flip()));
    if (s_force_full) {
        /*  下一层：交互物 / exit / 旧精灵的 old_* 已被 reset，
         *  不能再走 same_layout 快速路径，否则旧像素没人擦。  */
        same_layout = 0;
        s_force_full = 0;
        game_floor_reset();
    }

    doors_build();
#if GAME_PROFILE
    t_doors = (u32)clint_getTime(BSP_CLINT);
#endif

    /*  换图才需要重画 HUD（同图时 HUD 像素没变，重画只是白花命令） */
    if (!same_layout) {
        game_hud_invalidate();
        game_vital_invalidate();
    }
    game_item_invalidate();

    game_bullets_clear();
    g.vx8 = 0; g.vy8 = 0; g.rx = 0; g.ry = 0;
    if (s_entry_dir < 0) {
        g.x = (ARENA_X0 + ARENA_X1) / 2 - PW / 2;
        g.y = (ARENA_Y0 + ARENA_Y1) / 2 - PH / 2;
#if GAME_BOSS_SHORTCUT
        if (s_debug_boss_entry) g.y = ARENA_Y1 - PH - 64;
#endif
    } else {
        place_at_door(s_entry_dir);
    }
#if GAME_BOSS_SHORTCUT
    s_debug_boss_entry = 0;
#endif

    s_lock = 1;
    game_camera_snap();                 /* 换房后窗口立即对准新位置，不做过渡 */

    if (same_layout || game_background_ready()) {
#if GAME_BG_COPY
        /* The terrain is shared by normal rooms. Refresh just the door
         * footprints; a map/background invalidation falls back to a rebuild. */
        {
            u32 bg0 = gpu2d_getCmdDone();
            u32 bg_cmds;
            bg_cmds = game_background_refresh_doors();
            if (gpu2d_waitCmdDone(bg0 + bg_cmds, 10000000u) != 0) {
                bsp_printf("ROOM BUILD ERROR: same-map background timeout base=%u cmds=%u done=%u\r\n",
                           bg0, bg_cmds, gpu2d_getCmdDone());
                return 0;
            }
        }
#endif
        s_prop_dirty = GAME_FB_COUNT;
        g.door_dirty = GAME_FB_COUNT;
        game_flow_enter_room();
#if GAME_PROFILE
        t_flow = (u32)clint_getTime(BSP_CLINT);
        bsp_printf("ROOM TIMING same: select=%d doors=%d flow=%d total=%d us\r\n",
                   room_ticks_us(t_select - t0), room_ticks_us(t_doors - t_select),
                   room_ticks_us(t_flow - t_doors), room_ticks_us(t_flow - t0));
#endif
        return 1;
    }

    {
        int b;
        for (b = 0; b < GAME_FB_COUNT; ++b) {
            game_bullets_clear_history(b);
            g.old_item[b].active = 0;
            g.old_boss[b].active = 0;
            g.old_player[b].active = 0;
        }
    }

    /* Commit only the data. Build in paced row batches while the old
     * display base remains untouched until ROOM_BUILD_FINISH. */
    s_rebuild_buf = (signed char)((g.front + 1) % GAME_FB_COUNT);
    game_floor_prepare_begin(s_rebuild_buf == 0 ? FB_C :
                             (s_rebuild_buf == 1 ? FB_D : FB_THIRD));
    s_rebuild_phase = ROOM_BUILD_TILES;
    game_floor_view_rows(&s_rebuild_row, &s_rebuild_last);
#if GAME_PROFILE
    s_rebuild_t0 = t0;
    s_rebuild_frames = s_rebuild_cmds = s_rebuild_work_ticks = 0u;
    s_rebuild_max_ticks = s_rebuild_max_cmds = 0u;
    bsp_printf("ROOM PREP: select=%d doors=%d total=%d us rows=%d\r\n",
               room_ticks_us(t_select - t0), room_ticks_us(t_doors - t_select),
               room_ticks_us((u32)clint_getTime(BSP_CLINT) - t0),
               s_rebuild_last - s_rebuild_row + 1);
#endif
    return 1;
}
