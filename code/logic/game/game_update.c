//==============================================================================
//  game_update.c —— 一帧的模型更新（只改状态，不画）
//------------------------------------------------------------------------------
//  干什么      : 玩家 / 生命 / Boss / 子弹 / 怪 / 炸弹 / 金币 / 商店按固定顺序推进
//  注意        : 这里【不调】game_room_step()——换房在更新之后、绘制之前单独判
//==============================================================================
#include "game.h"

void game_update_step(u32 keys, u32 now)
{
    if (game_perf_frozen()) return;

#if GAME_BOSS_BULLET_SWEEP
    if (game_boss_stress_step()) {
        /* The test owns the bullets, but PAD movement and the Boss camera
         * must keep running so the visible-load test covers scrolling. */
        game_player_step(keys, now);
        game_camera_step();
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

    /* Restart is performed by the main loop, which first releases any
     * framebuffer still queued for the next display boundary. */
    if (g.phase == PHASE_GAMEOVER && (int)(now - g.gameover_until) >= 0) return;

    if (g.phase != PHASE_GAMEOVER && !game_vital_dead(now))
        game_player_step(keys, now);
    game_camera_step();
    game_boss_step(now);
    game_bullets_step(now);
    game_mobs_step(now);
    game_bombs_step(now);
    game_coins_step(now);
    game_shop_step(now);
}
