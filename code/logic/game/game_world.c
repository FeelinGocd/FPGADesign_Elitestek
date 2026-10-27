//==============================================================================
//  game_world.c —— 场景层：房间地图 / 铺地 / 脏区 / 可见性裁剪
//------------------------------------------------------------------------------
//  干什么      :
//    · g_maps[]   : 房间框架参数（尺寸 / 竞技场 / 门 / 木纹 / 角块），
//                   格子内容由 game_frame.c 生成；异形房才挂表
//    · s_map/s_rot: 异形房的表指针（生成标准矩形房时为 0）
//    · game_blit_tile(): 按地图把一格画出来（地板/墙/门/角块/空）
//    · game_draw_floor(): 开机整屏铺一遍（先铺黑，再铺格子，最后铺门）
//    · game_mark_dirty() / game_restore_dirty()：脏区恢复
//    · game_visible_world() : 硬件没有源裁剪 ⇒ 整块必须落在世界缓冲内，且与相机窗口相交
//    · game_visible_screen(): HUD 用（屏幕坐标）
//  上一级是谁  : game/game_main.c（每帧）、game/game_state.c（开机铺地）
//  接口契约    :
//    · 地图字符：. = 空(黑)  B = 基岩地板  W = 木墙  D = 门  C/X/Y/Z = 角块(翻转)
//    · 门贴图 40×64（门洞 32×64，左右各宽 4 px）：竖直占 1 格宽、2 格高；
//      旋转 90°/270° 时横过来（输出 64×40）。位置按贴图实际尺寸算，见 blit_door
//    · 门在 1×2 格里【只写一个 'D'】，另一格是 "."（编辑器 put() 的约定）；
//      两格都要当墙补：有邻居的门由第二趟 blit_door 补，没邻居的由 blit_cell 补
//      —— 少补一格，墙上就会留一个 32×32 的黑洞
//    · 门必须【最后画】，否则它盖住的那一格会被黑底刷掉
//  为什么这么做:
//    · 房间是"画出来的"，不是算出来的 —— 改布局只改地图，不动代码
//  参考        : docs/spec/软件_驱动与API.md §6.2 / §6.5、tools/sandbox
//==============================================================================
#include "game.h"
#include "CI_gpu2d.h"
#if GAME_BOSS_1X1_TEST
#include "bsp.h"
#endif

/*  房间框架不存表：墙 / 地板 / 门 / 角块由 game_frame.c 按参数生成
 *  （见下面 g_maps[] 与 game_frame.h 的规则说明）。
 *  ★ 两张框架原来占 6370 B（4 张 MAP_MAX 大小的 const 数组），现在几十字节；
 *    真正会变的只有"布局"（game_layout.c 里的对象表），框架全房间共用。
 *  异形房（手画的不规则地形）仍可把表指针填进 world_map_t.map/rot。 */







/*  地图池：一张图一条表项，房间布局（room_layout_t.map）指向这里的下标。
 *  ★ 框架 = 参数：墙/地板/门/角块由 game_frame.c 生成，不再整张表存。
 *    world_w/h 是相机可以移动的世界尺寸；普通房 1280×720
 *    （40×23 格 = 1280×736，最后 16 px 按老逻辑裁掉），Boss 房 1440×1120。 */
static const world_map_t g_maps[] = {
    /* 0 · 1×1 小房间 40×23：竞技场 9..31 / 4..16，四门居中，带 X/C/Z/Y 角块 */
    { 0, 0,
      { MAP_COLS, MAP_ROWS, 9, 4, 31, 16, { 20, 10, 20, 10 }, { 0, 1, 2, 3 }, 1 },
      SCR_W, SCR_H },
    /* 1 · 2×2 大房间（Boss）45×35：竞技场 8..36 / 7..27，纯木墙角 */
    { 0, 0,
      { BOSS_MAP_COLS, BOSS_MAP_ROWS, 8, 7, 36, 27, { 22, 17, 22, 17 }, { 0, 0, 0, 0 }, 0 },
      BOSS_MAP_COLS * TILE, BOSS_MAP_ROWS * TILE },
    /* 2 · 标准隐藏房框架：门 H 在内侧墙格，只占一格，外侧保留墙。
     *   设计器保存的自定义角块/地形由布局 terrain 优先提供。 */
    { 0, 0,
      { MAP_COLS, MAP_ROWS, 9, 4, 31, 16, { 20, 10, 20, 10 }, { 0, 1, 2, 3 }, 1, 'K', 'w', 'H', 'c' },
      SCR_W, SCR_H },
#if GAME_BOSS_1X1_TEST
    /* Same 1x1 frame as map 0, but a distinct floor identity keeps the
     * normal-room DDR background from being reused for the Boss room. */
    { 0, 0,
      { MAP_COLS, MAP_ROWS, 9, 4, 31, 16, { 20, 10, 20, 10 }, { 0, 1, 2, 3 }, 1 },
      SCR_W, SCR_H },
#endif
};

#define MAP_COUNT ((int)(sizeof(g_maps) / sizeof(g_maps[0])))

int game_world_map_key_for_layout(int layout)
{
    const room_layout_t *L = game_layout(layout);
    if (!L) return 0;              /* 没有布局（商店房）：按 map0 兜底 */
#if GAME_BOSS_1X1_TEST
    if (L->map == 1 && !L->terrain) return 3;
#endif
    if (L->terrain) return -1;     /* 异形房（L）：key 不能比，强制重选 */
    if ((int)L->map >= MAP_COUNT) return 0;
    return (int)L->map;
}

int game_world_map_count(void) { return MAP_COUNT; }

const world_map_t *game_world_map_at(int idx)
{
    if (idx < 0 || idx >= MAP_COUNT) return 0;
    return &g_maps[idx];
}

/*  当前房间的地图（由 game_world_select 按布局选）。  */
static const world_map_t   *s_mapinfo = &g_maps[0];
static const char          (*s_map)[MAP_MAX_COLS + 1];   /* 异形表；生成时为 0 */
static const unsigned char (*s_rot)[MAP_MAX_COLS];
static const unsigned char (*s_packed_rot)[LAYOUT_ROT_COLS];
static int                  s_map_w = MAP_COLS;
static int                  s_map_h = MAP_ROWS;
static int                  s_world_w = SCR_W;           /* 相机可移动范围 */
static int                  s_world_h = SCR_H;
static int                  s_is_boss;
static int                  s_map_idx;
static int                  s_terrain;                   /* 1 = 用的是布局自带地形 */
static unsigned char        s_flip;                      /* 异形房镜像：bit0 左右、bit1 上下 */
static int                  s_mx = -1, s_my = -1;        /* 镜像轴（内容外接框中心×2）：x' = s_mx - x */

/*  渲染缓存：16 种地形字符 + 2-bit 旋转 + 2-bit 门朝向，压成 1 byte/格。
 *  正式 RAM 预算默认关闭；需要压测房间切换时可用 GAME_WORLD_CACHE=1。 */
#if GAME_WORLD_CACHE
static unsigned char s_render_cache[MAP_MAX_ROWS][MAP_MAX_COLS];
#endif
static unsigned char s_cache_valid;

static unsigned char render_tile_code(char c)
{
    static const char codes[16] = { '.', 'B', 'W', 'D', 'C', 'X', 'Y', 'Z',
                                    'H', 'K', 'S', 'w', 'c', 'x', 'y', 'z' };
    int i;
    for (i = 0; i < 16; ++i) if (codes[i] == c) return (unsigned char)i;
    return 0u;
}

static char render_tile_char(unsigned char packed)
{
    static const char codes[16] = { '.', 'B', 'W', 'D', 'C', 'X', 'Y', 'Z',
                                    'H', 'K', 'S', 'w', 'c', 'x', 'y', 'z' };
    return codes[packed & 0x0Fu];
}
/* Reuse the existing DDR pixels, not a separate room image. Keep one large
 * terrain identity per buffer; small rooms only invalidate overwritten cells. */
#define FLOOR_BITS ((MAP_MAX_COLS * MAP_MAX_ROWS + 7) / 8)
typedef struct {
    int owner, active;
    unsigned char valid[FLOOR_BITS];
    unsigned char large;
} floor_record_t;
static floor_record_t s_floor[GAME_FB_COUNT];
static u32 s_floor_reused;
u32 game_floor_reused_cells(void) { return s_floor_reused; }

