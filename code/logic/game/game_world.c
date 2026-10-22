//==============================================================================
//  game_world.c —— 场景层：房间地图 / 铺地 / 脏区 / 可见性裁剪
//------------------------------------------------------------------------------
//  干什么      :
//    · s_map[][]  : 房间长什么样（40×23 格，每格 32 px，由 tools/sandbox 画出来）
//    · s_rot[][]  : 每一格的旋转（0..3，×90°）
//    · game_blit_tile(): 按地图把一格画出来（地板/墙/门/角块/空）
//    · game_draw_floor(): 开机整屏铺一遍（先铺黑，再铺格子，最后铺门）
//    · game_mark_dirty() / game_restore_dirty()：脏区恢复
//    · game_visible() : 硬件没有源裁剪 ⇒ 整块精灵必须在屏内才画
//  上一级是谁  : game/game_main.c（每帧）、game/game_state.c（开机铺地）
//  接口契约    :
//    · 地图字符：. = 空(黑)  B = 基岩地板  W = 木墙  D = 门  C/X/Y/Z = 角块(翻转)
//    · 门是 32×54：竖直占 1 格宽、2 格高；旋转 90°/270° 时横过来
//    · 门必须【最后画】，否则它盖住的那一格（地图里是 "."）会被黑底刷掉
//  为什么这么做:
//    · 房间是"画出来的"，不是算出来的 —— 改布局只改地图，不动代码
//  参考        : docs/spec/软件_驱动与API.md §6.2 / §6.5、tools/sandbox
//==============================================================================
#include "game.h"
#include "CI_gpu2d.h"

/*  地图：40 列 × 23 行（最后一格高 16 px），来自 tools/sandbox/output/room_map.txt  */
#define ROOM_MAP_DECL

static const char room_map_shared[MAP_ROWS][MAP_COLS + 1] = {
    "........................................",
    "........................................",
    ".......XWWWWWWWWWWWWDWWWWWWWWWWWWC......",
    ".......WXWWWWWWWWWWW.WWWWWWWWWWWCW......",
    ".......WWBBBBBBBBBBBBBBBBBBBBBBBWW......",
    ".......WWBBBBBBBBBBBBBBBBBBBBBBBWW......",
    ".......WWBBBBBBBBBBBBBBBBBBBBBBBWW......",
    ".......WWBBBBBBBBBBBBBBBBBBBBBBBWW......",
    ".......WWBBBBBBBBBBBBBBBBBBBBBBBWW......",
    ".......WWBBBBBBBBBBBBBBBBBBBBBBBWW......",
    ".......D.BBBBBBBBBBBBBBBBBBBBBBBD.......",
    ".......WWBBBBBBBBBBBBBBBBBBBBBBBWW......",
    ".......WWBBBBBBBBBBBBBBBBBBBBBBBWW......",
    ".......WWBBBBBBBBBBBBBBBBBBBBBBBWW......",
    ".......WWBBBBBBBBBBBBBBBBBBBBBBBWW......",
    ".......WWBBBBBBBBBBBBBBBBBBBBBBBWW......",
    ".......WWBBBBBBBBBBBBBBBBBBBBBBBWW......",
    ".......WZWWWWWWWWWWWDWWWWWWWWWWWYW......",
    ".......ZWWWWWWWWWWWW.WWWWWWWWWWWWY......",
    "........................................",
    "........................................",
    "........................................",
    "........................................",
};

static const unsigned char room_rot_shared[MAP_ROWS][MAP_COLS] = {
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,3,0,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,0,1,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,0,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
};

/*  4 间房各自的表。
 *  ★ 现在 4 间共用同一张地图；以后单独画，只改这两张指针表，
 *    再往上面加 room_map_1 / room_map_2 ... 即可。  */
static const char (*const room_maps[ROOM_COUNT])[MAP_COLS + 1] = {
    room_map_shared, room_map_shared, room_map_shared, room_map_shared
};
static const unsigned char (*const room_rots[ROOM_COUNT])[MAP_COLS] = {
    room_rot_shared, room_rot_shared, room_rot_shared, room_rot_shared
};

static const char          (*s_map)[MAP_COLS + 1] = room_map_shared;
static const unsigned char (*s_rot)[MAP_COLS]     = room_rot_shared;

void game_world_select(int room)
{
    if (room < 0 || room >= ROOM_COUNT) room = 0;
    s_map = room_maps[room];
    s_rot = room_rots[room];
}

const char          (*game_world_map(void))[MAP_COLS + 1] { return s_map; }
const unsigned char (*game_world_rot(void))[MAP_COLS]     { return s_rot; }

/*----------------------------------------------------------------------------*/
/*  这一格是什么素材（'.' = 空，返回 -1）  */
int game_tile_asset(int tx, int ty)
{
    char c;

    if (tx < 0 || tx >= MAP_COLS || ty < 0 || ty >= MAP_ROWS) return -1;
    c = s_map[ty][tx];

    switch (c) {
    case 'B': return ASSET_minecraft_bedrock;
    case 'W': return ASSET_minecraft_wood;
    case 'D': return ASSET_background_door_normal;
    case 'C': case 'X': case 'Y': case 'Z': return ASSET_background_corner_wood;
    default:  return -1;
    }
}

