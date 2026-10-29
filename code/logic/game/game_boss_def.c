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
/* 旧版按 300 发密度设计；新的试跑容量超过该值时按比例放大齐射。
 * 各层使用不同的位置和速度，实际分配前再由池预算截断最后一拍。
 * 字段顺序：count/gap/rest/step/base/speed/twin/speed2/spin/repeat/aim/motion/layers */
#define BOSS_LAYERS(n) (ENEMY_CAP > 300 ? ((n) * ENEMY_CAP + 299) / 300 : (n))
/* 更大的试跑池需要在最后一圈前补足发射量。超过 500 发容量后使用全部 64 个方向。 */
#define BOSS_RING_SHOTS (ENEMY_CAP > 500 ? ANGLE_STEPS : 50)
#define BOSS_RAIN_BURST(n) (ENEMY_CAP > 500 ? ((n) * ENEMY_CAP + 499) / 500 : (n))
const pattern_t g_boss_patterns[] = {
    { 16, 1, 70,  4, 16, 4, 0, 0,  0,  4, 0, MOTION_LINE, BOSS_LAYERS(5) }, /* CIRCUIT */
    { 16, 1, 66,  4, 16, 3, 1, 3,  1,  2, 0, MOTION_LINE, BOSS_LAYERS(5) }, /* ROT */
    {  4, 1, 80, 16,  0, 4, 0, 0,  2,  8, 0, MOTION_LINE, BOSS_LAYERS(10) }, /* CROSS */
    {  7, 3, 70,  2, 10, 3, 0, 0,  4,  1, 0 }, /* SPREAD（未使用） */
    {  7, 1, 70,  2,  4, 3, 0, 0,  6, 10, 0, MOTION_LINE, BOSS_LAYERS(5) }, /* SWEEP_FAN */
    { 12, 1, 66,  5, 16, 3, 1, 2,  2,  3, 0, MOTION_LINE, BOSS_LAYERS(5) }, /* DOUBLE_RING */
    { 13, 1, 76,  3, 62, 3, 0, 0,  0,  4, 0, MOTION_LINE, BOSS_LAYERS(6) }, /* WIDE_FAN */
    {  8, 1, 60,  8,  0, 5, 1, 5,  8,  4, 0, MOTION_LINE, BOSS_LAYERS(5) }, /* PULSE */
    {  4, 1, 66,  8,  0, 3, 1, 3,  5,  8, 0, MOTION_LINE, BOSS_LAYERS(5) }, /* SPIRAL_ARM */
    {  3, 8, 60,  0, 48, 4, 0, 0,  0,  1, 0 }, /* STRAIGHT_UP（未使用） */
    { 11, 1, 74,  2,  0, 3, 0, 0, 10,  6, 0, MOTION_LINE, BOSS_LAYERS(5) }, /* WIDE_SWEEP */
    {  1,10, 50,  0,  0, 4, 0, 0,  0,  1, 1 }, /* AIMED_1（未使用） */
    {  3, 1, 56,  3,  0, 4, 0, 0,  0, 12, 1, MOTION_LINE, BOSS_LAYERS(10) }, /* AIMED_3 */
    {  5, 1, 62,  3,  0, 5, 0, 0,  7,  7, 1, MOTION_LINE, BOSS_LAYERS(10) }, /* AIMED_5_FAST */
    {  2, 1, 58,  0,  0, 5, 1, 0,  0,  9, 1, MOTION_LINE, BOSS_LAYERS(10) }, /* AIMED_TWIN */
    {  4, 1, 56,  0,  0, 5, 0, 0,  0,  9, 1, MOTION_LINE, BOSS_LAYERS(10) }, /* AIMED_STREAM */
    {  5, 1, 60,  3,  0, 4, 0, 0,  5,  7, 1, MOTION_LINE, BOSS_LAYERS(10) }, /* AIMED_SPIN */
    { BOSS_RING_SHOTS, ENEMY_CAP > 300 ? 12 : 20, 70, 0, 0, 4, 0, 0, 2,
      GAME_BOSS_DENSE_RINGS + (ENEMY_CAP > 300), 0, MOTION_PULSE, 1 }, /* PULSE_RINGS */
    { BOSS_RING_SHOTS, 8, 70,  0,  0, 3, 0, 0,  1, GAME_BOSS_DENSE_RINGS, 0, MOTION_PULSE, 1 }
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
    { 18, 1, M_CENTER },    /* DENSE_RINGS：填满当前配置的敌弹上限 */
    {  0, 1, M_SWEEP_X },   /* CIRCUIT      放射一圈，开胃 */
    { 17, 1, M_CENTER },    /* PULSE_RINGS：从 Boss 中心发出十圈脉冲 */
    { 12, 1, M_SWEEP_Y },   /* AIMED_3      ★ 扇狙 */
    {  4, 1, M_LEFT },      /* SWEEP_FAN    ★ 从左侧旋转半圆 */
    { 15, 1, M_RIGHT },     /* AIMED_STREAM ★ 向右移动并连发 */
    {  1, 1, M_ORBIT },   /* ROT          前后对射 */
    { 13, 1, M_SWEEP_X },   /* AIMED_5_FAST ★ 五发快狙 */
    {  6, 1, M_UP },        /* WIDE_FAN：在上方炮位发射 */
    { 14, 1, M_SWEEP_Y },   /* AIMED_TWIN   ★ 正反两发瞄准线 */
    {  8, 1, M_ORBIT },     /* SPIRAL_ARM：环绕移动同时发射 */
    { 16, 1, M_DOWN },   /* AIMED_SPIN   ★ 绕玩家扫 */
    {  2, 1, M_CENTER },    /* CROSS        居中十字风车 */
    {  5, 1, M_SWEEP_Y },   /* DOUBLE_RING  内外双环 */
    { 10, 1, M_RIGHT },     /* WIDE_SWEEP   右侧慢速大扇扫 */
    {  7, 1, M_CENTER }     /* PULSE        居中八对快弹，收尾 */
};

