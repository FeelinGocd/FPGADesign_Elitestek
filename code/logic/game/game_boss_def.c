//==============================================================================
//  game_boss_def.c —— Boss 的【全部数据】：图案库 / 招式表 / Boss 定义
//------------------------------------------------------------------------------
//  干什么      : 只放数据，不放逻辑。引擎（game_boss.c）照着这里放弹幕。
//  上一级是谁  : game/game_boss.c 读 g_boss_def / g_boss_patterns / g_boss_moves
//  为什么这么做:
//    · 招式写成【数据】而不是 if/switch ⇒ 加招式不用改引擎，也不会漏分支
//    · 图案是"参数化的形状"：一圈 = count 发、每发转 step 格、可以 twin 出反向、
//      可以 repeat 轮 × spin 格让它转起来 ⇒ 一套参数能拼出十几种弹幕
//    · Boss 位移从 g_boss_motions[] 选；招式表的第三列是运动池编号。
//  怎么加招式  :
//    1) 在 g_boss_patterns[] 里加一行图案（或在现有图案上调参数）
//    2) 在 g_boss_moves[] 里按顺序插一条 { 图案下标, 连播几次, 运动编号 }
//==============================================================================
#include "game.h"

/*==============================================================================
 *  图案库
 *------------------------------------------------------------------------------
 *  count  : 一轮几发          gap  : 每发隔几帧
 *  rest   : 整招打完歇几帧    step : 每发角度往前走几格（64 格一圈）
 *  base   : 起始角度（0 右 / 16 下 / 32 左 / 48 上）
 *  speed  : 像素每帧          twin : 1 = 同时朝反向再发一发
 *  speed2 : twin 那发的速度（0 = 同速）⇒ 内外两条环
 *  spin   : 每轮再转几格      repeat: 这一招连续发几轮（>1 = 招式会动）
 *============================================================================*/
const pattern_t g_boss_patterns[] = {
    /*  0 CIRCUIT     : 16 方向均分一圈（静止） */
    { 16, 2, 70,  4, 16, 4, 0, 0,  0, 1, 0 },
    /*  1 ROT         : 前后对射，双臂异速 */
    { 16, 3, 66,  4, 16, 3, 1, 3,  0, 1, 0 },
    /*  2 CROSS       : 十字四发，每招自转 90 度 */
    {  4, 4, 80, 16,  0, 4, 0, 0, 16, 1, 0 },
    /*  3 SPREAD      : 静止扇形七发 */
    {  7, 3, 70,  2, 10, 3, 0, 0,  4, 1, 0 },
    /*  4 SWEEP_FAN   : 旋转半圆（扇形转 10 轮，扫过大半圈） */
    {  7, 2, 70,  2,  4, 3, 0, 0,  6, 10, 0 },
    /*  5 DOUBLE_RING : 前后对射，两臂速度不同（内外两条环） */
    { 12, 3, 66,  5, 16, 3, 1, 2,  0, 1, 0 },
    /*  6 WIDE_FAN    : 半圆 13 发横扫一片 */
    { 13, 2, 76,  3, 62, 3, 0, 0,  0, 1, 0 },
    /*  7 PULSE       : 八对快弹，压迫感最强 */
    {  8, 6, 60,  8,  0, 5, 1, 5,  8, 1, 0 },
    /*  8 SPIRAL_ARM  : 双臂各 4 个方向 × 6 轮 = 48 发，避开 64 发池截断 */
    {  4, 5, 66,  8,  0, 3, 1, 3,  5, 6, 0 },
    /*  9 STRAIGHT_UP : 朝上三连（一对一教学用的固定方向） */
    {  3, 8, 60,  0, 48, 4, 0, 0,  0, 1, 0 },
    /* 10 WIDE_SWEEP  : 扇形但转得慢、覆盖更宽（11 发 × 6 轮） */
    { 11, 2, 74,  2,  0, 3, 0, 0, 10,  6, 0 },
    /* 11 AIMED_1     : 单发自机狙（小怪最基础的一招） */
    {  1, 10, 50,  0,  0, 4, 0, 0,  0, 1, 1 },
    /* 12 AIMED_3     : 三发扇形朝玩家 */
    {  3,  8, 56,  3,  0, 4, 0, 0,  0, 1, 1 },
    /* 13 AIMED_5_FAST: 五发快速扇狙，每轮转 7 格 */
    {  5,  4, 62,  3,  0, 5, 0, 0,  7, 1, 1 },
    /* 14 AIMED_TWIN  : 沿瞄准线正反两发（像一道穿过去的激光） */
    {  2,  6, 58,  0,  0, 5, 1, 0,  0, 1, 1 },
    /* 15 AIMED_STREAM: 同向连发四颗（机枪感） */
    {  4,  3, 56,  0,  0, 5, 0, 0,  0, 1, 1 },
    /* 16 AIMED_SPIN  : 五发扇狙连扫 6 轮，绕着玩家转 */
    {  5,  3, 60,  3,  0, 4, 0, 0,  5, 6, 1 },
    /* 17 PULSE_RINGS: six offset rings, 16 simultaneous shots per ring. */
    { 16, 20, 70,  4,  0, 4, 0, 0,  2, 6, 0, MOTION_PULSE },
    /* 18 DENSE_RINGS: nine 36-shot rings fill 324 enemy slots in a 356-slot pool. */
    { 36,  8, 70,  2,  0, 3, 0, 0,  1, GAME_BOSS_DENSE_RINGS, 0, MOTION_PULSE }
};

