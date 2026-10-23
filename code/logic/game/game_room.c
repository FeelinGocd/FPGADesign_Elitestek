//==============================================================================
//  game_room.c —— 房间系统：网格生成 / 看邻居类型渲染门 / 走进门触发 / 切换
//------------------------------------------------------------------------------
//  干什么      :
//    · 在 6×6 网格上随机生成一片连通房间（不再硬编码房间表）
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
#include "CI_gpu2d.h"
#include "user_gpu2d.h"
#include "user_lineBuffer.h"

/*  一格房间：类型 + 网格坐标。没有房间的格子 type = ROOM_NONE。  */
typedef struct {
    signed char type;       /* ROOM_NONE / START / EMPTY / ITEM / SHOP / BOSS */
    signed char gx, gy;
    unsigned char w, h;     /* 构造：普通 1×1；Boss 2×2 的 anchor 填 2/2 */
    signed char owner;      /* -1 = 自己；>=0 = 被 2×2 房间覆盖时的 anchor */
} room_def_t;

static room_def_t s_room_def[ROOM_COUNT];
static int s_start_room;            /* 本轮起点房间的格子编号 */
static int s_boss_anchor = -1;      /* Boss 2×2 的左上角（规范房间 id） */
static int s_boss_entry = -1;       /* 生成时选中的 Boss 入口格（验证距离用） */

/*  当前房间里那 4 扇门：门格 + 门洞（两格）+ 朝向  */
typedef struct {
    int dx, dy;     /* 地图上 D 格（门洞的第一格） */
    int cx, cy;     /* 入口用的格子（门内侧 1 格，给 place_at_door 用） */
    int dir;        /* 0 上 / 1 右 / 2 下 / 3 左  */
} door_info_t;

static int s_room;
static int s_pending;
static int s_target;
static int s_entry_dir;         /* < 0 = 落在房间中心 */
static int s_lock;              /* 刚进过门 ⇒ 不重复触发 */
static unsigned char s_force_full;  /* 新一层：强制整屏重铺，清掉上一层的交互物像素 */
static unsigned char s_used[ROOM_COUNT];     /* 道具房：这一轮的道具已经拿过了 */
static unsigned char s_spawned[ROOM_COUNT];  /* 特殊房内容（道具/Boss）这一轮已经生成过 */
static unsigned char s_respawn[ROOM_COUNT];  /* 死亡卡：这间房下次进入要重新刷怪 */
static unsigned char s_reward[ROOM_COUNT];   /* 房间清理奖励：0..5 coin */
static unsigned char s_cleared[ROOM_COUNT];  /* 房间已经清过（奖励只发一次） */

static door_info_t s_doors[4];
static int s_door_count;

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
    x = s_room_def[room].gx;
    y = s_room_def[room].gy;
    nx = x + dx[dir & 3];
    ny = y + dy[dir & 3];
    if (nx < 0 || nx >= ROOM_GRID_W || ny < 0 || ny >= ROOM_GRID_H) return -1;
    return cell_id(nx, ny);
}

/*  某房间周围有几间房（4 邻域）  */
static int room_degree(int room)
{
    int d, n = 0;

    for (d = 0; d < 4; ++d) {
        int nb = room_neighbor(room, d);
        if (nb >= 0 && s_room_def[nb].type != ROOM_NONE) ++n;
    }
    return n;
}

/*  Boss 2×2：把一个叶子 Boss 格扩成 2×2；要求另外 3 格为空，
 *  且 2×2 外围只有它原本那一个普通邻居（保证只有一扇外门）。  */
