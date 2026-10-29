//==============================================================================
//  render_list.h —— 渲染队列：元素自己带 z，统一排序后下发
//------------------------------------------------------------------------------
//  干什么      :
//    · 一帧 = rl_begin() → 若干 rl_push(z, 画法, 参数) → rl_draw_all(fb, now, buf)
//    · 按 (z, 提交顺序) 排序，z 小的先画（在下面），z 大的后画（盖上面）
//  上一级是谁  : game/game_main.c 的 game_draw()
//  接口契约    :
//    · 同 z 时按 push 顺序画（稳定）⇒ 想让谁在上面，就晚一步 push
//    · z 的算法在 game.h：Z_SCENE / Z_ACTOR / Z_SHIELD / Z_HUD / Z_OVERLAY，
//      实体用 ACTOR_Z(底边 y) —— 越靠下越近、越靠前（俯视视角的透视）
//    · 队列满了不覆盖：丢掉并累加计数，由 PROBE 打出来（绘制路径里不打印）
//  为什么这么做:
//    · 硬件没有 z 缓冲，"谁盖谁"只由命令下发顺序决定（画家算法）；
//      以前把顺序写死在几个 layer_xxx() 里，加一种元素就得改排序逻辑。
//      现在每个元素在提交时给自己算一个 z，排序/绘制只有这一处
//  加一个新元素:
//      rl_push(ACTOR_Z(元素底边 y), my_draw_one, 下标);
//    再写一个 my_draw_one(fb, now, buf, arg) 就完了，不用动 game_main.c
//==============================================================================
#pragma once

#include "type.h"
#include "game_limits.h"

/* 一帧最多登记多少个元素：整个弹幕池 + 金币/怪物/场景/HUD。
 * ★ 余量要留够：满员时 rl_push 丢后来的元素，虽然 HUD 现在最先入队，
 *   但子弹/金币被丢会造成实体闪烁。  */
#define RL_MAX (GAME_BULLET_POOL_CAP + GAME_RENDER_EXTRA_SLOTS)
#define RL_ARG_BITS 11
#define RL_ARG_MAX ((1u << RL_ARG_BITS) - 1u)

/* 画法：fb = 后台帧缓存, now = 毫秒时钟, buf = 缓冲下标, arg = 元素下标 */
typedef void (*rl_draw_fn)(u32 fb, u32 now, int buf, int arg);

/** @brief 开始一帧：清空队列（必须在所有 rl_push 之前调一次） */
void rl_begin(void);

/** @brief 登记一个元素。z=-32768..32767，arg=0..2047；同 z 时先 push 的在下面。
 *  @return 1 = 收下, 0 = 队列满（这一帧这个元素不画，计数 +1） */
int  rl_push(int z, rl_draw_fn fn, int arg);

/** @brief 为同一画法的一组元素查询一次 ID。ID 跨 rl_begin 保持有效；失败返回 -1。 */
int  rl_register_draw(rl_draw_fn fn);

/** @brief 用已注册 ID 登记元素，避免每颗弹重复扫描画法表。失败计入 dropped。 */
int  rl_push_id(int z, int draw_id, int arg);

/** @brief 这一帧登记了几个元素（诊断用） */
int  rl_count(void);

/** @brief 从开机到现在一共丢了多少个（正常应恒为 0） */
u32  rl_dropped(void);

/** @brief 排序并依次调用每个元素的画法（z 从小到大 = 从底到顶） */
void rl_draw_all(u32 fb, u32 now, int buf);