/*  小怪的招式（预设）：单发/三发自机狙，间隔长、节奏慢  */
static const pattern_t g_minion_patterns[] = {
    { 1, 10, 50, 0, 0, 4, 0, 0, 0, 1, 1 },
    { 3,  8, 56, 3, 0, 4, 0, 0, 0, 1, 1 }
};
const move_t g_minion_moves[] = {
    { 0, 1, 0 },      /* AIMED_1 */
    { 1, 2, 0 }       /* AIMED_3 连放两轮 */
};
const int g_minion_move_n = (int)(sizeof(g_minion_moves) / sizeof(g_minion_moves[0]));

const move_set_t g_boss_move_set = {
    g_boss_patterns, (int)(sizeof(g_boss_patterns) / sizeof(g_boss_patterns[0])),
    g_boss_moves,    (int)(sizeof(g_boss_moves) / sizeof(g_boss_moves[0]))
};

const move_set_t g_minion_move_set = {
    g_minion_patterns, (int)(sizeof(g_minion_patterns) / sizeof(g_minion_patterns[0])),
    g_minion_moves,  (int)(sizeof(g_minion_moves) / sizeof(g_minion_moves[0]))
};
const int g_boss_move_n = (int)(sizeof(g_boss_moves) / sizeof(g_boss_moves[0]));

/* 运动目标始终限制在竞技场内，速度上限保证位置过渡连续。 */
static const boss_motion_t g_boss_motions[] = {
    { BOSS_SINE_X, 96, 5, 4 }, /* M_SWEEP_X：约 5.1 秒的水平巡逻 */
    { BOSS_STILL,   0, 0, 3 }, /* M_CENTER：回到中心 */
    { BOSS_LEFT,   80, 0, 3 }, /* M_LEFT：左侧炮位 */
    { BOSS_RIGHT,  80, 0, 3 }, /* M_RIGHT：右侧炮位 */
    { BOSS_SINE_Y, 96, 3, 4 }, /* M_SWEEP_Y：可见的纵向巡逻 */
    { BOSS_UP,     104, 0, 4 }, /* M_UP：上方炮位 */
    { BOSS_DOWN,   104, 0, 4 }, /* M_DOWN：下方炮位 */
    { BOSS_ORBIT,  112, 3, 4 }  /* M_ORBIT：112×84 px 椭圆轨迹 */
};

/*==============================================================================
 *  Boss 定义：血量 / 锁血线 / 弹幕雨 / 炮口 / 招式表 / 运动方式
 *============================================================================*/
const boss_def_t g_boss_def = {
    BOSS_HP_MAX,            /* hp */
    BOSS_GATE_1,            /* gate1 : 第一次锁血线 */
    BOSS_GATE_2,            /* gate2 : 第二次锁血线 */
    BOSS_PHASE_SHIFT_MS,     /* phase_shift_ms : 转阶段护盾持续时间 */
    BOSS_W,                 /* w */
    BOSS_H,                 /* h */
    67,                     /* muzzle_dx : ★ Boss 的左手 = 画面右侧（精灵是镜像的，宽处 x 2..73） */
    48,                     /* muzzle_dy :   手的高度 */
    &g_boss_move_set,       /* ★ 用哪套招式（换 Boss 只要换这个指针） */
    g_boss_motions,
    (unsigned char)(sizeof(g_boss_motions) / sizeof(g_boss_motions[0]))
};

/* 各阶段的 60 Hz 调度参数。所有机制共享 ENEMY_CAP，不另建弹幕池。
 * 字段依次为 rain_on/rest、shot_cap、rain_every/drop、first_move；
 * 召唤阶段属于两次战斗之间的过场。 */
const boss_phase_def_t g_boss_phases[BOSS_PHASE_COUNT] = {
    {   0,   0, ENEMY_CAP,  0,  0,  0 },
    { 360, 120,         0, ENEMY_CAP > 300 ? 4 : 8, BOSS_RAIN_BURST(28), 0 },
    { 180, 120, ENEMY_CAP * 15 / 16, 12, BOSS_RAIN_BURST(10), 10 }
};
