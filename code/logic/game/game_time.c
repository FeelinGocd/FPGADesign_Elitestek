// Game time stops during P pause; serial timeouts always use wall time.
#include "game.h"
#include "bsp.h"

static int s_paused;
static u32 s_pause_start, s_pause_elapsed;

u32 game_wall_ms(void)
{
    return (u32)(clint_getTime(BSP_CLINT) / (BSP_CLINT_HZ / 1000u));
}

u32 game_ms(void)
{
    return (s_paused ? s_pause_start : game_wall_ms()) - s_pause_elapsed;
}

int game_paused(void) { return s_paused; }

void game_pause_toggle(void)
{
    u32 wall = game_wall_ms();
    if (s_paused) s_pause_elapsed += wall - s_pause_start;
    else s_pause_start = wall;
    s_paused = !s_paused;
}

void game_time_reset(void)
{
    s_paused = 0;
    s_pause_start = s_pause_elapsed = 0u;
}
