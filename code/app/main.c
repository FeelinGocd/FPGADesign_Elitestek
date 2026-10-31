////////////////////////////////////////////////////////////////////////////////
// 2D 加速器 + HDMI 显示通道 Demo  (纯软件:C 里操作,不动 RTL)
//
// 外设基址(都在 IO_APB_SLAVE_0 = 0xf8100000 上):
//   KEY     槽0 +0x0000 = 0xf8100000  按键(低有效, 内部已转成"按下 = 1")
//                                     API: peripheral/APB0/key/user_key.h
//   GPU2D   槽2 +0x2000 = 0xf8102000  只读观测口: 忙/命令数/帧数/写读节拍/忙周期
//                                     API: peripheral/APB0/gpu2d/user_gpu2d.h
//   LINEBUF 槽2 +0x2040 = 0xf8102040  HDMI 显示通道: 往 DDR 帧缓存灌图 -> 屏上出画面
//                                     API: peripheral/APB0/lineBuffer/user_lineBuffer.h
//         槽 2 里两个 64B 子块由 PADDR[6] 分: 0x2000 观测 / 0x2040 显示。
//   2D 加速器【不靠 APB 下命令】: 命令走自定义指令 CI(cfu_push)
//                                     API: peripheral/CInstruction/CI_gpu2d.h
//         加速器【写】帧缓存, 显示通道【读】同一块内存, 两者是搭档;
//         帧缓存地址必须 < 0x1000_0000(Axi_Mux 只透传 28 位)。
//   GPIO0   SYSTEM_GPIO_0_IO_CTRL = 0xf8015000  SoC 自带 GPIO -> 板载 LED(见 userDef.h)
//
// (原 DDR 自检 CHECKER 已整块摘除: 它的槽 1 已空, 驱动文件保留但没人调用 —— 访问它会走
//  兜底分支返回 PSLVERROR=1)
////////////////////////////////////////////////////////////////////////////////

#include <stdint.h>
#include "bsp.h"
#include "userDef.h"
#include "riscv.h"
#include "gpio.h"
#include "user_key.h"
#include "user_lineBuffer.h"
#include "CI_gpu2d.h"
#include "user_gpu2d.h"      /* APB 槽 2: 命令完成计数 / 带宽观测 */
#include "compare_demo.h"   /* 分屏对比 Demo: 左半加速器 / 右半纯 CPU（基础要求 3c） */
#include "game_demo.h"      /* 游戏 demo: WASD 移动 + 小键盘瞄准 + 子弹池 + Boss + GUI */
#include "framebuffer_layout.h"

/*------------------------------------------------------------------------------
 * 启动时先跑哪个 Demo（运行中按 M 可在两者之间切换）:
 *   1 = 分屏对比（compare_demo.c）: 左半加速器 / 右半纯 CPU，同一组方块比帧率（基础要求 3c）
 *   2 = 游戏 demo（game/ 目录）: WASD 移动 + 小键盘瞄准 + 子弹池 + Boss + 道具 + GUI
 *   M = 串口虚拟手柄或文本命令 M + 回车：运行中切换 Demo
 *   ★ 2026-10-17：老的 demo_anim（动图）/ demo_combine（图片合成）已删除 —— 它们的功能
 *     都被游戏 demo 覆盖（动图→游戏动画、合成→道具/角色叠层），留着只占 flash。
 *----------------------------------------------------------------------------*/
#ifndef DEMO_RUN_SEL
#define DEMO_RUN_SEL     2       /* make DEMO_RUN_SEL=1 可单独构建分屏对比 */
#endif
#if DEMO_RUN_SEL != 1 && DEMO_RUN_SEL != 2
#error "DEMO_RUN_SEL must be 1 (compare) or 2 (game)"
#endif

/* LED 点亮极性: 0 = 高电平点亮(写 1 亮) / 1 = 低电平点亮(写 0 亮) */
#define LED_ACTIVE_LOW   0

#define LED_DRIVE(x)   ( (LED_ACTIVE_LOW) ? ((~(x)) & 0xF) : ((x) & 0xF) )

// 按键轮询间隔(微秒)。20ms 足够跟手,也不会把串口刷爆。
#define POLL_US          20000

#include "IRQ_frame.h"

void trap();          /* 定义在 src/IRQ_frame.c */
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

//  说明: trap() 已经搬到 src/IRQ_frame.c —— 那里要处理"帧起始中断", 不能再一律 crash

