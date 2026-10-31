//==============================================================================
//  game_log.h —— 发布构建的日志开关
//  默认关闭长串运行日志；make GAME_LOGS=1 可恢复串口诊断。
//==============================================================================
#pragma once

#ifndef GAME_LOG_ENABLE
#define GAME_LOG_ENABLE 0
#endif

#if GAME_LOG_ENABLE
#include "bsp.h"
#define GAME_LOG(...) bsp_printf(__VA_ARGS__)
#else
#define GAME_LOG(...) ((void)0)
#endif