/* Zero is invalid, so metadata lives in BSS instead of a stored data image. */
static int floor_key(void) { return s_map_idx * 4 + (int)s_flip + 1; }
static int floor_buf(u32 fb)
{
    if (fb == FB_C) return 0;
    if (fb == FB_D) return 1;
#if GAME_TRIPLE_BUFFER
    if (fb == FB_THIRD) return 2;
#endif
    return -1;
}

static void floor_bit(int buf, int tx, int ty, int valid)
{
    int n = ty * MAP_MAX_COLS + tx;
    unsigned char mask = (unsigned char)(1u << (n & 7));
    if (buf < 0 || tx < 0 || ty < 0 || tx >= MAP_MAX_COLS || ty >= MAP_MAX_ROWS) return;
    if (valid) s_floor[buf].valid[n >> 3] |= mask;
    else s_floor[buf].valid[n >> 3] &= (unsigned char)~mask;
}

static int floor_valid(int buf, int tx, int ty)
{
    int n = ty * MAP_MAX_COLS + tx;
    return s_floor[buf].owner == floor_key() &&
           (s_floor[buf].valid[n >> 3] & (1u << (n & 7)));
}

static void floor_written(u32 fb, int tx, int ty)
{
    int buf = floor_buf(fb);
    if (buf >= 0) floor_bit(buf, tx, ty, s_floor[buf].owner == floor_key());
}

void game_floor_reset(void)
{
    int b, i;
    for (b = 0; b < GAME_FB_COUNT; ++b) {
        s_floor[b].owner = s_floor[b].active = 0;
        s_floor[b].large = 0;
        for (i = 0; i < FLOOR_BITS; ++i) s_floor[b].valid[i] = 0;
    }
}

void game_floor_prepare_begin(u32 fb)
{
    int b = floor_buf(fb), i;
    int large = s_world_w > SCR_W || s_world_h > SCR_H;
    if (b < 0) return;
    s_floor_reused = 0u;
    if (s_floor[b].owner != floor_key() && (large || !s_floor[b].large)) {
        s_floor[b].owner = floor_key();
        s_floor[b].large = (unsigned char)large;
        for (i = 0; i < FLOOR_BITS; ++i) s_floor[b].valid[i] = 0;
    }
    s_floor[b].active = 0;
}

int game_floor_needs_prepare(u32 fb)
{
    int b = floor_buf(fb);
    return b >= 0 && s_floor[b].active != floor_key();
}
/*  竞技场边界（game.h 的 ARENA_* 宏取这几个变量）  */
int g_arena_x0 = 288, g_arena_y0 = 128, g_arena_x1 = 1024, g_arena_y1 = 544;

/*  4 扇门的位置在地图加载时缓存一次；恢复/标脏不再每次扫 40×23 格。  */
#define MAX_DOORS 8
static unsigned char s_door_tx[MAX_DOORS];
static unsigned char s_door_ty[MAX_DOORS];
static int           s_door_n;

/*  脏格队列：只在 dirty 从 0 变 1 时登记一次。
 *  上限 128；超过就置 overflow，恢复时回退到整表扫描（不会漏格）。
 *  128×2B = 256B，比每帧扫 40×23 格便宜。  */
#ifndef DIRTY_LIST_MAX
#define DIRTY_LIST_MAX 128
#endif
#ifndef GAME_BG_MERGE_MAX_CLEAN_CELLS
#define GAME_BG_MERGE_MAX_CLEAN_CELLS 2
#endif
#ifndef GAME_BG_EXACT_RECT
#define GAME_BG_EXACT_RECT 0
#endif
static int s_dirty_n;
static u32 s_restore_cells;
static u32 s_restore_cmds;
#if GAME_BG_EXACT_RECT
#define BG_RECT_MAX 512
#define BG_RECT_MERGE_PX 1024
typedef struct { short x0, y0, x1, y1; } bg_rect_t;
static bg_rect_t s_bg_rects[BG_RECT_MAX];
static int s_bg_rect_n;
static int s_bg_rect_overflow;

static int bg_rect_area(const bg_rect_t *r)
{
    return (r->x1 - r->x0) * (r->y1 - r->y0);
}

static int bg_rect_mergeable(const bg_rect_t *a, const bg_rect_t *b, bg_rect_t *out)
{
    int area;
    out->x0 = a->x0 < b->x0 ? a->x0 : b->x0;
    out->y0 = a->y0 < b->y0 ? a->y0 : b->y0;
    out->x1 = a->x1 > b->x1 ? a->x1 : b->x1;
    out->y1 = a->y1 > b->y1 ? a->y1 : b->y1;
    area = bg_rect_area(out);
    return area <= bg_rect_area(a) + bg_rect_area(b) + BG_RECT_MERGE_PX;
}

static void bg_rect_add(int x, int y, int w, int h)
{
    bg_rect_t *r;
    int x1 = x + w, y1 = y + h;
    if (s_bg_rect_overflow || w <= 0 || h <= 0) return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > game_world_w()) x1 = game_world_w();
    if (y1 > game_world_h()) y1 = game_world_h();
    if (x1 <= x || y1 <= y) return;
    if (s_bg_rect_n >= BG_RECT_MAX) { s_bg_rect_overflow = 1; return; }
    r = &s_bg_rects[s_bg_rect_n++];
    r->x0 = (short)x; r->y0 = (short)y;
    r->x1 = (short)x1; r->y1 = (short)y1;
}

static int bg_rect_less(const bg_rect_t *a, const bg_rect_t *b)
{
    return a->y0 < b->y0 || (a->y0 == b->y0 && a->x0 < b->x0);
}

static void bg_rect_sift(int root, int end)
{
    while (root * 2 + 1 <= end) {
        int child = root * 2 + 1;
        bg_rect_t tmp;
        if (child < end && bg_rect_less(&s_bg_rects[child], &s_bg_rects[child + 1])) ++child;
        if (!bg_rect_less(&s_bg_rects[root], &s_bg_rects[child])) break;
        tmp = s_bg_rects[root];
        s_bg_rects[root] = s_bg_rects[child];
        s_bg_rects[child] = tmp;
        root = child;
    }
}

static void bg_rect_sort(void)
{
    int i;
    for (i = s_bg_rect_n / 2 - 1; i >= 0; --i) bg_rect_sift(i, s_bg_rect_n - 1);
    for (i = s_bg_rect_n - 1; i > 0; --i) {
        bg_rect_t tmp = s_bg_rects[0];
        s_bg_rects[0] = s_bg_rects[i];
        s_bg_rects[i] = tmp;
        bg_rect_sift(0, i - 1);
    }
}

static void bg_rect_restore(u32 fb)
{
    int i, out = 0;
    bg_rect_sort();
    for (i = 0; i < s_bg_rect_n; ++i) {
        bg_rect_t current = s_bg_rects[i];
        for (;;) {
            int j, best = -1;
            int best_extra = BG_RECT_MERGE_PX + 1;
            bg_rect_t candidate, best_joined;
            int first = out > 24 ? out - 24 : 0;
            for (j = first; j < out; ++j) {
                if (bg_rect_mergeable(&current, &s_bg_rects[j], &candidate)) {
                    int extra = bg_rect_area(&candidate) - bg_rect_area(&current)
                              - bg_rect_area(&s_bg_rects[j]);
                    if (extra < best_extra) {
                        best = j;
                        best_extra = extra;
                        best_joined = candidate;
                    }
                }
            }
            if (best < 0) break;
            current = best_joined;
            s_bg_rects[best] = s_bg_rects[--out];
        }
        s_bg_rects[out++] = current;
    }
    s_bg_rect_n = out;
    for (i = 0; i < s_bg_rect_n; ++i) {
        const bg_rect_t *r = &s_bg_rects[i];
        CI_gpu2d_blitRect(FB_BG, (u32)r->x0, (u32)r->y0,
                          fb, (u32)r->x0, (u32)r->y0,
                          (u32)(r->x1 - r->x0), (u32)(r->y1 - r->y0));
        ++s_restore_cmds;
    }
    s_restore_cells = (u32)s_dirty_n;
}
#endif
static unsigned short s_dirty_cells[DIRTY_LIST_MAX];
static int            s_dirty_overflow;
static int            s_dirty_ready;
static int            s_dirty_min_tx, s_dirty_max_tx;
static int            s_dirty_min_ty, s_dirty_max_ty;
static u32            s_restore_direct_frames;
static u32            s_restore_copy_frames;
static unsigned char  s_background_valid;
static unsigned char  s_background_full;
static void floor_view_cells(int *tx0, int *ty0, int *tx1, int *ty1);
static u32 floor_rows(u32 fb, int row0, int row1);
static u32 floor_draw_doors(u32 fb);
static u32 blit_floor_cell(u32 fb, int tx, int ty);

