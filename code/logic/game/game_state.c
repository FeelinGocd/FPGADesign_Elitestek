//==============================================================================
//  game_state.c —— 状态定义 + 时钟 + 初始化 + 上屏
//------------------------------------------------------------------------------
//  干什么      :
//    · g            : 整个 demo 唯一的一份状态（别的模块只读写它）
//    · game_ms()    : 毫秒时钟（CLINT）
//    · game_init()  : 放一个道具、清两块缓冲、铺好地板、开显示
//    · game_finish_gpu() / game_present(): 一帧画完 → FENCE → 等显示真的翻页
//  上一级是谁  : game/game_main.c
//  接口契约    :
//    · game_init() 返回 0 = 失败，调用方直接退出主循环
//    · 上屏节拍靠【显示帧中断】：FENCE 只表示画完，翻页要等下一个帧边界
//  为什么这么做:
//    · 初始化期间显示是关着的（exclusive_begin），此时 frame_done 可能一直不动，
//      所以 FENCE 等的是"帧完成计数"，不能等显示相位。
//  参考        : docs/spec/软件_驱动与API.md §3（时序纪律）
//==============================================================================
#include "game.h"
#include "CI_gpu2d.h"
#include "user_gpu2d.h"
#include "user_lineBuffer.h"
#include "IRQ_frame.h"
#include "bsp.h"

//==================== 唯一的一份状态 ====================
game_t g;

/*  共享 sin 表：sin(2πi/64) × 64 —— 定点，不用浮点库  */
const signed char g_sin_tab[64] = {
       0,    6,   12,   19,   24,   30,   36,   41,   45,   49,   53,   56,   59,   61,   63,   64,
      64,   64,   63,   61,   59,   56,   53,   49,   45,   41,   36,   30,   24,   19,   12,    6,
       0,   -6,  -12,  -19,  -24,  -30,  -36,  -41,  -45,  -49,  -53,  -56,  -59,  -61,  -63,  -64,
     -64,  -64,  -63,  -61,  -59,  -56,  -53,  -49,  -45,  -41,  -36,  -30,  -24,  -19,  -12,   -6
};

/*----------------------------------------------------------------------------*/
u32 game_ms(void)
{
    return (u32)(clint_getTime(BSP_CLINT) / (BSP_CLINT_HZ / 1000u));
}

/*----------------------------------------------------------------------------*/
/*  FENCE 完成是命令侧事件；初始化期间显示关着，frame_done 可能根本不动，
 *  所以这里等"帧完成计数"，并带超时。  */
int game_finish_gpu(void)
{
    u32 before = gpu2d_getFrameDone();

    CI_gpu2d_fence();
    if (gpu2d_waitFrame(before, 3000000u) != 0) {
        bsp_printf("DEBUG: GPU fence timeout\r\n");
        return 0;
    }
    return 1;
}

/*----------------------------------------------------------------------------*/
/*  把显示基址的两个槽都写成刚画好的那块（相位免疫），再等下一个帧边界。  */
int game_present(u32 fb)
{
    u32 before, wait = 0u;

    lineBuffer_setFullBase(fb, fb);
#if IRQ_FRAME_USE
    before = IRQ_frame_count();
    while (IRQ_frame_count() == before && wait < 200000u) {
        bsp_uDelay(1u);
        ++wait;
    }
    if (IRQ_frame_count() == before) {
        bsp_printf("DEBUG: display IRQ timeout\r\n");
        return 0;
    }
#else
    (void)before; (void)wait;
    bsp_uDelay(20000u);
#endif
    return 1;
}

