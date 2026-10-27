//==============================================================================
//  game_demo.h —— 游戏 demo 的对外入口
//------------------------------------------------------------------------------
//  干什么      : 只暴露一个 game_demo_run()，实现在 game/ 目录下按功能拆开。
//  上一级是谁  : src/main.c（DEMO_RUN_SEL == 2）
//  接口契约    : 阻塞运行，直到内部错误或压测跑完才返回；正常游戏是死循环。
//  为什么这样拆:
//    · 原来 737 行全在 game_demo.c 里，改一处要翻全篇。现在按
//      状态 / 世界 / HUD / 角色 / 子弹 / Boss / 道具 / 主循环 分成 8 个 .c，
//      共用声明都在 game/game.h。
//  目录        : game/game.h       共用宏 / 类型 / 全局状态 g / 各模块接口
//                game/game_state.c 状态定义 + 时间 + 初始化 + 上屏
//                game/game_world.c 地板瓦片 + 脏区 + 可见性裁剪
//                game/game_hud.c   HUD 标签 / 数字
//                game/game_player.c 角色移动 / 动画 / 开火
//                game/game_bullet.c 子弹池
//                game/game_boss.c  Boss 绘制 + 弹幕
//                game/game_item.c  道具绘制 + 拾取
//                game/game_main.c  game_demo_run() 主循环
//  参考        : docs/spec/软件_驱动与API.md §6 / §8
//==============================================================================
#pragma once

#include "type.h"

/* 阻塞跑一局游戏（四方向移动 + 自动开火 + Boss 弹幕 + 道具拾取 + 脏区重画 + HUD） */
void game_demo_run(void);