u32 game_restore_dirty_cells(void)  { return s_restore_cells; }
u32 game_restore_dirty_cmds(void)   { return s_restore_cmds; }
u32 game_restore_direct_frames(void) { return s_restore_direct_frames; }
u32 game_restore_copy_frames(void)   { return s_restore_copy_frames; }

void game_background_invalidate(void)
{
    s_background_valid = 0u;
}

u32 game_background_build(u32 fb)
{
#if GAME_BG_COPY
    int tx, ty;
    u32 cmds = 1u; /* full-map clear */
    (void)fb;
    fb = FB_BG;
    CI_gpu2d_fillRect(fb, 0u, 0u, (u32)game_world_w(), (u32)game_world_h(), 0u);
    s_background_full = 1u;
    for (ty = 0; ty < s_map_h; ++ty)
        for (tx = 0; tx < s_map_w; ++tx)
            cmds += blit_floor_cell(fb, tx, ty);
    cmds += floor_draw_doors(fb);
    s_background_full = 0u;
    s_background_valid = 1u;
    return cmds;
#else
    (void)fb;
    s_background_valid = 0u;
    return 0u;
#endif
}

void game_dirty_begin(void)
{
#if GAME_BG_EXACT_RECT
    s_bg_rect_n = 0;
    s_bg_rect_overflow = 0;
#endif
    s_dirty_n = 0;
    s_dirty_overflow = 0;
    s_dirty_ready = 1;
    s_dirty_min_tx = GRID_W;
    s_dirty_max_tx = -1;
    s_dirty_min_ty = GRID_H;
    s_dirty_max_ty = -1;
    s_restore_cells = 0u;
    s_restore_cmds = 0u;
    s_restore_direct_frames = 0u;
    s_restore_copy_frames = 0u;
}

static void dirty_add(int tx, int ty)
{
    if (tx < s_dirty_min_tx) s_dirty_min_tx = tx;
    if (tx > s_dirty_max_tx) s_dirty_max_tx = tx;
    if (ty < s_dirty_min_ty) s_dirty_min_ty = ty;
    if (ty > s_dirty_max_ty) s_dirty_max_ty = ty;
    if (s_dirty_n < DIRTY_LIST_MAX)
        s_dirty_cells[s_dirty_n++] = (unsigned short)(ty * GRID_W + tx);
    else
        s_dirty_overflow = 1;
}

void game_world_select(int room)
{
    int tx, ty, idx = 0;
    const room_layout_t *L;

    (void)room;
    game_background_invalidate();
    s_cache_valid = 0u;
    s_is_boss = (game_room_type() == ROOM_BOSS);

    /* 布局里写的是地图下标；没有布局（比如商店房）就用池子第 0 张兜底。
     * ★ 异形房（L 形）自带逐格地形，直接用它，不走框架参数。 */
    L = game_layout(game_room_layout());
    s_terrain = (L && L->terrain) ? 1 : 0;
    if (s_terrain) {
        const layout_terrain_t *t = L->terrain;

        s_map = t->tiles;
        s_rot = 0;
        s_packed_rot = t->rot;
        s_map_w = t->cols;
        s_map_h = t->rows;
        g_arena_x0 = (int)t->ax0 * TILE;
        g_arena_y0 = (int)t->ay0 * TILE;
        g_arena_x1 = ((int)t->ax1 + 1) * TILE;
        g_arena_y1 = ((int)t->ay1 + 1) * TILE;
        s_world_w = t->world_w;
        s_world_h = t->world_h;
        s_map_idx = 1000 + (int)L->map;  /* 地图身份与对象方案解耦；同一空间可复用地形 */
        idx = -1;                                /* 标记：不走 g_maps */
        /*  ★ L 房的 4 种形状：地形只存一份，读数时按 flip 翻镜像。
         *    镜像轴取【内容外接框】的中心，不是画布中心：L 的画布 45 宽、内容只有
         *    x6..34，按画布中心翻会把整间房平移 4 格，跑到竞技场外面去。  */
        s_flip = (unsigned char)(game_room_flip() & 3);
        if (s_flip) {                        /* 只有要翻的时候才算镜像轴（Boss 房不翻）  */
            int x0 = s_map_w, x1 = -1, y0 = s_map_h, y1 = -1;

            for (ty = 0; ty < s_map_h; ++ty)
                for (tx = 0; tx < s_map_w; ++tx) {
                    if (s_map[ty][tx] == '.') continue;
                    if (tx < x0) x0 = tx;
                    if (tx > x1) x1 = tx;
                    if (ty < y0) y0 = ty;
                    if (ty > y1) y1 = ty;
                }
            s_mx = x0 + x1;
            s_my = y0 + y1;
        }
    }
    if (!s_terrain) {
        if (L && (int)L->map < MAP_COUNT) idx = (int)L->map;
#if GAME_BOSS_1X1_TEST
        if (s_is_boss) idx = 3;
#endif
    s_map_idx = idx;
        s_mapinfo = &g_maps[idx];
        s_map = s_mapinfo->map;
        s_rot = s_mapinfo->rot;
        s_packed_rot = 0;
        s_map_w = s_mapinfo->frame.cols;
        s_map_h = s_mapinfo->frame.rows;
        s_world_w = s_mapinfo->world_w;
        s_world_h = s_mapinfo->world_h;
        g_arena_x0 = (int)s_mapinfo->frame.ax0 * TILE;
        g_arena_y0 = (int)s_mapinfo->frame.ay0 * TILE;
        g_arena_x1 = ((int)s_mapinfo->frame.ax1 + 1) * TILE;
        g_arena_y1 = ((int)s_mapinfo->frame.ay1 + 1) * TILE;
        s_flip = 0u;                             /* 标准矩形房没有镜像 */
#if GAME_BOSS_1X1_TEST
        if (s_is_boss)
            bsp_printf("BOSS TEST: 1x1 arena, world=%dx%d, camera fixed\r\n",
                       s_world_w, s_world_h);
#endif
    }

    /* Read source geometry once while building both render and door indexes. */
    s_door_n = 0;
    for (ty = 0; ty < s_map_h; ++ty)
        for (tx = 0; tx < s_map_w; ++tx) {
            char dc;
            unsigned char rot;

            if (!s_terrain) {
                /* Standard rooms are generated from frame parameters. Avoid
                 * routing every cell through the public query trio; frame_tile
                 * and frame_rot are pure and the direction is only meaningful
                 * for D/H cells. */
                dc = frame_tile(&s_mapinfo->frame, tx, ty);
                rot = frame_rot(&s_mapinfo->frame, tx, ty);
            } else {
                dc = game_world_tile_at(tx, ty);
                rot = game_world_rot_at(tx, ty);
            }
#if GAME_WORLD_CACHE
            s_render_cache[ty][tx] = (unsigned char)(render_tile_code(dc) |
                ((rot & 3u) << 4) |
                ((dc == 'D' || dc == 'H') ?
                 ((game_world_dir_at(tx, ty) & 3) << 6) : 0u));
#endif
            if ((dc == 'D' || dc == 'H') && s_door_n < MAX_DOORS) {
                s_door_tx[s_door_n] = (unsigned char)tx;
                s_door_ty[s_door_n] = (unsigned char)ty;
                ++s_door_n;
            }
        }
    s_cache_valid = GAME_WORLD_CACHE ? 1u : 0u;
}

int game_world_door_count(void)
{
    return s_door_n;
}

void game_world_door_at(int index, int *tx, int *ty)
{
    if (index < 0 || index >= s_door_n) {
        if (tx) *tx = -1;
        if (ty) *ty = -1;
        return;
    }
    if (tx) *tx = (int)s_door_tx[index];
    if (ty) *ty = (int)s_door_ty[index];
}

/*  ★ 镜像（s_flip）：L 房 4 种形状只存一份地形，读的时候把坐标翻过去。
 *    · 坐标：tx -> cols-1-tx（左右翻）/ ty -> rows-1-ty（上下翻）
 *    · 角块字符：C/X/Y/Z 要跟着换（按 k=0..3 异或 flip）
 *    · 格子角度：翻一个方向 ⇒ 1↔3；两个都翻（180°）⇒ 0↔2 也换
 *    · 门的朝向就是格子角度，所以门的左右朝向自动跟着翻，不用另存一份  */