static int room_boss_expand(int boss)
{
    static const signed char ox[4] = { -1, 0, -1, 0 };
    static const signed char oy[4] = { -1, -1, 0, 0 };
    int k;

    for (k = 0; k < 4; ++k) {
        int x0 = (int)s_room_def[boss].gx + ox[k];
        int y0 = (int)s_room_def[boss].gy + oy[k];
        int dx, dy, ok = 1;
        int ext_n = 0;
        int ext[8];

        if (x0 < 0 || y0 < 0 || x0 + 1 >= ROOM_GRID_W || y0 + 1 >= ROOM_GRID_H) continue;
        for (dy = 0; dy < 2 && ok; ++dy)
            for (dx = 0; dx < 2; ++dx) {
                int id = cell_id(x0 + dx, y0 + dy);

                if (id == boss) continue;
                if (s_room_def[id].type != ROOM_NONE) { ok = 0; break; }
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
                        if (s_room_def[id].type == ROOM_NONE) continue;
                        for (j = 0; j < ext_n; ++j) if (ext[j] == id) break;
                        if (j == ext_n) {
                            if (ext_n >= 8) { ok = 0; break; }
                            ext[ext_n++] = id;
                        }
                    }
                }
            }
        if (!ok || ext_n != 1) continue;

        {
            int anchor = cell_id(x0, y0);

            for (dy = 0; dy < 2; ++dy)
                for (dx = 0; dx < 2; ++dx) {
                    int id = cell_id(x0 + dx, y0 + dy);

                    s_room_def[id].type = ROOM_BOSS;
                    if (id == anchor) {
                        s_room_def[id].w = 2;
                        s_room_def[id].h = 2;
                        s_room_def[id].owner = -1;
                    } else {
                        s_room_def[id].w = 0;
                        s_room_def[id].h = 0;
                        s_room_def[id].owner = (signed char)anchor;
                    }
                }
            s_boss_anchor = anchor;
            s_boss_entry = boss;
        }
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

            if (s_room_def[id].type == ROOM_BOSS) {
                s_room_def[id].type = ROOM_NONE;
                s_room_def[id].w = 1;
                s_room_def[id].h = 1;
                s_room_def[id].owner = -1;
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

        if (s_room_def[i].type != ROOM_BOSS) continue;
        dx = (int)s_room_def[i].gx - (int)s_room_def[other].gx;
        dy = (int)s_room_def[i].gy - (int)s_room_def[other].gy;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx <= 1 && dy <= 1) return 1;
    }
    return 0;
}

/*  这一类型的房间用哪张"特殊门"；普通房（START/EMPTY）返回 -1。  */
static int room_special_door(int type)
{
    if (type == ROOM_ITEM) return ASSET_background_door_accessory;
    if (type == ROOM_SHOP) return ASSET_background_door_shop;
    if (type == ROOM_BOSS) return ASSET_background_door_boss;
    return -1;
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
        s_room_def[i].type = ROOM_NONE;
        s_room_def[i].gx = (signed char)(i % ROOM_GRID_W);
        s_room_def[i].gy = (signed char)(i / ROOM_GRID_W);
        s_room_def[i].w = 1;
        s_room_def[i].h = 1;
        s_room_def[i].owner = -1;
    }
    s_boss_anchor = -1;
    s_boss_entry = -1;
}

/*  候选格 nb 放下去后，会不会形成 2×2 四格全是房间（回字形）  */
static int room_cell_present(int x, int y, int cand)
{
    if (x < 0 || x >= ROOM_GRID_W || y < 0 || y >= ROOM_GRID_H) return 0;
    if (cell_id(x, y) == cand) return 1;
    return s_room_def[cell_id(x, y)].type != ROOM_NONE;
}

static int room_would_2x2(int cand)
{
    int x = s_room_def[cand].gx;
    int y = s_room_def[cand].gy;
    int dx, dy;

    for (dy = -1; dy <= 0; ++dy)
        for (dx = -1; dx <= 0; ++dx) {
            int x0 = x + dx, y0 = y + dy;

            if (x0 < 0 || y0 < 0 || x0 + 1 >= ROOM_GRID_W || y0 + 1 >= ROOM_GRID_H) continue;
            if (room_cell_present(x0, y0, cand) && room_cell_present(x0 + 1, y0, cand) &&
                room_cell_present(x0, y0 + 1, cand) && room_cell_present(x0 + 1, y0 + 1, cand))
                return 1;
        }
    return 0;
}

/*  全图扫描：任何 2×2 四房块都算回字形（4 间房两两相邻、四面门）  */
static int room_has_2x2(void)
{
    int x, y;

    for (y = 0; y + 1 < ROOM_GRID_H; ++y)
        for (x = 0; x + 1 < ROOM_GRID_W; ++x) {
            int t00 = (int)s_room_def[cell_id(x, y)].type;
            int t10 = (int)s_room_def[cell_id(x + 1, y)].type;
            int t01 = (int)s_room_def[cell_id(x, y + 1)].type;
            int t11 = (int)s_room_def[cell_id(x + 1, y + 1)].type;

            /*  Boss 本身就是 2×2 一整间房，不算“四房回字形”。  */
            if (t00 == ROOM_BOSS || t10 == ROOM_BOSS ||
                t01 == ROOM_BOSS || t11 == ROOM_BOSS) continue;
            if (t00 != ROOM_NONE && t10 != ROOM_NONE &&
                t01 != ROOM_NONE && t11 != ROOM_NONE) return 1;
        }
    return 0;
}

