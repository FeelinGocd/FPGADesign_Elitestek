////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2013-2026 Efinix Inc. All rights reserved.
// Full license header bsp/efinix/EfxSapphireSocRV64/include/LICENSE.MD
////////////////////////////////////////////////////////////////////////////////

//==============================================================================
//  userDef.h —— 应用侧 BSP 配置与板载外设地址
//------------------------------------------------------------------------------
//  干什么      : 选择调试模块/日志级别，并提供 KEY、GPIO0 等应用侧别名。
//  上一级是谁  : src/main.c 及各外设驱动。
//  修改注意    :
//    · 调试日志会占用 UART 带宽，正式性能测量时通常只保留错误级输出；
//    · GPIO 未在 IP Manager 中启用时，下面的编译期检查会主动报错。
//==============================================================================

#include "soc.h"

#ifndef SRC_USERDEF_H_
#define SRC_USERDEF_H_

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------------*/
/* 用户配置
/* -----------------------------------------------------------------------------*/

/* -----------------------------------------------------------------------------*/
/*  用户调试配置
/* -----------------------------------------------------------------------------*/
// --- DEBUG_MODE ---
    // 0 = 关闭断言并移除日志
    // 1 = 开启断言，并按模块/级别过滤日志
#define DEBUG_MODE 1
// --- ACTIVE_DEBUG_MOD --- 可选调试模块
    // DBG_MOD_SYS          // RISCV 扩展模块日志
    // DBG_MOD_IRQ          // 中断与 mtrap 日志
    // DBG_MOD_FAULT        // 系统故障日志
    // DBG_MOD_UART         // UART 驱动日志
    // DBG_MOD_I2C          // I2C 驱动日志
    // DBG_MOD_SPI          // SPI 驱动日志
    // DBG_MOD_SPI_FLASH    // SPI Flash 驱动日志
    // DBG_MOD_RTC          // RTC 驱动日志
    // DBG_MOD_CAM          // CAM 驱动日志
    // DBG_MOD_SENSOR       // 温度传感器日志
    // DBG_MOD_MAIN         // main.c 日志
    // DBG_MOD_ALL          // 全部模块日志
#define ACTIVE_DEBUG_MOD   DBG_MOD_ALL

// --- ACTIVE_MIN_LVL ---
    // DBG_LVL_ALL     0   // 显示 Info、Warn、Error
    // DBG_LVL_WARN    1   // 显示 Warn、Error
    // DBG_LVL_ERR     2   // 只显示 Error
    // DBG_LVL_NONE    3   // 完全静默
#define ACTIVE_MIN_LVL   DBG_LVL_WARN

/* -----------------------------------------------------------------------------*/
/*  APB 外设
/* -----------------------------------------------------------------------------*/
/* KEY 外设: 地址绑定在 peripheral/APB0/key/user_key.h
 *     KEY_BASE = IO_APB_SLAVE_0_INPUT = 0xf8100000 ; KEY = ((KEYType *)KEY_BASE)
 * 用法: KEY->key_data / KEY->key_irq_en / KEY->key_id */

/* 板载 LED: GPIO0 bit3..bit0 -> LED3..LED0
 *   bit0 = GPIOR_27 = LED0 (3.3V)    bit1 = GPIOR_24 = LED1 (3.3V)
 *   bit2 = GPIOL_P_12 = LED2 (1.8V)  bit3 = GPIOL_P_02 = LED3 (1.8V) */
#ifdef SYSTEM_GPIO_0_IO_CTRL
    #define GPIO0   SYSTEM_GPIO_0_IO_CTRL
#else
    #error "GPIO is disabled, please enable it in IP Manager!!!"
#endif


#ifdef __cplusplus
}
#endif // C_plusplus

#endif /* SRC_USERDEF_H_ */
