/* 自动生成，别手改 —— 见 game_layout_data.h 的说明 */
#include "game.h"
#include "game_layout_data.h"

#define O(x, y, k) { (unsigned char)(x), (unsigned char)(y), (unsigned char)(k), 0 }
#define OP(x, y, k, p) { (unsigned char)(x), (unsigned char)(y), (unsigned char)(k), (unsigned char)(p) }

/* Shared rows; per-terrain uint16 indexes preserve every cell/rotation. */
static const char LT_rows[24][LAYOUT_MAX_COLS + 1] = {
    ".............................................",
    "......ZWWWWWWWDWWWWWWWWWWWWWDWWWWWX..........",
    "......WZWWWWWW.WWWWWWWWWWWWW.WWWWXW..........",
    "......WWBBBBBBBBBBBBBBBBBBBBBBBBBWW..........",
    "......D.BBBBBBBBBBBBBBBBBBBBBBBBBD...........",
    "......WCWWWWWWDWWWWWWWWZBBBBBBBBBWW..........",
    "......CWWWWWWW.WWWWWWWZWBBBBBBBBBWW..........",
    "......................WWBBBBBBBBBWW..........",
    "......................D.BBBBBBBBBD...........",
    "......................WZWWWWDWWWWXW..........",
    "......................ZWWWWW.WWWWWX..........",
    "........................................",
    ".......ywwwwwwwwwwwwwwwwwwwwwwwwwc......",
    ".......wywwwwwwwwwwwHwwwwwwwwwwwcw......",
    ".......wwKKKKKKKKKKKKKKKKKKKKKKKww......",
    ".......wHKKKKKKKKKKKKKKKKKKKKKKKHw......",
    ".......wzwwwwwwwwwwwHwwwwwwwwwwwxw......",
    ".......zwwwwwwwwwwwwwwwwwwwwwwwwwx......",
    "......YWWWWWWWDWWWWWWWWWWWWWDWWWWWWWC........",
    "......WYWWWWWW.WWWWWWWWWWWWW.WWWWWWCW........",
    "......WWBBBBBBBBBBBBBBBBBBBBBBBBBBBWW........",
    "......D.BBBBBBBBBBBBBBBBBBBBBBBBBBBD.........",
    "......WCWWWWWWDWWWWWWWWWWWWWDWWWWWWCW........",
    "......CWWWWWWW.WWWWWWWWWWWWW.WWWWWWWC........",
};
static const unsigned char RT_rows[21][LAYOUT_ROT_COLS] = {
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 16, 0, 0, 0, 0, 0, 0, 16, 0, 0, 0 },
    { 0, 112, 0, 0, 0, 0, 0, 0, 20, 0, 0, 0 },
    { 0, 240, 0, 0, 0, 0, 0, 0, 20, 0, 0, 0 },
    { 0, 48, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0 },
    { 0, 176, 170, 170, 170, 170, 0, 0, 20, 0, 0, 0 },
    { 0, 160, 170, 138, 170, 234, 0, 0, 20, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 240, 0, 0, 20, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 48, 0, 0, 4, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 48, 170, 170, 26, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 128, 170, 168, 42, 0, 0, 0 },
    { 0, 192, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0 },
    { 0, 192, 3, 0, 0, 0, 0, 0, 5, 0, 0, 0 },
    { 0, 192, 168, 170, 170, 170, 170, 170, 4, 0, 0, 0 },
    { 0, 0, 170, 170, 170, 170, 170, 170, 2, 0, 0, 0 },
    { 0, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 176, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0 },
    { 0, 240, 0, 0, 0, 0, 0, 0, 64, 1, 0, 0 },
    { 0, 48, 0, 0, 0, 0, 0, 0, 64, 0, 0, 0 },
    { 0, 176, 170, 170, 170, 170, 170, 170, 106, 1, 0, 0 },
    { 0, 160, 170, 138, 170, 170, 170, 168, 170, 1, 0, 0 },
};

/* 1x1_000：40×23，0 个对象 */
const obj_place_t L__1x1_000[] = {
    { 0, 0, 0, 0 },   /* 占位：空布局，n = 0 */
};