/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
/*  纯数据复位（不碰帧缓冲）：初始化和"R 键重开"共用  */
void game_reset_state(void)
{
    u32 seed = game_ms();
    int i;

    /* 玩家在初始房正中间（房间系统负责摆位置） */
    game_room_reset();
    g.dir = DOWN;
    g.moving = 0; g.was_moving = 0; g.last_dir = DOWN;
    g.anim = 0u; g.fire_tick = 0u; g.anim_step = 0u;
    g.vx8 = 0; g.vy8 = 0; g.rx = 0; g.ry = 0;
    g.fire_triple = 0;
    g.item_mask = 0u;                       /* 道具池清空 */
    g.item_order_n = 0;                     /* 拾取顺序也清空 */
    game_vital_reset();                     /* 3 颗心、3 条命 */

    /*  ★ 重开时地砖会整屏重铺 ⇒ 上一帧的字全被盖掉了。
     *    这里把 UI 的"上次画的值"作废，下一帧两块缓冲都会老老实实重画。
     *    （不这样做的话：重开后 GUI 一片空白，要等数值变了才慢慢出现。）  */
    game_hud_invalidate();
    game_vital_invalidate();
    game_item_invalidate();                            /* 初始 3 条命（1UP! 再加一） */
    g.bullet_type = BULLET_A;
    g.held_type = ITEM_CHILI;
    g.pickup_until = 0u;
    g.hurt_until = 0u;
    g.toast_until = 0u;
    g.toast_type = ITEM_CHILI;
    g.countdown_until = 0u;

    g.boss_x = (SCR_W - BOSS_W) / 2;
    g.boss_y = (SCR_H - BOSS_H) / 2;
    g.boss_pattern = 0;
    g.boss_tick = 0u;
    g.stage = 1;                            /* 重开也从第 1 关开始 */
    g.score = 0;                            /* 本局得分清零 */
    g.boss_hp = g_boss_def.hp;
    g.boss_gate = g_boss_def.gate1;
    g.boss_locked = 0;
    g.boss_lock_until = 0u;
    g.boss_flash_until = 0u;
    g.boss_dead_until = 0u;
    g.boss_blink_cnt = 0;
    g.boss_blink_on = 0;
    g.boss_blink_next = 0u;
    g.boss_blink_cool = 0u;
    game_shot_stop(&g.shot);

    /* 道具放在屏幕正中：捡了它 Boss 才现身 */
    g.item.active = ROOM_NO_SPAWN ? 0 : 1;
    g.item.type = (int)((seed >> 4) % (u32)ITEM_COUNT);
    g.item.x = (SCR_W - ITEM_W) / 2;
    g.item.y = (SCR_H - ITEM_H) / 2;
    g.phase = PHASE_PICKUP;

    g.front = 0;
    g.cmd_frame = 0;
    g.fps = 0;
    g.fps_epoch = 0u; g.fps_frames = 0u; g.cmd_before = 0u;
    g.probe_epoch = 0u; g.probe_frames = 0u; g.probe_frame_no = 0u;
    g.probe_peak = 0; g.probe_peak_frame = 0; g.probe_peak_cmd = 0;

    for (i = 0; i < N_BULLETS; ++i) {
        g.bullets[i].active = 0;
        g.old_bullets[0][i].active = 0;
        g.old_bullets[1][i].active = 0;
    }
    g.old_item[0].active = 0;
    g.old_item[1].active = 0;
    g.old_boss[0].active = 0;
    g.old_boss[1].active = 0;
    g.old_player[0].active = 0;
    g.old_player[1].active = 0;
    g.front = 0;
}

/*----------------------------------------------------------------------------*/
/*  进入"拾取阶段"：场上放一个道具（屏幕正中），Boss 不出现  */
/*----------------------------------------------------------------------------*/
/*  命尽 → GAME OVER 画面：冻结玩法、清弹幕、定时回第 1 关  */
void game_enter_gameover(u32 now)
{
    g.phase          = PHASE_GAMEOVER;
    g.gameover_until = now + GAMEOVER_MS;
    g.gameover_stage = g.stage;

    game_bullets_clear();

    /* GAME OVER ??????????????????????
     * game_reset_state() ?????????????? */
    game_shot_stop(&g.shot);            /* Boss 停火 */
    bsp_printf("GAME OVER: stage %d (auto restart in %d ms)\r\n",
               g.stage, (int)GAMEOVER_MS);
}

