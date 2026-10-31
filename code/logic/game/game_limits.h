//==============================================================================
//  game_limits.h —— 子弹池与渲染队列的共享容量
//------------------------------------------------------------------------------
//  干什么      : 给玩法、渲染队列和诊断工具提供同一组固定容量。
//  上一级是谁  : game/game.h、game/render_list.h
//  接口契约    :
//    · 敌方弹默认上限 500，另有 32 个槽位只留给玩家弹；
//    · 默认敌弹上限 500，N 增加 25，B 减少 25，范围 500～600；
//    · 渲染队列再为 HUD、道具等静态元素预留额外槽位。
//  修改注意    :
//    · 这是 RAM 预算和队列容量的共同来源，修改后必须做整机编译与压测；
//    · ENEMY_CAP 是开局上限；实际分配使用 game_enemy_bullet_limit()。
//==============================================================================
#pragma once

/* 玩法、渲染队列和容量诊断共用这一组容量。 */
#ifndef GAME_ENEMY_BULLET_CAP
#define GAME_ENEMY_BULLET_CAP 500
#endif
#define GAME_PLAYER_BULLET_RESERVE 32
#ifndef GAME_ENEMY_BULLET_MAX
#define GAME_ENEMY_BULLET_MAX 600
#endif
#if GAME_ENEMY_BULLET_MAX < GAME_ENEMY_BULLET_CAP || GAME_ENEMY_BULLET_MAX + GAME_PLAYER_BULLET_RESERVE > 1024
#error "Runtime bullet maximum must include the initial cap and fit 1024 slots"
#endif
#define GAME_BULLET_LIMIT_STEP 25
#define GAME_BULLET_POOL_MAX (GAME_ENEMY_BULLET_MAX + GAME_PLAYER_BULLET_RESERVE)
#define GAME_BULLET_POOL_CAP GAME_BULLET_POOL_MAX
#ifndef GAME_RENDER_EXTRA_SLOTS
#define GAME_RENDER_EXTRA_SLOTS 192
#endif
