//==============================================================================
//  game_world.c —— 场景层：房间地图 / 铺地 / 脏区 / 可见性裁剪
//------------------------------------------------------------------------------
//  干什么      :
//    · s_map[][]  : 房间长什么样（40×23 格，每格 32 px，由 tools/sandbox 画出来）
//    · s_rot[][]  : 每一格的旋转（0..3，×90°）
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

/*  地图：40 列 × 23 行（最后一格高 16 px），来自 tools/sandbox/output/room_map.txt  */
#define ROOM_MAP_DECL

static const char room_map_shared[MAP_MAX_ROWS][MAP_MAX_COLS + 1] = {
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

static const unsigned char room_rot_shared[MAP_MAX_ROWS][MAP_MAX_COLS] = {
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

/*  Boss 大房间：45×35 格 = 1440×1120 px；房间主体 33×25 格 = 1056×800（横长）。
 *  比可见窗口 1280×720 大 ⇒ 显示窗口可以在里面平移（相机）。
 *  外墙 2 格厚；内部地板 46×24 格；四扇门在四边中央。  */
static const char boss_map_shared[MAP_MAX_ROWS][MAP_MAX_COLS + 1] = {
    ".............................................",
    ".............................................",
    ".............................................",
    ".............................................",
    ".............................................",
    "......XWWWWWWWWWWWWWWWDWWWWWWWWWWWWWWWC......",
    "......WXWWWWWWWWWWWWWW.WWWWWWWWWWWWWWCW......",
    "......WWBBBBBBBBBBBBBBBBBBBBBBBBBBBBBWW......",
    "......WWBBBBBBBBBBBBBBBBBBBBBBBBBBBBBWW......",
    "......WWBBBBBBBBBBBBBBBBBBBBBBBBBBBBBWW......",
    "......WWBBBBBBBBBBBBBBBBBBBBBBBBBBBBBWW......",
    "......WWBBBBBBBBBBBBBBBBBBBBBBBBBBBBBWW......",
    "......WWBBBBBBBBBBBBBBBBBBBBBBBBBBBBBWW......",
    "......WWBBBBBBBBBBBBBBBBBBBBBBBBBBBBBWW......",
    "......WWBBBBBBBBBBBBBBBBBBBBBBBBBBBBBWW......",
    "......WWBBBBBBBBBBBBBBBBBBBBBBBBBBBBBWW......",
    "......WWBBBBBBBBBBBBBBBBBBBBBBBBBBBBBWW......",
    "......D.BBBBBBBBBBBBBBBBBBBBBBBBBBBBBD.......",
    "......WWBBBBBBBBBBBBBBBBBBBBBBBBBBBBBWW......",
    "......WWBBBBBBBBBBBBBBBBBBBBBBBBBBBBBWW......",
    "......WWBBBBBBBBBBBBBBBBBBBBBBBBBBBBBWW......",
    "......WWBBBBBBBBBBBBBBBBBBBBBBBBBBBBBWW......",
    "......WWBBBBBBBBBBBBBBBBBBBBBBBBBBBBBWW......",
    "......WWBBBBBBBBBBBBBBBBBBBBBBBBBBBBBWW......",
    "......WWBBBBBBBBBBBBBBBBBBBBBBBBBBBBBWW......",
    "......WWBBBBBBBBBBBBBBBBBBBBBBBBBBBBBWW......",
    "......WWBBBBBBBBBBBBBBBBBBBBBBBBBBBBBWW......",
    "......WWBBBBBBBBBBBBBBBBBBBBBBBBBBBBBWW......",
    "......WZWWWWWWWWWWWWWWDWWWWWWWWWWWWWWYW......",
    "......ZWWWWWWWWWWWWWWW.WWWWWWWWWWWWWWWY......",
    ".............................................",
    ".............................................",
    ".............................................",
    ".............................................",
    "............................................."
};

/*  旋转只给墙和门用（角块靠翻转字符 X/C/Y/Z）；上 0 / 右 1 / 下 2 / 左 3。  */
static const unsigned char boss_rot_shared[MAP_MAX_ROWS][MAP_MAX_COLS] = {
    [ 0] = { [0] = 3, [1] = 3, [2] = 3, [3] = 3, [4] = 3, [5] = 3, [6] = 3, [7] = 3, [37] = 1, [38] = 1, [39] = 1, [40] = 1, [41] = 1, [42] = 1, [43] = 1, [44] = 1 },
    [ 1] = { [0] = 3, [1] = 3, [2] = 3, [3] = 3, [4] = 3, [5] = 3, [6] = 3, [7] = 3, [37] = 1, [38] = 1, [39] = 1, [40] = 1, [41] = 1, [42] = 1, [43] = 1, [44] = 1 },
    [ 2] = { [0] = 3, [1] = 3, [2] = 3, [3] = 3, [4] = 3, [5] = 3, [6] = 3, [7] = 3, [37] = 1, [38] = 1, [39] = 1, [40] = 1, [41] = 1, [42] = 1, [43] = 1, [44] = 1 },
    [ 3] = { [0] = 3, [1] = 3, [2] = 3, [3] = 3, [4] = 3, [5] = 3, [6] = 3, [7] = 3, [37] = 1, [38] = 1, [39] = 1, [40] = 1, [41] = 1, [42] = 1, [43] = 1, [44] = 1 },
    [ 4] = { [0] = 3, [1] = 3, [2] = 3, [3] = 3, [4] = 3, [5] = 3, [6] = 3, [7] = 3, [37] = 1, [38] = 1, [39] = 1, [40] = 1, [41] = 1, [42] = 1, [43] = 1, [44] = 1 },
    [ 5] = { [0] = 3, [1] = 3, [2] = 3, [3] = 3, [4] = 3, [5] = 3, [39] = 1, [40] = 1, [41] = 1, [42] = 1, [43] = 1, [44] = 1 },
    [ 6] = { [0] = 3, [1] = 3, [2] = 3, [3] = 3, [4] = 3, [5] = 3, [6] = 3, [38] = 1, [39] = 1, [40] = 1, [41] = 1, [42] = 1, [43] = 1, [44] = 1 },
    [ 7] = { [0] = 3, [1] = 3, [2] = 3, [3] = 3, [4] = 3, [5] = 3, [6] = 3, [7] = 3, [37] = 1, [38] = 1, [39] = 1, [40] = 1, [41] = 1, [42] = 1, [43] = 1, [44] = 1 },
    [ 8] = { [0] = 3, [1] = 3, [2] = 3, [3] = 3, [4] = 3, [5] = 3, [6] = 3, [7] = 3, [37] = 1, [38] = 1, [39] = 1, [40] = 1, [41] = 1, [42] = 1, [43] = 1, [44] = 1 },
    [ 9] = { [0] = 3, [1] = 3, [2] = 3, [3] = 3, [4] = 3, [5] = 3, [6] = 3, [7] = 3, [37] = 1, [38] = 1, [39] = 1, [40] = 1, [41] = 1, [42] = 1, [43] = 1, [44] = 1 },
    [10] = { [0] = 3, [1] = 3, [2] = 3, [3] = 3, [4] = 3, [5] = 3, [6] = 3, [7] = 3, [37] = 1, [38] = 1, [39] = 1, [40] = 1, [41] = 1, [42] = 1, [43] = 1, [44] = 1 },
    [11] = { [0] = 3, [1] = 3, [2] = 3, [3] = 3, [4] = 3, [5] = 3, [6] = 3, [7] = 3, [37] = 1, [38] = 1, [39] = 1, [40] = 1, [41] = 1, [42] = 1, [43] = 1, [44] = 1 },
    [12] = { [0] = 3, [1] = 3, [2] = 3, [3] = 3, [4] = 3, [5] = 3, [6] = 3, [7] = 3, [37] = 1, [38] = 1, [39] = 1, [40] = 1, [41] = 1, [42] = 1, [43] = 1, [44] = 1 },
    [13] = { [0] = 3, [1] = 3, [2] = 3, [3] = 3, [4] = 3, [5] = 3, [6] = 3, [7] = 3, [37] = 1, [38] = 1, [39] = 1, [40] = 1, [41] = 1, [42] = 1, [43] = 1, [44] = 1 },
    [14] = { [0] = 3, [1] = 3, [2] = 3, [3] = 3, [4] = 3, [5] = 3, [6] = 3, [7] = 3, [37] = 1, [38] = 1, [39] = 1, [40] = 1, [41] = 1, [42] = 1, [43] = 1, [44] = 1 },
    [15] = { [0] = 3, [1] = 3, [2] = 3, [3] = 3, [4] = 3, [5] = 3, [6] = 3, [7] = 3, [37] = 1, [38] = 1, [39] = 1, [40] = 1, [41] = 1, [42] = 1, [43] = 1, [44] = 1 },
    [16] = { [0] = 3, [1] = 3, [2] = 3, [3] = 3, [4] = 3, [5] = 3, [6] = 3, [7] = 3, [37] = 1, [38] = 1, [39] = 1, [40] = 1, [41] = 1, [42] = 1, [43] = 1, [44] = 1 },
    [17] = { [0] = 3, [1] = 3, [2] = 3, [3] = 3, [4] = 3, [5] = 3, [6] = 3, [7] = 3, [37] = 1, [38] = 1, [39] = 1, [40] = 1, [41] = 1, [42] = 1, [43] = 1, [44] = 1 },
    [18] = { [0] = 3, [1] = 3, [2] = 3, [3] = 3, [4] = 3, [5] = 3, [6] = 3, [7] = 3, [37] = 1, [38] = 1, [39] = 1, [40] = 1, [41] = 1, [42] = 1, [43] = 1, [44] = 1 },
    [19] = { [0] = 3, [1] = 3, [2] = 3, [3] = 3, [4] = 3, [5] = 3, [6] = 3, [7] = 3, [37] = 1, [38] = 1, [39] = 1, [40] = 1, [41] = 1, [42] = 1, [43] = 1, [44] = 1 },
    [20] = { [0] = 3, [1] = 3, [2] = 3, [3] = 3, [4] = 3, [5] = 3, [6] = 3, [7] = 3, [37] = 1, [38] = 1, [39] = 1, [40] = 1, [41] = 1, [42] = 1, [43] = 1, [44] = 1 },
    [21] = { [0] = 3, [1] = 3, [2] = 3, [3] = 3, [4] = 3, [5] = 3, [6] = 3, [7] = 3, [37] = 1, [38] = 1, [39] = 1, [40] = 1, [41] = 1, [42] = 1, [43] = 1, [44] = 1 },
    [22] = { [0] = 3, [1] = 3, [2] = 3, [3] = 3, [4] = 3, [5] = 3, [6] = 3, [7] = 3, [37] = 1, [38] = 1, [39] = 1, [40] = 1, [41] = 1, [42] = 1, [43] = 1, [44] = 1 },
    [23] = { [0] = 3, [1] = 3, [2] = 3, [3] = 3, [4] = 3, [5] = 3, [6] = 3, [7] = 3, [37] = 1, [38] = 1, [39] = 1, [40] = 1, [41] = 1, [42] = 1, [43] = 1, [44] = 1 },
    [24] = { [0] = 3, [1] = 3, [2] = 3, [3] = 3, [4] = 3, [5] = 3, [6] = 3, [7] = 3, [37] = 1, [38] = 1, [39] = 1, [40] = 1, [41] = 1, [42] = 1, [43] = 1, [44] = 1 },
    [25] = { [0] = 3, [1] = 3, [2] = 3, [3] = 3, [4] = 3, [5] = 3, [6] = 3, [7] = 3, [37] = 1, [38] = 1, [39] = 1, [40] = 1, [41] = 1, [42] = 1, [43] = 1, [44] = 1 },
    [26] = { [0] = 3, [1] = 3, [2] = 3, [3] = 3, [4] = 3, [5] = 3, [6] = 3, [7] = 3, [37] = 1, [38] = 1, [39] = 1, [40] = 1, [41] = 1, [42] = 1, [43] = 1, [44] = 1 },
    [27] = { [0] = 3, [1] = 3, [2] = 3, [3] = 3, [4] = 3, [5] = 3, [6] = 3, [7] = 3, [37] = 1, [38] = 1, [39] = 1, [40] = 1, [41] = 1, [42] = 1, [43] = 1, [44] = 1 },
    [28] = { [0] = 2, [1] = 2, [2] = 2, [3] = 2, [4] = 2, [5] = 2, [6] = 3, [8] = 2, [9] = 2, [10] = 2, [11] = 2, [12] = 2, [13] = 2, [14] = 2, [15] = 2, [16] = 2, [17] = 2, [18] = 2, [19] = 2, [20] = 2, [21] = 2, [22] = 2, [23] = 2, [24] = 2, [25] = 2, [26] = 2, [27] = 2, [28] = 2, [29] = 2, [30] = 2, [31] = 2, [32] = 2, [33] = 2, [34] = 2, [35] = 2, [36] = 2, [38] = 1, [39] = 2, [40] = 2, [41] = 2, [42] = 2, [43] = 2, [44] = 2 },
    [29] = { [0] = 2, [1] = 2, [2] = 2, [3] = 2, [4] = 2, [5] = 2, [7] = 2, [8] = 2, [9] = 2, [10] = 2, [11] = 2, [12] = 2, [13] = 2, [14] = 2, [15] = 2, [16] = 2, [17] = 2, [18] = 2, [19] = 2, [20] = 2, [21] = 2, [22] = 2, [23] = 2, [24] = 2, [25] = 2, [26] = 2, [27] = 2, [28] = 2, [29] = 2, [30] = 2, [31] = 2, [32] = 2, [33] = 2, [34] = 2, [35] = 2, [36] = 2, [37] = 2, [39] = 2, [40] = 2, [41] = 2, [42] = 2, [43] = 2, [44] = 2 },
    [30] = { [0] = 2, [1] = 2, [2] = 2, [3] = 2, [4] = 2, [5] = 2, [6] = 2, [7] = 2, [8] = 2, [9] = 2, [10] = 2, [11] = 2, [12] = 2, [13] = 2, [14] = 2, [15] = 2, [16] = 2, [17] = 2, [18] = 2, [19] = 2, [20] = 2, [21] = 2, [22] = 2, [23] = 2, [24] = 2, [25] = 2, [26] = 2, [27] = 2, [28] = 2, [29] = 2, [30] = 2, [31] = 2, [32] = 2, [33] = 2, [34] = 2, [35] = 2, [36] = 2, [37] = 2, [38] = 2, [39] = 2, [40] = 2, [41] = 2, [42] = 2, [43] = 2, [44] = 2 },
    [31] = { [0] = 2, [1] = 2, [2] = 2, [3] = 2, [4] = 2, [5] = 2, [6] = 2, [7] = 2, [8] = 2, [9] = 2, [10] = 2, [11] = 2, [12] = 2, [13] = 2, [14] = 2, [15] = 2, [16] = 2, [17] = 2, [18] = 2, [19] = 2, [20] = 2, [21] = 2, [22] = 2, [23] = 2, [24] = 2, [25] = 2, [26] = 2, [27] = 2, [28] = 2, [29] = 2, [30] = 2, [31] = 2, [32] = 2, [33] = 2, [34] = 2, [35] = 2, [36] = 2, [37] = 2, [38] = 2, [39] = 2, [40] = 2, [41] = 2, [42] = 2, [43] = 2, [44] = 2 },
    [32] = { [0] = 2, [1] = 2, [2] = 2, [3] = 2, [4] = 2, [5] = 2, [6] = 2, [7] = 2, [8] = 2, [9] = 2, [10] = 2, [11] = 2, [12] = 2, [13] = 2, [14] = 2, [15] = 2, [16] = 2, [17] = 2, [18] = 2, [19] = 2, [20] = 2, [21] = 2, [22] = 2, [23] = 2, [24] = 2, [25] = 2, [26] = 2, [27] = 2, [28] = 2, [29] = 2, [30] = 2, [31] = 2, [32] = 2, [33] = 2, [34] = 2, [35] = 2, [36] = 2, [37] = 2, [38] = 2, [39] = 2, [40] = 2, [41] = 2, [42] = 2, [43] = 2, [44] = 2 },
    [33] = { [0] = 2, [1] = 2, [2] = 2, [3] = 2, [4] = 2, [5] = 2, [6] = 2, [7] = 2, [8] = 2, [9] = 2, [10] = 2, [11] = 2, [12] = 2, [13] = 2, [14] = 2, [15] = 2, [16] = 2, [17] = 2, [18] = 2, [19] = 2, [20] = 2, [21] = 2, [22] = 2, [23] = 2, [24] = 2, [25] = 2, [26] = 2, [27] = 2, [28] = 2, [29] = 2, [30] = 2, [31] = 2, [32] = 2, [33] = 2, [34] = 2, [35] = 2, [36] = 2, [37] = 2, [38] = 2, [39] = 2, [40] = 2, [41] = 2, [42] = 2, [43] = 2, [44] = 2 },
    [34] = { [0] = 2, [1] = 2, [2] = 2, [3] = 2, [4] = 2, [5] = 2, [6] = 2, [7] = 2, [8] = 2, [9] = 2, [10] = 2, [11] = 2, [12] = 2, [13] = 2, [14] = 2, [15] = 2, [16] = 2, [17] = 2, [18] = 2, [19] = 2, [20] = 2, [21] = 2, [22] = 2, [23] = 2, [24] = 2, [25] = 2, [26] = 2, [27] = 2, [28] = 2, [29] = 2, [30] = 2, [31] = 2, [32] = 2, [33] = 2, [34] = 2, [35] = 2, [36] = 2, [37] = 2, [38] = 2, [39] = 2, [40] = 2, [41] = 2, [42] = 2, [43] = 2, [44] = 2 },
};







/*  当前房间的地图（普通房 / Boss 大房间由 game_world_select 选）。  */
static const char          (*s_map)[MAP_MAX_COLS + 1] = room_map_shared;
static const unsigned char (*s_rot)[MAP_MAX_COLS]     = room_rot_shared;
static int                  s_map_w = MAP_COLS;
static int                  s_map_h = MAP_ROWS;
static int                  s_is_boss;

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
    int tx, ty;

    (void)room;
    s_is_boss = (game_room_type() == ROOM_BOSS);
    if (s_is_boss) {
        s_map = boss_map_shared;
        s_rot = boss_rot_shared;
        s_map_w = BOSS_MAP_COLS;
        s_map_h = BOSS_MAP_ROWS;
        /* 内部地板：cols 8..36 / rows 7..27 ⇒ x 256..1184, y 224..896（928×672） */
        g_arena_x0 = 256;  g_arena_y0 = 224;
        g_arena_x1 = 1184; g_arena_y1 = 896;
    } else {
        s_map = room_map_shared;
        s_rot = room_rot_shared;
        s_map_w = MAP_COLS;
        s_map_h = MAP_ROWS;
        /* 普通房内部：原 40×23 地图的竞技场 */
        g_arena_x0 = 288; g_arena_y0 = 128;
        g_arena_x1 = 1024; g_arena_y1 = 544;
    }

    s_door_n = 0;
    for (ty = 0; ty < s_map_h; ++ty)
        for (tx = 0; tx < s_map_w; ++tx) {
            if (s_map[ty][tx] != 'D') continue;
            if (s_door_n >= MAX_DOORS) return;
            s_door_tx[s_door_n] = (unsigned char)tx;
            s_door_ty[s_door_n] = (unsigned char)ty;
            ++s_door_n;
        }
}

const char          (*game_world_map(void))[MAP_MAX_COLS + 1] { return s_map; }
const unsigned char (*game_world_rot(void))[MAP_MAX_COLS]     { return s_rot; }
int  game_world_cols(void)    { return s_map_w; }
int  game_world_rows(void)    { return s_map_h; }
/*  ★ 普通房保持原来的可见世界 1280×720（地图 40×23 的最后 16 px 仍按老逻辑裁掉）；
 *    Boss 房才是完整 1440×1120 世界。  */
int  game_world_w(void)       { return s_is_boss ? (s_map_w * TILE) : SCR_W; }
int  game_world_h(void)       { return s_is_boss ? (s_map_h * TILE) : SCR_H; }
int  game_world_is_boss(void) { return s_is_boss; }

/*----------------------------------------------------------------------------*/
/*  这一格是什么素材（'.' = 空，返回 -1）  */
int game_tile_asset(int tx, int ty)
{
    char c;

    if (tx < 0 || tx >= s_map_w || ty < 0 || ty >= s_map_h) return -1;
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

    if (tx < 0 || tx >= s_map_w || ty < 0 || ty >= s_map_h) return 0u;
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

static int is_door(int tx, int ty)
{
    if (tx < 0 || tx >= s_map_w || ty < 0 || ty >= s_map_h) return 0;
    if (s_map[ty][tx] != 'D') return 0;
    /*  ★ 门【贴图】只看邻居格有没有房间，不看里面有没有怪。
     *    有怪 / Boss 活着只影响 game_room_door_open()（能不能传送），
     *    不能把门变成木墙 —— 否则同一间房清怪前后门贴图会跳变。  */
    return game_room_door_exists(s_rot[ty][tx] & 3);
}

/*  这一格是不是某扇门的内半格？
 *  门在 1×2 格里【只写一个 'D'】，另一格地图里是 '.'（编辑器 put() 就是这么存的）。
 *  是的话通过 (*dtx,*dty) 返回门那格的坐标 —— 补墙时走向要和门那格完全一致。  */
static int door_inner_cell(int tx, int ty, int *dtx, int *dty)
{
    int r;

    if (ty > 0 && s_map[ty - 1][tx] == 'D') {          /* 门在上一行：上/下门 */
        r = s_rot[ty - 1][tx] & 3;
        if (r == 0 || r == 2) { *dtx = tx; *dty = ty - 1; return 1; }
    }
    /*  ★ 横向门在贴图上占【D 格 + 右边一格】的墙厚，和门朝向无关：
     *    左门时右格是门洞内半格，右门时右格是门洞外侧的墙皮。
     *    两扇门的右格都可能是 '.'，都必须补成同走向的墙，否则右门
     *    "没有邻居、不画门"时，右格会留下一个 32×32 的黑洞（看起来像
     *    门/窗整体偏了一格）。  */
    if (tx > 0 && s_map[ty][tx - 1] == 'D') {          /* D 在左一列：左门内半格 / 右门外侧墙 */
        r = s_rot[ty][tx - 1] & 3;
        if (r & 1)            { *dtx = tx - 1; *dty = ty; return 1; }
    }
    if (tx + 1 < s_map_w && s_map[ty][tx + 1] == 'D') { /* D 在右一列：右门内半格（本图是 B，留作兜底） */
        r = s_rot[ty][tx + 1] & 3;
        if (r == 1)           { *dtx = tx + 1; *dty = ty; return 1; }
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
    int draw_h;
    int ai = game_tile_asset(tx, ty);
    int dtx, dty;

    if (tx < 0 || tx >= s_map_w || ty < 0 || ty >= s_map_h) return;
    if ((int)y >= game_world_h()) return;
    draw_h = TILE;
    if ((int)y + draw_h > game_world_h()) draw_h = game_world_h() - (int)y;
    c = s_map[ty][tx];

    if (ai < 0) {                                    /* 空：纯黑（门的内半格除外） */
        /*  ★ 门占 1×2 格，地图只写一个 'D'，另一格是 '.'：
         *    有邻居时第二趟 blit_door 会补墙 + 画门；【没邻居时没人画它】⇒
         *    这里必须补成同走向的墙，否则门后面会是一个 32×32 的黑洞。 */
        if (door_inner_cell(tx, ty, &dtx, &dty)) {
            const asset_desc_t *wall = &g_assets[ASSET_minecraft_wood];

            CI_gpu2d_spriteFlip(wall->base_word, wall->stride_word, fb,
                                x, y, TILE, (u32)draw_h, tile_rot(dtx, dty));
            return;
        }
        CI_gpu2d_fillRect(fb, x, y, TILE, (u32)draw_h, 0x00000000u);
        return;
    }
    if (c == 'D') {
        if (game_room_door_exists(s_rot[ty][tx] & 3)) return;  /* 有邻居：第二趟画门 */
        ai = ASSET_minecraft_wood;                             /* 没邻居：当墙画 */
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

/*  一扇门在屏幕上的左上角（输出矩形）。横向门的贴图会旋转 90°，输出宽高互换。
 *  ★ 三种门（normal / accessory / boss）都是 40×64，左右各探进墙 4 px；
 *    位置按【这张门贴图】的实际尺寸算，不能按 32 格写死。  */
static void door_xy(int tx, int ty, int *ox, int *oy)
{
    int r = s_rot[ty][tx] & 3;
    int ai = game_room_door_asset(r);
    const asset_desc_t *door = &g_assets[ai];
    int dw = (int)door->width;
    int dh = (int)door->height;

    if (r & 1) {                                    /* 横向门（贴图旋转 90°） */
        *ox = (r == 1) ? (int)ARENA_X1 : (int)ARENA_X0 - dh;
        *oy = ty * TILE - (dw - TILE) / 2;
    } else {                                        /* 竖直门 */
        *ox = tx * TILE - (dw - TILE) / 2;
        *oy = (r == 0) ? (int)ARENA_Y0 - dh : (int)ARENA_Y1;
    }
}

/*  画一扇门。
 *
 *  ★ 门洞本身还是墙上的 1×2 格（竖直门 32×64；横向门 64×32），但【贴图可以比门洞大】：
 *    现在三种门都是 40×64 —— 左右各探进墙里 4 px，正好把 2 格厚的墙填满，
 *    不用再补以前那 10 px。用哪张门贴图由 game_room_door_asset() 按房间类型决定。
 *    ★ 两格墙贴图先铺满门洞（透明边后面是墙，不是黑洞），门再盖上去。
 */
static void blit_door(u32 fb, int tx, int ty)
{
    int r = s_rot[ty][tx] & 3;
    int ai = game_room_door_asset(r);
    const asset_desc_t *door = &g_assets[ai];
    const asset_desc_t *wall = &g_assets[ASSET_minecraft_wood];
    int horiz = (r & 1) != 0;                       /* 90° / 270° ⇒ 横着 */
    u32 rot = tile_rot(tx, ty);                     /* ★ 和这一格墙完全同一个角度 */
    int x, y;

    /* ① 门洞的两格先铺成墙（和门同一个旋转，接缝才对得上）  */
    CI_gpu2d_spriteFlip(wall->base_word, wall->stride_word, fb,
        (u32)(tx * TILE), (u32)(ty * TILE), TILE, TILE, rot);
    CI_gpu2d_spriteFlip(wall->base_word, wall->stride_word, fb,
        (u32)((horiz ? tx + 1 : tx) * TILE),
        (u32)((horiz ? ty : ty + 1) * TILE), TILE, TILE, rot);

    /* ② 门按贴图尺寸摆：竖直门贴地板的上/下边，横向门贴地板的左/右边  */
    door_xy(tx, ty, &x, &y);

    /*  门有透明边 + 半透明玻璃 ⇒ 走 α 混合（不透明的地方结果和不透明一样）  */
    CI_gpu2d_spriteAlphaRect(door->base_word, door->stride_word, fb,
        (u32)x, (u32)y, door->width, door->height, rot);
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

    CI_gpu2d_fillRect(fb, 0u, 0u, (u32)game_world_w(), (u32)game_world_h(), 0x00000000u);

    for (ty = 0; ty < s_map_h; ++ty)
        for (tx = 0; tx < s_map_w; ++tx)
            blit_cell(fb, tx, ty);

    for (ty = 0; ty < s_map_h; ++ty)                 /* 门最后画 */
        for (tx = 0; tx < s_map_w; ++tx)
            if (is_door(tx, ty)) blit_door(fb, tx, ty);

    /* HUD 固定屏幕位置：game_hud_static 内部自己加相机偏移 */
    game_hud_static(fb);
}

/*----------------------------------------------------------------------------*/
u32 game_floor_cmd_count(void)
{
    /*  手册 §1.1：一条命令 = 2 条 CI；这里返回【命令数】，gpu2d_exclusive_end 里再 ×2。
     *
     *  ★ 取【下界】，绝不多算：
     *    · 地图 40×23 格；
     *    · 4 个 'D' 格：有邻居时 blit_cell 不画（0 条），没邻居时画木墙（1 条）；
     *    · 4 个方向的门：有邻居才画 3 条（2 墙 + α 门），没邻居 0 条；
     *    · 两者相加 = 920 + 2×邻居数，最少 920（0 邻居），最多 928（4 邻居）；
     *    · 静态 HUD = 67 条（标签 38 + 数字 21 + COINS 8）。
     *  所以下界 = 1 + 920 + 67 = 988。实际只会 ≥ 988；少等不会黑屏，
     *  多算一条（像之前 1004）就会一直等不到而黑屏。  */
    return 1u + (u32)(s_map_w * s_map_h) + HUD_LABEL_CMD_COUNT;
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
void game_mark_doors_dirty(unsigned char dirty[GRID_H][GRID_W])
{
    int k;

    for (k = 0; k < s_door_n; ++k) {
        int tx = s_door_tx[k];
        int ty = s_door_ty[k];
        const asset_desc_t *door;
        int r, ai, x, y, ow, oh;

        r = s_rot[ty][tx] & 3;
        ai = game_room_door_asset(r);           /* 这扇门用哪种门贴图 */
        door = &g_assets[ai];

        door_xy(tx, ty, &x, &y);
        ow = (r & 1) ? (int)door->height : (int)door->width;
        oh = (r & 1) ? (int)door->width  : (int)door->height;

        game_mark_dirty(dirty, x, y, ow, oh);
    }
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
        const asset_desc_t *door;
        int r, ai, x, y, ow, oh, x0, x1, y0, y1, rx, ry, need = 0;

        if (!is_door(tx, ty)) continue;

        r = s_rot[ty][tx] & 3;
        ai = game_room_door_asset(r);           /* 这扇门用哪种门贴图 */
        door = &g_assets[ai];

        door_xy(tx, ty, &x, &y);
        ow = (r & 1) ? (int)door->height : (int)door->width;
        oh = (r & 1) ? (int)door->width  : (int)door->height;

        x0 = x / TILE; x1 = (x + ow - 1) / TILE;
        y0 = y / TILE; y1 = (y + oh - 1) / TILE;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 >= GRID_W) x1 = GRID_W - 1;
        if (y1 >= GRID_H) y1 = GRID_H - 1;

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
