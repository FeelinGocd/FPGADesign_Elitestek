//==============================================================================
//  game_perf.c —— 压测脚手架（量"元素数 vs 帧率"用，正常游戏时整段被优化掉）
//------------------------------------------------------------------------------
//  干什么      :
//    · game_perf_init()      按模式预置子弹（mode 1 用第一档，2/3 用满 N_BULLETS）
//    · game_perf_frozen()    mode 1：冻结玩法，只跑绘制路径
//    · game_perf_pinned()    mode 2/3：子弹撞墙/命中都挪回网格，保持恒定在飞数
//    · game_perf_keep_bg()   mode 3 返回 0：故意不恢复背景（分离背景恢复的代价）
//    · game_perf_tick()      每 120 帧打一行：帧数 / 毫秒 / 帧率 / 每条命令
//  上一级是谁  : game/game_main.c（tick）、game/game_state.c（init）、game/game_bullet.c（pinned）
//  接口契约    :
//    · 所有分支都是【运行时 if】而不是 #if：PERF_TEST_MODE 是常量，
//      -O2 会把 mode 0 下的整段代码删掉，但代码永远参与编译（不会"平时没编到、有错也发现不了"）
//    · 这个文件不实现任何游戏规则，删掉它 + 去掉几处 if，游戏照样跑
//  参考        : docs/spec/实测数据.md §3 / §6
//==============================================================================
#include "game.h"
#include "user_gpu2d.h"
#include "bsp.h"

static const int counts[PERF_LEVELS] = { 16, 32, 48, 64, 80, 96 };

static u32 t0, cmd0, frames;
static int level;

/*----------------------------------------------------------------------------*/
void game_perf_init(void)
{
    if (PERF_TEST_MODE == 1) {
        /* 固定模型：命令数只跟子弹数有关，运动和碰撞都不参与。 */
        game_bullets_perf_fill(counts[0]);
    } else if (PERF_TEST_MODE >= 2) {
        int i;

        game_bullets_perf_fill(PERF_TEST_BULLETS);
        for (i = 0; i < PERF_TEST_BULLETS && i < N_BULLETS; ++i) {
            g.bullets[i].vx = (i % 3) - 1;
            g.bullets[i].vy = ((i / 3) % 3) - 1;
            if (g.bullets[i].vx == 0 && g.bullets[i].vy == 0) g.bullets[i].vy = 1;
        }
    }
    frames = 0u;
    level = 0;
    t0 = game_ms();
    cmd0 = gpu2d_getCmdDone();
}

/*----------------------------------------------------------------------------*/
int game_perf_frozen(void)   { return PERF_TEST_MODE == 1; }
int game_perf_pinned(void)   { return PERF_TEST_MODE >= 2; }
int game_perf_keep_bg(void)  { return PERF_TEST_MODE != 3; }

/*----------------------------------------------------------------------------*/
/*  返回 0 = 压测跑完了，游戏应该退出（只有 mode 1 会跑完）  */
int game_perf_tick(u32 now)
{
    u32 dt, dc;

    if (PERF_TEST_MODE == 0) return 1;
    if (++frames < 120u) return 1;

    dt = now - t0;
    dc = gpu2d_getCmdDone() - cmd0;
    if (dt == 0u) dt = 1u;

    /* 这个 BSP 的 printf 只支持工程里用过的整数格式；不用 %u（会被当成浮点）。 */
    if (PERF_TEST_MODE == 1) {
#if GAME_UART_TELEMETRY
        bsp_printf("PERF bullets=%d: %d frames, %d ms, %d fps, %d cmd/frame\r\n",
                   counts[level], (int)frames, (int)dt,
                   (int)((frames * 1000u) / dt), (int)(dc / frames));
#endif
        if (++level >= PERF_LEVELS) {
#if GAME_UART_TELEMETRY
            bsp_printf("PERF DONE: all bullet levels completed\r\n");
#endif
            return 0;
        }
        game_bullets_perf_fill(counts[level]);
    } else {
#if GAME_UART_TELEMETRY
        bsp_printf("PERF dynamic: %d frames, %d ms, %d fps, %d cmd/frame\r\n",
                   (int)frames, (int)dt,
                   (int)((frames * 1000u) / dt), (int)(dc / frames));
#endif
    }
    frames = 0u;
    t0 = now;
    cmd0 = gpu2d_getCmdDone();
    return 1;
}
