////////////////////////////////////////////////////////////////////////////////
// KEY -> LED Demo     (纯软件:C 里读按键、写 LED,不动 RTL)
//
// 外设:
//   KEY      = IO_APB_SLAVE_0 + 0x0000 = 0xf8100000   自定义 APB 从设备(槽 0)
//              API 见 peripheral/APB0/key/user_key.h
//   CHECKER  = IO_APB_SLAVE_0 + 0x1000 = 0xf8101000   自定义 APB 从设备(槽 1)
//              API 见 peripheral/APB0/checker/user_checker.h
//              让 FPGA 自己做 DDR 读写自检, CPU 只看结果
//   LINEBUF  = IO_APB_SLAVE_0 + 0x2000 = 0xf8102000   自定义 APB 从设备(槽 2)
//              API 见 peripheral/APB0/lineBuffer/user_lineBuffer.h
//              HDMI 显示通道: 往 DDR 帧缓存灌图 -> 屏上出画面
//   GPIO0    = SYSTEM_GPIO_0_IO_CTRL   = 0xf8015000   SoC 自带 GPIO -> 板载 LED
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
//
// CHECKER 外设 API(基址 0xf8101000,译码到槽 1):
//   checker_getId()      RO   恒为 0x43484B30
//   checker_runOnce(ms)  --   跑一次自检并等结果,返回 1 = 通过
//   checker_getErrCode() RO   0 无错 / 1 不一致 / 2 响应错 / 3 超时 / 4 图案未加载
//   checker_getErrAddr() RO   第一个不一致的地址
//   详细寄存器表见 peripheral/APB0/checker/user_checker.h
//
// LINEBUFFER 外设 API(基址 0xf8102000,译码到槽 2):
//   lineBuffer_getId()                  RO   恒为 0x4C423031
//   lineBuffer_setFullBase(c, d)        RW   全屏两块缓冲基址
//   lineBuffer_enable(1)                RW   开显示(不锁存,写 0 立即停)
//   lineBuffer_setSplit(x)              RW   分屏(帧边界生效)
//   lineBuffer_doneAcc() / doneCpu()    W1S  交帧,下一个帧边界翻页
//   lineBuffer_getFpsAcc()              RO   帧率(没交帧事件时恒 0)
//   详细寄存器表见 peripheral/APB0/lineBuffer/user_lineBuffer.h
////////////////////////////////////////////////////////////////////////////////

#include <stdint.h>
#include "bsp.h"
#include "userDef.h"
#include "riscv.h"
#include "plic.h"
#include "gpio.h"
#include "user_key.h"
#include "user_checker.h"
#include "user_lineBuffer.h"

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

////////////////////////////////////////////////////////////////////////////////
// 帧缓存地址(显示通道用,自己定)
//
//   要求: 5760 字节对齐、地址 < 0x1000_0000(Axi_Mux 只透传 28 位地址)、
//         不要和别的用途撞车。
//   本工程: 程序跑在内部 RAM(0x00001000, 124KB), 不占 DDR;
//           DDR 自检 checker 用 0x0020_0000 起 64KB —— 别撞上。
//
//   每块 6,220,800 字节 = 0x5EEC00:
//       C: 0x0800_0000 ~ 0x085E_EBFF   (上电默认显示这一块)
//       D: 0x0880_0000 ~ 0x08DE_EBFF   (画好了写 DONE 切过去)
////////////////////////////////////////////////////////////////////////////////
#define LB_FRAME_C       0x08000000u
#define LB_FRAME_D       0x08800000u

// 分屏用四个半幅(每块 2.97MB, 4MB 一块, 互不重叠)
//   A/B 是两块完整缓冲; 每块又分 左半(加速器画) / 右半(CPU 画)
#define LB_A_L           0x09000000u
#define LB_A_R           0x09400000u
#define LB_B_L           0x09800000u
#define LB_B_R           0x09C00000u

// 灌几行。
//   上板第一次验证时用 16: 屏上最上面几行是图案、下面还是花屏, 一眼判断地址/行距/像素序;
//   整屏验证就用 LINEBUFFER_HEIGHT(1080), 屏上应该是一整屏干净的图案。
#define LB_TEST_LINES    LINEBUFFER_HEIGHT