const int g_boss_pattern_n = (int)(sizeof(g_boss_patterns) / sizeof(g_boss_patterns[0]));

/*==============================================================================
 *  招式表：按顺序播放，播完回到第一条
 *------------------------------------------------------------------------------
 *  三列依次是图案编号、连播次数、运动池编号。
 *  （连播时角度会接着转，所以 2 次 SWEEP_FAN = 扫得更远）
 *============================================================================*/
enum { M_SWEEP_X, M_CENTER, M_LEFT, M_RIGHT, M_SWEEP_Y,
       M_UP, M_DOWN, M_ORBIT };
const move_t g_boss_moves[] = {
    { 18, 1, M_CENTER },    /* DENSE_RINGS: fill enemy quota, player keeps 32 slots */
    {  0, 1, M_SWEEP_X },   /* CIRCUIT      放射一圈，开胃 */
    { 17, 1, M_CENTER },    /* PULSE_RINGS: six pulsing rings from Boss center */
    { 12, 1, M_SWEEP_Y },   /* AIMED_3      ★ 扇狙 */
    {  4, 1, M_LEFT },      /* SWEEP_FAN    ★ 从左侧旋转半圆 */
    { 15, 1, M_RIGHT },     /* AIMED_STREAM ★ 向右移动并连发 */
    {  1, 1, M_ORBIT },   /* ROT          前后对射 */
    { 13, 1, M_SWEEP_X },   /* AIMED_5_FAST ★ 五发快狙 */
    {  6, 1, M_UP },        /* WIDE_FAN: upper firing position */
    { 14, 1, M_SWEEP_Y },   /* AIMED_TWIN   ★ 正反两发瞄准线 */
    {  8, 1, M_ORBIT },     /* SPIRAL_ARM: orbit while firing */
    { 16, 1, M_DOWN },   /* AIMED_SPIN   ★ 绕玩家扫 */
    {  2, 1, M_CENTER },    /* CROSS        居中十字风车 */
    {  5, 1, M_SWEEP_Y },   /* DOUBLE_RING  内外双环 */
    { 10, 1, M_RIGHT },     /* WIDE_SWEEP   右侧慢速大扇扫 */
    {  7, 1, M_CENTER }     /* PULSE        居中八对快弹，收尾 */
};

/*  小怪的招式（预设）：单发/三发自机狙，间隔长、节奏慢  */
const move_t g_minion_moves[] = {
    { 11, 1, 0 },      /* AIMED_1 */
    { 12, 2, 0 }       /* AIMED_3 连放两轮 */
};
const int g_minion_move_n = (int)(sizeof(g_minion_moves) / sizeof(g_minion_moves[0]));

const move_set_t g_boss_move_set = {
    g_boss_patterns, (int)(sizeof(g_boss_patterns) / sizeof(g_boss_patterns[0])),
    g_boss_moves,    (int)(sizeof(g_boss_moves) / sizeof(g_boss_moves[0]))
};

const move_set_t g_minion_move_set = {
    g_boss_patterns, (int)(sizeof(g_boss_patterns) / sizeof(g_boss_patterns[0])),
    g_minion_moves,  (int)(sizeof(g_minion_moves) / sizeof(g_minion_moves[0]))
};
const int g_boss_move_n = (int)(sizeof(g_boss_moves) / sizeof(g_boss_moves[0]));

/* Motion targets stay inside the arena; speed caps keep transitions continuous. */
static const boss_motion_t g_boss_motions[] = {
    { BOSS_SINE_X, 96, 5, 4 }, /* M_SWEEP_X: original 5.1 s patrol */
    { BOSS_STILL,   0, 0, 3 }, /* M_CENTER: return to centre */
    { BOSS_LEFT,   80, 0, 3 }, /* M_LEFT: left firing position */
    { BOSS_RIGHT,  80, 0, 3 }, /* M_RIGHT: right firing position */
    { BOSS_SINE_Y, 96, 3, 4 }, /* M_SWEEP_Y: visible vertical patrol */
    { BOSS_UP,     104, 0, 4 }, /* M_UP: upper firing position */
    { BOSS_DOWN,   104, 0, 4 }, /* M_DOWN: lower firing position */
    { BOSS_ORBIT,  112, 3, 4 }  /* M_ORBIT: 112 x 84 px ellipse */
};

/*==============================================================================
 *  Boss 定义：血量 / 锁血线 / 弹幕雨 / 炮口 / 招式表 / 运动方式
 *============================================================================*/
const boss_def_t g_boss_def = {
    BOSS_HP_MAX,            /* hp */
    BOSS_GATE_1,            /* gate1 : 第一次锁血线 */
    BOSS_GATE_2,            /* gate2 : 第二次锁血线 */
    RAIN_MS,                /* rain_ms   : 锁血弹幕雨持续多久 */
    20,                     /* rain_wave : 每 20 帧一波雨 */
    RAIN_DROP,              /* rain_drop : 一波几颗 */
    BOSS_W,                 /* w */
    BOSS_H,                 /* h */
    67,                     /* muzzle_dx : ★ Boss 的左手 = 画面右侧（精灵是镜像的，宽处 x 2..73） */
    48,                     /* muzzle_dy :   手的高度 */
    &g_boss_move_set,       /* ★ 用哪套招式（换 Boss 只要换这个指针） */
    g_boss_motions,
    (unsigned char)(sizeof(g_boss_motions) / sizeof(g_boss_motions[0]))
};
