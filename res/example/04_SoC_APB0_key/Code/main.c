////////////////////////////////////////////////////////////////////////////////
// KEY -> LED Demo     (纯软件:C 里读按键、写 LED,不动 RTL)
//
// 两个外设:
//   KEY   = IO_APB_SLAVE_0         = 0xf8100000   自定义 APB 从设备
//           API 见 driver/user_key.h
//           实现见 peripheral/APB0/key/user_key.c
//   GPIO0 = SYSTEM_GPIO_0_IO_CTRL  = 0xf8015000   SoC 自带 GPIO -> 板载 LED
//
// 按键与 LED 的对应(都是 4 个,一一对应):
//   KEY0 (GPIOL_07) -> LED0 (GPIOR_27)
//   KEY1 (GPIOR_22) -> LED1 (GPIOR_24)
//   KEY2 (GPIOR_21) -> LED2 (GPIOL_P_12)
//   KEY3 (GPIOL_03) -> LED3 (GPIOL_P_02)
//
// 板级按键为低电平有效(外部上拉),key.v 内部已统一转成"按下 = 1"
// 所以读到的 KEY_DATA 里,1 就代表该键被按下。
//
// KEY 外设 API(基址 0xf8100000,译码到槽 0):
//   key_getData()   RO   bit[3:0] 消抖后的按键状态,按下 = 1
//   key_getIrqEn()  RW   bit[3:0] 中断使能
//   key_setIrqEn()  RW   写中断使能
//   key_getId()     RO   恒为 0x4B455930
////////////////////////////////////////////////////////////////////////////////

#include <stdint.h>
#include "bsp.h"
#include "userDef.h"
#include "riscv.h"
#include "plic.h"
#include "gpio.h"
#include "user_key.h"

////////////////////////////////////////////////////////////////////////////////
// LED 点亮极性
//
//   开机有 2 秒自检:先 4 个全写 1(亮 2 秒),再全写 0(灭 2 秒)。
//   看清是哪一段让 LED 亮的,然后把下面改成:
//       0 = 高电平点亮(写 1 亮)
//       1 = 低电平点亮(写 0 亮)
////////////////////////////////////////////////////////////////////////////////
#define LED_ACTIVE_LOW   0

#define LED_DRIVE(x)   ( (LED_ACTIVE_LOW) ? ((~(x)) & 0xF) : ((x) & 0xF) )

// 按键轮询间隔(微秒)。20ms 足够跟手,也不会把串口刷爆。
#define POLL_US          20000

void trap();
void crash();
void trap_entry();
void main();

////////////////////////////////////////////////////////////////////////////////
// 出错就停下,别跑飞
////////////////////////////////////////////////////////////////////////////////
void crash(){
    bsp_printf("\r\n*** CRASH ***\r\n");
    while(1);
}

////////////////////////////////////////////////////////////////////////////////
// 异常/中断总入口,由 ../common/trap.S 的 trap_entry 调用
////////////////////////////////////////////////////////////////////////////////
void trap(){
    int32_t mcause    = csr_read(mcause);
    int32_t interrupt = mcause < 0;
    if(interrupt){
        // 这个 demo 没开中断,走到这里说明有意外
        crash();
    } else {
        // 异常 = 多半是 PSLVERROR 造成的总线错误
        crash();
    }
}

////////////////////////////////////////////////////////////////////////////////
// 主流程
////////////////////////////////////////////////////////////////////////////////
void main(){
    bsp_init();
    bsp_printf("\r\n*** KEY -> LED Demo ***\r\n");

    /* ---------------------------------------------------------------
     * 1. 外设自检:读 KEY_ID
     *    读到 0x4B455930 说明整条 APB 通路是通的
     * ------------------------------------------------------------- */
    u32 id = key_getId();
    bsp_printf("KEY_ID = 0x%x ", id);
    if(id != KEY_ID_VALUE){
        bsp_printf("(expect 0x%x) -> FAIL\r\n", KEY_ID_VALUE);
        crash();
    }
    bsp_printf("OK\r\n");

    /* ---------------------------------------------------------------
     * 2. LED 通路自检 + 极性确认
     * ------------------------------------------------------------- */
    gpio_setOutputEnable(GPIO0, 0xF);

    bsp_printf("LED self-test: write 0xF (2s) ...\r\n");
    gpio_setOutput(GPIO0, LED_DRIVE(0xF));
    bsp_uDelay(2000000);

    bsp_printf("LED self-test: write 0x0 (2s) ...\r\n");
    gpio_setOutput(GPIO0, LED_DRIVE(0x0));
    bsp_uDelay(2000000);

    /* ---------------------------------------------------------------
     * 3. 按键 -> LED 实时跟随
     *    按住 KEYn,LEDn 就亮;松开就灭
     * ------------------------------------------------------------- */
    bsp_printf("Press KEYn -> LEDn lights up.\r\n");
    bsp_printf("        hex    bit3 bit2 bit1 bit0\r\n");

    u32 last = 0xFFFFFFFF;      // 给个不可能的值,保证第一次一定打印
    while(1){
        u32 st = key_getData();

        // 按键状态直接驱动 LED
        gpio_setOutput(GPIO0, LED_DRIVE(st));

        // 状态变了就在串口打一行
        if(st != last){
            bsp_printf("  key = 0x%x    %d    %d    %d    %d\r\n",
                       st,
                       (st >> 3) & 1, (st >> 2) & 1,
                       (st >> 1) & 1, (st >> 0) & 1);
            last = st;
        }

        bsp_uDelay(POLL_US);
    }
}
