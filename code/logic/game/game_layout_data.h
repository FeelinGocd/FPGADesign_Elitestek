/* 自动生成，别手改 —— 源：tools/map_designer/output/<房间名>.json
 * 生成：python tools/map_designer/serve.py --emit-layouts --apply
 * sha256:6dccc9ca247bf253d5537483072f32c24008089449d3777e557b3917063520c8
 *
 *  1x1_000              40×23  map=0          0 个对象
 *  1x1_001              40×23  map=0          37 个对象
 *  1x1_002              40×23  map=0          61 个对象
 *  L_001                45×35  共享地形=3         66 个对象
 *  map_1x1_type0        40×23  map=0          37 个对象
 *  map_L                45×35  共享地形=3         0 个对象
 *  map_hidden_normal    40×23  共享地形=6         0 个对象
 *  room_2x2             45×35  共享地形=7         0 个对象
 */
#ifndef GAME_LAYOUT_DATA_H
#define GAME_LAYOUT_DATA_H

#include "game_layout.h"

#define LAY__1x1_000                 0
#define LAY__1x1_001                 1
#define LAY__1x1_002                 2
#define LAY_L_001                    3
#define LAY_map_1x1_type0            4
#define LAY_map_L                    5
#define LAY_map_hidden_normal        6
#define LAY_room_2x2                 7

#define LAYOUT_COUNT 8
#define LAYOUT_SHARED_MAPS 1

extern const room_layout_t g_layouts[LAYOUT_COUNT];
extern const unsigned short g_layout_ids[LAYOUT_COUNT]; /* 0 = 保留布局/旧参考布局 */
extern const obj_place_t L__1x1_000[];
extern const obj_place_t L__1x1_001[];
extern const obj_place_t L__1x1_002[];
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
