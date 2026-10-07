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
#include "demo_anim.h"       /* 动图 Demo: 双缓冲 + 脏区局部重放 */
#include "demo_split.h"      /* 分屏对比 Demo: 左半硬件 / 右半纯 CPU (基础要求 3c) */
#include "demo_glass.h"      /* alpha 玻璃背景 Demo: 玻璃用 alpha 平铺满屏(临时占位) */

/*------------------------------------------------------------------------------
 * 跑哪个 Demo(改这一行就切换):
 *   0 = 全屏动图: 一张 sprite 素材左右跑(双缓冲 + 脏区 + ck + 镜像)
 *   1 = 分屏对比: 左半加速器 / 右半纯 CPU, 同一组方块比帧率(基础要求 3c)
 *   2 = alpha 玻璃背景: 玻璃素材用 alpha 混合平铺满屏 + 最左一条 ck 硬边对照
 *----------------------------------------------------------------------------*/
#define DEMO_RUN_SEL     2

/* LED 点亮极性: 0 = 高电平点亮(写 1 亮) / 1 = 低电平点亮(写 0 亮) */
#define LED_ACTIVE_LOW   0

#define LED_DRIVE(x)   ( (LED_ACTIVE_LOW) ? ((~(x)) & 0xF) : ((x) & 0xF) )

// 按键轮询间隔(微秒)。20ms 足够跟手,也不会把串口刷爆。
#define POLL_US          20000

/* 帧缓存地址(显示通道用, 自己定): 要求 5760 字节对齐、地址 < 0x1000_0000(Axi_Mux 只透传
 * 28 位)、不和别的用途撞车。程序跑在内部 RAM(0x00001000, 124KB), 不占 DDR。
 * 每块 6,220,800 字节 = 0x5EEC00:
 *   C: 0x0800_0000 ~ 0x085E_EBFF (上电默认显示这块)  D: 0x0880_0000 ~ 0x08DE_EBFF */
#define LB_FRAME_C       0x08000000u
#define LB_FRAME_D       0x08800000u

// 分屏用四个半幅(每块 2.97MB, 4MB 一块, 互不重叠)
//   A/B 是两块完整缓冲; 每块又分 左半(加速器画) / 右半(CPU 画)
#define LB_A_L           0x09000000u
#define LB_A_R           0x09400000u
#define LB_B_L           0x09800000u
#define LB_B_R           0x09C00000u

// 铺几行: 只看最上面几行时可用 16(便于判断地址/行距/像素序), 整屏就是 LINEBUFFER_HEIGHT
#define LB_TEST_LINES    LINEBUFFER_HEIGHT

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
//   ② 动图 Demo : 显示通道初始化 + demo_anim_run()
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
         *   素材那边由 res/func/pic2pram.py 保证: 透明像素 = 调色板第 0 项 = 纯白,
         *   且【不透明区里没有纯白】(转换时会自检并打印)。 */
        lineBuffer_setCkeyColor(0x00FFFFFFu);

        /* 底图: C / D 都铺背景色(动图 Demo 自己往上画); 分屏四块也备好,
         *   这样按 KEY0 切到分屏时不会看到花屏 */
        bsp_printf("HDMI: C/D 铺底色 + 分屏四块 ...\r\n");
        lineBuffer_fillSolid(LB_FRAME_C, LB_TEST_LINES, 0u, 0u, 0u);
        lineBuffer_fillSolid(LB_FRAME_D, LB_TEST_LINES, 0u, 0u, 0u);
        lineBuffer_fillHalf(LB_A_L, LB_TEST_LINES,   0u,   0u, 255u);   /* A 左 = 蓝 */
        lineBuffer_fillHalf(LB_B_L, LB_TEST_LINES,   0u, 255u, 255u);   /* B 左 = 青 */
        lineBuffer_fillHalf(LB_A_R, LB_TEST_LINES, 255u,   0u,   0u);   /* A 右 = 红 */
        lineBuffer_fillHalf(LB_B_R, LB_TEST_LINES,   0u, 255u,   0u);   /* B 右 = 绿 */
        lineBuffer_setSplitBase(LB_A_L, LB_A_R, LB_B_L, LB_B_R);

        lineBuffer_setFullBase(LB_FRAME_C, LB_FRAME_D);

        /* --- 开显示 --- */
        lineBuffer_enable(1);
        bsp_printf("HDMI: C=0x%08x D=0x%08x, %d lines, enable=%d split=%d (showing C)\r\n",
                   LB_FRAME_C, LB_FRAME_D, LB_TEST_LINES,
                   lineBuffer_isEnabled(), lineBuffer_isSplit());
        bsp_printf("CKEY: 键色=0x%06x (相等 = 不写)  [0x2078]\r\n", lineBuffer_getCkeyColor());
        bsp_uDelay(2000000);

        /* --- 开"帧起始"中断: 让 CPU 在硬件帧边界那一刻被叫醒 -------------------
         *   这一版 ISR 先【只计数】: 用来确认"中断确实每个显示帧来一次"。
         *   确认之后再把这帧的绘制搬进 ISR。 */
#if IRQ_FRAME_USE
        IRQ_frame_init();
        bsp_printf("IRQ: 帧起始中断已开(PLIC 号 %d), 先只看计数\r\n",
                   SYSTEM_PLIC_USER_INTERRUPT_A_INTERRUPT);
