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
    /* 2 · 隐藏房：几何和 0 号**完全一样**，地板/墙/角块/门全换隐藏房皮肤
     *   ★ 这就是"只存贴图种类"：0 字节地形表，几何永远和普通房一致。  */
    { 0, 0,
      { MAP_COLS, MAP_ROWS, 9, 4, 31, 16, { 20, 10, 20, 10 }, { 0, 1, 2, 3 }, 1, 'K', 'w', 'H', 'c' },
      SCR_W, SCR_H },
};

#define MAP_COUNT ((int)(sizeof(g_maps) / sizeof(g_maps[0])))

int game_world_map_key_for_layout(int layout)
{
    const room_layout_t *L = game_layout(layout);
    if (!L) return 0;              /* 没有布局（商店房）：按 map0 兜底 */
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
static int                  s_map_w = MAP_COLS;
static int                  s_map_h = MAP_ROWS;
static int                  s_world_w = SCR_W;           /* 相机可移动范围 */
static int                  s_world_h = SCR_H;
static int                  s_is_boss;
static int                  s_map_idx;
static int                  s_terrain;                   /* 1 = 用的是布局自带地形 */
static unsigned char        s_flip;                      /* 异形房镜像：bit0 左右、bit1 上下 */
static int                  s_mx = -1, s_my = -1;        /* 镜像轴（内容外接框中心×2）：x' = s_mx - x */

/*  渲染缓存：每格的字符 / 贴图角度 / 门朝向只算一次；blit 时直接读数组，不再查表+镜像  */
typedef struct {
    char tile;
    unsigned char rot_dir; /* bit0..1 = rotation, bit2..3 = door direction */
} render_cell_t;
static render_cell_t s_render_cache[MAP_MAX_ROWS][MAP_MAX_COLS];
static unsigned char s_cache_valid;
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
#define DIRTY_LIST_MAX 128
static unsigned short s_dirty_cells[DIRTY_LIST_MAX];
static int            s_dirty_n;
static int            s_dirty_overflow;
static int            s_dirty_ready;

void game_dirty_begin(void)
{
    s_dirty_n = 0;
    s_dirty_overflow = 0;
    s_dirty_ready = 1;
}

static void dirty_add(int tx, int ty)
{
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
    s_cache_valid = 0u;
    s_is_boss = (game_room_type() == ROOM_BOSS);

    /* 布局里写的是地图下标；没有布局（比如商店房）就用池子第 0 张兜底。
     * ★ 异形房（L 形）自带逐格地形，直接用它，不走框架参数。 */
    L = game_layout(game_room_layout());
    s_terrain = (L && L->terrain) ? 1 : 0;
    if (s_terrain) {
        const layout_terrain_t *t = L->terrain;

        s_map = t->tiles;
        s_rot = t->rot;
        s_map_w = t->cols;
        s_map_h = t->rows;
        g_arena_x0 = (int)t->ax0 * TILE;
        g_arena_y0 = (int)t->ay0 * TILE;
        g_arena_x1 = ((int)t->ax1 + 1) * TILE;
        g_arena_y1 = ((int)t->ay1 + 1) * TILE;
        s_world_w = t->world_w;
        s_world_h = t->world_h;
        s_map_idx = 1000 + game_room_layout();   /* 同布局判定用，不会和 g_maps 下标撞 */
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
    s_map_idx = idx;
        s_mapinfo = &g_maps[idx];
        s_map = s_mapinfo->map;
        s_rot = s_mapinfo->rot;
        s_map_w = s_mapinfo->frame.cols;
        s_map_h = s_mapinfo->frame.rows;
        s_world_w = s_mapinfo->world_w;
        s_world_h = s_mapinfo->world_h;
        g_arena_x0 = (int)s_mapinfo->frame.ax0 * TILE;
        g_arena_y0 = (int)s_mapinfo->frame.ay0 * TILE;
        g_arena_x1 = ((int)s_mapinfo->frame.ax1 + 1) * TILE;
        g_arena_y1 = ((int)s_mapinfo->frame.ay1 + 1) * TILE;
        s_flip = 0u;                             /* 标准矩形房没有镜像 */
    }

    s_door_n = 0;
    for (ty = 0; ty < s_map_h; ++ty)
        for (tx = 0; tx < s_map_w; ++tx) {
            {
                char dc = game_world_tile_at(tx, ty);

                if (dc != 'D' && dc != 'H') continue;   /* 隐藏门也要进脏区/重画表 */
            }
            if (s_door_n >= MAX_DOORS) return;
            s_door_tx[s_door_n] = (unsigned char)tx;
            s_door_ty[s_door_n] = (unsigned char)ty;
            ++s_door_n;
        }

    for (ty = 0; ty < s_map_h; ++ty)
        for (tx = 0; tx < s_map_w; ++tx) {
            s_render_cache[ty][tx].tile = game_world_tile_at(tx, ty);
            s_render_cache[ty][tx].rot_dir = (unsigned char)(
                (game_world_rot_at(tx, ty) & 3u) |
                ((game_world_dir_at(tx, ty) & 3) << 2));
        }
    s_cache_valid = 1u;
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
    if (s_cache_valid) return s_render_cache[ty][tx].tile;
    if (s_flip & 1u) { tx = s_mx - tx; if (tx < 0 || tx >= s_map_w) return '.'; }
    if (s_flip & 2u) { ty = s_my - ty; if (ty < 0 || ty >= s_map_h) return '.'; }
    if (!s_map) return frame_tile(&s_mapinfo->frame, tx, ty);
    c = s_map[ty][tx];
    return s_flip ? mirror_corner(c, s_flip) : c;
}

unsigned char game_world_rot_at(int tx, int ty)
{
    unsigned char r;

    if (tx < 0 || ty < 0 || tx >= s_map_w || ty >= s_map_h) return 0;
    if (s_cache_valid) return (unsigned char)(s_render_cache[ty][tx].rot_dir & 3u);
    if (s_flip & 1u) { tx = s_mx - tx; if (tx < 0 || tx >= s_map_w) return 0; }
    if (s_flip & 2u) { ty = s_my - ty; if (ty < 0 || ty >= s_map_h) return 0; }
    if (!s_rot) return frame_rot(&s_mapinfo->frame, tx, ty);
    r = (unsigned char)(s_rot[ty][tx] & 3);
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
    if (s_cache_valid) return (int)((s_render_cache[ty][tx].rot_dir >> 2) & 3u);
    if (s_flip & 1u) { tx = s_mx - tx; if (tx < 0 || tx >= s_map_w) return 0; }
    if (s_flip & 2u) { ty = s_my - ty; if (ty < 0 || ty >= s_map_h) return 0; }
    if (!s_rot) d = (int)frame_rot(&s_mapinfo->frame, tx, ty) & 3;
    else        d = (int)(s_rot[ty][tx] & 3);
    if (!s_flip) return d;
    if ((s_flip & 3) == 3) return (d ^ 2) & 3;      /* 180°：上下左右都反 */
    if (s_flip & 1) return (d & 1) ? (d ^ 2) : d;   /* 左右翻：1↔3 */
    return (d & 1) ? d : (d ^ 2);                   /* 上下翻：0↔2 */
}

const char          (*game_world_map(void))[MAP_MAX_COLS + 1] { return s_map; }
const unsigned char (*game_world_rot(void))[MAP_MAX_COLS]     { return s_rot; }
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
    static const u32 rot_flag[4] = { 0u,
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
    static const u32 corner_flag[4][4] = {
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
        *ox = tx * TILE;
        *oy = ty * TILE;
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
    int one = (ai == ASSET_background_interact_hidden);   /* 隐藏门只有 1 格 */
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

    /*  门有透明边 + 半透明玻璃 ⇒ 走 α 混合（不透明的地方结果和不透明一样）  */
    CI_gpu2d_spriteAlphaRect(door->base_word, door->stride_word, fb,
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
static void blit_floor_cell(u32 fb, int tx, int ty)
{
    int dtx, dty;

    if (game_world_tile_at(tx, ty) == '.' &&
        !door_inner_cell(tx, ty, &dtx, &dty)) return;
    blit_cell(fb, tx, ty);
}

void game_draw_floor(u32 fb)
{
    int tx, ty;

    CI_gpu2d_fillRect(fb, 0u, 0u, (u32)game_world_w(), (u32)game_world_h(), 0x00000000u);

    for (ty = 0; ty < s_map_h; ++ty)
        for (tx = 0; tx < s_map_w; ++tx)
            blit_floor_cell(fb, tx, ty);

    /*  门最后画：位置已在 game_world_select() 里缓存，不再扫全图 */
    for (tx = 0; tx < game_world_door_count(); ++tx) {
        int dx, dy;
        game_world_door_at(tx, &dx, &dy);
        if (is_door(dx, dy)) blit_door(fb, dx, dy);
    }

    /* HUD 固定屏幕位置：game_hud_static 内部自己加相机偏移 */
    game_hud_static(fb);
}

/*----------------------------------------------------------------------------*/
u32 game_floor_cmd_count(void)
{
    int tx, ty;
    u32 cells = 0u;

    /*  命令数 = 1 条 fillRect 黑底 + 每个要画的格子 1 条 blit_cell */
    for (ty = 0; ty < s_map_h; ++ty)
        for (tx = 0; tx < s_map_w; ++tx) {
            int dtx, dty;
            if (game_world_tile_at(tx, ty) != '.' ||
                door_inner_cell(tx, ty, &dtx, &dty))
                ++cells;
        }

    /*  再加 HUD 静态标签的命令数 */
    return 1u + cells + HUD_LABEL_CMD_COUNT;
}

/*----------------------------------------------------------------------------*/
void game_mark_dirty(unsigned char dirty[GRID_H][GRID_W], int x, int y, int w, int h)
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
            if (!dirty[ty][tx]) {
                dirty[ty][tx] = 1u;
                dirty_add(tx, ty);
            }
}

/*----------------------------------------------------------------------------*/
/*  HUD 的脏区：HUD 的 (x,y) 是屏幕坐标，实际画在 fb + cam 处 ⇒ 世界坐标要加相机。 */
void game_mark_hud_dirty(unsigned char dirty[GRID_H][GRID_W], int x, int y, int w, int h)
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

    if (ai != ASSET_background_interact_hidden) {
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

void game_mark_doors_dirty(unsigned char dirty[GRID_H][GRID_W])
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

/*----------------------------------------------------------------------------*/
void game_restore_dirty(u32 fb, unsigned char dirty[GRID_H][GRID_W])
{
    int tx, ty, k;

    if (!s_dirty_ready || s_dirty_overflow) {
        /* 普通房只扫原来的 40×23；Boss 房才扫 45×35（行为与改大世界之前一致） */
        for (ty = 0; ty < game_world_rows(); ++ty)
            for (tx = 0; tx < game_world_cols(); ++tx)
                if (dirty[ty][tx]) blit_cell(fb, tx, ty);
    } else {
        for (k = 0; k < s_dirty_n; ++k) {
            unsigned short code = s_dirty_cells[k];

            tx = code % GRID_W;
            ty = code / GRID_W;
            if (dirty[ty][tx]) blit_cell(fb, tx, ty);
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
                if (dirty[ry][rx]) { need = 1; break; }

        if (need) blit_door(fb, tx, ty);
    }
}

/*  脏格队列里有没有格子落在 [x0,x1) × [y0,y1)？
 *  overflow 时返回 1（保守：让 HUD 整面板重画，绝不会漏）。  */
int game_dirty_touches(int x0, int y0, int x1, int y1)
{
    int k;

    if (!s_dirty_ready || s_dirty_overflow) return 1;

    for (k = 0; k < s_dirty_n; ++k) {
        unsigned short code = s_dirty_cells[k];
        int tx = code % GRID_W;
        int ty = code / GRID_W;

        if (tx >= x0 && tx < x1 && ty >= y0 && ty < y1) return 1;
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
