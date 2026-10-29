//==============================================================================
//  game_layout.h —— 固定房间布局（房间排布）+ 房间类型表
//------------------------------------------------------------------------------
//  干什么      :
//    · room_layout_t : 一间房"长什么样" = 一组固定对象（怪 / 网 / 石头 / 箱子）的格子坐标
//    · room_kind_t   : 房间类型 = 用哪几张布局 + 门贴图 + 小地图颜色 + 内容规则
//  为什么这么做:
//    · 以前每间房的怪/网/石头都是随机撒的，房间之间没有区别，也没法"设计"一间房；
//      改成固定布局后，加一种房间 = 加一张表（obj_place_t 数组），逻辑代码一行不用动
//    · 布局用"格子坐标 + 32 px"描述，和 tools/map_designer 的对象层同构：
//      设计器里摆好的 (x, y, kind) 可以直接抄成这里的表
//  加一种房间/布局:
//    1) 写 static const obj_place_t L_xxx[]（坐标见 .c 顶部的摆位规矩）；
//    2) 加进某个 room_kind 的 layouts 池（0xFF 结尾）；
//    3) 需要全新对象种类时，加 OBJ_xxx + game_state.c 的 spawn_layout_objects 一个 case
//  参考        : tools/map_designer/README.md
//==============================================================================
#pragma once

#include "type.h"

/*  对象种类：和 tools/map_designer 的 obj kind 一一对应  */
enum { OBJ_SLIME = 0, OBJ_BUG, OBJ_WEB, OBJ_STONE, OBJ_BOX, OBJ_CRATE };

/*  一个固定对象：格子坐标（40×23，32 px/格）；param 留给具体种类扩展  */
typedef struct {
    unsigned char x, y, kind, param;
} obj_place_t;

/*  异形房自带的逐格地形（标准矩形房不用，直接走 game_world.c 的 g_maps[]）。
 *  尺寸上限和 game.h 的 MAP_MAX_* 一致（game.h 里有 static assert 对着）。  */
#define LAYOUT_MAX_COLS 45
#define LAYOUT_MAX_ROWS 35
#define LAYOUT_ROT_COLS ((LAYOUT_MAX_COLS + 3) / 4)

typedef struct {
    const char          (*tiles)[LAYOUT_MAX_COLS + 1];
    const unsigned char (*rot)[LAYOUT_ROT_COLS]; /* four 2-bit rotations/byte */
    unsigned char cols, rows;           /* 画布格数 */
    short ax0, ay0, ax1, ay1;           /* 竞技场格坐标（含边界） */
    short world_w, world_h;             /* 世界像素尺寸（相机范围） */
    unsigned char mirror_x, mirror_y;   /* 内容外接框 min+max，与地形镜像轴一致 */
    const unsigned short *tile_rows;    /* shared row indexes; 0 = direct legacy rows */
    const unsigned short *rot_rows;
} layout_terrain_t;

static inline char game_layout_tile_at(const layout_terrain_t *t, int x, int y)
{
    return t->tiles[t->tile_rows ? t->tile_rows[y] : y][x];
}

typedef struct {
    const char        *name;
    const obj_place_t *objs;
    unsigned char      n_objs;
    unsigned char      map;             /* 矩形：g_maps[] 下标；逐格地图：生成器分配的共享地图身份 */
    const layout_terrain_t *terrain;    /* 非 0 = 指向共享固定地形，多个对象布局可以共用 */
} room_layout_t;

/* 输入标准布局格坐标，输出当前朝向的格坐标；破坏状态仍按标准坐标记录。 */
static inline void game_layout_obj_cell(const room_layout_t *layout, const obj_place_t *obj,
                                        int flip, int *x, int *y)
{
    *x = obj->x;
    *y = obj->y;
    if (!layout->terrain) return;
    if (flip & 1) *x = (int)layout->terrain->mirror_x - *x;
    if (flip & 2) *y = (int)layout->terrain->mirror_y - *y;
}

/*  房间类型标志  */
enum {
    RK_SPECIAL   = 1u << 0,   /* 特殊房：特殊门贴图 + 独立小地图颜色 + 生成时按叶子规则 */
    RK_FIGHT     = 1u << 1,   /* 进房即战斗（Boss） */
    RK_NO_MOB    = 1u << 2,   /* 不刷小怪 */
    RK_NO_WEB    = 1u << 3,   /* 不刷蜘蛛网 */
    RK_NO_STONE  = 1u << 4,   /* 不刷石头 */
    RK_ITEM_HERE = 1u << 5    /* 进房在房间中心放一个道具（道具房） */
};

typedef struct {
    const char   *name;
    unsigned char flags;
    short         door_asset;   /* 特殊门贴图；-1 = 普通门 */
    unsigned int  color;        /* 小地图颜色 0x00RRGGBB */
    unsigned char layouts[8];   /* 策略参考布局，首项指定空间；实际方案扫描生成表，无 8 张限制 */
    unsigned char n_layouts;
} room_kind_t;

/*  按房间类型取描述（越界/ROOM_NONE 返回 EMPTY 那份，永不返回 NULL）  */
const room_kind_t *game_kind(int room_type);
/*  1 = 特殊房（有特殊门 / 独立颜色），等价于旧 room_special_door(type) >= 0  */
int                game_kind_special(int room_type);
/*  按布局下标取布局；idx < 0 或越界返回 0（没有固定布局，走随机兜底）  */
const room_layout_t *game_layout(int idx);
/*  按 (房间类型, 房间 id) 确定性挑一张布局：同一间房每次进都一样  */
int                game_layout_pick(int room_type, int room_id);
/* 按参考布局的固定空间选对象方案；L 型从左下空缺的同一空间池选择。 */
int                game_layout_pick_like(int prototype, int room_type, int room_id);
