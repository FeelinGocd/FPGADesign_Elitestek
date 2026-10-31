/* 自动生成，别手改 —— 源：tools/map_designer/output/<房间名>.json
 * 生成：python tools/map_designer/serve.py --emit-layouts --apply
 * sha256:79eceba7796314aa8052300eb2c8ddb9bcbd4e9e3b472090f18b9c0d8a598aca
 *
 *  1x1_000              40×23  map=0          0 个对象
 *  1x1_001              40×23  map=0          37 个对象
 *  1x1_002              40×23  map=0          61 个对象
 *  1x1_003              40×23  map=0          69 个对象
 *  1x1_004              40×23  map=0          71 个对象
 *  L_001                45×35  共享地形=5         66 个对象
 *  map_1x1_type0        40×23  map=0          37 个对象
 *  map_L                45×35  共享地形=5         0 个对象
 *  map_hidden_normal    40×23  共享地形=8         0 个对象
 *  room_2x2             45×35  共享地形=9         0 个对象
 */
#ifndef GAME_LAYOUT_DATA_H
#define GAME_LAYOUT_DATA_H

#include "game_layout.h"

#define LAY__1x1_000                 0
#define LAY__1x1_001                 1
#define LAY__1x1_002                 2
#define LAY__1x1_003                 3
#define LAY__1x1_004                 4
#define LAY_L_001                    5
#define LAY_map_1x1_type0            6
#define LAY_map_L                    7
#define LAY_map_hidden_normal        8
#define LAY_room_2x2                 9

#define LAYOUT_COUNT 10
#define LAYOUT_SHARED_MAPS 1

extern const room_layout_t g_layouts[LAYOUT_COUNT];
extern const unsigned short g_layout_ids[LAYOUT_COUNT]; /* 0 = 保留布局/旧参考布局 */
extern const obj_place_t L__1x1_000[];
extern const obj_place_t L__1x1_001[];
extern const obj_place_t L__1x1_002[];
extern const obj_place_t L__1x1_003[];
extern const obj_place_t L__1x1_004[];
extern const obj_place_t L_L_001[];
extern const layout_terrain_t T_L_001;
extern const obj_place_t L_map_1x1_type0[];
extern const obj_place_t L_map_L[];
#define T_map_L T_L_001
extern const obj_place_t L_map_hidden_normal[];
extern const layout_terrain_t T_map_hidden_normal;
extern const obj_place_t L_room_2x2[];
extern const layout_terrain_t T_room_2x2;

#endif /* GAME_LAYOUT_DATA_H */