/*----------------------------------------------------------------------------*/
void game_enter_pickup(void)
{
    g.phase = PHASE_PICKUP;
    g.item.active = ROOM_NO_SPAWN ? 0 : 1;
    g.item.type = (g.item.type + 1) % ITEM_COUNT;    /* 四件道具轮着给 */
    g.item.x = (SCR_W - ITEM_W) / 2;
    g.item.y = (SCR_H - ITEM_H) / 2;

    g.boss_hp = g_boss_def.hp;
    g.boss_gate = g_boss_def.gate1;
    g.boss_locked = 0;
    g.boss_lock_until = 0u;
    g.boss_flash_until = 0u;
    game_shot_stop(&g.shot);
    g.boss_tick = 0u;
    g.countdown_until = 0u;
    g.old_boss[0].active = 0;               /* 不要再擦一个不在的 Boss */
    g.old_boss[1].active = 0;
}

/*----------------------------------------------------------------------------*/
/*  捡到道具 → 3-2-1 倒计时（倒计时里 Boss 还不出现）  */
void game_begin_countdown(void)
{
    g.phase = PHASE_COUNTDOWN;
    g.countdown_until = game_ms() + COUNTDOWN_MS;
    g.boss_hp = g_boss_def.hp;
    g.boss_gate = g_boss_def.gate1;
    g.boss_locked = 0;
    game_shot_stop(&g.shot);
    g.boss_tick = 0u;
    bsp_printf("COUNTDOWN: stage %d\r\n", g.stage);
}

/*----------------------------------------------------------------------------*/
/*  倒计时结束 → Boss 现身开打  */
void game_start_fight(void)
{
    g.phase = PHASE_FIGHT;
    game_shot_init(&g.shot, g_boss_def.moves, 1, BULLET_URANUS);
    game_shot_begin(&g.shot, 0);
    g.boss_hp = g_boss_def.hp;
    g.boss_gate = g_boss_def.gate1;
    g.boss_locked = 0;
    g.boss_tick = 0u;
    g.boss_dead_until = 0u;
    g.countdown_until = 0u;
    bsp_printf("FIGHT: stage %d\r\n", g.stage);
}

/*----------------------------------------------------------------------------*/
/*  重开一局：数据复位 + 两块缓冲重新铺地板（旧精灵自然被覆盖掉）  */
int game_restart(void)
{
    static const u32 fb[2] = { FB_C, FB_D };
    u32 cmd0;
    int bi;

    bsp_printf("RESTART: hold R released, new game\r\n");

    game_reset_state();

    cmd0 = gpu2d_getCmdDone();
    gpu2d_exclusive_begin();
    for (bi = 0; bi < 2; ++bi) game_draw_floor(fb[bi]);
    lineBuffer_setFullBase(FB_C, FB_D);
    if (gpu2d_exclusive_end(cmd0 + 2u * game_floor_cmd_count()) != 0) {
        bsp_printf("RESTART ERROR: timeout\r\n");
        return 0;
    }
    return game_present(FB_C);
}

/*----------------------------------------------------------------------------*/
int game_init(void)
{
    static const u32 fb[2] = { FB_C, FB_D };
    u32 init_cmd0;
    int bi;

    game_reset_state();

    bsp_printf("DEBUG: accessory at (%d,%d), bullet=A\r\n", g.item.x, g.item.y);

    game_perf_init();

    bsp_printf("INIT 1/7: disable display\r\n");
    init_cmd0 = gpu2d_getCmdDone();
    gpu2d_exclusive_begin();

    bsp_printf("INIT 2/7: full-screen mode\r\n");
    lineBuffer_setSplit(0);

    /* 等一个帧边界，让显示侧先锁存"全屏"几何，再开始画。 */
    bsp_printf("INIT 3/7: wait display boundary\r\n");
    bsp_uDelay(20000u);

    bsp_printf("INIT 4/7: draw floor into both buffers\r\n");
    for (bi = 0; bi < 2; ++bi) {
        bsp_printf("INIT 5/7: buffer %d\r\n", bi);
        game_draw_floor(fb[bi]);
    }

    bsp_printf("INIT 6/7: wait GPU\r\n");
    lineBuffer_setFullBase(FB_C, FB_D);
    if (gpu2d_exclusive_end(init_cmd0 + 2u * game_floor_cmd_count()) != 0) {
        bsp_printf("INIT ERROR: initial floor timeout\r\n");
        return 0;
    }

    bsp_printf("INIT 7/7: %dx%d ready, display re-enabled\r\n", SCR_W, SCR_H);
    if (!game_present(FB_C)) return 0;
    return 1;
}