/*  角度 → 硬件标志  */
static u32 rot_flag_of(int r)
{
    /*  ★ 角度和硬件标志的对应关系（按 RTL 推出来的，别再凭感觉改）：
     *    ROT90  = 取【源列】当输出行（顺时针 90°）
     *    MX(bit5) 在旋转路里是"从列下端往上读" ⇒ 输出【上下翻】
     *    MY(bit6) 让列号往回走            ⇒ 输出【左右翻】
     *  所以：
     *    90°  = ROT90 | MY      （转完再左右翻 = 顺时针 90°）
     *    180° = MX | MY
     *    270° = ROT90 | MX
     */
    static const u32 rot_flag[4] = { 0u,
                                     GPU2D_ROT90 | GPU2D_FLIP_Y,
                                     GPU2D_FLIP_X | GPU2D_FLIP_Y,
                                     GPU2D_ROT90 | GPU2D_FLIP_X };
    return rot_flag[r & 3];
}

/*  这一格的角度。
 *  ★ 左右两列（90° / 270°）要互换，否则木纹方向和拼接对不上。
 *    门和门底下垫的墙都用这个函数 ⇒ 三处纹理方向自然一致。  */
static u32 tile_rot(int tx, int ty)
{
    int r;

    if (tx < 0 || tx >= MAP_COLS || ty < 0 || ty >= MAP_ROWS) return 0u;
    r = (int)s_rot[ty][tx] & 3;
    r = (r == 1) ? 3 : ((r == 3) ? 1 : r);
    return rot_flag_of(r);
}

/*  角块那四个字符 = 翻转（不旋转）  */
static u32 corner_flip(int tx, int ty)
{
    char c = s_map[ty][tx];

    if (c == 'X') return GPU2D_FLIP_X;
    if (c == 'Y') return GPU2D_FLIP_Y;
    if (c == 'Z') return GPU2D_FLIP_X | GPU2D_FLIP_Y;
    return 0u;
}

static u32 corner_transform(int tx, int ty)
{
    /* 素材符号镜像与地图文件旋转都有效，不能只取其中一项。 */
    return corner_flip(tx, ty) | tile_rot(tx, ty);
}

/*  这一格实际多高：最后一行只有 16 px（720 = 22×32 + 16）。
 *  ★ 填 32 会写穿屏幕、污染另一块缓冲 ⇒ 必须按实际高度裁  */
static int cell_h(int ty)
{
    int y = ty * TILE;

    if (y + TILE <= SCR_H) return TILE;
    return (SCR_H > y) ? (SCR_H - y) : 0;
}

static int is_door(int tx, int ty)
{
    if (tx < 0 || tx >= MAP_COLS || ty < 0 || ty >= MAP_ROWS) return 0;
    if (s_map[ty][tx] != 'D') return 0;
    return game_room_door_open(s_rot[ty][tx] & 3);   /* ★ 锁死的门不算门 */
}

/*----------------------------------------------------------------------------*/
/*  画一格（不含门；门单独一趟画在最上面）  */
static void blit_cell(u32 fb, int tx, int ty)
{
    char c;
    const asset_desc_t *a;
    u32 x = (u32)(tx * TILE);
    u32 y = (u32)(ty * TILE);
    int draw_h;
    int ai = game_tile_asset(tx, ty);

    if (tx < 0 || tx >= MAP_COLS || ty < 0 || ty >= MAP_ROWS) return;
    if ((int)y >= SCR_H) return;
    draw_h = TILE;
    if ((int)y + draw_h > SCR_H) draw_h = SCR_H - (int)y;
    c = s_map[ty][tx];

    if (ai < 0) {                                    /* 空：纯黑 */
        CI_gpu2d_fillRect(fb, x, y, TILE, (u32)draw_h, 0x00000000u);
        return;
    }
    if (c == 'D') {
        if (game_room_door_open(s_rot[ty][tx] & 3)) return;   /* 开着的门：第二趟 */
        ai = ASSET_minecraft_wood;                             /* 锁死的门：当墙画 */
        c = 'W';
    }

    a = &g_assets[ai];

    /*  ★ 这些素材全不透明 ⇒ 用 REPLACE，不要 CK：
     *     基岩里有几颗像素被量化到索引 0（键色白），CK 会把它抠掉
     *     ⇒ 旧帧的人物/弹幕就留在原地，看起来像"残留"。  */
    if (c == 'C' || c == 'X' || c == 'Y' || c == 'Z')
        CI_gpu2d_spriteFlip(a->base_word, a->stride_word, fb,
            x, y, a->width, (u32)draw_h, corner_transform(tx, ty));
    else
        CI_gpu2d_spriteFlip(a->base_word, a->stride_word, fb,
            x, y, TILE, (u32)draw_h, tile_rot(tx, ty));
}