////////////////////////////////////////////////////////////////////////////////
// 主流程:
//   ① 初始化自测: 读回 4 个外设 ID, 一次把【APB 总线 / 槽位译码 / CI】三条通路查完
//   Demo 入口：GPU/CPU 对比或游戏 demo
//   之后进主循环: 按键控制显示 / LED 反映状态
//   (原来那 7 种上板自测的代码已移除, 留档在 docs/spec/软件_驱动与API.md §7)
////////////////////////////////////////////////////////////////////////////////
void main(){
    bsp_init();
    IRQ_trapInstall();       /* 先把陷阱入口装上: 后面任何总线错误都能打印出来, 不会打到重启 */
    bsp_printf("\r\n*** 2D 加速器 + 显示通道 ***\r\n");

    /* ================= ① 初始化自测: 4 个外设 ID =================
     *   任一不对就停下来并打印查错方向 —— 后面"画不出来"时能先排除 总线/槽位/CI。 */
    {
        u32 id_key = key_getId();               /* APB 槽 0            */
        u32 id_lb  = lineBuffer_getId();        /* 槽 2 显示子块 0x2040 */
        u32 id_gpu = gpu2d_getId();             /* 槽 2 观测子块 0x2000 */
        u32 id_ci  = CI_gpu2d_checkId();        /* CI 通道             */

        bsp_printf("自检 KEY_ID     = 0x%08x (期望 0x%08x) %s\r\n",
                   id_key, KEY_ID_VALUE, (id_key == KEY_ID_VALUE) ? "OK" : "FAIL");
        bsp_printf("自检 LINEBUF_ID = 0x%08x (期望 0x%08x) %s\r\n",
                   id_lb, LINEBUFFER_ID_VALUE, (id_lb == LINEBUFFER_ID_VALUE) ? "OK" : "FAIL");
        bsp_printf("自检 GPU2D_ID   = 0x%08x (期望 0x%08x) %s\r\n",
                   id_gpu, GPU2D_ID_VALUE, (id_gpu == GPU2D_ID_VALUE) ? "OK" : "FAIL");
        bsp_printf("自检 CI_ID      = 0x%08x (期望 0x%08x) %s\r\n",
                   id_ci, GPU2D_CI_ID, (id_ci == GPU2D_CI_ID) ? "OK" : "FAIL");

        if (id_key != KEY_ID_VALUE || id_lb != LINEBUFFER_ID_VALUE ||
            id_gpu != GPU2D_ID_VALUE || id_ci != GPU2D_CI_ID) {
            bsp_printf("  查错: APB0_Decoder 槽位(槽0/槽2) + APB0_Interconnect 回程;\r\n");
            bsp_printf("        槽 2 内两个子块由 PADDR[6] 分: 0x2000 观测 / 0x2040 显示;\r\n");
            bsp_printf("        CI 读到 0 或全 F ⇒ cpu0_customInstruction_* 没接上 / CI 时钟复位\r\n");
            crash();
        }
        bsp_printf("自检全部通过\r\n");
    }

    /* ================= ② 功能: 显示初始化 + 动图 Demo =================
     *   灌图时显示是关着的(enable=0), 免得 CPU 写 DDR 和显示取数抢带宽造成闪屏。 */
    {
        /* ★ 先把状态写成确定值再灌图: 显示通道的复位来自 PLL 锁定信号, 只重下软件
         *   (不重配 FPGA)时它的 APB 寄存器是【保留】的, 可能带着上次运行留下的
         *   SPLIT/ENABLE, 会让"这次跑什么"变得不可预期。 */
        lineBuffer_enable(0);
        lineBuffer_setSplit(0);

        /* 颜色键控(ck)的键色: 明确写一次(复位默认就是白 0x00FFFFFF)。
         *   画精灵时"等于这个键色的像素【不写】"—— 那些位置保留帧缓存里原有的内容,
         *   所以精灵外面不会再盖一层白框, 也不用"先擦背景再压精灵"。
         *   素材那边由 tools/img2hex/pic2pram.py 保证: 透明像素 = 调色板第 0 项 = 纯白,
         *   且【不透明区里没有纯m'm'm'm白】(转换时会自检并打印)。 */
        lineBuffer_setCkeyColor(0x00FFFFFFu);

        /* 两种 Demo 的入口都会在自身初始化流程中重新配置全屏/分屏基址。
         * 这里只统一关闭显示并设置确定状态，避免复用上一次运行残留的配置。 */

        /* --- 开"帧起始"中断: 通知主循环显示边界 -------------------
         *   ISR 只清事件并计数；游戏逻辑按独立时间步更新，绘制和交帧留在主循环。
         *   显示基址须在边界前写好，不能等 ISR 进入后才提交。 */
        #if IRQ_FRAME_USE
                IRQ_frame_init();
                bsp_printf("IRQ: 帧起始中断已开(PLIC 号 %d), 先只看计数\r\n",
                        SYSTEM_PLIC_USER_INTERRUPT_A_INTERRUPT);
        #else
                bsp_printf("IRQ: 帧起始中断【关闭】(IRQ_FRAME_USE=0):\r\n");
                bsp_printf("     0x2070/0x2074 是新 bitstream 才有的寄存器, 现在碰它会总线错误起不来。\r\n");
                bsp_printf("     重新综合并烧好 FPGA 之后, 把 IRQ_frame.h 里的 IRQ_FRAME_USE 改成 1。\r\n");
        #endif

        /* 从这里开始由 Demo 接管显示和输入。M 请求会让当前 Demo 返回，
         * 主循环随后完整初始化另一套流程；帧缓存、全屏/分屏模式由各 Demo
         * 的初始化路径重新配置。 */
        {
            int active_demo = DEMO_RUN_SEL;
            for (;;) {
                int switch_requested;

                if (active_demo == 1) {
                    switch_requested = compare_demo_run();
                    if (!switch_requested) break;
                    active_demo = 2;
                } else {
                    switch_requested = game_demo_run();
                    if (!switch_requested) break;
                    active_demo = 1;
                }
                bsp_printf("DEMO: active=%s (M to switch)\r\n",
                           active_demo == 1 ? "GPU/CPU compare" : "game");
            }
        }
    }

    /* 输入由 Demo 接管，这里不再运行旧的帧切换主循环。 */
    while(1) bsp_uDelay(1000000u);
}