/*  从起点做 4 邻域 BFS，dist[i] = 走到第 i 间房要经过几道门。
 *  -1 = 不可达（正常生成不应出现，validate 会拦）。  */
static void room_dist_from_start(int dist[ROOM_COUNT])
{
    int q[ROOM_COUNT];
    int head = 0, tail = 0;
    int i, d;

    for (i = 0; i < ROOM_COUNT; ++i) dist[i] = -1;
    if (s_start_room < 0 || s_start_room >= ROOM_COUNT) return;
    if (s_room_def[s_start_room].type == ROOM_NONE) return;

    dist[s_start_room] = 0;
    q[tail++] = s_start_room;
    while (head < tail) {
        int r = q[head++];

        for (d = 0; d < 4; ++d) {
            int nb = room_neighbor(r, d);

            if (nb >= 0 && s_room_def[nb].type != ROOM_NONE && dist[nb] < 0) {
                dist[nb] = dist[r] + 1;
                q[tail++] = nb;
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
    int dx = (int)s_room_def[a].gx - (int)s_room_def[b].gx;
    int dy = (int)s_room_def[a].gy - (int)s_room_def[b].gy;

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

        if (r == s_start_room) continue;
        if (s_room_def[r].type == ROOM_NONE) continue;
        if (room_degree(r) != 1) continue;          /* 必须是叶子 */
        leaves[nl++] = r;
        if (dist[r] > maxd) maxd = dist[r];
    }

    if (nl < 3 || maxd < 2) return 0;               /* 至少 3 片叶子，Boss 不挨起点 */

    /*  Boss 只在"最远那一档"里选；道具 / 商店从其余叶子里挑，
     *  并且三间两两不能 8 邻域相邻。  */
    for (i = 0; i < nl; ++i) {
        int boss = leaves[i];

        if (dist[boss] != maxd) continue;

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

                s_room_def[item].type = ROOM_ITEM;
                s_room_def[shop].type = ROOM_SHOP;
                return 1;
            }
        }
    }
    return 0;
}

/*  一次生成尝试：随机起点 + 随机生长到 11~13 间房 + 放特殊房  */
static int room_try_generate(void)
{
    int start, n, guard, target;
    int rooms[ROOM_COUNT];
    signed char from_dir[ROOM_COUNT];   /* 每个格子是从哪个方向长出来的 */

    room_clear();

    start = (int)(room_rand() % (u32)ROOM_COUNT);
    s_start_room = start;
    s_room_def[start].type = ROOM_EMPTY;
    from_dir[start] = -1;
    rooms[0] = start;
    n = 1;

    target = 11 + (int)(room_rand() % 3u);      /* 11..13 间 */
    if (target > ROOM_COUNT) target = ROOM_COUNT;

    guard = 0;
    while (n < target && guard < 2000) {
        int r = rooms[room_rand() % (u32)n];
        int d = (int)(room_rand() & 3u);
        int nb;

        ++guard;

        /*  ★ 重复回廊降权：如果这一步和上一步同方向（继续拉直走廊），
         *    只有 25% 概率接受；其余 75% 强制换方向 ⇒ 多拐弯、少重复长走廊。  */
        if (from_dir[r] == d && (room_rand() % 4u) != 0u) continue;

        nb = room_neighbor(r, d);
        if (nb < 0 || s_room_def[nb].type != ROOM_NONE) continue;

        /*  ★ 回字形 = 2×2 四格都是房，四间两两相邻；第四格放下就成环，直接拒绝  */
        if (room_would_2x2(nb)) continue;

        s_room_def[nb].type = ROOM_EMPTY;
        from_dir[nb] = (signed char)d;
        rooms[n++] = nb;
    }
    if (n < target) return 0;

    s_room_def[start].type = ROOM_START;
    if (!room_try_specials(rooms, n)) return 0;
    return 1;
}