static const char k_corner_ch[4] = { 'C', 'X', 'Y', 'Z' };
static const unsigned char k_mirror_rot[4][4] = {
    { 0, 1, 2, 3 },     /* 原图 */
    { 0, 3, 2, 1 },     /* 左右翻 */
    { 0, 3, 2, 1 },     /* 上下翻 */
    { 2, 3, 0, 1 }      /* 180° */
};

static char mirror_corner(char c, unsigned char flip)
{
    int k;

    if (c == 'C') k = 0;
    else if (c == 'X') k = 1;
    else if (c == 'Y') k = 2;
    else if (c == 'Z') k = 3;
    else return c;                      /* 地板 / 墙 / 门：镜像不改变字符 */
    return k_corner_ch[k ^ (int)flip];
}

/*  逐格取值：标准房走生成器，异形房走表。越界返回 '.' / 0。  */
char game_world_tile_at(int tx, int ty)
{
    char c;

    if (tx < 0 || ty < 0 || tx >= s_map_w || ty >= s_map_h) return '.';
#if GAME_WORLD_CACHE
    if (s_cache_valid) return render_tile_char(s_render_cache[ty][tx]);
#endif
    if (s_flip & 1u) { tx = s_mx - tx; if (tx < 0 || tx >= s_map_w) return '.'; }
    if (s_flip & 2u) { ty = s_my - ty; if (ty < 0 || ty >= s_map_h) return '.'; }
    if (!s_map) return frame_tile(&s_mapinfo->frame, tx, ty);
    c = s_map[ty][tx];
    return s_flip ? mirror_corner(c, s_flip) : c;
}

static unsigned char world_source_rot(int tx, int ty)
{
    if (s_packed_rot)
        return (unsigned char)((s_packed_rot[ty][tx >> 2] >> ((tx & 3) * 2)) & 3u);
    if (s_rot) return (unsigned char)(s_rot[ty][tx] & 3u);
    return frame_rot(&s_mapinfo->frame, tx, ty);
}

unsigned char game_world_rot_at(int tx, int ty)
{
    unsigned char r;

    if (tx < 0 || ty < 0 || tx >= s_map_w || ty >= s_map_h) return 0;
#if GAME_WORLD_CACHE
    if (s_cache_valid) return (unsigned char)((s_render_cache[ty][tx] >> 4) & 3u);
#endif
    if (s_flip & 1u) { tx = s_mx - tx; if (tx < 0 || tx >= s_map_w) return 0; }
    if (s_flip & 2u) { ty = s_my - ty; if (ty < 0 || ty >= s_map_h) return 0; }
    r = world_source_rot(tx, ty);
    return s_flip ? k_mirror_rot[s_flip & 3][r] : r;
}

/*  这一格的【朝向】（0 上 / 1 右 / 2 下 / 3 左），门用它找邻居。
 *  ★ 和 rot_at 分开：rot_at 答的是"贴图怎么转"，这里答的是"朝哪一边"。
 *    只翻一个方向时两者一致；【上下翻】时不一样 —— 180° 的贴图旋转 ≠ 朝向上下互换。
 *    混用会让门指到错的那一格（缺口左上那种摆法最先炸）。  */
int game_world_dir_at(int tx, int ty)
{
    int d;

    if (tx < 0 || ty < 0 || tx >= s_map_w || ty >= s_map_h) return 0;
#if GAME_WORLD_CACHE
    if (s_cache_valid) return (int)((s_render_cache[ty][tx] >> 6) & 3u);
#endif
    if (s_flip & 1u) { tx = s_mx - tx; if (tx < 0 || tx >= s_map_w) return 0; }
    if (s_flip & 2u) { ty = s_my - ty; if (ty < 0 || ty >= s_map_h) return 0; }
    d = (int)world_source_rot(tx, ty);
    if (!s_flip) return d;
    if ((s_flip & 3) == 3) return (d ^ 2) & 3;      /* 180°：上下左右都反 */
    if (s_flip & 1) return (d & 1) ? (d ^ 2) : d;   /* 左右翻：1↔3 */
    return (d & 1) ? d : (d ^ 2);                   /* 上下翻：0↔2 */
}

int  game_world_cols(void)    { return s_map_w; }
int  game_world_rows(void)    { return s_map_h; }
/*  世界像素尺寸 = 地图表里的 world_w/h：普通房 1280×720，Boss 房 1440×1120。
 *  ★ 相机是否跟随只看这个值（比窗口大才跟随），不再有"是不是 Boss 房"的特例。 */
int  game_world_w(void)       { return s_world_w; }
int  game_world_h(void)       { return s_world_h; }
int  game_world_map_index(void) { return s_map_idx; }
int  game_world_is_boss(void) { return s_is_boss; }
int  game_world_terrain(void) { return s_terrain; }   /* 1 = 异形房（逐格地形） */

/*----------------------------------------------------------------------------*/
/*  这一格是什么素材（'.' = 空，返回 -1）  */
int game_tile_asset(int tx, int ty)
{
    char c;

    if (tx < 0 || tx >= s_map_w || ty < 0 || ty >= s_map_h) return -1;
    c = game_world_tile_at(tx, ty);

    switch (c) {
    /*  ★ 2026-10-25：木地板/木墙/墙角换成新画的 `background_*` 三件套
     *    （旧的 minecraft_* 已全部删掉）。规则不变：
     *    地板铺满、墙按木纹角度旋转、墙角走 C/X/Y/Z 翻转。  */
    case 'B': return ASSET_background_floor_wood; /* 普通房地板 = 木地板 */
    case 'K': return ASSET_background_floor_hidden; /* 隐藏房地板 */
    case 'W': return ASSET_background_wall_wood;
    case 'w': return ASSET_background_wall_hidden;  /* 隐藏房墙 */
    case 'D': return ASSET_background_door_normal;
    /*  隐藏门 'H'：未炸开前和隐藏房墙一模一样（玩家看不出、也走不过去）。
     *  真正的开门逻辑（炸弹冲击波 → 记已开 → 换贴图/放行）见 game_room_door_asset_at。 */
    case 'H': return ASSET_background_wall_hidden;
    case 'C': case 'X': case 'Y': case 'Z': return ASSET_background_corner_wood;
    case 'c': case 'x': case 'y': case 'z': return ASSET_background_corner_hidden;
    default:  return -1;
    }
}

/*  角度 → 硬件标志
 *  ★ 硬件事实（RTL + tb_m2loopback 第 11 段逐字节验过）：
 *      裸 ROT90 的映射是 out(x,y) = src(y,x) —— 是【转置 / 沿主对角线镜像】，不是旋转！
 *      顺时针 90° = ROT90 | FLIP_X   （旋转时 PaletteSource 拿 MX 选这一列从哪头取）
 *      逆时针 90° = ROT90 | FLIP_Y   （CmdExec 拿 MY 改列号起点）
 *      180°       = FLIP_X | FLIP_Y
 *  ★ 别再按 CI_gpu2d.h 的注释推「ROT90 是顺时针、旋转屏蔽 MX」：
 *    屏蔽的只是 Decoder 的字内反序，MX 在旋转路径上仍然有效。 */
static u32 rot_flag_of(int r)
{
    static const unsigned char rot_flag[4] = { 0u,
                                     GPU2D_ROT90 | GPU2D_FLIP_X,   /* 顺时针 90° */
                                     GPU2D_FLIP_X | GPU2D_FLIP_Y,  /* 180° */
                                     GPU2D_ROT90 | GPU2D_FLIP_Y }; /* 顺时针 270° */
    return rot_flag[r & 3];
}

/*  这一格的角度（墙 / 门：直接用地图里的 rot）。  */
static u32 tile_rot(int tx, int ty)
{
    if (tx < 0 || tx >= s_map_w || ty < 0 || ty >= s_map_h) return 0u;
    return rot_flag_of((int)game_world_rot_at(tx, ty) & 3);
}

/*  角块：字符 = 翻转（X 左右翻 / Y 上下翻 / Z 两次翻），rot = 顺时针 90°，
 *  顺序是【先按字符翻、再按 rot 顺时针转】—— 和设计器 tools/map_designer 的预览一致。
 *  ★ 字符不是「朝向序号」：以前写成 o^=1 / 3-o / +2，等于把翻转当成了旋转，
 *    rot=0 的 X/Y 会画成别的角（房间右下角看起来就是左下角）。 */