/* 1x1_001：40×23，37 个对象 */
const obj_place_t L__1x1_001[] = {
    O(20,5,OBJ_WEB), O(20,6,OBJ_STONE), O(20,7,OBJ_STONE), O(18,8,OBJ_BUG), O(20,8,OBJ_STONE),
    O(22,8,OBJ_SLIME), O(19,9,OBJ_WEB), O(20,9,OBJ_STONE), O(21,9,OBJ_WEB), O(11,10,OBJ_WEB),
    O(12,10,OBJ_STONE), O(13,10,OBJ_STONE), O(14,10,OBJ_STONE), O(15,10,OBJ_STONE),
    O(16,10,OBJ_STONE), O(17,10,OBJ_STONE), O(18,10,OBJ_STONE), O(19,10,OBJ_STONE),
    O(20,10,OBJ_WEB), O(21,10,OBJ_STONE), O(22,10,OBJ_STONE), O(23,10,OBJ_STONE),
    O(24,10,OBJ_STONE), O(25,10,OBJ_STONE), O(26,10,OBJ_STONE), O(27,10,OBJ_STONE),
    O(28,10,OBJ_STONE), O(29,10,OBJ_WEB), O(19,11,OBJ_WEB), O(20,11,OBJ_STONE),
    O(21,11,OBJ_WEB), O(18,12,OBJ_SLIME), O(20,12,OBJ_STONE), O(22,12,OBJ_BUG),
    O(20,13,OBJ_STONE), O(20,14,OBJ_STONE), O(20,15,OBJ_WEB),
};

/* 1x1_002：40×23，61 个对象 */
const obj_place_t L__1x1_002[] = {
    O(9,4,OBJ_CRATE), O(10,4,OBJ_CRATE), O(11,4,OBJ_STONE), O(12,4,OBJ_STONE),
    O(28,4,OBJ_STONE), O(29,4,OBJ_STONE), O(30,4,OBJ_CRATE), O(31,4,OBJ_CRATE),
    O(9,5,OBJ_CRATE), O(10,5,OBJ_STONE), O(11,5,OBJ_STONE), O(29,5,OBJ_STONE),
    O(30,5,OBJ_STONE), O(31,5,OBJ_CRATE), O(9,6,OBJ_STONE), O(10,6,OBJ_STONE),
    O(30,6,OBJ_STONE), O(31,6,OBJ_STONE), O(9,7,OBJ_STONE), O(20,7,OBJ_WEB), O(31,7,OBJ_STONE),
    O(17,8,OBJ_WEB), O(20,8,OBJ_BUG), O(20,8,OBJ_WEB), O(23,8,OBJ_WEB), O(18,9,OBJ_WEB),
    O(20,9,OBJ_WEB), O(22,9,OBJ_WEB), O(17,10,OBJ_BUG), O(19,10,OBJ_WEB), O(20,10,OBJ_WEB),
    O(21,10,OBJ_WEB), O(23,10,OBJ_BUG), O(18,11,OBJ_WEB), O(20,11,OBJ_WEB), O(22,11,OBJ_WEB),
    O(17,12,OBJ_WEB), O(20,12,OBJ_BUG), O(20,12,OBJ_WEB), O(23,12,OBJ_WEB), O(9,13,OBJ_STONE),
    O(20,13,OBJ_WEB), O(31,13,OBJ_STONE), O(9,14,OBJ_STONE), O(10,14,OBJ_STONE),
    O(30,14,OBJ_STONE), O(31,14,OBJ_STONE), O(9,15,OBJ_CRATE), O(10,15,OBJ_STONE),
    O(11,15,OBJ_STONE), O(29,15,OBJ_STONE), O(30,15,OBJ_STONE), O(31,15,OBJ_CRATE),
    O(9,16,OBJ_CRATE), O(10,16,OBJ_CRATE), O(11,16,OBJ_STONE), O(12,16,OBJ_STONE),
    O(28,16,OBJ_STONE), O(29,16,OBJ_STONE), O(30,16,OBJ_CRATE), O(31,16,OBJ_CRATE),
};

