/* 自动生成，别手改 —— 源：tools/map_designer/output/<房间名>.json
 * 生成：python tools/map_designer/serve.py --emit-layouts --apply
 * sha256:eef4213e4d6dc8f59e0aa6a89eda43ccc8a36be99d1532557594b662e4526912
 *
 *  map_1x1_type0        40×23  map=0          0 个对象
 *  map_L                45×35  异形(自带地形)       0 个对象
 *  map_hidden_normal    40×23  map=2          0 个对象
 *  room_2x2             45×35  异形(自带地形)       0 个对象
 */
#ifndef GAME_LAYOUT_DATA_H
#define GAME_LAYOUT_DATA_H

#include "game_layout.h"

#define LAY_map_1x1_type0            0
#define LAY_map_L                    1
#define LAY_map_hidden_normal        2
#define LAY_room_2x2                 3

#define LAYOUT_COUNT 4

extern const room_layout_t g_layouts[LAYOUT_COUNT];
extern const obj_place_t L_map_1x1_type0[];
extern const obj_place_t L_map_L[];
extern const layout_terrain_t T_map_L;
extern const obj_place_t L_map_hidden_normal[];
extern const obj_place_t L_room_2x2[];
extern const layout_terrain_t T_room_2x2;

#endif /* GAME_LAYOUT_DATA_H */