/*  画一扇门。
 *  ★ 门图是 32×54，而墙是 2 格（64）厚：门塞不满，差的那 10 px 要先用【同方向的墙】补，
 *    再把门【贴着场地】摆进去：
 *      上门 → 门的底边贴地板顶边   (y = ARENA_Y0 - 54)
 *      下门 → 门的顶边贴地板底边   (y = ARENA_Y1)
 *      左门 → 门的右边贴地板左边   (x = ARENA_X0 - 54)
 *      右门 → 门的左边贴地板右边   (x = ARENA_X1)
 */
static void blit_door(u32 fb, int tx, int ty)
{
    const asset_desc_t *door = &g_assets[ASSET_background_door_normal];
    const asset_desc_t *wall = &g_assets[ASSET_minecraft_wood];
    int r = s_rot[ty][tx] & 3;
    int horiz = (r & 1) != 0;                       /* 90° / 270° ⇒ 横着 */
    u32 rot = tile_rot(tx, ty);                     /* ★ 和这一格墙完全同一个角度 */
    int dh = (int)door->height;                     /* 54 */
    int x = tx * TILE, y = ty * TILE;

    /* ① 门占的 2 格先铺成墙（和门同一个旋转，接缝才对得上）  */
    CI_gpu2d_spriteFlip(wall->base_word, wall->stride_word, fb,
        (u32)x, (u32)y, TILE, TILE, rot);
    CI_gpu2d_spriteFlip(wall->base_word, wall->stride_word, fb,
        (u32)(horiz ? (tx + 1) * TILE : tx * TILE),
        (u32)(horiz ? ty * TILE : (ty + 1) * TILE), TILE, TILE, rot);

    /* ② 门贴着场地摆  */
    if (r == 0)      y = ARENA_Y0 - dh;             /* 上：底边贴地板 */
    else if (r == 2) y = ARENA_Y1;                  /* 下：顶边贴地板 */
    else if (r == 1) x = ARENA_X1;                  /* 右：左边贴地板右边缘 */
    else             x = ARENA_X0 - dh;             /* 左：右边贴地板 */

    /*  门上有半透明玻璃 ⇒ 走 α 混合（不透明的地方结果和不透明一样）  */
    CI_gpu2d_spriteAlphaRect(door->base_word, door->stride_word, fb,
        (u32)x, (u32)y, TILE, (u32)dh, rot);
}

/*----------------------------------------------------------------------------*/
void game_blit_tile(u32 fb, int tx, int ty, int fill_bg)
{
    (void)fill_bg;
    blit_cell(fb, tx, ty);
    if (is_door(tx, ty)) blit_door(fb, tx, ty);
}

/*----------------------------------------------------------------------------*/
void game_draw_floor(u32 fb)
{
    int tx, ty;

    CI_gpu2d_fillRect(fb, 0u, 0u, SCR_W, SCR_H, 0x00000000u);

    for (ty = 0; ty < MAP_ROWS; ++ty)
        for (tx = 0; tx < MAP_COLS; ++tx)
            blit_cell(fb, tx, ty);

    for (ty = 0; ty < MAP_ROWS; ++ty)                /* 门最后画 */
        for (tx = 0; tx < MAP_COLS; ++tx)
            if (is_door(tx, ty)) blit_door(fb, tx, ty);

    game_hud_static(fb);
}

/*----------------------------------------------------------------------------*/
u32 game_floor_cmd_count(void)
{
    return 1u + (u32)(MAP_COLS * MAP_ROWS) + 4u * 3u + HUD_LABEL_CMD_COUNT;
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
            dirty[ty][tx] = 1u;
}

/*----------------------------------------------------------------------------*/
void game_restore_dirty(u32 fb, unsigned char dirty[GRID_H][GRID_W])
{
    int tx, ty;

    for (ty = 0; ty < GRID_H; ++ty)
        for (tx = 0; tx < GRID_W; ++tx)
            if (dirty[ty][tx]) blit_cell(fb, tx, ty);

    /*  门只有 4 扇：只要脏区碰到它的 2 格，就整扇重画  */
    for (ty = 0; ty < MAP_ROWS; ++ty)
        for (tx = 0; tx < MAP_COLS; ++tx) {
            int horiz, tx2, ty2;

            if (!is_door(tx, ty)) continue;
            horiz = (s_rot[ty][tx] & 1) != 0;
            tx2 = horiz ? tx + 1 : tx;
            ty2 = horiz ? ty : ty + 1;

            if ((tx < GRID_W && ty < GRID_H && dirty[ty][tx]) ||
                (tx2 < GRID_W && ty2 < GRID_H && dirty[ty2][tx2]))
                blit_door(fb, tx, ty);
        }
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
int game_visible(int x, int y, int w, int h)
{
    return x >= 0 && y >= 0 && x + w <= SCR_W && y + h <= SCR_H;
}