////////////////////////////////////////////////////////////////////////////////
// 本轮跑哪种显示测试:
//   6 = 边画边显示 : 分屏下 CPU 只画【右半】(左半是加速器那半, 保持不动),
//                  反复"写后台右半 -> doneCpu", 显示始终开着
//                  验: 右半交替时左半纹丝不动、不撕裂; 读 FPS_CPU 得 CPU 的半屏帧率
//                  ★ 注意: CPU 写入只有 ~17MB/s(指令开销限制), 压不出带宽竞争 ——
//                    真正的带宽压力要等加速器(它能写几百 MB/s)
//   5 = 按键控制 : 全屏(C 标尺)/分屏(A·L红 A·R绿 B·L蓝 B·R黄)两套图都灌好,
//                  然后交给按键: KEY0 切全屏/分屏, KEY1 交帧(acc), KEY2 交帧(cpu),
//                  KEY3 开关 ENABLE; LED3/LED2 常显 ENABLE/SPLIT
//   4 = 分屏专项 : A 左半红 / A 右半绿 / B 左半蓝 / B 右半黄(都带白边);
//                  先全屏 -> 切 SPLIT=1(左红右绿) -> doneAcc(左变蓝) -> doneCpu(右变黄)
//   3 = 翻页专项 : C 标尺图案 / D 纯蓝; 交帧后看屏是不是变纯蓝(且不撕裂),
//                  同时回读 DONE_ACC 和帧率 —— 用来定位"翻页没生效"
//   2 = 标尺模式 : C 灌"标尺"图案(8 根等间距彩色竖线 + 5 根白横线),
//                  用来精确量出显示的水平/垂直映射有没有平移或拉伸
//   1 = 排错模式 : C 只灌"坐标标记"图案(四角块 + 十字中线 + 顶行白线),
//                  静止显示不翻页, 并把 DDR 里的内容读回来打印
//                  用途: 分清"是 DDR 里内容不对" 还是 "显示通路映射不对"
//   0 = 正常验证 : C 全屏渐变 + D 8 竖彩条 + 翻页
////////////////////////////////////////////////////////////////////////////////
#define LB_TEST_MODE     6

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
     * 2. (原 DDR 读写自检) —— DDR 自检 checker 已整块从系统摘除(省 LUT)
     *    APB 槽 1 现在不译码, 访问会得到 PSLVERROR, 所以这里不再读它。
     *    要用回来时: 把 Accelerator_Top 里的 DDR_checker 例化和 Axi_Mux 的 S1
     *    接回去, 并恢复 APB 侧的译码即可。
     * ------------------------------------------------------------- */
#if 0
    /* 原自检代码, 留作参照 */
    u32 cid = checker_getId();
    bsp_printf("DDR_CHK_ID = 0x%x ", cid);
    if(cid != CHECKER_ID_VALUE){
        bsp_printf("(expect 0x%x) -> SKIP\r\n", CHECKER_ID_VALUE);
    } else {
        bsp_printf("OK\r\n");

        /* 刚上电时 DDR 可能还没校准完。
         * RTL 里本模块的复位是 sys_rst & cal_done,校准完成前写 CTRL 无效,
         * 表现为超时(errcode = 3)。等一会儿重试一次就好。 */
        u32 ok = checker_runOnce(500);
        if(!ok && checker_getErrCode() == CHECKER_ERR_TIMEOUT){
            bsp_printf("DDR not ready yet (calibrating?), retry in 200ms ...\r\n");
            bsp_uDelay(200000);
            ok = checker_runOnce(500);
        }

        bsp_printf("DDR self-test: %s", ok ? "PASS" : "FAIL");
        if(!ok){
            bsp_printf("  errcode=%d addr=0x%08x exp=0x%08x act=0x%08x",
                       checker_getErrCode(), checker_getErrAddr(),
                       checker_getErrExp(),  checker_getErrAct());
        }
        bsp_printf("\r\n");
    }