/* 1x1_003：40×23，69 个对象 */
const obj_place_t L__1x1_003[] = {
    O(9,4,OBJ_CRATE), O(10,4,OBJ_CRATE), O(11,4,OBJ_WEB), O(18,4,OBJ_STONE), O(22,4,OBJ_STONE),
    O(29,4,OBJ_WEB), O(30,4,OBJ_CRATE), O(31,4,OBJ_CRATE), O(9,5,OBJ_CRATE), O(10,5,OBJ_WEB),
    O(18,5,OBJ_STONE), O(22,5,OBJ_STONE), O(30,5,OBJ_WEB), O(31,5,OBJ_CRATE), O(9,6,OBJ_WEB),
    O(14,6,OBJ_AIMER), O(26,6,OBJ_AIMER), O(31,6,OBJ_WEB), O(9,8,OBJ_STONE), O(10,8,OBJ_STONE),
    O(11,8,OBJ_STONE), O(12,8,OBJ_STONE), O(13,8,OBJ_STONE), O(14,8,OBJ_STONE),
    O(15,8,OBJ_STONE), O(25,8,OBJ_STONE), O(26,8,OBJ_STONE), O(27,8,OBJ_STONE),
    O(28,8,OBJ_STONE), O(29,8,OBJ_STONE), O(30,8,OBJ_STONE), O(31,8,OBJ_STONE),
    O(20,9,OBJ_STONE), O(19,10,OBJ_STONE), O(20,10,OBJ_BOX), O(21,10,OBJ_STONE),
    O(20,11,OBJ_STONE), O(9,12,OBJ_STONE), O(10,12,OBJ_STONE), O(11,12,OBJ_STONE),
    O(12,12,OBJ_STONE), O(13,12,OBJ_STONE), O(14,12,OBJ_STONE), O(15,12,OBJ_STONE),
    O(25,12,OBJ_STONE), O(26,12,OBJ_STONE), O(27,12,OBJ_STONE), O(28,12,OBJ_STONE),
    O(29,12,OBJ_STONE), O(30,12,OBJ_STONE), O(31,12,OBJ_STONE), O(9,14,OBJ_WEB),
    O(14,14,OBJ_AIMER), O(26,14,OBJ_AIMER), O(31,14,OBJ_WEB), O(9,15,OBJ_CRATE),
    O(10,15,OBJ_WEB), O(18,15,OBJ_STONE), O(22,15,OBJ_STONE), O(30,15,OBJ_WEB),
    O(31,15,OBJ_CRATE), O(9,16,OBJ_CRATE), O(10,16,OBJ_CRATE), O(11,16,OBJ_WEB),
    O(18,16,OBJ_STONE), O(22,16,OBJ_STONE), O(29,16,OBJ_WEB), O(30,16,OBJ_CRATE),
    O(31,16,OBJ_CRATE),
};

/* 1x1_004：40×23，71 个对象 */
const obj_place_t L__1x1_004[] = {
    O(9,4,OBJ_STONE), O(10,4,OBJ_STONE), O(30,4,OBJ_STONE), O(31,4,OBJ_STONE), O(9,5,OBJ_STONE),
    O(10,5,OBJ_BOX), O(11,5,OBJ_STONE), O(29,5,OBJ_STONE), O(30,5,OBJ_CRATE), O(31,5,OBJ_STONE),
    O(10,6,OBJ_STONE), O(16,6,OBJ_WEB), O(17,6,OBJ_WEB), O(23,6,OBJ_WEB), O(24,6,OBJ_WEB),
    O(30,6,OBJ_STONE), O(15,7,OBJ_STONE), O(18,7,OBJ_STONE), O(19,7,OBJ_STONE),
    O(20,7,OBJ_STONE), O(21,7,OBJ_STONE), O(22,7,OBJ_STONE), O(25,7,OBJ_STONE),
    O(15,8,OBJ_STONE), O(19,8,OBJ_AIMER), O(20,8,OBJ_STONE), O(21,8,OBJ_AIMER),
    O(25,8,OBJ_STONE), O(15,9,OBJ_STONE), O(19,9,OBJ_SLIME), O(20,9,OBJ_STONE), O(21,9,OBJ_BUG),
    O(25,9,OBJ_STONE), O(15,10,OBJ_STONE), O(19,10,OBJ_SLIME), O(20,10,OBJ_STONE),
    O(21,10,OBJ_BUG), O(25,10,OBJ_STONE), O(15,11,OBJ_STONE), O(19,11,OBJ_SLIME),
    O(20,11,OBJ_STONE), O(21,11,OBJ_BUG), O(25,11,OBJ_STONE), O(15,12,OBJ_STONE),
    O(19,12,OBJ_AIMER), O(20,12,OBJ_STONE), O(21,12,OBJ_AIMER), O(25,12,OBJ_STONE),
    O(15,13,OBJ_STONE), O(18,13,OBJ_STONE), O(19,13,OBJ_STONE), O(20,13,OBJ_STONE),
    O(21,13,OBJ_STONE), O(22,13,OBJ_STONE), O(25,13,OBJ_STONE), O(10,14,OBJ_STONE),
    O(16,14,OBJ_WEB), O(17,14,OBJ_WEB), O(23,14,OBJ_WEB), O(24,14,OBJ_WEB), O(30,14,OBJ_STONE),
    O(9,15,OBJ_STONE), O(10,15,OBJ_CRATE), O(11,15,OBJ_STONE), O(29,15,OBJ_STONE),
    O(30,15,OBJ_BOX), O(31,15,OBJ_STONE), O(9,16,OBJ_STONE), O(10,16,OBJ_STONE),
    O(30,16,OBJ_STONE), O(31,16,OBJ_STONE),
};