/*  极端兜底：13 格的三叶 T 形（仍是算法摆出来的，不是写死的地图表）。
 *
 *        (4,0)
 *        (4,1)
 *        (4,2)
 *  (0,3)(1,3)(2,3)(3,3)(4,3)(5,3)
 *        (1,2)      (3,4)
 *        (1,1)      (3,5)
 *
 *  start = (2,3)；三片特殊叶子：
 *    item = (1,1) 距离 3，shop = (3,5) 距离 3，boss = (4,0) 距离 5（最远）。
 *  三间两两不 8 邻域相邻，且各自只接一间普通房；没有 2×2 四房块。  */
static void room_fallback(void)
{
    static const unsigned char cells[13][2] = {
        {0,3},{1,3},{2,3},{3,3},{4,3},{5,3},
        {1,2},{1,1},
        {3,4},{3,5},
        {4,2},{4,1},{4,0}
    };
    int i;

    room_clear();
    for (i = 0; i < 13; ++i)
        s_room_def[cell_id(cells[i][0], cells[i][1])].type = ROOM_EMPTY;

    s_start_room = cell_id(2, 3);
    s_room_def[s_start_room].type = ROOM_START;
    s_room_def[cell_id(1, 1)].type = ROOM_ITEM;
    s_room_def[cell_id(3, 5)].type = ROOM_SHOP;

    /*  Boss 占 2×2：(4,0)(5,0)(4,1)(5,1)；只有 (4,2) 一个外门。  */
    {
        int anchor = cell_id(4, 0);
        int dx, dy;

        for (dy = 0; dy < 2; ++dy)
            for (dx = 0; dx < 2; ++dx) {
                int id = cell_id(4 + dx, 0 + dy);

                s_room_def[id].type = ROOM_BOSS;
                if (id == anchor) {
                    s_room_def[id].w = 2;
                    s_room_def[id].h = 2;
                    s_room_def[id].owner = -1;
                } else {
                    s_room_def[id].w = 0;
                    s_room_def[id].h = 0;
                    s_room_def[id].owner = (signed char)anchor;
                }
            }
        s_boss_anchor = anchor;
        s_boss_entry  = anchor;
    }
}

static void room_layout_generate(void)
{
    int attempt, i;

    s_rand = 0x9E3779B9u ^ (u32)game_ms() ^ (++s_run * 0x85EBCA6Bu);
    for (attempt = 0; attempt < 50; ++attempt) {
        if (room_try_generate() && game_room_validate() == 0) break;
    }
    if (attempt >= 50) room_fallback();

    /*  每间房清理奖励 0..5 coin；起点房不发（0）。  */
    for (i = 0; i < ROOM_COUNT; ++i) {
        int t = (int)s_room_def[i].type;

        s_reward[i] = (t == ROOM_NONE || t == ROOM_START)
                    ? 0u : (unsigned char)(room_rand() % 6u);
        s_cleared[i] = 0;
    }
}

/*============================================================================*/
/*  门洞几何 / 进门  */
/*============================================================================*/

/*  一扇门的门洞 = 地图上 D 格 + 朝向房间内的那一格，共 2 格：
 *    上/下门：D 格 + 它下面一格；
 *    左门  ：D 格 + 它右边一格；
 *    右门  ：D 格 + 它左边一格（D 在场地右边界外，内半格在场地内）。
 *  ★ 右门如果也往右算，门洞会落到场地外的黑边里，玩家永远触发不了传送。  */
static void door_slot(const door_info_t *door, int *x0, int *y0, int *w, int *h)
{
    int horiz = (door->dir & 1) != 0;              /* 90° / 270° ⇒ 横向门 */

    *x0 = ((door->dir == 1) ? door->dx - 1 : door->dx) * TILE;
    *y0 = door->dy * TILE;
    *w  = horiz ? 2 * TILE : TILE;
    *h  = horiz ? TILE : 2 * TILE;
}

