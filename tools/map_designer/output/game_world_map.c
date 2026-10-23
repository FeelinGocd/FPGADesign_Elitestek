/* ==========================================================================
 *  game_world_map.c —— 房间地图数据（自动生成，别手改）
 *  -------------------------------------------------------------------------
 *  生成器 : tools/map_designer/serve.py --emit-c
 *  源文件 : tools/map_designer/output/room_map.txt
 *  指纹   : sha256:304de6c5a42411b4695fdb70d55470ed76852f6374a667c659eb4e0139e878a8
 *  模板   : empty（id 1）
 *  统计   : 门 4 · 墙 144 · 地板 299 · 角块 8
 *  门     : N(20,2) W(7,10) E(32,10) S(20,17)
 *  对象   : 史莱姆×2 甲虫×1 蜘蛛网×2 石头×2 木箱×1 板条箱×1 出口×1
 *  -------------------------------------------------------------------------
 *  用法：把两张地图表 + room_objects 整体替换进 game_world.c，或者
 *        先把 MAP_STORAGE_ROWS/COLS 定义成 MAP_MAX_ROWS/COLS，再
 *        #include "game_world_map.c"。后一种做法让普通房地图和对象层
 *        始终来自同一个文件。
 * ========================================================================== */

#ifndef GAME_WORLD_MAP_DATA_C
#define GAME_WORLD_MAP_DATA_C

#ifndef MAP_COLS
#define MAP_COLS 40
#endif
#ifndef MAP_ROWS
#define MAP_ROWS 23
#endif

#ifndef MAP_STORAGE_ROWS
#define MAP_STORAGE_ROWS MAP_ROWS
#endif
#ifndef MAP_STORAGE_COLS
#define MAP_STORAGE_COLS MAP_COLS
#endif

#define ROOM_MAP_SHA256 "304de6c5a42411b4695fdb70d55470ed76852f6374a667c659eb4e0139e878a8"
#define MAP_TEMPLATE_ROOM 1   /* empty */
#define MAP_TEMPLATE_NAME "empty"

#ifndef MAP_OBJ_SLIME
#define MAP_OBJ_SLIME 0
#endif
#ifndef MAP_OBJ_BUG
#define MAP_OBJ_BUG 1
#endif
#ifndef MAP_OBJ_WEB
#define MAP_OBJ_WEB 2
#endif
#ifndef MAP_OBJ_STONE
#define MAP_OBJ_STONE 3
#endif
#ifndef MAP_OBJ_BOX
#define MAP_OBJ_BOX 4
#endif
#ifndef MAP_OBJ_CRATE
#define MAP_OBJ_CRATE 5
#endif
#ifndef MAP_OBJ_EXIT
#define MAP_OBJ_EXIT 6
#endif

static const char room_map_shared[MAP_STORAGE_ROWS][MAP_STORAGE_COLS + 1] = {
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

static const unsigned char room_rot_shared[MAP_STORAGE_ROWS][MAP_STORAGE_COLS] = {
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 3, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 3, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 3, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 3, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 3, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 3, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 3, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 3, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 3, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 3, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 3, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 3, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 3, 0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 0, 1, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};

#define ROOM_OBJECT_COUNT 10
/* kind, x, y, arg（坐标是 32 px 格，不是像素） */
static const unsigned char room_objects[ROOM_OBJECT_COUNT ? ROOM_OBJECT_COUNT : 1][4] = {
    { 6, 20, 5, 0 },  /* exit */
    { 4, 17, 6, 0 },  /* box */
    { 2, 15, 7, 0 },  /* web */
    { 0, 12, 8, 0 },  /* slime */
    { 0, 27, 8, 0 },  /* slime */
    { 3, 11, 10, 0 },  /* stone */
    { 3, 29, 10, 0 },  /* stone */
    { 1, 20, 12, 0 },  /* bug */
    { 2, 25, 13, 0 },  /* web */
    { 5, 23, 15, 0 },  /* crate */
};

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(room_map_shared[0]) == MAP_STORAGE_COLS + 1, "room_map row width");
_Static_assert(sizeof(room_rot_shared[0]) == MAP_STORAGE_COLS, "room_rot row width");
#endif

#endif /* GAME_WORLD_MAP_DATA_C */