#endif

    /* ---------------------------------------------------------------
     * 3. HDMI 显示通道
     *    灌图 -> 写基址 -> 开使能。灌图时显示是关着的(enable=0),
     *    免得 CPU 写 DDR 和显示取数抢带宽造成闪屏。
     *    判读见文件头的 LB_TEST_MODE 说明。
     * ------------------------------------------------------------- */
    u32 lid = lineBuffer_getId();
    bsp_printf("LINEBUF_ID = 0x%x ", lid);
    if(lid != LINEBUFFER_ID_VALUE){
        bsp_printf("(expect 0x%x) -> SKIP\r\n", LINEBUFFER_ID_VALUE);
    } else {
        bsp_printf("OK\r\n");

        /* ★ 先把状态写成确定值再灌图。
         *   显示通道的复位来自 PLL 锁定信号, 只重下软件(不重配 FPGA)时
         *   它的 APB 寄存器是【保留】的 —— 上电可能带着上一次运行留下的
         *   SPLIT/ENABLE, 会让"这次跑什么"变得不可预期。 */
        lineBuffer_enable(0);
        lineBuffer_setSplit(0);

#if (LB_TEST_MODE == 6)
        /* 边画边显示: 分屏四块 ——
         *   左半(A_L/B_L) = 加速器那半, 灌成固定蓝, 全程不动
         *   右半(A_R/B_R) = CPU 那半, 两块灌成不同色, 靠 doneCpu 交替 */
        bsp_printf("HDMI: fill 分屏四块 (左半蓝固定 / A右红 B右绿) ...\r\n");
        lineBuffer_fillHalf(LB_A_L, LB_TEST_LINES,   0u,   0u, 255u);
        lineBuffer_fillHalf(LB_B_L, LB_TEST_LINES,   0u,   0u, 255u);
        lineBuffer_fillHalf(LB_A_R, LB_TEST_LINES, 255u,   0u,   0u);
        lineBuffer_fillHalf(LB_B_R, LB_TEST_LINES,   0u, 255u,   0u);
        lineBuffer_setSplitBase(LB_A_L, LB_A_R, LB_B_L, LB_B_R);
#elif (LB_TEST_MODE == 5)
        /* 按键控制: 全屏两块 + 分屏四块都灌好, 后面靠按键来回切 */
        bsp_printf("HDMI: fill C(ruler) D(white) A_L(red) A_R(green) B_L(blue) B_R(yellow) ...\r\n");
        lineBuffer_fillRulerPattern(LB_FRAME_C);
        lineBuffer_fillSolid(LB_FRAME_D, LB_TEST_LINES, 255u, 255u, 255u);
        lineBuffer_fillHalf(LB_A_L, LB_TEST_LINES, 255u,   0u,   0u);
        lineBuffer_fillHalf(LB_A_R, LB_TEST_LINES,   0u, 255u,   0u);
        lineBuffer_fillHalf(LB_B_L, LB_TEST_LINES,   0u,   0u, 255u);
        lineBuffer_fillHalf(LB_B_R, LB_TEST_LINES, 255u, 255u,   0u);
        lineBuffer_setSplitBase(LB_A_L, LB_A_R, LB_B_L, LB_B_R);
#elif (LB_TEST_MODE == 4)
        /* 分屏: 四个半幅各灌一种颜色(每幅两端有白边, 用来夹出半幅边界) */
        bsp_printf("HDMI: fill A_L(red) A_R(green) B_L(blue) B_R(yellow) ...\r\n");
        lineBuffer_fillHalf(LB_A_L, LB_TEST_LINES, 255u,   0u,   0u);
        lineBuffer_fillHalf(LB_A_R, LB_TEST_LINES,   0u, 255u,   0u);
        lineBuffer_fillHalf(LB_B_L, LB_TEST_LINES,   0u,   0u, 255u);
        lineBuffer_fillHalf(LB_B_R, LB_TEST_LINES, 255u, 255u,   0u);
        lineBuffer_setSplitBase(LB_A_L, LB_A_R, LB_B_L, LB_B_R);
#elif (LB_TEST_MODE == 0)
        bsp_printf("HDMI: fill C (full-screen gradient) ...\r\n");
        lineBuffer_fillTestPattern(LB_FRAME_C, LB_TEST_LINES);

        bsp_printf("HDMI: fill D (8 color bars) ...\r\n");
        lineBuffer_fillColorBars(LB_FRAME_D, LB_TEST_LINES);
#elif (LB_TEST_MODE >= 2)
        bsp_printf("HDMI: fill C (ruler pattern) ...\r\n");
        lineBuffer_fillRulerPattern(LB_FRAME_C);

        bsp_printf("HDMI: fill D (solid blue, 供对照) ...\r\n");
        lineBuffer_fillSolid(LB_FRAME_D, LB_TEST_LINES, 0u, 0u, 255u);
#else
        bsp_printf("HDMI: fill C (marker pattern) ...\r\n");
        lineBuffer_fillMarkerPattern(LB_FRAME_C);

        /* D 灌成纯蓝: 屏上只要出现蓝色, 就说明显示的是 D 而不是 C */
        bsp_printf("HDMI: fill D (solid blue, 供对照) ...\r\n");
        lineBuffer_fillSolid(LB_FRAME_D, LB_TEST_LINES, 0u, 0u, 255u);
#endif

        lineBuffer_setFullBase(LB_FRAME_C, LB_FRAME_D);

        /* --- 开显示 --- */
        lineBuffer_enable(1);
        bsp_printf("HDMI: C=0x%08x D=0x%08x, %d lines, enable=%d split=%d (showing C)\r\n",
                   LB_FRAME_C, LB_FRAME_D, LB_TEST_LINES,
                   lineBuffer_isEnabled(), lineBuffer_isSplit());
        bsp_uDelay(2000000);

#if (LB_TEST_MODE == 6)
        /* --- CPU 一边画图一边显示 -------------------------------------
         * 正式流程里"画"的一方永远写【后台缓冲】, 画完才交帧 ——
         * 这里用 CPU 扮演"画"的一方, 反复: 写后台 -> 交帧 -> 写后台 ...
         *   显示 C 时写 D; 显示 D 时写 C(靠 acc 索引交替, 和 doneAcc 同步)
         *
         * 屏上期望: 标尺图案 和 纯白 交替, 每次整体一次换完, 【不撕裂】
         * 串口给的 FPS_ACC 就是 CPU 的实际帧率(硬件数的交帧事件/秒)
         *   —— 这个数字直接说明 CPU 软件路径只能做低频更新, 高速出图必须靠加速器
         * ------------------------------------------------------------- */
        /* 切分屏: 左半蓝(不动) + 右半红 */
        lineBuffer_setSplit(1);
        bsp_uDelay(2000000);
        bsp_printf("  屏上应该是: 左半蓝(固定) + 右半红\r\n");

        {
            u32 k, fmax = 0u;
            bsp_printf("HDMI: CPU 只画右半(3.1MB/帧), 8 轮 ...\r\n");
            for(k = 0u; k < 8u; k++){
                u32 f;
                /* 显示 A_R(红) 时写 B_R(绿); 显示 B_R(绿) 时写 A_R(红) */
                if(k & 1u)
                    lineBuffer_fillHalf(LB_A_R, LB_TEST_LINES, 255u,   0u,   0u);
                else
                    lineBuffer_fillHalf(LB_B_R, LB_TEST_LINES,   0u, 255u,   0u);

                lineBuffer_doneCpu();                    /* 只交 CPU 那一路 -> 只翻右半 */
                f = lineBuffer_getFpsCpu();
                if(f > fmax)
                    fmax = f;
                bsp_printf("  #%0d 画完右半并 doneCpu, FPS_CPU=%d\r\n", k, f);
            }
            bsp_printf("  FPS_CPU 峰值 = %d (= CPU 每秒能画几帧【半屏】)\r\n", fmax);
            bsp_printf("  请确认: 右半红/绿交替时, 【左半始终是蓝不动】, 也没有撕裂/闪烁\r\n");
        }
#elif (LB_TEST_MODE == 5)
        bsp_printf("HDMI: 全屏已开(C=标尺图案); 后面用按键切\r\n");
#elif (LB_TEST_MODE == 4)
        /* --- 分屏专项 -------------------------------------------------
         * 现在屏上: 整屏 C(上次全屏图案), 或者上电第一次就是花的 —— 都正常。
         * 切 SPLIT 后(帧边界生效): 左半红 + 右半绿, 中缝在屏幕正中,
         * 左右各能看到两条白边(半幅边界)。
         * 再 doneAcc: 左半变蓝(右半不动); 再 doneCpu: 右半变黄。
         * ★ 这两次是【左右独立翻页】, 看到"只有一半变"才对。 */
        bsp_printf("HDMI: setSplit(1) -> 期望: 左半红 + 右半绿, 中缝在正中\r\n");
        lineBuffer_setSplit(1);
        bsp_uDelay(3000000);
        bsp_printf("HDMI: doneAcc -> 期望: 只左半变蓝(右半还是绿)\r\n");
        lineBuffer_doneAcc();
        bsp_uDelay(3000000);
        bsp_printf("HDMI: doneCpu -> 期望: 右半也变黄\r\n");
        lineBuffer_doneCpu();
        bsp_uDelay(3000000);
        bsp_printf("  现在应该是 左蓝 + 右黄; split=%d\r\n", lineBuffer_isSplit());
#elif (LB_TEST_MODE == 3)
        /* --- 翻页专项: 交帧后屏上应该从"标尺"整屏变"纯蓝", 且不撕裂 --- */
        bsp_printf("HDMI: doneAcc -> 期望屏上变纯蓝(切到 D)\r\n");
        lineBuffer_doneAcc();
        bsp_printf("  DONE_ACC 写完立刻回读 = %d (1 = 写进寄存器了)\r\n",
                   lineBuffer_getDoneAcc());
        bsp_uDelay(3000000);
        /* ★ 决定性探针: DONE_ACC 是粘滞的, 只有 handler 在帧边界翻页消费后才清 0。
         *   所以这一读直接回答"帧边界有没有发生、handler 有没有消费":
         *     读回 0 -> 已被消费 -> 翻页真的发生了
         *     读回 1 -> 还挂着   -> handler 没消费(帧边界那一路有问题) */
        bsp_printf("  3 秒后 DONE_ACC 回读 = %d (0 = 已被 handler 消费 -> 翻页已发生)\r\n",
                   lineBuffer_getDoneAcc());
        /* --- 帧率通路专项 ---------------------------------------------
         * 帧率是"每个 1 秒窗口里发生了几次交帧事件", 而且每窗口只锁存一次 ——
         * 只交一次帧的话, 那个 1 只存在一个窗口, 很容易错过。
         * 所以这里【边连续交帧边读】: 每个窗口里都有事件, 帧率必然非 0。
         *   读到 >=1 -> 通路正常(之前读 0 是采样时机问题)
         *   一直 0  -> 通路真有问题, 要查 handler 的窗口计数
         * 副作用: 屏上会在"标尺 / 纯蓝"之间来回跳, 这是正常的(翻页在连发)。 */
        {
            u32 fmax = 0u, k;
            bsp_printf("HDMI: 连续交帧 5 秒, 边交边读帧率 ...\r\n");
            for(k = 0u; k < 25u; k++){
                u32 f;
                lineBuffer_doneAcc();               /* 每 200ms 交一帧 */
                bsp_uDelay(200000);
                f = lineBuffer_getFpsAcc();
                if(f > fmax)
                    fmax = f;
                bsp_printf("  #%02d FPS_ACC=%d\r\n", k, f);
            }
            bsp_printf("  FPS acc peak = %d\r\n", fmax);
            if(fmax >= 1u)
                bsp_printf("  -> 帧率通路正常: 之前读 0 是「只交一次帧、那个 1 只存在一个窗口」\r\n");
            else
                bsp_printf("  -> 帧率通路异常: 连续交帧也读不到, 要查 handler 的窗口计数\r\n");
        }
        bsp_printf("  屏上现在是纯蓝 = 翻页成功; 还是标尺 = 翻页没生效\r\n");
#elif (LB_TEST_MODE == 2 || LB_TEST_MODE == 1)
        /* 排错模式: 不翻页, 把 DDR 里的内容读回来打一下 ——
         * 确认"写进去的"就是我们以为写进去的。 */
        bsp_printf("  readback 四角: (0,0)=0x%06x (1919,0)=0x%06x (0,1079)=0x%06x (1919,1079)=0x%06x\r\n",
                   lineBuffer_readPixel(LB_FRAME_C, 0, 0),
                   lineBuffer_readPixel(LB_FRAME_C, 1919, 0),
                   lineBuffer_readPixel(LB_FRAME_C, 0, 1079),
                   lineBuffer_readPixel(LB_FRAME_C, 1919, 1079));
        bsp_printf("  对照: 标尺图案竖线在 x=0/256/512/... 横白线在 y=0/270/540/810/1077\r\n");
        bsp_printf("        (959,100) 不在竖线上, (100,540) 在白横线上 —— 黑/白都算对\r\n");
#else
        /* --- 翻页: 交帧 -> 下一个帧边界切到 D --- */
        bsp_printf("HDMI: doneAcc -> flip to D (color bars)\r\n");
        lineBuffer_doneAcc();
        bsp_uDelay(2000000);
        {
            u32 fmax = 0u, k;
            for(k = 0u; k < 30u; k++){
                u32 f = lineBuffer_getFpsAcc();
                if(f > fmax)
                    fmax = f;
                bsp_uDelay(50000);
            }
            bsp_printf("HDMI: FPS acc peak = %d (期望 >=1)\r\n", fmax);
        }
        bsp_printf("HDMI: doneAcc -> flip back to C\r\n");
        lineBuffer_doneAcc();
        bsp_uDelay(2000000);
        bsp_printf("HDMI: now showing C again\r\n");
#endif
    }

    /* ---------------------------------------------------------------
     * 4. LED 通路自检 + 极性确认
     * ------------------------------------------------------------- */
    gpio_setOutputEnable(GPIO0, 0xF);

    bsp_printf("LED self-test: write 0xF (2s) ...\r\n");
    gpio_setOutput(GPIO0, LED_DRIVE(0xF));
    bsp_uDelay(2000000);

    bsp_printf("LED self-test: write 0x0 (2s) ...\r\n");
    gpio_setOutput(GPIO0, LED_DRIVE(0x0));
    bsp_uDelay(2000000);

    /* ---------------------------------------------------------------
     * 5. 按键控制显示(上板调试用)
     *     KEY0 -> 全屏 / 分屏 切换
     *     KEY1 -> 交帧(acc): 全屏翻 C<->D, 分屏翻左半
     *     KEY2 -> 交帧(cpu): 分屏翻右半
     *     KEY3 -> ENABLE 开关(关掉立刻停取数, 再开会等下一个帧边界起取数)
     *    LED3 = ENABLE, LED2 = SPLIT(其余两位不用)
     *
     *    要点(屏上对照着看):
     *      切模式 → 下一个帧边界整体换几何, 切换瞬间不该撕裂
     *      交帧   → 下一个帧边界翻页, 画面整体一次换完
     *      关使能 → 立刻停在当前行, 屏上保持最后一帧不动
     * ------------------------------------------------------------- */
    bsp_printf("\r\n按键: KEY0=全屏/分屏  KEY1=交帧(acc)  KEY2=交帧(cpu)  KEY3=ENABLE开关\r\n");
    bsp_printf("LED: LED3=ENABLE  LED2=SPLIT\r\n");

    gpio_setOutputEnable(GPIO0, 0xF);

    u32 last = 0xFFFFFFFF;      // 给个不可能的值,保证第一次一定打印
    while(1){
        u32 st   = key_getData();
        u32 down = st & ~last;  // 软件做沿检测: 只认"刚按下"那一下

        if(down & 0x1u){                            /* KEY0: 切全屏/分屏 */
            lineBuffer_setSplit(lineBuffer_isSplit() ? 0u : 1u);
            bsp_printf("KEY0 -> split=%d (下一个帧边界生效)\r\n", lineBuffer_isSplit());
        }
        if(down & 0x2u){                            /* KEY1: 交帧(acc) */
            lineBuffer_doneAcc();
            bsp_printf("KEY1 -> doneAcc, DONE_ACC 立刻回读=%d\r\n", lineBuffer_getDoneAcc());
        }
        if(down & 0x4u){                            /* KEY2: 交帧(cpu) */
            lineBuffer_doneCpu();
            bsp_printf("KEY2 -> doneCpu\r\n");
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