static u32 corner_transform(int tx, int ty)
{
    static const unsigned char corner_flag[4][4] = {
        /*  C                             X                                      Y                                      Z */
        { 0u,                            GPU2D_FLIP_X,                          GPU2D_FLIP_Y,                          GPU2D_FLIP_X | GPU2D_FLIP_Y },
        { GPU2D_ROT90 | GPU2D_FLIP_X,    GPU2D_ROT90 | GPU2D_FLIP_X | GPU2D_FLIP_Y, GPU2D_ROT90,                        GPU2D_ROT90 | GPU2D_FLIP_Y },
        { GPU2D_FLIP_X | GPU2D_FLIP_Y,   GPU2D_FLIP_Y,                          GPU2D_FLIP_X,                          0u },
        { GPU2D_ROT90 | GPU2D_FLIP_Y,    GPU2D_ROT90,                           GPU2D_ROT90 | GPU2D_FLIP_X | GPU2D_FLIP_Y, GPU2D_ROT90 | GPU2D_FLIP_X },
    };
    int r = (int)game_world_rot_at(tx, ty) & 3;
    char c = game_world_tile_at(tx, ty);
    int k = (c == 'X') ? 1 : (c == 'Y') ? 2 : (c == 'Z') ? 3 : 0;

    return corner_flag[r][k];
}

static int is_door(int tx, int ty)
{
    char c;

    if (tx < 0 || tx >= s_map_w || ty < 0 || ty >= s_map_h) return 0;
    c = game_world_tile_at(tx, ty);
    if (c != 'D' && c != 'H') return 0;              /* 'H' = 隐藏门，和 'D' 同级 */
    /*  ★ 门【贴图】只看邻居格有没有房间，不看里面有没有怪。
     *    有怪 / Boss 活着只影响 game_room_door_open()（能不能传送），
     *    不能把门变成木墙 —— 否则同一间房清怪前后门贴图会跳变。
     *  ★ 隐藏门（或通到隐藏房的门）**没炸开之前一律当墙**。  */
    return game_room_door_visible(tx, ty);
}

/*  门在 1×2 格里【只写一个 'D'】，另一格是 '.'（编辑器 put() 的约定）。
 *  原图里那格永远在 D 的 +x（横向门）/ +y（竖直门）；**镜像之后会翻到 -x / -y**。
 *  所以"门洞第二格"必须按当前房间的镜像算，不能写死 +1。  */
static int door_pair_sx(int tx, int ty)
{
    if (game_world_tile_at(tx, ty) == 'H') return 0;   /* 隐藏门只占 1 格 */
    return (game_world_dir_at(tx, ty) & 1) ? ((s_flip & 1u) ? -1 : 1) : 0;
}

static int door_pair_sy(int tx, int ty)
{
    if (game_world_tile_at(tx, ty) == 'H') return 0;
    return (game_world_dir_at(tx, ty) & 1) ? 0 : ((s_flip & 2u) ? -1 : 1);
}

/* A D footprint is two cells, while its opened hidden entrance sprite is one.
 * Select the member that touches floor, including mirrored L-room footprints. */
void game_world_hidden_cell(int tx, int ty, int *hx, int *hy)
{
    static const signed char dx[4] = {0, 1, 0, -1};
    static const signed char dy[4] = {-1, 0, 1, 0};
    int i, k, x, y;
    *hx = tx;
    *hy = ty;
    if (game_world_tile_at(tx, ty) == 'H') return;
    for (i = 0; i < 2; ++i) {
        x = tx + (i ? door_pair_sx(tx, ty) : 0);
        y = ty + (i ? door_pair_sy(tx, ty) : 0);
        for (k = 0; k < 4; ++k) {
            char c = game_world_tile_at(x + dx[k], y + dy[k]);
            if (c == 'B' || c == 'K' || c == 'S') {
                *hx = x;
                *hy = y;
                return;
            }
        }
    }
}

/*  这一格是不是某扇门洞的第二格？
 *  是的话通过 (*dtx,*dty) 返回门那格的坐标 —— 补墙时走向要和门那格完全一致。
 *  ★ 四个邻格都查：镜像会让"第二格"落在 D 的任何一侧。  */
static int door_inner_cell(int tx, int ty, int *dtx, int *dty)
{
    static const signed char nx4[4] = { 0, 1, 0, -1 };
    static const signed char ny4[4] = { -1, 0, 1, 0 };
    int k;

    for (k = 0; k < 4; ++k) {
        int dx = tx + nx4[k];
        int dy = ty + ny4[k];

        {
            char nc = game_world_tile_at(dx, dy);

            if (nc != 'D') continue;                   /* 只有普通门占两格；隐藏门 1 格，没有门洞 */
        }
        if (dx + door_pair_sx(dx, dy) != tx) continue;
        if (dy + door_pair_sy(dx, dy) != ty) continue;
        *dtx = dx;
        *dty = dy;
        return 1;
    }
    return 0;
}

/*----------------------------------------------------------------------------*/
/*  画一格（不含门；门单独一趟画在最上面）  */
static void blit_cell(u32 fb, int tx, int ty)
{
    char c;
    const asset_desc_t *a;
    u32 x = (u32)(tx * TILE);
    u32 y = (u32)(ty * TILE);
    u32 w, rot;
    int draw_h;
    int ai = game_tile_asset(tx, ty);
    int dtx, dty;

    if (tx < 0 || tx >= s_map_w || ty < 0 || ty >= s_map_h) return;
    if ((int)y >= game_world_h()) return;
    floor_written(fb, tx, ty);
    draw_h = TILE;
    if ((int)y + draw_h > game_world_h()) draw_h = game_world_h() - (int)y;
    c = game_world_tile_at(tx, ty);

    if (ai < 0) {                                    /* 空：纯黑（门的内半格除外） */
        /*  ★ 门占 1×2 格，地图只写一个 'D'，另一格是 '.'：
         *    有邻居时第二趟 blit_door 会补墙 + 画门；【没邻居时没人画它】⇒
         *    这里必须补成同走向的墙，否则门后面会是一个 32×32 的黑洞。 */
        if (door_inner_cell(tx, ty, &dtx, &dty)) {
            const asset_desc_t *wall = &g_assets[ASSET_background_wall_wood];

            CI_gpu2d_spriteFlip(wall->base_word, wall->stride_word, fb,
                                x, y, TILE, (u32)draw_h, tile_rot(dtx, dty));
            return;
        }
        CI_gpu2d_fillRect(fb, x, y, TILE, (u32)draw_h, 0x00000000u);
        return;
    }
    if (c == 'D' || c == 'H') {                       /* 'H' = 隐藏门（未炸开时当墙） */
        if (game_room_door_visible(tx, ty)) return;    /* 可见：第二趟画门 */
        /* 不可见：当墙画。隐藏门背后是【隐藏房墙】，普通门背后是木墙。  */
        ai = (c == 'H') ? ASSET_background_wall_hidden : ASSET_background_wall_wood;
        c = 'W';
    }

    a = &g_assets[ai];

    /*  ★ 这些素材全不透明 ⇒ 用 REPLACE，不要 CK：
     *     基岩里有几颗像素被量化到索引 0（键色白），CK 会把它抠掉
     *     ⇒ 旧帧的人物/弹幕就留在原地，看起来像"残留"。  */
    if (c == 'C' || c == 'X' || c == 'Y' || c == 'Z') {
        w = (u32)a->width;
        rot = corner_transform(tx, ty);
    } else {
        w = (u32)TILE;
        rot = tile_rot(tx, ty);
    }
    CI_gpu2d_spriteFlip(a->base_word, a->stride_word, fb, x, y, w, (u32)draw_h, rot);
}

/*  一扇门在屏幕上的【输出矩形】。横向门的贴图会旋转 90°，输出宽高互换。
 *  ★ 三种门（normal / accessory / boss）都是 40×64，左右各探进墙 4 px；
 *    位置按【这张门贴图】的实际尺寸算，不能按 32 格写死。
 *  ★ 画门、标脏、脏区重画三处都用这一份 —— 以前三处各写一遍"宽高互换"，
 *    加一种尺寸不一样的门就要三处同时改。  */