/*  从地图里扫出 4 扇门：门格 + 入口格 + 朝向  */
static void doors_build(void)
{
    const char (*m)[MAP_MAX_COLS + 1] = game_world_map();
    const unsigned char (*r)[MAP_MAX_COLS] = game_world_rot();
    int x, y, n = 0;
    int cols = game_world_cols();
    int rows = game_world_rows();

    s_door_count = 0;
    for (x = 0; x < 4; ++x) {
        s_doors[x].dx = 0;
        s_doors[x].dy = 0;
        s_doors[x].cx = 0;
        s_doors[x].cy = 0;
        s_doors[x].dir = 0;
    }

    for (y = 0; y < rows && n < 4; ++y)
        for (x = 0; x < cols && n < 4; ++x) {
            int dir = r[y][x] & 3;

            if (m[y][x] != 'D') continue;

            s_doors[n].dx = x;
            s_doors[n].dy = y;
            s_doors[n].dir = dir;
            /*  ★ 入口格固定在门内侧一格：这一格被 cell_blocked() 保护，不会长石头。 */
            if (dir == 0)      { s_doors[n].cx = x;     s_doors[n].cy = y + 1; }
            else if (dir == 2) { s_doors[n].cx = x;     s_doors[n].cy = y - 1; }
            else if (dir == 1) { s_doors[n].cx = x - 1; s_doors[n].cy = y;     }
            else               { s_doors[n].cx = x + 1; s_doors[n].cy = y;     }
            ++n;
        }

    s_door_count = n;
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
        if (!game_room_door_exists(door->dir)) continue;
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
    return (int)s_room_def[s_room].type;
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
    *gx = (int)s_room_def[id].gx;
    *gy = (int)s_room_def[id].gy;
}

int game_room_type_at(int gx, int gy)
{
    if (gx < 0 || gx >= ROOM_GRID_W || gy < 0 || gy >= ROOM_GRID_H) return ROOM_NONE;
    return (int)s_room_def[cell_id(gx, gy)].type;
}

/*  邻居格有没有房间（门本身存在，不看是否被怪锁住）  */
int game_room_door_exists(int dir)
{
    int i, nb;

    if (s_room < 0 || s_room >= ROOM_COUNT) return 0;

    /*  Boss 2×2：从四个 Boss 格里任意一格找同方向的外部普通邻居。
     *  这样即使玩家被摆在 anchor（左上角），也仍能找到那扇外门。  */
    if (s_room_def[s_room].type == ROOM_BOSS) {
        for (i = 0; i < ROOM_COUNT; ++i) {
            if (s_room_def[i].type != ROOM_BOSS) continue;
            nb = room_neighbor(i, dir & 3);
            if (nb >= 0 && s_room_def[nb].type != ROOM_NONE &&
                s_room_def[nb].type != ROOM_BOSS) return 1;
        }
        return 0;
    }

    nb = room_neighbor(s_room, dir & 3);
    return (nb >= 0 && s_room_def[nb].type != ROOM_NONE);
}

/*  这个方向的门【能不能传送玩家】。
 *  ★ 门的贴图不要用这个函数判断 —— 贴图永远按 game_room_door_exists()
 *    渲染，有没有怪只影响传送（见 game_world.c 的 is_door）。  */
int game_room_door_open(int dir)
{
    if (s_room < 0 || s_room >= ROOM_COUNT) return 0;

    /*  Boss 还活着（战斗阶段）：Boss 房的门不能传送。
     *  用 phase 而不是只看 HP：进房时先放人、再 spawn Boss。  */
    if (s_room_def[s_room].type == ROOM_BOSS &&
        g.phase == PHASE_FIGHT && g.boss_hp > 0) return 0;

    /*  当前房间还有小怪：不能传送（门贴图仍然是门）  */
    if (game_mobs_alive() > 0) return 0;

    return game_room_door_exists(dir);
}
/*  看周围 4 格的类型决定门贴图：
 *    · 自己是特殊房 -> 自己的特殊门；
 *    · 否则邻居是特殊房 -> 邻居的特殊门；
 *    · 其余 -> normal。  */