#else
        bsp_printf("IRQ: 帧起始中断【关闭】(IRQ_FRAME_USE=0):\r\n");
        bsp_printf("     0x2070/0x2074 是新 bitstream 才有的寄存器, 现在碰它会总线错误起不来。\r\n");
        bsp_printf("     重新综合并烧好 FPGA 之后, 把 IRQ_frame.h 里的 IRQ_FRAME_USE 改成 1。\r\n");
#endif

        /* --- Demo(见文件头的 DEMO_RUN_SEL 开关) ---------------------------
         *   0) 全屏动图: 双缓冲 + 脏区"局部重放"(规范 docs/spec/软件_驱动与API.md §6)
         *   1) 分屏对比: 左半硬件/右半 CPU(基础要求 3c) —— 它会自己切到 split
         *   2) alpha 玻璃背景: 玻璃用 alpha 平铺满屏, 最左一条用 ck 做对照
         *   ★ 演示期间别按 KEY0(切分屏会换翻页几何) / KEY3(会把显示关掉) */
#if   (DEMO_RUN_SEL == 1)
        demo_split_run();
#elif (DEMO_RUN_SEL == 2)
        demo_glass_run();
#else
        demo_anim_run();
#endif
    }

    /* ================= ③ 按键 + LED =================
     *   KEY0 = 全屏/分屏(下一个帧边界换几何, 不该撕裂)
     *   KEY1/KEY2 = 交帧 acc/cpu(下一个帧边界翻页, 画面整体一次换完)
     *   KEY3 = ENABLE 开关(关掉立刻停在当前行, 屏上保持最后一帧不动)
     *   LED3 = ENABLE, LED2 = SPLIT(其余两位不用) */
    bsp_printf("\r\n按键: KEY0=全屏/分屏  KEY1=交帧(acc)  KEY2=交帧(cpu)  KEY3=ENABLE开关\r\n");
    bsp_printf("LED: LED3=ENABLE  LED2=SPLIT\r\n");

    gpio_setOutputEnable(GPIO0, 0xF);

    u32 last = 0xFFFFFFFF;      // 给个不可能的值,保证第一次一定打印
    while(1){
        u32 st   = key_getData();
        u32 down = st & ~last;  // 软件做沿检测: 只认"刚按下"那一下

        if(down & 0x1u){                            /* KEY0: 切全屏/分屏 */
            u32 spl = lineBuffer_isSplit() ? 0u : 1u;
            lineBuffer_setSplit(spl);
            bsp_printf("KEY0 -> split=%d (下一个帧边界生效)\r\n", spl);

            /* ★ 必须等硬件把新模式锁进去再画: isSplit() 读的是【待生效】值, setSplit() 后
             *   软件立刻是新值, 而硬件要到【下一个帧边界】才换几何; 中间这十几毫秒两边跨距
             *   不一致, 软件按新跨距算地址、硬件按旧跨距逐行递推, 块会被抹成两列错行。
             *   等 20ms > 一帧(16.7ms) 即可。 */
            bsp_uDelay(20000);

            /* 切完模式按【新模式】重画一块, 验"加速器跟着显示模式走": 基址是软件给的、硬件
             *   只补跨距(5760 <-> 2880), 所以必须软件重画才看得出对错。判据: 分屏下跨距若
             *   对不上, 块会被抹成斜条纹而不是矩形。
             * 两个候选缓冲都画, 不用管当前显示哪一块; 全屏那支不画任何东西 —— 标记块会被
             * 动图当成"花屏残留"(脏区永远碰不到它), 所以没有 else。 */
            if(spl){
                CI_gpu2d_fillRect(LB_A_L, 300u, 200u, 200u, 100u, 0x0000FF00u);
                CI_gpu2d_fillRect(LB_B_L, 300u, 200u, 200u, 100u, 0x0000FF00u);
            }
        }
        if(down & 0x2u){                            /* KEY1: 交帧(acc) */
            lineBuffer_doneGpu2d();
            bsp_printf("KEY1 -> doneGpu2d, 0x206C 立刻回读=%d\r\n", lineBuffer_getDoneGpu2d());
        }
        if(down & 0x4u){                            /* KEY2: 原功能=交帧(cpu) */
            /* ★ 暂时屏蔽: 动图 Demo 现在用 KEY2 做"暂停/继续", 这条先关掉。
             *   要恢复就把下面两行解开。 */
            /* lineBuffer_doneCpu();
               bsp_printf("KEY2 -> doneCpu\r\n"); */
        }
        if(down & 0x8u){                            /* KEY3: 开关使能 */
            u32 en = lineBuffer_isEnabled() ? 0u : 1u;
            lineBuffer_enable(en);
            bsp_printf("KEY3 -> enable=%d\r\n", en);
        }

        // LED: 高两位常显状态
        gpio_setOutput(GPIO0, LED_DRIVE((lineBuffer_isEnabled() ? 0x8u : 0x0u) |
                                        (lineBuffer_isSplit()   ? 0x4u : 0x0u)));

        // 按键状态变了就在串口打一行(保留原来的观测)
        if(st != last){
            bsp_printf("  key = 0x%x\r\n", st);
            last = st;
        }

        bsp_uDelay(POLL_US);
    }
}
