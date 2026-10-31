//==============================================================================
//  game_layout.c —— 房间类型表（哪种房间用哪些布局、门贴图、小地图颜色）
//------------------------------------------------------------------------------
//  干什么      :
//    · 房间【类型】的语义都在这里：flags / 门贴图 / 小地图颜色 / 布局池；
//    · 布局【数据】（L_xxx 对象数组 + g_layouts 表）不在这里 ——
//      由 tools/map_designer 生成到 game_layout_data.c/.h：
//          output/<房间名>.json  →  serve.py --emit-layouts --apply
//      所以"加一间房 = 设计器存一份 <房间名>.json + 重新生成"，不用手抄坐标。
//
//  两层的关系：
//    frame（房间框架，1×1 / 2×2）  ← game_world.c 的 g_maps[]（参数化生成）
//    layout（房内摆位）            ← game_layout_data.c（设计器导出）
//    kind（房间类型）              ← 本文件（纯策略）
//
//  摆位三条规矩（validate 管不到，靠设计器里的约定）：
//    · 竖向门在 x = 20（上/下门），横向门在 y = 10（左/右门）——
//      这两条通道上【不放石头/箱子】，否则人进了门却走不过去；
//    · 房间中心 (20,10) 留空：复位/换房时玩家落在这里；
//    · 竞技场地板是 x 9..31、y 4..16（40×23 框架）。
//    ★ 45×35 的 Boss 房是另一套坐标（竞技场 8..36 / 7..27，门在 22 / 17）。
//==============================================================================
#include "game.h"
#include "game_layout_data.h"

#ifndef GAME_TEST_START_LAYOUT
#define GAME_TEST_START_LAYOUT -1
#endif

/*  房间类型表：下标必须和 game.h 的 ROOM_xxx 枚举一一对应。
 *  布局池里的 LAY_xxx 是生成文件里的宏（按名字排序分配下标）。  */
static const room_kind_t g_kinds[] = {
    /* ROOM_START：起点房，固定 1×1（和普通房共用同一张 1x1 布局） */
    { "start", RK_NO_MOB, -1, 0x00808080u, { LAY_map_1x1_type0, 0xFF, 0xFF, 0xFF }, 1 },
    /* ROOM_EMPTY：普通房 = 房间池（布局池可以有多张，按房间 id 确定性抽） */
    { "empty", 0, -1, 0x00808080u, { LAY_map_1x1_type0, 0xFF, 0xFF, 0xFF }, 1 },
    /* ROOM_ITEM：道具房（不挂固定布局：中心要留空放道具，随机刷小怪兜底） */
    { "item", RK_SPECIAL | RK_ITEM_HERE | RK_NO_WEB,
      ASSET_background_door_accessory, 0x00FFD800u, { 0xFF, 0xFF, 0xFF, 0xFF }, 0 },
    /* ROOM_SHOP：商店房（没有布局，内容由 game_shop 管） */
    { "shop", RK_SPECIAL | RK_NO_MOB | RK_NO_WEB | RK_NO_STONE,
      ASSET_background_door_shop, 0x003060C0u, { 0xFF, 0xFF, 0xFF, 0xFF }, 0 },
    /* ROOM_BOSS：Boss 房（空布局 = 只有地形，内容由 game_boss 管） */
    { "boss", RK_SPECIAL | RK_FIGHT | RK_NO_MOB | RK_NO_WEB,
      ASSET_background_door_boss, 0x00E02020u, { LAY_room_2x2, 0xFF, 0xFF, 0xFF }, 1 },
    /* ROOM_HIDDEN：隐藏房（基岩地板 + 四面隐藏门，中心高品道具 + 2 颗雷）
     *  ★ flags 不能带 RK_SPECIAL —— 否则会被"特殊房必须是叶子 / 两两 8 邻域不相邻"判死 */
    { "hidden", 0, -1, 0x002040C0u, { LAY_map_hidden_normal, 0xFF, 0xFF, 0xFF }, 1 },
    /* ROOM_HIDDEN_SUPER：本版不生成，颜色/布局先占位（和普通隐藏房区分开） */
    { "hidden_super", 0, -1, 0x00103080u, { LAY_map_hidden_normal, 0xFF, 0xFF, 0xFF }, 1 },
};

#define KIND_COUNT ((int)(sizeof(g_kinds) / sizeof(g_kinds[0])))

const room_kind_t *game_kind(int room_type)
{
    if (room_type < 0 || room_type >= KIND_COUNT) room_type = ROOM_EMPTY;
    return &g_kinds[room_type];
}

int game_kind_special(int room_type)
{
    return (game_kind(room_type)->flags & RK_SPECIAL) != 0;
}

const room_layout_t *game_layout(int idx)
{
    if (idx < 0 || idx >= LAYOUT_COUNT) return 0;
    return &g_layouts[idx];
}

/*  确定性挑选：同一间房（room_id）每次进都拿到同一张布局。
 *  用 room_id 做散列而不是用随机数，是为了"重进房间/死亡回退"后房间长一样。  */
int game_layout_pick_like(int prototype, int room_type, int room_id)
{
    const room_layout_t *base = game_layout(prototype);
    unsigned int h;
    int i, count = 0, numbered = 0;
    if (!base) return -1;

    h = (unsigned int)room_id * 2654435761u + (unsigned int)room_type * 40503u;
    h ^= h >> 13;
    for (i = 0; i < LAYOUT_COUNT; ++i)
        if (g_layouts[i].map == base->map && g_layouts[i].terrain == base->terrain && g_layout_ids[i])
            numbered = 1;
    for (i = 0; i < LAYOUT_COUNT; ++i)
        if (g_layouts[i].map == base->map && g_layouts[i].terrain == base->terrain &&
            (!numbered || g_layout_ids[i])) ++count;
    h %= (unsigned int)count;
    for (i = 0; i < LAYOUT_COUNT; ++i) {
        if (g_layouts[i].map != base->map || g_layouts[i].terrain != base->terrain) continue;
        if (numbered && !g_layout_ids[i]) continue;
        if (h-- == 0u) return i;
    }
    return prototype;
}

int game_layout_pick(int room_type, int room_id)
{
    const room_kind_t *k = game_kind(room_type);
    int i;
#if GAME_TEST_START_LAYOUT >= 0
    if (room_type == ROOM_START) return GAME_TEST_START_LAYOUT;
#endif
    if (k->n_layouts == 0) return -1;
    if (room_type == ROOM_START) {
        /* 1x1_000 是唯一保留的起点空布局；旧参考布局仍可保留在生成表中。 */
        for (i = 0; i < LAYOUT_COUNT; ++i) {
            if (g_layouts[i].map == 0 && !g_layouts[i].terrain &&
                g_layout_ids[i] == 0 && g_layouts[i].n_objs == 0)
                return i;
        }
        return k->layouts[0];
    }
    /* 本轮只扩充普通房布局；特殊房保留指定方案，避免混入普通对象池。 */
    if (room_type != ROOM_EMPTY) return k->layouts[0];
    return game_layout_pick_like(k->layouts[0], room_type, room_id);
}