/* L_001：45×35 异形地形 */
static const unsigned short LTI_L_001[35] = { 0, 0, 0, 0, 0, 1, 2, 3, 3, 3, 3, 4, 3, 3, 3, 3, 5, 6, 7, 7, 7, 7, 8, 7, 7, 7, 7, 7, 9, 10, 0, 0, 0, 0, 0 };
static const unsigned short RTI_L_001[35] = { 0, 0, 0, 0, 0, 1, 2, 3, 3, 3, 3, 4, 3, 3, 3, 3, 5, 6, 7, 7, 7, 7, 8, 7, 7, 7, 7, 7, 9, 10, 0, 0, 0, 0, 0 };
const layout_terrain_t T_L_001 = {
    LT_rows, RT_rows, 45, 35, 8, 7, 32, 27, 1440, 1120, 40, 34, LTI_L_001, RTI_L_001
};

/* L_001：45×35，66 个对象 */
const obj_place_t L_L_001[] = {
    O(8,7,OBJ_CRATE), O(9,7,OBJ_CRATE), O(10,7,OBJ_STONE), O(23,7,OBJ_STONE), O(30,7,OBJ_STONE),
    O(31,7,OBJ_CRATE), O(32,7,OBJ_CRATE), O(8,8,OBJ_CRATE), O(9,8,OBJ_STONE), O(23,8,OBJ_STONE),
    O(31,8,OBJ_STONE), O(32,8,OBJ_CRATE), O(8,9,OBJ_STONE), O(13,9,OBJ_STONE),
    O(14,9,OBJ_STONE), O(15,9,OBJ_STONE), O(23,9,OBJ_STONE), O(32,9,OBJ_STONE),
    O(11,10,OBJ_STONE), O(17,10,OBJ_STONE), O(23,10,OBJ_WEB), O(11,11,OBJ_STONE),
    O(13,11,OBJ_BUG), O(14,11,OBJ_BUG), O(15,11,OBJ_BUG), O(17,11,OBJ_STONE), O(23,11,OBJ_WEB),
    O(11,12,OBJ_STONE), O(17,12,OBJ_STONE), O(23,12,OBJ_WEB), O(8,13,OBJ_STONE),
    O(13,13,OBJ_STONE), O(14,13,OBJ_STONE), O(15,13,OBJ_STONE), O(23,13,OBJ_STONE),
    O(8,14,OBJ_CRATE), O(9,14,OBJ_STONE), O(23,14,OBJ_STONE), O(8,15,OBJ_CRATE),
    O(9,15,OBJ_CRATE), O(10,15,OBJ_STONE), O(23,15,OBJ_STONE), O(24,16,OBJ_STONE),
    O(25,16,OBJ_STONE), O(26,16,OBJ_STONE), O(27,16,OBJ_WEB), O(28,16,OBJ_WEB),
    O(29,16,OBJ_WEB), O(30,16,OBJ_STONE), O(31,16,OBJ_STONE), O(32,16,OBJ_STONE),
    O(24,17,OBJ_SLIME), O(28,17,OBJ_SLIME), O(32,17,OBJ_SLIME), O(24,25,OBJ_STONE),
    O(32,25,OBJ_STONE), O(24,26,OBJ_CRATE), O(25,26,OBJ_STONE), O(31,26,OBJ_STONE),
    O(32,26,OBJ_CRATE), O(24,27,OBJ_CRATE), O(25,27,OBJ_CRATE), O(26,27,OBJ_STONE),
    O(30,27,OBJ_STONE), O(31,27,OBJ_CRATE), O(32,27,OBJ_CRATE),
};

