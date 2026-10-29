#pragma once

/* Shared by gameplay and the standalone render queue. */
#ifndef GAME_ENEMY_BULLET_CAP
#define GAME_ENEMY_BULLET_CAP 500
#endif
#define GAME_PLAYER_BULLET_RESERVE 32
#define GAME_BULLET_POOL_CAP (GAME_ENEMY_BULLET_CAP + GAME_PLAYER_BULLET_RESERVE)
#define GAME_RENDER_EXTRA_SLOTS 128