static void door_rect(int tx, int ty, int *ox, int *oy, int *ow, int *oh)
{
    int r = (int)game_world_rot_at(tx, ty);
    int ai = game_room_door_asset_at(tx, ty);
    const asset_desc_t *door = &g_assets[ai];
    int dw = (int)door->width;
    int dh = (int)door->height;

    if (ow) *ow = (r & 1) ? dh : dw;      /* 输出尺寸（横向门宽高互换） */
    if (oh) *oh = (r & 1) ? dw : dh;

    /*  ★ 隐藏门只占 1 格：直接贴在这一格，背后铺的是隐藏房墙（见 blit_door）。
     *    ★ 比例 40×64 的门不能用这个分支：它们是"贴满门洞、左右探进墙里 4 px"。  */
    if (ai == ASSET_background_interact_hidden) {
        int hx, hy;
        game_world_hidden_cell(tx, ty, &hx, &hy);
        *ox = hx * TILE;
        *oy = hy * TILE;
        return;
    }

    /*  ★ 按【门格】算，不要按竞技场边缘算：L 形房的内凹门不在外边缘上，
     *    以前那种写法会把内凹门画到房间边上去（看起来多出一扇门）。  */
    if (r & 1) {                                    /* 横向门（贴图旋转 90°） */
        /*  贴图盖住"门洞两格"：镜像后第二格在左边，贴图就要往左挪一格  */
        *ox = ((door_pair_sx(tx, ty) < 0) ? (tx - 1) : tx) * TILE;
        *oy = ty * TILE - (dw - TILE) / 2;
    } else {                                        /* 竖直门 */
        *ox = tx * TILE - (dw - TILE) / 2;
        *oy = ((door_pair_sy(tx, ty) < 0) ? (ty - 1) : ty) * TILE;
    }
}

/*  画一扇门。
 *
 *  ★ 普通门（normal / accessory / boss）门洞 1×2 格、贴图 40×64（左右各探进墙 4 px）；
 *    隐藏门只有 1 格（32×32），背后铺【隐藏房墙】，不占第二格、也没有门洞。
 *    用哪张门贴图由 game_room_door_asset() 按房间类型决定。
 *    ★ 墙贴图先铺满（透明边后面是墙，不是黑洞），门再盖上去。
 */
static void blit_door(u32 fb, int tx, int ty)
{
    int ai = game_room_door_asset_at(tx, ty);
    int one = (game_world_tile_at(tx, ty) == 'H');
    const asset_desc_t *door = &g_assets[ai];
    const asset_desc_t *wall = &g_assets[one ? ASSET_background_wall_hidden
                                             : ASSET_background_wall_wood];
    u32 rot = tile_rot(tx, ty);                     /* ★ 和这一格墙完全同一个角度 */
    u32 drot = rot_flag_of(game_world_dir_at(tx, ty));  /* 门本身按【朝向】画（镜像后才是同一条门） */
    int x, y;

    /* ① 先铺墙：隐藏门只铺自己这一格（1 格，背后就是隐藏房墙）；
     *    普通门铺满两格门洞（和门同一个旋转，接缝才对得上）。  */
    CI_gpu2d_spriteFlip(wall->base_word, wall->stride_word, fb,
        (u32)(tx * TILE), (u32)(ty * TILE), TILE, TILE, rot);
    if (!one)
        CI_gpu2d_spriteFlip(wall->base_word, wall->stride_word, fb,
            (u32)((tx + door_pair_sx(tx, ty)) * TILE),
            (u32)((ty + door_pair_sy(tx, ty)) * TILE), TILE, TILE, rot);

    /* ② 门按贴图尺寸摆：竖直门贴地板的上/下边，横向门贴地板的左/右边  */
    door_rect(tx, ty, &x, &y, 0, 0);

    /*  门素材只有硬边透明，没有半透明像素。 */
    /* Door assets are hard-edged: transparent pixels use the white key color
     * and opaque pixels are fully opaque. CK preserves the wall underneath
     * without the destination readback required by alpha blending. */
    CI_gpu2d_spriteFlipCK(door->base_word, door->stride_word, fb,
        (u32)x, (u32)y, door->width, door->height, drot);

}

/*----------------------------------------------------------------------------*/
void game_blit_tile(u32 fb, int tx, int ty, int fill_bg)
{
    (void)fill_bg;
    blit_cell(fb, tx, ty);
    if (is_door(tx, ty)) blit_door(fb, tx, ty);
}

/*----------------------------------------------------------------------------*/
/*  整屏铺地专用：'.' 不画（不 fill，省命令和带宽）
 *  门洞内侧那格仍要走 blit_cell()，否则门旁边会露一块黑 */
static u32 blit_floor_cell(u32 fb, int tx, int ty)
{
    int dtx, dty;

    char c = game_world_tile_at(tx, ty);

    if (ty * TILE >= game_world_h()) return 0u;
    floor_written(fb, tx, ty);
    if (c == '.' && !door_inner_cell(tx, ty, &dtx, &dty)) return 0u;
    if ((c == 'D' || c == 'H') && game_room_door_visible(tx, ty)) return 0u;
    blit_cell(fb, tx, ty);
    return 1u;
}

static void floor_view_cells(int *tx0, int *ty0, int *tx1, int *ty1)
{
    int x0 = g.cam_x / TILE;
    int y0 = g.cam_y / TILE;
    int x1 = (g.cam_x + SCR_W - 1) / TILE;
    int y1 = (g.cam_y + SCR_H - 1) / TILE;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= s_map_w) x1 = s_map_w - 1;
    if (y1 >= s_map_h) y1 = s_map_h - 1;
    *tx0 = x0; *ty0 = y0; *tx1 = x1; *ty1 = y1;
}

/* Chunk coordinates cover whole tiles intersecting the current viewport.
 * The camera and selected map must remain fixed throughout a build. */
void game_floor_view_rows(int *ty0, int *ty1)
{
    int tx0, tx1;
    floor_view_cells(&tx0, ty0, &tx1, ty1);
}

static u32 floor_rows(u32 fb, int row0, int row1)
{
    int tx, ty, tx0, ty0, tx1, ty1;
    u32 cmds = 0u;

    floor_view_cells(&tx0, &ty0, &tx1, &ty1);
    if (row0 < ty0) row0 = ty0;
    if (row1 > ty1) row1 = ty1;
    if (row0 > row1 || tx0 > tx1) return 0u;
    for (ty = row0; ty <= row1; ++ty)
        for (tx = tx0; tx <= tx1; ++tx)
            cmds += blit_floor_cell(fb, tx, ty);
    return cmds;
}

u32 game_draw_floor_rows(u32 fb, int row0, int row1)
{
    int tx, ty, tx0, ty0, tx1, ty1;
    int buf = floor_buf(fb);
    u32 cmds = 0u;

    floor_view_cells(&tx0, &ty0, &tx1, &ty1);
    if (row0 < ty0) row0 = ty0;
    if (row1 > ty1) row1 = ty1;
    for (ty = row0; ty <= row1; ++ty)
        for (tx = tx0; tx <= tx1; ++tx) {
            int dtx, dty;
            char c;
            if (buf >= 0 && floor_valid(buf, tx, ty)) {
                ++s_floor_reused;
                continue;
            }
            c = game_world_tile_at(tx, ty);
            if (c == '.' && !door_inner_cell(tx, ty, &dtx, &dty)) {
                int start = tx, y1 = (ty + 1) * TILE;
                /* Merge invalid black cells, rather than clearing whole rows
                 * and destroying valid pixels retained from the large room. */
                do {
                    floor_written(fb, tx, ty);
                    ++tx;
                } while (tx <= tx1 &&
                         !(buf >= 0 && floor_valid(buf, tx, ty)) &&
                         game_world_tile_at(tx, ty) == '.' &&
                         !door_inner_cell(tx, ty, &dtx, &dty));
                if (y1 > s_world_h) y1 = s_world_h;
                CI_gpu2d_fillRect(fb, (u32)(start * TILE), (u32)(ty * TILE),
                                  (u32)((tx - start) * TILE),
                                  (u32)(y1 - ty * TILE), 0u);
                --tx;
                ++cmds;
            } else {
                cmds += blit_floor_cell(fb, tx, ty);
            }
        }
    return cmds;
}

static u32 floor_draw_doors(u32 fb)
{
    int k;
    u32 cmds = 0u;

    /* Doors overlap adjacent cells, so emit them after all terrain chunks. */
    for (k = 0; k < game_world_door_count(); ++k) {
        int dx, dy;
        game_world_door_at(k, &dx, &dy);
        if (is_door(dx, dy) && game_room_door_exists_at(dx, dy)) {
            int ox, oy, ow, oh;
            door_rect(dx, dy, &ox, &oy, &ow, &oh);
            if (s_background_full || game_visible_world(ox, oy, ow, oh)) {
                blit_door(fb, dx, dy);
                cmds += (game_world_tile_at(dx, dy) == 'H') ? 2u : 3u;
            }
        }
    }
    return cmds;
}

u32 game_draw_floor_finish(u32 fb)
{
    u32 cmds = floor_draw_doors(fb);
    game_hud_static(fb);
    s_floor[floor_buf(fb)].active = floor_key();
    return cmds + HUD_LABEL_CMD_COUNT;
}