/* map_1x1_type0：40×23，37 个对象 */
const obj_place_t L_map_1x1_type0[] = {
    O(20,5,OBJ_WEB), O(20,6,OBJ_STONE), O(20,7,OBJ_STONE), O(18,8,OBJ_BUG), O(20,8,OBJ_STONE),
    O(22,8,OBJ_SLIME), O(19,9,OBJ_WEB), O(20,9,OBJ_STONE), O(21,9,OBJ_WEB), O(11,10,OBJ_WEB),
    O(12,10,OBJ_STONE), O(13,10,OBJ_STONE), O(14,10,OBJ_STONE), O(15,10,OBJ_STONE),
    O(16,10,OBJ_STONE), O(17,10,OBJ_STONE), O(18,10,OBJ_STONE), O(19,10,OBJ_STONE),
    O(20,10,OBJ_WEB), O(21,10,OBJ_STONE), O(22,10,OBJ_STONE), O(23,10,OBJ_STONE),
    O(24,10,OBJ_STONE), O(25,10,OBJ_STONE), O(26,10,OBJ_STONE), O(27,10,OBJ_STONE),
    O(28,10,OBJ_STONE), O(29,10,OBJ_WEB), O(19,11,OBJ_WEB), O(20,11,OBJ_STONE),
    O(21,11,OBJ_WEB), O(18,12,OBJ_SLIME), O(20,12,OBJ_STONE), O(22,12,OBJ_BUG),
    O(20,13,OBJ_STONE), O(20,14,OBJ_STONE), O(20,15,OBJ_WEB),
};

/* map_L：45×35，0 个对象 */
const obj_place_t L_map_L[] = {
    { 0, 0, 0, 0 },   /* 占位：空布局，n = 0 */
};

/* map_hidden_normal：40×23 异形地形 */
static const unsigned short LTI_map_hidden_normal[23] = { 11, 11, 12, 13, 14, 14, 14, 14, 14, 14, 15, 14, 14, 14, 14, 14, 14, 16, 17, 11, 11, 11, 11 };
static const unsigned short RTI_map_hidden_normal[23] = { 0, 0, 0, 11, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 13, 14, 0, 0, 0, 0 };
const layout_terrain_t T_map_hidden_normal = {
    LT_rows, RT_rows, 40, 23, 9, 4, 31, 16, 1280, 720, 40, 20, LTI_map_hidden_normal, RTI_map_hidden_normal
};

/* map_hidden_normal：40×23，0 个对象 */
const obj_place_t L_map_hidden_normal[] = {
    { 0, 0, 0, 0 },   /* 占位：空布局，n = 0 */
};

/* room_2x2：45×35 异形地形 */
static const unsigned short LTI_room_2x2[35] = { 0, 0, 0, 0, 0, 18, 19, 20, 20, 20, 20, 20, 20, 21, 20, 20, 20, 20, 20, 20, 20, 21, 20, 20, 20, 20, 20, 20, 22, 23, 0, 0, 0, 0, 0 };
static const unsigned short RTI_room_2x2[35] = { 0, 0, 0, 0, 0, 15, 16, 17, 17, 17, 17, 17, 17, 18, 17, 17, 17, 17, 17, 17, 17, 18, 17, 17, 17, 17, 17, 17, 19, 20, 0, 0, 0, 0, 0 };
const layout_terrain_t T_room_2x2 = {
    LT_rows, RT_rows, 45, 35, 8, 7, 34, 27, 1440, 1120, 42, 34, LTI_room_2x2, RTI_room_2x2
};

/* room_2x2：45×35，0 个对象 */
const obj_place_t L_room_2x2[] = {
    { 0, 0, 0, 0 },   /* 占位：空布局，n = 0 */
};

const room_layout_t g_layouts[LAYOUT_COUNT] = {
    { "1x1_000", L__1x1_000, 0, 0, 0 },
    { "1x1_001", L__1x1_001, 37, 0, 0 },
    { "1x1_002", L__1x1_002, 61, 0, 0 },
    { "1x1_003", L__1x1_003, 69, 0, 0 },
    { "1x1_004", L__1x1_004, 71, 0, 0 },
    { "L_001", L_L_001, 66, 5, &T_L_001 },
    { "map_1x1_type0", L_map_1x1_type0, 37, 0, 0 },
    { "map_L", L_map_L, 0, 5, &T_L_001 },
    { "map_hidden_normal", L_map_hidden_normal, 0, 8, &T_map_hidden_normal },
    { "room_2x2", L_room_2x2, 0, 9, &T_room_2x2 },
};
const unsigned short g_layout_ids[LAYOUT_COUNT] = { 0, 1, 2, 3, 4, 1, 0, 0, 0, 0 };