int game_room_door_asset(int dir)
{
    int d = dir & 3;
    int nb, cur, dst;

    if (s_room < 0 || s_room >= ROOM_COUNT)
        return ASSET_background_door_normal;

    nb = room_neighbor(s_room, d);
    cur = room_special_door((int)s_room_def[s_room].type);
    if (cur >= 0) return cur;

    if (nb >= 0 && s_room_def[nb].type != ROOM_NONE) {
        dst = room_special_door((int)s_room_def[nb].type);
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
        int type = (int)s_room_def[i].type;

        if (type == ROOM_NONE) continue;
        if (type == ROOM_START) ++start;
        else if (type == ROOM_ITEM) ++item;
        else if (type == ROOM_SHOP) ++shop;
        else if (type == ROOM_BOSS &&
                 s_room_def[i].w == 2 && s_room_def[i].h == 2 &&
                 s_room_def[i].owner < 0) ++boss;   /* 一条 2×2 房间记录 */
    }
    if (start != 1) ++bad;
    if (item != 1) ++bad;
    if (boss != 1) ++bad;               /* 一条 Boss 房间记录，覆盖 4 格 */
    if (shop != 1) ++bad;

    /*  构造记录：普通房 1×1；Boss 一个 2×2 anchor + 三个 owner 覆盖格。  */
    {
        int anchors = 0, covered = 0;

        for (i = 0; i < ROOM_COUNT; ++i) {
            int t = (int)s_room_def[i].type;

            if (t == ROOM_BOSS) {
                if (s_room_def[i].w == 2 && s_room_def[i].h == 2 &&
                    s_room_def[i].owner < 0) {
                    ++anchors;
                } else if (s_room_def[i].w == 0 && s_room_def[i].h == 0 &&
                           s_room_def[i].owner >= 0 &&
                           s_room_def[s_room_def[i].owner].type == ROOM_BOSS) {
                    ++covered;
                } else {
                    ++bad;
                }
            } else if (t != ROOM_NONE) {
                if (s_room_def[i].w != 1 || s_room_def[i].h != 1 ||
                    s_room_def[i].owner != -1) ++bad;
            }
        }
        if (anchors != 1 || covered != 3) ++bad;
    }

    /*  特殊房之间 8 邻域不相邻  */
    for (i = 0; i < ROOM_COUNT; ++i) {
        if (room_special_door((int)s_room_def[i].type) < 0) continue;
        for (j = i + 1; j < ROOM_COUNT; ++j) {
            int dx, dy;

            if (room_special_door((int)s_room_def[j].type) < 0) continue;
            /*  Boss 2×2 内部四格是同一间房，不互相算“特殊房相邻”。  */
            if (s_room_def[i].type == ROOM_BOSS && s_room_def[j].type == ROOM_BOSS) continue;
            dx = (int)s_room_def[i].gx - (int)s_room_def[j].gx;
            dy = (int)s_room_def[i].gy - (int)s_room_def[j].gy;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            if (dx <= 1 && dy <= 1) ++bad;
        }
    }

    /*  特殊房只能接一间普通房（叶子）  */
    for (i = 0; i < ROOM_COUNT; ++i) {
        int type = (int)s_room_def[i].type;
        int one = -1;

        if (room_special_door(type) < 0) continue;
        if (type == ROOM_BOSS) continue;            /* Boss 是 2×2 整间，不按普通叶子算 */
        if (room_degree(i) != 1) { ++bad; continue; }
        for (d = 0; d < 4; ++d) {
            int nb = room_neighbor(i, d);
            if (nb >= 0 && s_room_def[nb].type != ROOM_NONE) { one = nb; break; }
        }
        if (one < 0 || room_special_door((int)s_room_def[one].type) >= 0) ++bad;
    }

    /*  Boss 房不能直接连起点房  */
    for (i = 0; i < ROOM_COUNT; ++i) {
        if (s_room_def[i].type != ROOM_BOSS) continue;
        for (d = 0; d < 4; ++d) {
            int nb = room_neighbor(i, d);
            if (nb == s_start_room) ++bad;
        }
    }

    /*  普通房可以同时接多间特殊房（每个方向看邻居类型各自渲染门），不再限制  */

    /*  回字形：任何 2×2 四格都是房间（四间两两相邻）都不允许  */
    if (room_has_2x2()) ++bad;

    /*  所有房间必须从起点连通  */
    {
        unsigned char seen[ROOM_COUNT];
        int stack[ROOM_COUNT], top = 0;

        for (i = 0; i < ROOM_COUNT; ++i) seen[i] = 0;
        if (s_start_room >= 0 && s_start_room < ROOM_COUNT &&
            s_room_def[s_start_room].type != ROOM_NONE) {
            seen[s_start_room] = 1;
            stack[top++] = s_start_room;
            while (top > 0) {
                int c = stack[--top];
                for (d = 0; d < 4; ++d) {
                    int nb = room_neighbor(c, d);
                    if (nb >= 0 && s_room_def[nb].type != ROOM_NONE && !seen[nb]) {
                        seen[nb] = 1;
                        stack[top++] = nb;
                    }
                }
            }
        }
        for (i = 0; i < ROOM_COUNT; ++i)
            if (s_room_def[i].type != ROOM_NONE && !seen[i]) ++bad;
    }

    return bad;
}

