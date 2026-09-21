////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2013-2026 Efinix Inc. All rights reserved.
// Full license header bsp/efinix/EfxSapphireSocRV64/include/LICENSE.MD
////////////////////////////////////////////////////////////////////////////////

#include "soc.h"

#ifndef SRC_USERDEF_H_
#define SRC_USERDEF_H_

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------------*/
/* User Configuration                         	        	
/* -----------------------------------------------------------------------------**/

/* -----------------------------------------------------------------------------*/
/*  USER DEBUG CONFIGURATION
/* -----------------------------------------------------------------------------*/
// --- DEBUG_MODE --- 
    // 0 = Asserts OFF, Logs removed
    // 1 = Asserts ON, Logs filtered
#define DEBUG_MODE 1
// --- ACTIVE_DEBUG_MOD --- This is the list of available module to debug
    // DBG_MOD_SYS          // Enable Log for RISCV Extension
    // DBG_MOD_IRQ          // Enable Log for IRQ, mtrap
    // DBG_MOD_FAULT        // Enable Log for System Fault  
    // DBG_MOD_UART         // Enable Log for UART Driver
    // DBG_MOD_I2C          // Enable Log for I2C Driver   
    // DBG_MOD_SPI          // Enable Log for SPI Driver
    // DBG_MOD_SPI_FLASH    // Enable Log for SPI FLASH Driver
    // DBG_MOD_RTC          // Enable Log for RTC Driver
    // DBG_MOD_CAM          // Enable Log for CAM Driver
    // DBG_MOD_SENSOR       // Enable Log for Sensor (temp sensor)
    // DBG_MOD_MAIN         // Enable Log for main.c
    // DBG_MOD_ALL          // Enable all Logs
#define ACTIVE_DEBUG_MOD   DBG_MOD_ALL

// --- ACTIVE_MIN_LVL ---
    // DBG_LVL_ALL     0   // Show Info, Warn, Error
    // DBG_LVL_WARN    1   // Show Warn, Error
    // DBG_LVL_ERR     2   // Show Error only
    // DBG_LVL_NONE    3   // Silence
#define ACTIVE_MIN_LVL   DBG_LVL_WARN

/* -----------------------------------------------------------------------------*/
/*  APB 外设
/* -----------------------------------------------------------------------------*/
/* KEY 外设的地址绑定在 driver/user_key.h:
 *     KEY_BASE = IO_APB_SLAVE_0_INPUT = 0xf8100000
 *     KEY      = ((KEYType *)KEY_BASE)
 * 用法: KEY->key_data / KEY->key_irq_en / KEY->key_id
 */

/* 板载 LED:soc_gpio[3:0] -> LED3..LED0
 *   GPIO0 bit0 = GPIOR_27 = LED0 (3.3V)
 *   GPIO0 bit1 = GPIOR_24 = LED1 (3.3V)
 *   GPIO0 bit2 = GPIOL_P_12 = LED2 (1.8V)
 *   GPIO0 bit3 = GPIOL_P_02 = LED3 (1.8V)
 */
#ifdef SYSTEM_GPIO_0_IO_CTRL
    #define GPIO0   SYSTEM_GPIO_0_IO_CTRL
#else
    #error "GPIO is disabled, please enable it in IP Manager!!!"
#endif


#ifdef __cplusplus
}
#endif // C_plusplus

#endif /* SRC_USERDEF_H_ */