void game_floor_prepare_frame(u32 fb)
{
    int ty0, ty1;
    game_floor_prepare_begin(fb);
    game_floor_view_rows(&ty0, &ty1);
    game_draw_floor_rows(fb, ty0, ty1);
    floor_draw_doors(fb);
    s_floor[floor_buf(fb)].active = floor_key();
}

void game_draw_floor(u32 fb)
{
    int ty0, ty1, b = floor_buf(fb), i;

    /* Boot/restart discards retained pixels for this buffer. */
    s_floor[b].owner = s_floor[b].active = 0;
    s_floor[b].large = 0;
    for (i = 0; i < FLOOR_BITS; ++i) s_floor[b].valid[i] = 0;
    game_floor_prepare_begin(fb);
    /* Boot/restart still initializes both buffers while display is disabled. */
    CI_gpu2d_fillRect(fb, 0u, 0u, (u32)game_world_w(), (u32)game_world_h(), 0u);
    game_floor_view_rows(&ty0, &ty1);
    floor_rows(fb, ty0, ty1);
    game_draw_floor_finish(fb);
}

/*----------------------------------------------------------------------------*/
u32 game_floor_cmd_count(void)
{
    int tx, ty, tx0, ty0, tx1, ty1;
    u32 cells = 0u;

    /* Count the actual command stream. Door anchors are drawn in the
     * dedicated door pass, while their inner '.' cell may draw a wall. */
    floor_view_cells(&tx0, &ty0, &tx1, &ty1);
    for (ty = ty0; ty <= ty1; ++ty)
        for (tx = tx0; tx <= tx1; ++tx) {
            int dtx, dty;
            char c = game_world_tile_at(tx, ty);
            if (c == '.' && !door_inner_cell(tx, ty, &dtx, &dty)) continue;
            if ((c == 'D' || c == 'H') &&
                game_room_door_visible(tx, ty)) continue;
            ++cells;
        }

    for (tx = 0; tx < game_world_door_count(); ++tx) {
        int dx, dy, ox, oy, ow, oh;
        game_world_door_at(tx, &dx, &dy);
        if (!is_door(dx, dy) || !game_room_door_exists_at(dx, dy)) continue;
        door_rect(dx, dy, &ox, &oy, &ow, &oh);
        if (!game_visible_world(ox, oy, ow, oh)) continue;
        cells += (game_world_tile_at(dx, dy) == 'H') ? 2u : 3u;
    }

    /*  再加 HUD 静态标签的命令数 */
    return 1u + cells + HUD_LABEL_CMD_COUNT;
}

u32 game_background_cmd_count(void)
{
    int tx, ty, doors = 0;
    u32 cells = 0u;

    for (ty = 0; ty < s_map_h; ++ty)
        for (tx = 0; tx < s_map_w; ++tx) {
            int dtx, dty;
            char c = game_world_tile_at(tx, ty);
            if (c == '.' && !door_inner_cell(tx, ty, &dtx, &dty)) continue;
            ++cells;
        }
    for (tx = 0; tx < game_world_door_count(); ++tx) {
        int dx, dy;
        game_world_door_at(tx, &dx, &dy);
        if (is_door(dx, dy) && game_room_door_exists_at(dx, dy))
            doors += (game_world_tile_at(dx, dy) == 'H') ? 2 : 3;
    }
    return 1u + cells + (u32)doors;
}

/* Use the existing dirty queue as transition-only scratch. The caller
 * collects footprints while the old room and both old sprite records exist. */
void game_floor_damage(int buf, dirty_map_t dirty)
{
    int k, tx, ty;
    if (!s_dirty_ready || s_dirty_overflow) {
        for (ty = 0; ty < GRID_H; ++ty)
            for (tx = 0; tx < GRID_W; ++tx)
                if (dirty_get(dirty, tx, ty)) floor_bit(buf, tx, ty, 0);
    } else {
        for (k = 0; k < s_dirty_n; ++k) {
            int code = s_dirty_cells[k];
            tx = code % GRID_W; ty = code / GRID_W;
            if (dirty_get(dirty, tx, ty)) floor_bit(buf, tx, ty, 0);
        }
    }
}

/*----------------------------------------------------------------------------*/
void game_mark_dirty(dirty_map_t dirty, int x, int y, int w, int h)
{
    int tx0 = x / TILE, ty0 = y / TILE;
    int tx1 = (x + w - 1) / TILE, ty1 = (y + h - 1) / TILE;
    int tx, ty;

#if GAME_BG_EXACT_RECT
    bg_rect_add(x, y, w, h);
#endif


    if (tx0 < 0) tx0 = 0;
    if (ty0 < 0) ty0 = 0;
    if (tx1 >= GRID_W) tx1 = GRID_W - 1;
    if (ty1 >= GRID_H) ty1 = GRID_H - 1;

    for (ty = ty0; ty <= ty1; ++ty)
        for (tx = tx0; tx <= tx1; ++tx)
            if (!dirty_get(dirty, tx, ty)) {
                dirty_set(dirty, tx, ty);
                dirty_add(tx, ty);
            }
}

/*----------------------------------------------------------------------------*/
/*  HUD 的脏区：HUD 的 (x,y) 是屏幕坐标，实际画在 fb + cam 处 ⇒ 世界坐标要加相机。 */
void game_mark_hud_dirty(dirty_map_t dirty, int x, int y, int w, int h)
{
    game_mark_dirty(dirty, x + g.cam_x, y + g.cam_y, w, h);
}

/*----------------------------------------------------------------------------*/
/*  换房后把 4 扇门【实际占的矩形】标脏（门开/关 + 门类型都变了，两块缓冲各要重画一次）。
 *  ★ 三种门都是 40×64，比门洞左右各宽 4 px ⇒ 脏区也要带上探进墙里的那一圈，
 *    否则门从开到关时，墙里那 4 px 的旧门框会留下来。
 *  ★ 地图没变时 game_room_commit 不再整屏重铺，门的状态就靠这两帧的脏区更新。  */
/*  门的【清理范围】：画门用 door_rect()，标脏用这个。
 *  ★ 隐藏入口只有 32×32（一格），但它占的位置以前可能画过 40×64 的门
 *    （左右各探进墙 4 px）——只按 32×32 标脏，那圈 4 px 的旧门边没人擦，
 *    屏幕上就留一圈"门的边缘"（2026-10-25 上板抓到）。
 *    所以隐藏入口按【普通门】的尺寸和位置算清理范围，让旁边那几格也跟着重画。  */
static void door_clean_rect(int tx, int ty, int *ox, int *oy, int *ow, int *oh)
{
    int ai = game_room_door_asset_at(tx, ty);

    if (ai != ASSET_background_interact_hidden || game_world_tile_at(tx, ty) == 'H') {
        door_rect(tx, ty, ox, oy, ow, oh);
        return;
    }

    {
        int r = (int)game_world_rot_at(tx, ty);
        const asset_desc_t *nd = &g_assets[ASSET_background_door_normal];
        int dw = (int)nd->width;
        int dh = (int)nd->height;

        if (ow) *ow = (r & 1) ? dh : dw;
        if (oh) *oh = (r & 1) ? dw : dh;

        if (r & 1) {                                /* 横向门 */
            *ox = ((door_pair_sx(tx, ty) < 0) ? (tx - 1) : tx) * TILE;
            *oy = ty * TILE - (dw - TILE) / 2;
        } else {                                    /* 竖直门 */
            *ox = tx * TILE - (dw - TILE) / 2;
            *oy = ((door_pair_sy(tx, ty) < 0) ? (ty - 1) : ty) * TILE;
        }
    }
}

void game_mark_doors_dirty(dirty_map_t dirty)
{
    int k;

    for (k = 0; k < s_door_n; ++k) {
        int x, y, ow, oh;

        door_clean_rect(s_door_tx[k], s_door_ty[k], &x, &y, &ow, &oh);
        game_mark_dirty(dirty, x, y, ow, oh);
    }
}

/*  一扇门的矩形落在脏格网格上的范围 [x0..x1] × [y0..y1]（已夹到 GRID 内）。
 *  ★ 和 game_mark_doors_dirty() 用同一个 door_clean_rect()：标脏和"要不要重画"永远一致。  */