/*  剧情跳转（Boss 击破后回起点）：target = 房间格子编号。  */
/*  调试传送：按房间类型找一间房（网格里第一个命中）  */
int game_room_find_type(int type)
{
    int i;

    for (i = 0; i < ROOM_COUNT; ++i)
        if (s_room_def[i].type == type) return i;
    return -1;
}

void game_room_request(int target, int entry_dir)
{
    if (target < 0 || target >= ROOM_COUNT) return;
    if (s_room_def[target].type == ROOM_NONE) return;

    s_target    = target;
    s_entry_dir = (entry_dir < 0) ? -1 : (entry_dir & 3);
    s_pending   = 1;
}

/*  死亡卡回退：强制整屏重铺，避免把死亡房间的怪/Boss 旧像素带到上一房间。  */
void game_room_request_full(int target, int entry_dir)
{
    if (target < 0 || target >= ROOM_COUNT) return;
    if (s_room_def[target].type == ROOM_NONE) return;

    s_force_full = 1;
    game_room_request(target, entry_dir);
}

void game_room_mark_used(int room)
{
    if (room >= 0 && room < ROOM_COUNT) s_used[room] = 1;
}

int game_room_is_used(int room)
{
    return (room >= 0 && room < ROOM_COUNT) ? (int)s_used[room] : 0;
}

void game_room_mark_spawned(int room)
{
    if (room >= 0 && room < ROOM_COUNT) s_spawned[room] = 1;
}

int game_room_is_spawned(int room)
{
    return (room >= 0 && room < ROOM_COUNT) ? (int)s_spawned[room] : 0;
}

void game_room_clear_spawned(int room)
{
    if (room >= 0 && room < ROOM_COUNT) s_spawned[room] = 0;
}

void game_room_mark_respawn(int room)
{
    if (room >= 0 && room < ROOM_COUNT) s_respawn[room] = 1;
}

int game_room_take_respawn(int room)
{
    if (room < 0 || room >= ROOM_COUNT || !s_respawn[room]) return 0;
    s_respawn[room] = 0;
    return 1;
}

int game_room_is_cleared(int room)
{
    return (room >= 0 && room < ROOM_COUNT) ? (int)s_cleared[room] : 1;
}

/*  房间清理奖励：第一次调用返回 0..5，之后返回 0（只发一次）  */
int game_room_take_reward(void)
{
    int id = game_room_id();

    if (id < 0 || id >= ROOM_COUNT) return 0;
    if (s_cleared[id]) return 0;
    s_cleared[id] = 1;
    return (int)s_reward[id];
}

/*  新的一轮：重新生成一片房间，道具/Boss 重新生成  */
void game_room_new_run(void)
{
    int i;

    room_layout_generate();
    s_room = s_start_room;
    s_force_full = 1;                       /* 下一层的地图指针虽然相同，但必须整屏重铺 */
    for (i = 0; i < ROOM_COUNT; ++i) {
        s_used[i] = 0;
        s_spawned[i] = 0;
        s_respawn[i] = 0;
    }
    g.exit_active = 0;
    g.last_room = -1;
    g.last_door = -1;
    g.exit_dirty = 2;                       /* 把旧 exit 擦掉（两块缓冲） */
    g.boss_wait_until = 0u;
    g.boss_drop_pending = 0;                /* 上一关没捡的 Boss 饰品不带进新一关 */
    g.item.active = 0;
    game_mobs_reset();
    game_coins_reset();
    game_web_reset();
    game_stones_reset();
    game_shop_reset();
    game_minimap_reset();                   /* 新一层：地图从初始房间重新点亮 */
    game_minimap_refresh();
    game_vital_refill();                    /* 新一关血量补满，命数保留 */
}

