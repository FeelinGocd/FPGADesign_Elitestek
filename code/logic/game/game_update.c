//==============================================================================
//  game_update.c —— 一帧的模型更新（只改状态，不画）
//------------------------------------------------------------------------------
//  干什么      : 玩家 / 生命 / Boss / 子弹 / 怪 / 炸弹 / 金币 / 商店按固定顺序推进
//  注意        : 这里【不调】game_room_step()——换房在更新之后、绘制之前单独判
//==============================================================================
#include "game.h"

void game_update_step(u32 keys, u32 aim_keys, u32 now)
{
    if (game_perf_frozen()) return;

#if GAME_BOSS_BULLET_SWEEP
    if (game_boss_stress_step()) {
        /* 压测接管弹幕，但手柄移动和 Boss 相机仍需运行，
         * 这样可见负载测试同时覆盖画面滚动。 */
        game_player_step(keys, aim_keys, now);
        game_camera_step();
#if GAME_CAPACITY_AUDIT
        game_capacity_camera(now);
#endif
        game_bullets_stress_step();
        return;
    }
#endif

    if (game_take_spawn_item() && g.phase != PHASE_GAMEOVER)
        game_item_spawn_at_player();
    if (game_take_bomb() && g.phase != PHASE_GAMEOVER)
        game_bomb_place(now);

    game_vital_step(now);
    if (game_vital_over()) {
        game_vital_clear_over();
        game_enter_gameover(now);
        return;
    }

    /* 重开由主循环执行；主循环会先释放仍在等待下一次显示边界的帧缓存。 */
    if (g.phase == PHASE_GAMEOVER && (int)(now - g.gameover_until) >= 0) return;

    if (g.phase != PHASE_GAMEOVER && !game_vital_dead(now))
        game_player_step(keys, aim_keys, now);
    game_camera_step();
    game_boss_step(now);
    game_bullets_step(now);
    game_mobs_step(now);
    game_bombs_step(now);
    game_coins_step(now);
    game_shop_step(now);
}