static void door_dirty_range(int tx, int ty, int *x0, int *y0, int *x1, int *y1)
{
    int x, y, ow, oh;

    door_clean_rect(tx, ty, &x, &y, &ow, &oh);
    *x0 = x / TILE;
    *y0 = y / TILE;
    *x1 = (x + ow - 1) / TILE;
    *y1 = (y + oh - 1) / TILE;
    if (*x0 < 0) *x0 = 0;
    if (*y0 < 0) *y0 = 0;
    if (*x1 >= GRID_W) *x1 = GRID_W - 1;
    if (*y1 >= GRID_H) *y1 = GRID_H - 1;
}

static void restore_dirty_direct(u32 fb, dirty_map_t dirty)
{
    int k, tx, ty;
    for (k = 0; k < s_dirty_n; ++k) {
        unsigned short code = s_dirty_cells[k];
        tx = code % GRID_W;
        ty = code / GRID_W;
        if (dirty_get(dirty, tx, ty)) {
            blit_cell(fb, tx, ty);
            ++s_restore_cells;
            ++s_restore_cmds;
        }
    }
    for (k = 0; k < s_door_n; ++k) {
        int x = s_door_tx[k], y = s_door_ty[k];
        int x0, y0, x1, y1, rx, ry, need = 0;
        if (!is_door(x, y)) continue;
        door_dirty_range(x, y, &x0, &y0, &x1, &y1);
        for (ry = y0; ry <= y1 && !need; ++ry)
            for (rx = x0; rx <= x1; ++rx)
                if (dirty_get(dirty, rx, ry)) { need = 1; break; }
        if (need) blit_door(fb, x, y);
    }
}

/*----------------------------------------------------------------------------*/
void game_restore_dirty(u32 fb, dirty_map_t dirty)
{
    int tx, ty, k;

 #if GAME_BG_COPY
    if (s_background_valid) {
        if (s_dirty_ready && !s_dirty_overflow && s_dirty_max_tx < 0)
            return;
        if (s_dirty_ready && !s_dirty_overflow &&
            s_dirty_n <= GAME_BG_DIRECT_MAX_CELLS) {
            ++s_restore_direct_frames;
            restore_dirty_direct(fb, dirty);
            return;
        }
        ++s_restore_copy_frames;
#if GAME_BG_EXACT_RECT
        if (s_dirty_ready && !s_bg_rect_overflow) {
            bg_rect_restore(fb);
            return;
        }
#endif
        /* Restore horizontal runs from the clean DDR background. This keeps
         * the dirty-cell semantics unchanged while reducing one terrain
         * sprite command per cell to one DDR copy per run. */
        {
            int row0 = 0, row1 = game_world_rows() - 1;
            int col0 = 0, col1 = game_world_cols() - 1;
            if (s_dirty_ready && !s_dirty_overflow && s_dirty_max_tx >= 0) {
                row0 = s_dirty_min_ty;
                row1 = s_dirty_max_ty;
                col0 = s_dirty_min_tx;
                col1 = s_dirty_max_tx;
                if (row0 < 0) row0 = 0;
                if (col0 < 0) col0 = 0;
                if (row1 >= game_world_rows()) row1 = game_world_rows() - 1;
                if (col1 >= game_world_cols()) col1 = game_world_cols() - 1;
            }
            for (ty = row0; ty <= row1; ++ty) {
                tx = col0;
                while (tx <= col1) {
                int start, copy_end, dirty_cells, gap;
                while (tx <= col1 && !dirty_get(dirty, tx, ty)) ++tx;
                if (tx > col1) break;
                start = tx;
                while (tx <= col1 && dirty_get(dirty, tx, ty)) ++tx;
                copy_end = tx;
                dirty_cells = copy_end - start;

                /* A short clean gap may be copied between nearby dirty runs.
                 * Restoration precedes current-frame actors, so this
                 * reduces command overhead without leaving stale pixels. */
                for (gap = 0; gap < 2 && copy_end < col1; ++gap) {
                    int q = copy_end;
                    while (q <= col1 && !dirty_get(dirty, q, ty)) ++q;
                    if (q > col1 || q - copy_end > GAME_BG_MERGE_MAX_CLEAN_CELLS) break;
                    copy_end = q;
                    while (copy_end <= col1 && dirty_get(dirty, copy_end, ty)) {
                        ++copy_end;
                        ++dirty_cells;
                    }
                }
                tx = copy_end;
                CI_gpu2d_blitRect(FB_BG,
                                  (u32)(start * TILE), (u32)(ty * TILE),
                                  fb,
                                  (u32)(start * TILE), (u32)(ty * TILE),
                                  (u32)((copy_end - start) * TILE), TILE);
                s_restore_cells += (u32)dirty_cells;
                ++s_restore_cmds;
                }
            }
        }

        /* Doors are part of the clean background and need no special pass. */
        return;
    }
 #endif

    if (!s_dirty_ready || s_dirty_overflow) {
        /* 普通房只扫原来的 40×23；Boss 房才扫 45×35（行为与改大世界之前一致） */
        for (ty = 0; ty < game_world_rows(); ++ty)
            for (tx = 0; tx < game_world_cols(); ++tx)
                if (dirty_get(dirty, tx, ty)) {
                    blit_cell(fb, tx, ty);
                    ++s_restore_cells;
                    ++s_restore_cmds;
                }
    } else {
        for (k = 0; k < s_dirty_n; ++k) {
            unsigned short code = s_dirty_cells[k];

            tx = code % GRID_W;
            ty = code / GRID_W;
            if (dirty_get(dirty, tx, ty)) {
                blit_cell(fb, tx, ty);
                ++s_restore_cells;
                ++s_restore_cmds;
            }
        }
    }

    /*  门只有 4 扇：只要脏区碰到【门的实际矩形】（含 40×64 探进墙的边），就整扇重画  */
    for (k = 0; k < s_door_n; ++k) {
        int tx = s_door_tx[k];
        int ty = s_door_ty[k];
        int x0, y0, x1, y1, rx, ry, need = 0;

        if (!is_door(tx, ty)) continue;

        door_dirty_range(tx, ty, &x0, &y0, &x1, &y1);
        for (ry = y0; ry <= y1 && !need; ++ry)
            for (rx = x0; rx <= x1; ++rx)
                if (dirty_get(dirty, rx, ry)) { need = 1; break; }

        if (need) blit_door(fb, tx, ty);
    }
}

/* The DDR restore path may copy a short clean gap between two dirty runs.
 * Static sprites on that gap must be redrawn too, even though their own
 * tiles were never marked dirty. Expanding horizontally by the merge limit
 * is conservative and covers every pixel the merged copy can overwrite. */
int game_dirty_touches(int x0, int y0, int x1, int y1)
{
    int k;
    int reach = 0;

    if (!s_dirty_ready || s_dirty_overflow) return 1;
#if GAME_BG_COPY
    if (s_background_valid && s_dirty_n > GAME_BG_DIRECT_MAX_CELLS) {
#if GAME_BG_EXACT_RECT
        /* The optional rectangle coalescer can cover a much larger gap. */
        return 1;
#else
        reach = GAME_BG_MERGE_MAX_CLEAN_CELLS;
#endif
    }
#endif

    for (k = 0; k < s_dirty_n; ++k) {
        unsigned short code = s_dirty_cells[k];
        int tx = code % GRID_W;
        int ty = code / GRID_W;

        if (tx >= x0 - reach && tx < x1 + reach &&
            ty >= y0 && ty < y1) return 1;
    }
    return 0;
}

/*----------------------------------------------------------------------------*/
void game_restore_rect(u32 fb, int x, int y, int w, int h)
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
            game_blit_tile(fb, tx, ty, 1);
}

/*----------------------------------------------------------------------------*/
/*  世界坐标的可见性（场景 / 实体 / 子弹 / 怪）
 *    两条都要过：
 *      ① 整块落在【世界缓冲】内 —— 硬件没有源裁剪，越界写会毁相邻内存；
 *      ② 与【相机窗口】相交 —— 不相交就不下发命令（大房间里这条是省带宽的主力）。
 *    普通房 cam=0、世界=1280×720，等价于改大房间之前的行为。  */
int game_visible_world(int x, int y, int w, int h)
{
    if (x < 0 || y < 0) return 0;
    if (x + w > game_world_w() || y + h > game_world_h()) return 0;
    if (x + w <= g.cam_x || x >= g.cam_x + SCR_W) return 0;
    if (y + h <= g.cam_y || y >= g.cam_y + SCR_H) return 0;
    return 1;
}

/*  屏幕坐标的可见性（HUD）。HUD 画在 game_view_fb(fb) 上、坐标仍是屏幕坐标。  */
int game_visible_screen(int x, int y, int w, int h)
{
    return x >= 0 && y >= 0 && x + w <= SCR_W && y + h <= SCR_H;
}