int game_room_id(void)
{
    /*  Boss 占 2×2：四格统一用左上角做规范 id（状态/刷怪/门/回退都用它）。  */
    if (s_room >= 0 && s_room < ROOM_COUNT &&
        s_room_def[s_room].type == ROOM_BOSS) {
        if (s_room_def[s_room].owner >= 0) return s_room_def[s_room].owner;
        if (s_room_def[s_room].w == 2 && s_room_def[s_room].h == 2) return s_room;
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
    int i;

    for (i = 0; i < s_door_count; ++i) {
        int x, y;

        if (s_doors[i].dir != dir) continue;

        x = s_doors[i].cx * TILE + TILE / 2 - (PB_OFF_X + PB_W / 2);
        y = s_doors[i].cy * TILE + TILE / 2 - (PB_OFF_Y + PB_H / 2);

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
    s_force_full = 1;                       /* 下一层的地图指针虽然相同，但必须整屏重铺 */
    s_pending = 0;
    s_lock = 0;
    s_entry_dir = -1;
    for (i = 0; i < ROOM_COUNT; ++i) {
        s_used[i] = 0;
        s_spawned[i] = 0;
        s_respawn[i] = 0;
    }
    game_mobs_reset();
    game_coins_reset();
    game_web_reset();
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

    if (!game_room_door_open(door->dir)) return 0;

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
        int target = room_neighbor(s_room, dir);

        if (target < 0 || s_room_def[target].type == ROOM_NONE) return;

        s_target    = target;
        s_entry_dir = dir ^ 2;
        s_pending   = 1;
    }
}

/*----------------------------------------------------------------------------*/
/*  真正换房：状态 + 门/地图更新。  */
int game_room_commit(void)
{
    static const u32 fb[2] = { FB_C, FB_D };
    const char          (*old_map)[MAP_MAX_COLS + 1] = game_world_map();
    const unsigned char (*old_rot)[MAP_MAX_COLS]     = game_world_rot();
    u32 cmd0;
    int bi;
    int same_layout;

    /*  死亡卡：记录“从哪间房来、上一间房往死亡房间开的门”。
     *  s_entry_dir 是新房间的入口方向，旧房间对应的出口方向是 ^2。  */
    g.last_room = game_room_id();
    g.last_door = (s_entry_dir < 0) ? -1 : ((s_entry_dir ^ 2) & 3);

    s_room = s_target;
    s_pending = 0;
    game_world_select(game_room_id());
    game_minimap_refresh();                 /* 进入新房间：刷新小地图 */

    same_layout = (game_world_map() == old_map) && (game_world_rot() == old_rot);
    if (s_force_full) {
        /*  下一层：交互物 / exit / 旧精灵的 old_* 已被 reset，
         *  不能再走 same_layout 快速路径，否则旧像素没人擦。  */
        same_layout = 0;
        s_force_full = 0;
    }

    doors_build();

    game_hud_invalidate();
    game_vital_invalidate();
    game_item_invalidate();

    game_bullets_clear();
    g.vx8 = 0; g.vy8 = 0; g.rx = 0; g.ry = 0;
    if (s_entry_dir < 0) {
        g.x = (ARENA_X0 + ARENA_X1) / 2 - PW / 2;
        g.y = (ARENA_Y0 + ARENA_Y1) / 2 - PH / 2;
    } else {
        place_at_door(s_entry_dir);
    }

    s_lock = 1;
    game_camera_snap();                 /* 换房后窗口立即对准新位置，不做过渡 */

    if (same_layout) {
        g.door_dirty = 2;
        game_flow_enter_room();
        return 1;
    }

    {
        int i;
        for (i = 0; i < N_BULLETS; ++i) {
            g.old_bullets[0][i].active = 0;
            g.old_bullets[1][i].active = 0;
        }
        g.old_item[0].active = 0;   g.old_item[1].active = 0;
        g.old_boss[0].active = 0;   g.old_boss[1].active = 0;
        g.old_player[0].active = 0; g.old_player[1].active = 0;
        g.front = 0;
    }

    cmd0 = gpu2d_getCmdDone();
    gpu2d_exclusive_begin();
    for (bi = 0; bi < 2; ++bi) game_draw_floor(fb[bi]);
    lineBuffer_setFullBase(game_view_fb(FB_C), game_view_fb(FB_D));
    if (gpu2d_exclusive_end(cmd0 + 2u * game_floor_cmd_count()) != 0) return 0;
    if (!game_present(FB_C)) return 0;
    game_camera_synced();               /* 整屏已重铺，HUD 也在新位置画好了 */

    g.door_dirty = 0;
    game_flow_enter_room();
    return 1;
}
