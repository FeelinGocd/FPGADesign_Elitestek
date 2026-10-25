//==============================================================================
//  compare_demo.c —— 分屏帧率对比: 左半【硬件加速器】 / 右半【纯 CPU 逐像素】
//------------------------------------------------------------------------------
//  对应赛题基础要求 3c:
//      "提供一个基础测试 Demo(例如: 大量方块的随机移动 ...), 并能在屏幕上
//       实时对比显示【纯软件 CPU 渲染】与【硬件加速渲染】的帧率差异"
//
//  画面
//      分屏半幅 = 640 x 720（可见宽度 640 = 720p 的 1280 的一半）。
//      ★ 行距仍是 2880 字节（= 960 像素画布）：720p 只收窄了可见窗口，
//        帧缓冲物理布局沿用 1080p 时代的跨距，见 docs/spec/硬件_显示通路.md §13。
//        所以：可见 x 范围 0..639，画布 x 范围 0..959，多余部分不读不写。
//      左半(A_L/B_L) 由 2D 加速器用 FILL 命令画; 右半(A_R/B_R) 由 CPU 逐字节写。
//      两边跑【同一个方块数组、同一条运动轨迹】—— 只有"谁在画"不同。
//
//  公平性(答辩一定会问, 这几条就是答案)
//      1. 方块数 / 尺寸 / 位置 / 速度完全相同: 同一个模型, 同一帧的位置。
//      2. 一帧要写的像素数完全相同: 都是"擦 N 个旧矩形 + 画 N 个新矩形"。
//      3. 右边不偷懒: volatile 逐字节写, 不用 memcpy、不靠编译器优化。
//      4. 两边各自独立交帧(doneGpu2d / doneCpu), 显示侧按各自的索引翻页
//         => 帧率是自然拉开的, 不是人为限速。
//
//  为什么右半要【分片】(本 Demo 最关键的一处设计)
//      右半若一帧一次性画完, CPU 会被独占上百毫秒; 这段时间左边连命令都发不出去
//      => 左边也被拖到和 CPU 一样快, 对比就没了。
//      所以把"一帧的 2N 个矩形"拆开, 每个显示帧只做 K 个:
//          擦 48 个 = 6 个显示帧, 画 48 个 = 6 个显示帧  => 一整帧 12 个显示帧
//      => 右半显示帧率 = 60 / 12 = 5 fps, 左半照旧 60 fps。
//      ★ 每帧 CPU 花的钱必须远小于 16.7ms, 否则一样拖累左边 —— 串口会打印实测值。
//
//  脏区为什么这样擦(双缓冲下的坑, 见 docs/spec/软件_驱动与API.md §6.2)
//      后台缓冲里装的是【这块缓冲上一次被画时】的内容, 不是"上一帧"的内容。
//      所以擦的是【逐缓冲各记一份的位置】(fx[2][N] / rx[2][N]), 不是上一帧的位置。
//      顺序: 先擦完所有旧位置 -> 再画全部方块(不管这一帧动没动);
//      这样方块互相重叠也一定对 —— 被擦掉的那块会被后一次绘制补回来。
//
//  地址必须与 main.c 里的 LB_A_* / LB_B_* 一致(那四个宏在 main.c 内部, 没进头文件)。
//==============================================================================
#include "compare_demo.h"
#include "CI_gpu2d.h"
#include "user_gpu2d.h"
#include "user_lineBuffer.h"
#include "assets_desc.h"        /* 素材表 g_assets[] (pic2pram.py 生成) */
#include "vexriscv.h"           /* data_cache_invalidate_all: 写完帧缓存要写回 cache */
#include "user_key.h"           /* KEY1/KEY2: 现场开关(诊断用) */
#include "bsp.h"

/*------------------------------------------------------------------------------
 * 分屏半幅几何与地址(★ 和 main.c 的 LB_A_L/LB_A_R/LB_B_L/LB_B_R 必须一致)
 *----------------------------------------------------------------------------*/
#define SPLIT_HALF_W        640                 /* 可见宽：1280 / 2（720p） */
#define SPLIT_HALF_H        720                 /* 可见高：720p 的 720 行 */
#define SPLIT_LINE_BYTES    2880u               /* 画布行距仍是 960*3（RTL 分屏段跨距 = 5760/2） */
#define SPLIT_A_L           0x09000000u
#define SPLIT_A_R           0x09400000u
#define SPLIT_B_L           0x09800000u
#define SPLIT_B_R           0x09C00000u

/*------------------------------------------------------------------------------
 * 画面参数
 *----------------------------------------------------------------------------*/
/* ★★ 元素个数/尺寸改成【运行时变量】(g_blk_n / g_blk_w / g_blk_h),
 *    这样同一份代码能一档一档往上加, 不用来回编译。数组按 MAX 开。 */
#if SPLIT_MODE_LADDER
#define SPLIT_BLK_MAX       1024                /* 测量台架: 数组要按阶梯最高一档开 */
#else
#define SPLIT_BLK_MAX       64                  /* 正常 demo: 48 个元素, 留点余量 */
#endif
#define SPLIT_CMP_N         48                  /* 对比模式: 元素个数 */
#define SPLIT_CMP_W         64                  /* 对比模式: 元素尺寸 */
#define SPLIT_CMP_H         64
#define SPLIT_LAD_W         16                  /* 加压模式: 元素尺寸(接近弹幕子弹) */
#define SPLIT_LAD_H         16
#define SPLIT_SPEED_MIN     2                   /* 每帧位移 2~4 像素 */
#define SPLIT_SPEED_MAX     4
#define SPLIT_BG_COLOR      0x00101418u         /* 底色(深灰蓝) */

/* 右半每显示帧做几个矩形(擦/画各算一个) —— 调这个就是调右半的帧率
 *   8  -> 一帧 96 个矩形 = 12 个显示帧 -> 约 5 fps
 *   16 -> 6 个显示帧 -> 约 10 fps(CPU 每帧更吃力, 注意别超过 16.7ms) */
#define SPLIT_CPU_OPS_PER_FRAME 8

/*------------------------------------------------------------------------------
 * 屏幕上的帧率数字(两位, 用 PRAM 里的数字素材画, 每位 1 条 sprite 命令)
 *   ★ 素材编号: pic2pram.py 按【--dir 给的顺序】编号 —— 先 character 后 base/num,
 *     所以 0/1 是跑步帧, 2..11 是数字 '0'..'9'。
 *   ★ 数字按【图层】处理(见 docs/spec/软件_驱动与API.md §5.2): 它是最上面的一层
 *     (LAYER_HUD = 192), 每帧的画法固定成四步:
 *         ① 擦全部元素的旧位置   ② 擦数字框(层 0 的背景)
 *         ③ 按 (层号, id) 顺序画全部元素   ④ 画数字(永远最后)
 *     ⇒ HUD 被擦掉也无所谓, 第 ④ 步每帧都会补回来 —— 所以
 *       【方块/子弹可以从数字下面随便穿过, 不再需要"避开活动区"】。
 *     代价只有每帧 4 条命令, 可以忽略。
 *----------------------------------------------------------------------------*/
#define SPLIT_DIGIT_ASSET0  ASSET_num0_16x24    /* 素材表里数字 '0' 的下标（用生成的名字，别写数字）*/
/*  16×8 小数字加入后，16×24 在素材表里不再连续 ⇒ 显式表取大号数字。  */
static const int s_split_digits[10] = {
    ASSET_num0_16x24, ASSET_num1_16x24, ASSET_num2_16x24, ASSET_num3_16x24,
    ASSET_num4_16x24, ASSET_num5_16x24, ASSET_num6_16x24, ASSET_num7_16x24,
    ASSET_num8_16x24, ASSET_num9_16x24
};
#define SPLIT_INFO_X        20
#define SPLIT_INFO_Y        20
#define SPLIT_INFO_DW       16                  /* 数字素材宽（2026-10-12 重绘成 16x24）*/
#define SPLIT_INFO_DH       24                  /* 数字素材高 */
#define SPLIT_INFO_GAP      2                   /* 两位之间留 2 像素 */
#define SPLIT_INFO_STRIP_W  (2 * SPLIT_INFO_DW + SPLIT_INFO_GAP)
/* 左半每帧的命令数 = 擦 N + 擦数字框 2 + 画 N + 画数字 2 + FENCE 1 */
#define SPLIT_L_CMDS_PER_FRAME  (2 * g_blk_n + 5)

/* 屏幕上那两个帧率数字多久刷一次(帧) —— 60 帧 = 1 秒@60fps
 *   ★ 窗口必须是"右半一整帧(12 个显示帧)"的整数倍, 否则整数计数会让数字来回跳:
 *     以前窗口 30 帧里只装得下 30/13 ≈ 2.3 个 CPU 帧 ⇒ 屏幕在 4 和 6 之间跳
 *     (真实平均是 4.6, 不是限速也不是坏掉 —— 是量化)。现在 60/12 = 5 正好。
 *   ★ 以前只在"一档跑完"时更新, 换档后屏幕上会挂着上一档的旧数字(比如 30)整整 2 秒,
 *     看着像"帧率显示坏了"。现在改成按固定窗口刷新 + 换档立刻清零。 */
#define SPLIT_SHOW_EVERY    60

#define SPLIT_FRAMES        1800                /* 跑多少帧(60fps 约 30 秒) */
#define SPLIT_REPORT_EVERY  60                  /* 每多少帧打一行统计 */

/*------------------------------------------------------------------------------
 * ★★ 跑哪个: 0 = 【正常方案】左右对比(左半硬件 / 右半 CPU)   ← 默认
 *             1 = 阶梯加压(测量台架, 平时不用)
 *   加压模式: 元素数按下面的阶梯逐档往上加, 每档跑 SPLIT_LEVEL_FRAMES 帧,
 *             【只跑左半(加速器)】—— 右半的 CPU 分片每帧要占 ~8ms, 会把
 *             "硬件到底能跑多快"测歪, 所以加压档里把它停掉(右半保持一张静态图)。
 *   每档结束打一行: 元素数 / 实测命令数 / 实测 fps / 忙周期 / 写字节数。
 *   (已经在 16x16 上量过一轮: 1024 个会撞带宽墙; 详细结论见 docs/spec)
 *----------------------------------------------------------------------------*/
#define SPLIT_MODE_LADDER   0
#define SPLIT_LEVEL_FRAMES  120                 /* 每档跑多少显示帧(≈2 秒) */

/* 阶梯: 元素个数 + 元素尺寸(只在 SPLIT_MODE_LADDER=1 时用) */
typedef struct { int n, w, h; } lad_t;
#define SPLIT_LADDER_NUM    7
static const lad_t g_ladder[SPLIT_LADDER_NUM] = {
    {  32, 16, 16 }, {  64, 16, 16 }, { 128, 16, 16 }, { 256, 16, 16 },
    { 512, 16, 16 }, {1024, 16, 16 },
    { 256, 32, 32 }        /* 尺寸对照: 同命令数、4 倍面积 */
};
#define TICKS_PER_MS        (BSP_CLINT_HZ / 1000u)
#define TICKS_PER_US        (BSP_CLINT_HZ / 1000000u)

/* 右半"一整帧"要几个显示帧 = 擦 ceil(N/K) 片 + 画 ceil(N/K) 片
 *   ★ 交帧【不单独占一片】: 画完最后一片的当帧就交(见 right_slice 的 CS_DRAW)。
 *     48 个 / 每片 8 个 = 6 + 6 = 12 个显示帧 ⇒ 60/12 = 5 fps。
 *   ★ 这个数最好能整除 SPLIT_SHOW_EVERY, 屏幕上的数字才不会来回跳。 */
#define SPLIT_CPU_FRAME_TICKS \
    (((2 * g_blk_n) + SPLIT_CPU_OPS_PER_FRAME - 1) / SPLIT_CPU_OPS_PER_FRAME)

/*==============================================================================
 * 状态机
 *==============================================================================*/
/* 顶层: 一个状态做一件事, 做完设下一个状态 */
typedef enum {
    ST_INIT = 0,        /* 建模型 + 切分屏 + 等几何生效 */
    ST_PAINT_L,         /* 开局: 左半 A_L / B_L 各整帧画一遍(加速器) */
    ST_PAINT_R,         /* 开局: 右半 A_R / B_R 各整帧画一遍(CPU, 一次性) */
    ST_MOVE,            /* 每显示帧: 模型走一步(左右共用同一份位置) */
    ST_L_FRAME,         /* 左半: 发这一帧的 2N+1 条命令 + 交帧 */
    ST_R_SLICE,         /* 右半: 推进 CPU 侧子状态机一片 */
    ST_WAIT,            /* 等这一个显示帧过去(锁 60Hz 节拍) */
    ST_REPORT,          /* 每 SPLIT_REPORT_EVERY 帧统计一次 */
    ST_KEY,             /* 现场开关: KEY1 = 信息带开关, KEY2 = 方块避让开关 */
#if SPLIT_MODE_LADDER
    ST_LV_SETUP,        /* 加压档: 换一档(重新撒点 + 两块重画 + 计数快照) */
    ST_LV_REPORT,       /* 加压档: 一档跑完出数 */
#endif
    ST_DONE
} split_state_t;

/* 右半(CPU 侧)子状态机: 一"帧" = 擦一遍 + 画一遍 + 交帧 */
typedef enum {
    CS_ERASE = 0,       /* 擦 N 个旧位置, 每片 K 个 */
    CS_DRAW             /* 画 N 个方块(用冻结快照的位置), 每片 K 个;
                         *   ★ 画完最后一片的当帧就把"写回 cache -> 交帧 -> 翻缓冲 ->
                         *     冻结下一帧快照"一起做掉(以前它是单独一个 CS_FLUSH 片)。
                         *     这样一整帧正好 12 个显示帧 = 5 fps, 屏幕数字才稳得住。 */
} cpu_state_t;

/*==============================================================================
 * 数据
 *==============================================================================*/
/* 层号(见 docs/spec/软件_驱动与API.md §5.1): 0 = 最底, 越大越上, 间隔 32 便于插层 */
#define LAYER_MAP           0
#define LAYER_GROUND_MARK   8
#define LAYER_PROP_BACK     32
#define LAYER_ACTOR         64
#define LAYER_PROP_FRONT    96
#define LAYER_FX            128
#define LAYER_HUD           192
#define LAYER_DEBUG         224

typedef struct {
    int x, y;               /* 当前位置(半幅坐标) */
    int vx, vy;             /* 速度(像素/帧, 撞边反向) */
    u32 color;

    u8  layer;              /* ★ 层号: 决定"谁盖谁"(不再靠数组顺序) */
    u16 id;                 /* ★ 稳定排序用; 创建时分配, 之后永不改变 */

    /* 位置历史【跟着元素走】, 不是按数组下标存 ——
     * 这样元素表被打乱(shuffle)之后, 擦除仍然擦在正确的位置。 */
    int fx[2], fy[2];       /* 左半: 每块缓冲各记一份"上次画它时它在哪" */
    int rx[2], ry[2];       /* 右半: 同上 */
    int sx, sy;             /* 右半: 这一帧开始时冻结的位置快照(分片期间不许变) */
} blk_t;

static blk_t g_b[SPLIT_BLK_MAX];

/* 绘制顺序: 按 (层号, id) 升序排好的【元素下标表】, 由 layer_order_rebuild() 重建 */
static int   g_order[SPLIT_BLK_MAX];

/* 运行时状态 */
static split_state_t g_st;
static cpu_state_t   g_cs;
static int   g_l_idx;                   /* 左半正在画哪块(0=A_L 1=B_L) */
static int   g_r_idx;                   /* 右半正在画哪块(0=A_R 1=B_R) */
static int   g_cs_cnt;                  /* 本子状态里已经做了几个矩形 */
static int   g_blk_n;                   /* 当前这一档的元素个数 */
static int   g_blk_w, g_blk_h;          /* 当前这一档的元素尺寸 */
static int   g_lv;                      /* 加压档: 现在在第几档 */

static u32   g_lv_frames;               /* 本档已经跑了几帧 */
static u32   g_lv_t0;                   /* 本档开始时的 CLINT */
static u32   g_lv_cmd0, g_lv_wr0, g_lv_busy0;  /* 本档开始时的加速器计数 */

static u32   g_rnd = 0x1234ABCDu;
static u32   g_frame;                   /* 顶层帧计数(每显示帧 +1) */
static u32   g_win;                     /* 本统计窗内的帧数 */
static u32   g_l_frames, g_r_frames;    /* 左右各自交帧次数 */
static u32   g_win_r_frames;
static u32   g_t_prev, g_t_total;
static u32   g_cpu_sum, g_cpu_max;      /* CPU 每帧耗时(us) */
static u32   g_show_l, g_show_r;        /* 屏幕上正在显示的两个帧率值 */
static u32   g_show_t0;                 /* 屏幕数字这一窗的起点(CLINT) */
static u32   g_show_win;                /* 屏幕数字这一窗已经过了几帧 */
static u32   g_show_r_prev;             /* 上一窗末尾的 g_r_frames(算右半帧率用) */

/* ---- 显示给哪一块(相位免疫用) ----
 *   ★★ 为什么需要: 硬件每帧按内部索引 acc_idx/cpu_idx 在 A/B 之间二选一,
 *      而这两个索引【软件读不到】, 开机初值还可能被上一次运行(或主循环里的
 *      KEY1 手动翻页)弄脏 —— 一旦和软件的假设差一位, 软件就会一直在画
 *      【正在显示的那一块】, 表现是方块上半截被擦掉还没重画就扫过去了
 *      (而且重新下载 bitstream 后又会"自己好", 因为 FPGA 复位把索引清 0 了)。
 *   ★ 解法: 把 A/B 两个基址槽【都写成同一块】, 索引取哪个都一样 ——
 *      显示哪一块完全由软件说了算, 与开机初值无关。 */
static u32   g_l_disp, g_r_disp;        /* 左边/右边下一步要显示的那块 */

/* ---- 现场开关(诊断用, 按 KEY1/KEY2 切) ---- */
static int   g_info_on    = 1;          /* 0 = 不画信息带(数字) */
static int   g_freeze     = 0;          /* 1 = 方块冻住不走(场景静止, 用来看"还闪不闪") */
static u32   g_key_prev   = 0xFFFFFFFFu;

static const u32 g_pal[8] = {
    0x00FF4040u, 0x0040FF40u, 0x004040FFu, 0x00FFFF40u,
    0x00FF40FFu, 0x0040FFFFu, 0x00FFA040u, 0x00A0A0FFu
};

/*==============================================================================
 * 小工具
 *==============================================================================*/
static u32 rnd(void)
{
    g_rnd ^= g_rnd << 13;
    g_rnd ^= g_rnd >> 17;
    g_rnd ^= g_rnd << 5;
    return g_rnd;
}

static int rnd_range(int lo, int hi)
{
    return lo + (int)(rnd() % (u32)(hi - lo + 1));
}

/*------------------------------------------------------------------------------
 * CPU 侧: 逐字节填一个矩形(volatile 逐字节, 不许被优化成 memset/memcpy)
 *   坐标是半幅坐标(0..959 / 0..1079), base 是半幅基址。
 *----------------------------------------------------------------------------*/
static void cpu_fill(u32 base, int x, int y, int w, int h, u32 color)
{
    u8  cb = (u8)( color        & 0xFFu);
    u8  cg = (u8)((color >>  8) & 0xFFu);
    u8  cr = (u8)((color >> 16) & 0xFFu);
    int xx, yy;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SPLIT_HALF_W) w = SPLIT_HALF_W - x;
    if (y + h > SPLIT_HALF_H) h = SPLIT_HALF_H - y;
    if (w <= 0 || h <= 0) return;

    for (yy = 0; yy < h; yy++) {
        volatile u8 *p = (volatile u8 *)(base + (u32)(y + yy) * SPLIT_LINE_BYTES
                                              + (u32)x * 3u);
        for (xx = 0; xx < w; xx++) {
            p[0] = cb;
            p[1] = cg;
            p[2] = cr;
            p += 3;
        }
    }
}

/* HUD 的两步(定义在下面, 左半/右半都要用, 先声明) */
static void info_erase(u32 fb);
static void info_digits(u32 fb, u32 v);

/*==============================================================================
 * 左半(硬件): 三件事各一个函数, 由 ST_L_FRAME 一次调用完
 *==============================================================================*/
/* 整帧画好一块缓冲(开局用) */
static void left_paint_full(u32 back, int idx)
{
    int i;

    CI_gpu2d_fillRect(back, 0u, 0u, SPLIT_HALF_W, SPLIT_HALF_H, SPLIT_BG_COLOR);
    for (i = 0; i < g_blk_n; i++)
        CI_gpu2d_fillRect(back, (u32)g_b[i].x, (u32)g_b[i].y,
                          g_blk_w, g_blk_h, g_b[i].color);
    for (i = 0; i < g_blk_n; i++) {
        g_b[i].fx[idx] = g_b[i].x;
        g_b[i].fy[idx] = g_b[i].y;
    }
    /*  ★★ 这里【绝对不能发 FENCE】: FENCE 完成会让硬件置 DONE_ACC,
     *     显示就会在下一个帧边界白白多翻一页 —— 软件以为"正在显示 A_L",
     *     实际显示的是 B_L, 于是整局都在画【正在显示的那一块】(撕裂)。
     *     因为写命令紧跟帧边界下发(扫描线在屏幕顶部), 现象就是"上方那一段
     *     是新帧、下面是旧帧", 方块一到上方就跳/闪。
     *     exclusive_end() 等的是 CMD_DONE, 不发 FENCE 一样能等画完。 */
}

/* 把"该显示哪块"写进显示通道的两个槽(分屏用 A/B 四个半幅槽) */
static void display_commit(void)
{
    lineBuffer_setSplitBase(g_l_disp, g_r_disp, g_l_disp, g_r_disp);
}

/* 一帧: 擦 N 个旧位置 -> 画全部方块 -> 记住新位置 -> FENCE -> 交帧 */
static void left_frame(void)
{
    u32 back = g_l_idx ? SPLIT_B_L : SPLIT_A_L;
    int i;

    /* ---------------- ① 擦旧位置 ----------------
     *   ★ 擦的是"【这块缓冲】上次画它时它在哪"(g_l_fx/g_l_fy), 不是"上一帧的位置":
     *     双缓冲的后台缓冲里装的是这块缓冲上一次被画时的内容。
     *   ★ 擦完再把【全部】元素按当前位置重画一遍 ⇒ 元素互相重叠也一定对。
     *   ★ 试过两种"少写字"的擦法(只擦旧−新露出来的一圈 / 擦旧∪新的包围盒),
     *     实测都不划算: 差集带写字只省 14% 却让命令数涨 49%; 包围盒面积反而更大、
     *     写字多 35%。**就保留最朴素的"整个旧矩形"。** (数据见 docs/spec) */
    for (i = 0; i < g_blk_n; i++) {                     /* ① 擦旧位置(顺序无关) */
        int e = g_order[i];
        CI_gpu2d_fillRect(back,
                          (u32)g_b[e].fx[g_l_idx], (u32)g_b[e].fy[g_l_idx],
                          g_blk_w, g_blk_h, SPLIT_BG_COLOR);
    }

    info_erase(back);                                   /* ② 擦数字框(层 0 的背景) */

    /* ③ 画全部元素 —— ★ 按 (层号, id) 顺序, 不是数组顺序 */
    for (i = 0; i < g_blk_n; i++) {
        int e = g_order[i];
        CI_gpu2d_fillRect(back,
                          (u32)g_b[e].x, (u32)g_b[e].y,
                          g_blk_w, g_blk_h, g_b[e].color);
        g_b[e].fx[g_l_idx] = g_b[e].x;                      /* ③ 记住这块缓冲现在是什么 */
        g_b[e].fy[g_l_idx] = g_b[e].y;
    }

    info_digits(back, g_show_l);                        /* ④ 画数字(层 192, 永远最上) */

    CI_gpu2d_fence();
    /* ★ 绝不在这里写 lineBuffer_doneGpu2d(): FENCE 完成时【硬件自己置位 DONE_ACC】
     *   (APB0_GPU_2D.v: acc_evt = acc_frame_done | 软件写1)。软件抢在前面写 1,
     *   显示会在帧边界翻到"还没画完的那块" => 撕裂。 */

    g_l_disp = back;                        /* ★ 下一帧边界显示这块(两个槽都写它) */
    display_commit();
    g_l_idx ^= 1;                                           /* 下一帧画另一块 */
    g_l_frames++;
}

/*==============================================================================
 * 右半(CPU): 状态推进
 *==============================================================================*/
/* 整帧画好一块缓冲(开局用, 不分片 —— 反正还没开始跑) */
static void right_paint_full(u32 base, int idx)
{
    int i;

    cpu_fill(base, 0, 0, SPLIT_HALF_W, SPLIT_HALF_H, SPLIT_BG_COLOR);
    for (i = 0; i < g_blk_n; i++) {
        cpu_fill(base, g_b[i].x, g_b[i].y, g_blk_w, g_blk_h, g_b[i].color);
        g_b[i].rx[idx] = g_b[i].x;
        g_b[i].ry[idx] = g_b[i].y;
    }
    data_cache_invalidate_all();        /* ★ 写回 D-cache, 否则显示读出器读不到 */
}

/* 推进 CPU 子状态机一片(每显示帧调一次) */
static void right_slice(void)
{
    u32 base = g_r_idx ? SPLIT_B_R : SPLIT_A_R;
    int k, e;

    switch (g_cs) {
        case CS_ERASE:
            /* ★ 新的一帧(本片是擦除阶段的第一片): 先把数字框擦成背景(层 0),
             *   这样后面的方块才能盖在它上面; 数字本体在"画完最后一片的当帧"里最后画。 */
            if (g_cs_cnt == 0) info_erase(base);
            for (k = 0; k < SPLIT_CPU_OPS_PER_FRAME; k++) {
                if (g_cs_cnt >= g_blk_n) break;
                e = g_order[g_cs_cnt];
                cpu_fill(base,
                         g_b[e].rx[g_r_idx], g_b[e].ry[g_r_idx],
                         g_blk_w, g_blk_h, SPLIT_BG_COLOR);
                g_cs_cnt++;
            }
            if (g_cs_cnt >= g_blk_n) { g_cs = CS_DRAW; g_cs_cnt = 0; }
            break;

        case CS_DRAW:
            for (k = 0; k < SPLIT_CPU_OPS_PER_FRAME; k++) {
                if (g_cs_cnt >= g_blk_n) break;
                e = g_order[g_cs_cnt];                  /* ★ 按 (层号, id) 顺序画 */
                cpu_fill(base,
                         g_b[e].sx, g_b[e].sy,
                         g_blk_w, g_blk_h, g_b[e].color);
                g_cs_cnt++;
            }
            /* ★ 画完最后一片的【当帧】就把交帧做掉(以前它是单独一片 CS_FLUSH):
             *     擦 6 片 + 画 6 片 = 12 个显示帧 ⇒ 右半稳定 5 fps,
             *     而且 1 秒窗口(60 帧)里正好 5 帧 ⇒ 屏幕数字稳稳显示 5, 不再 4/6 乱跳。
             *   ★ 交帧提前到当帧是安全的: 像素早就写完了(cache 先 invalidate),
             *     显示端只在【帧边界】翻页, 所以最多提前一个帧边界看到它, 不会撕裂。
             *   ★ 数字(层 192)这批命令由加速器执行, 它在下一个帧边界之前画完;
             *     万一晚一拍, 屏幕上显示的还是上一帧那两位数字(值本来也一样), 看不出来。 */
            if (g_cs_cnt >= g_blk_n) {
                data_cache_invalidate_all();    /* 写回 cache: CPU 的像素先落盘 */
                info_digits(base, g_show_r);
                lineBuffer_doneCpu();           /* 这一整帧画完了 -> 交帧 */
                g_r_disp = base;                /* ★ 下一帧边界显示这块(两个槽都写它) */
                display_commit();
                g_r_frames++;
                g_win_r_frames++;
                for (k = 0; k < g_blk_n; k++) {  /* 这块缓冲现在的样子 = 快照 */
                    g_b[k].rx[g_r_idx] = g_b[k].sx;
                    g_b[k].ry[g_r_idx] = g_b[k].sy;
                }
                g_r_idx ^= 1;                        /* 下一帧画另一块 */
                for (k = 0; k < g_blk_n; k++) {  /* 冻结下一帧的位置快照 */
                    g_b[k].sx = g_b[k].x;
                    g_b[k].sy = g_b[k].y;
                }
                g_cs = CS_ERASE;
                g_cs_cnt = 0;
            }
            break;
    }
}

/*==============================================================================
 * 屏幕上的帧率数字(PRAM 素材 + ck, 每位 1 条命令)
 *==============================================================================*/
/* HUD 的第①步(层 0 = 背景): 把两个数字框擦成底色
 *   ★ 必须画在【元素之前】—— 它是背景层, 方块要能盖在它上面;
 *   ★ 数字的位置是固定的两个框(素材本身就是 32x48, 字形不会超出框),
 *     所以"擦两个框"就够了, 不用跟踪上一次显示的是什么值。 */
static void info_erase(u32 fb)
{
    if (!g_info_on) return;
    CI_gpu2d_fillRect(fb, SPLIT_INFO_X, SPLIT_INFO_Y,
                      SPLIT_INFO_DW, SPLIT_INFO_DH, SPLIT_BG_COLOR);
    CI_gpu2d_fillRect(fb, SPLIT_INFO_X + SPLIT_INFO_DW + SPLIT_INFO_GAP, SPLIT_INFO_Y,
                      SPLIT_INFO_DW, SPLIT_INFO_DH, SPLIT_BG_COLOR);
}

/* HUD 的第②步(层 192 = 最上): 画两位数字
 *   ★ 必须画在【全部元素之后】—— 这就是"HUD 永远在最上"的实现;
 *     被方块擦掉也无所谓, 这一遍每帧都会补回来。
 *   ★ 用 ck 画: 字形以外的像素不写 ⇒ 底下的方块能透出来, 不会留一个方块洞。 */
static void info_digits(u32 fb, u32 v)
{
    const asset_desc_t *a1;
    const asset_desc_t *a0;

    if (!g_info_on) return;
    a1 = &g_assets[s_split_digits[(int)((v / 10u) % 10u)]];
    a0 = &g_assets[s_split_digits[(int)(v % 10u)]];

    CI_gpu2d_spriteRectCK(a1->base_word, a1->stride_word, fb,
                          SPLIT_INFO_X, SPLIT_INFO_Y, a1->width, a1->height);
    CI_gpu2d_spriteRectCK(a0->base_word, a0->stride_word, fb,
                          SPLIT_INFO_X + SPLIT_INFO_DW + SPLIT_INFO_GAP, SPLIT_INFO_Y,
                          a0->width, a0->height);
}

/*==============================================================================
 * 模型
 *==============================================================================*/
static void blocks_init(void)
{
    int i;

    g_rnd = 0x1234ABCDu;                    /* 固定种子 => 每次上电画面一样, 好复现 */
    for (i = 0; i < g_blk_n; i++) {
        g_b[i].x     = rnd_range(0, SPLIT_HALF_W - g_blk_w);
        g_b[i].y     = rnd_range(0, SPLIT_HALF_H - g_blk_h);   /* ★ 不再避开信息带 */
        g_b[i].vx    = (rnd() & 1u) ?  rnd_range(SPLIT_SPEED_MIN, SPLIT_SPEED_MAX)
                                    : -rnd_range(SPLIT_SPEED_MIN, SPLIT_SPEED_MAX);
        g_b[i].vy    = (rnd() & 1u) ?  rnd_range(SPLIT_SPEED_MIN, SPLIT_SPEED_MAX)
                                    : -rnd_range(SPLIT_SPEED_MIN, SPLIT_SPEED_MAX);
        g_b[i].color = g_pal[i & 7u];
        g_b[i].id    = (u16)i;                  /* 稳定排序用, 之后永不改变 */
        /*  ★ 层号故意和数组下标【交错】: 这样"数组顺序 ≠ 绘制顺序",
         *    一眼就能看出层号真在管排序(把元素表打乱后画面必须逐像素不变)。 */
        g_b[i].layer = (i & 1) ? LAYER_FX : LAYER_ACTOR;
    }
}

/* 按 (层号升序, id 升序) 重建绘制顺序表
 *   ★ 元素表被打乱(shuffle)之后必须重调 —— 顺序表里存的是【下标】, 不是元素本身。
 *   ★ 插入排序: N 只有几十个, 成本可忽略; 判据里带 id 是为了"同层内先后稳定"。 */
static void layer_order_rebuild(void)
{
    int i, j, key;

    for (i = 0; i < g_blk_n; i++) g_order[i] = i;

    for (i = 1; i < g_blk_n; i++) {
        key = g_order[i];
        j   = i - 1;
        while (j >= 0 &&
               ((g_b[g_order[j]].layer >  g_b[key].layer) ||
                (g_b[g_order[j]].layer == g_b[key].layer &&
                 g_b[g_order[j]].id    >  g_b[key].id))) {
            g_order[j + 1] = g_order[j];
            j--;
        }
        g_order[j + 1] = key;
    }
}

/* 把元素表【打乱】(元素自身带着层号/id/位置/历史一起走)
 *   ★ 打乱之后画面必须【逐像素不变】: 绘制顺序由 (层号, id) 决定, 与数组怎么摆无关。
 *     这是"层号真的在管排序"的现场验证(按 KEY0)。 */
static void blocks_shuffle(void)
{
    int i, j;
    blk_t t;

    for (i = g_blk_n - 1; i > 0; i--) {
        j = (int)(rnd() % (u32)(i + 1));
        t = g_b[i]; g_b[i] = g_b[j]; g_b[j] = t;
    }
    layer_order_rebuild();
}

/* 设定这一档的元素个数/尺寸, 然后重新随机撒一遍(固定种子 => 可复现) */
static void blocks_setup(int n, int w, int h)
{
    g_blk_n = n;
    g_blk_w = w;
    g_blk_h = h;
    blocks_init();
    layer_order_rebuild();              /* ★ 元素表建好/换档之后都要重建顺序表 */
}

/*  一维弹跳：走一步，撞边就贴边并反向（x / y 两个轴共用）。  */
static void bounce(int *pos, int *vel, int lo, int hi)
{
    *pos += *vel;
    if (*pos < lo) { *pos = lo; *vel = -*vel; }
    if (*pos > hi) { *pos = hi; *vel = -*vel; }
}

static void blocks_step(void)
{
    int i;

    for (i = 0; i < g_blk_n; i++) {
        bounce(&g_b[i].x, &g_b[i].vx, 0, SPLIT_HALF_W - g_blk_w);
        bounce(&g_b[i].y, &g_b[i].vy, 0, SPLIT_HALF_H - g_blk_h);   /* ★ 走满全高 */
    }
}

/* 锁到显示帧边界(两步, 顺序不能反):
 *   ① 等这一帧的 FENCE 真的画完 -> 硬件置起 DONE_ACC
 *   ② 等显示在帧边界把它消费掉 -> 刚过了一个帧边界
 * 少了 ①, 第一帧会在 FENCE 还没完成时就往下走, 节拍对不上显示。 */
static void wait_display_frame(void)
{
    u32 w = 0u;

    while (!lineBuffer_getDoneGpu2d() && w < 40000u) { bsp_uDelay(1u); w++; }
    w = 0u;
    while ( lineBuffer_getDoneGpu2d() && w < 40000u) { bsp_uDelay(1u); w++; }
}

//==============================================================================
// 主流程(顶层状态机)
//==============================================================================
void compare_demo_run(void)
{
#if SPLIT_MODE_LADDER
    /* 先把第 0 档设好, 表头才印得出真实数字 */
    g_lv = 0;
    blocks_setup(g_ladder[0].n, g_ladder[0].w, g_ladder[0].h);

    bsp_printf("\r\n==== 元素数阶梯加压: 只跑左半(加速器), 量 60fps 的元素数极限 ====\r\n");
    bsp_printf("  半幅 %dx%d, 行距 %d; 元素 %dx%d, 速度 %d~%d 像素/帧\r\n",
               SPLIT_HALF_W, SPLIT_HALF_H, (int)SPLIT_LINE_BYTES,
               g_blk_w, g_blk_h, SPLIT_SPEED_MIN, SPLIT_SPEED_MAX);
    {
        u32 i;
        bsp_printf("  阶梯: ");
        for (i = 0u; i < SPLIT_LADDER_NUM; i++)
            bsp_printf(" %d个%dx%d", g_ladder[i].n, g_ladder[i].w, g_ladder[i].h);
        bsp_printf("   每档 %d 帧(≈2 秒)\r\n", SPLIT_LEVEL_FRAMES);
    }
    bsp_printf("  ★ 右半不跑 CPU(留一张静态底色): CPU 每帧要占 ~8ms, 会把硬件的数测歪\r\n");
    bsp_printf("  ★ 左半每帧命令数 = 擦 N + 画 N + 数字 3 + FENCE 1 = 2N+4\r\n");
#else
    blocks_setup(SPLIT_CMP_N, SPLIT_CMP_W, SPLIT_CMP_H);

    bsp_printf("\r\n==== 分屏帧率对比 Demo: 左半硬件加速 / 右半纯 CPU ====\r\n");
    bsp_printf("  半幅 %dx%d, 行距 %d; 方块 %d 个 %dx%d, 速度 %d~%d 像素/帧\r\n",
               SPLIT_HALF_W, SPLIT_HALF_H, (int)SPLIT_LINE_BYTES,
               g_blk_n, g_blk_w, g_blk_h, SPLIT_SPEED_MIN, SPLIT_SPEED_MAX);
    bsp_printf("  左半: 加速器(擦 %d + 擦HUD 2 + 画 %d + 画HUD 2 + FENCE = %d 条/帧)\r\n",
               g_blk_n, g_blk_n, SPLIT_L_CMDS_PER_FRAME);
    bsp_printf("  右半: CPU 逐字节写; 一帧 = 擦 %d 片 + 画 %d 片 + 交帧 1 片, "
               "每片 %d 个矩形 => %d 个显示帧才交一帧(约 %d fps)\r\n",
               g_blk_n / SPLIT_CPU_OPS_PER_FRAME,
               g_blk_n / SPLIT_CPU_OPS_PER_FRAME,
               SPLIT_CPU_OPS_PER_FRAME,
               SPLIT_CPU_FRAME_TICKS, 60 / SPLIT_CPU_FRAME_TICKS);
    bsp_printf("  两边【同一组方块、同一条轨迹】, 只有画的人不同(见 src/compare_demo.c)\r\n");
    bsp_printf("  屏幕上: 每条半幅左上角 (%d,%d) 显示该半幅的帧率(两位, PRAM 数字素材; 素材 %d 张)\r\n",
               SPLIT_INFO_X, SPLIT_INFO_Y, ASSET_NUM);
    bsp_printf("  图层: 元素带层号(ACTOR=64 / FX=128), 绘制顺序按 (层号, id) —— 见 docs/spec/软件_驱动与API.md §5\r\n");
    bsp_printf("  HUD: 数字是层 192 的元素, 每帧四步(擦元素->擦数字框->按层画元素->画数字)\r\n");
    bsp_printf("       ⇒ 方块/子弹可以从数字下面穿过去, 不再需要避开活动区\r\n");
    bsp_printf("  诊断开关: KEY0=打乱元素表(验层号)  KEY1=数字(HUD)绘制 开/关  KEY3=冻住方块\r\n");
#endif

    if (CI_gpu2d_checkId() != GPU2D_CI_ID) { bsp_printf("  !! CI 通路不对, 停\r\n"); return; }

    g_st = ST_INIT;
    g_frame = 0u;
    g_t_total = 0u;

    while (g_st != ST_DONE) {
        switch (g_st) {

        /* ---------------- 初始化 ---------------- */
        case ST_INIT:
            lineBuffer_setSplit(1);         /* 分屏: 帧边界才生效 */
            bsp_uDelay(20000);              /* ★ 等 >1 帧, 让硬件把 2880 跨距锁进去 */
            /* ★ 把显示钉到 A(两个槽都写 A): 从这一刻起"显示哪块"由软件决定,
             *   不再依赖 acc_idx/cpu_idx 的开机初值 —— 见 g_l_disp 的注释 */
            g_l_disp = SPLIT_A_L;
            g_r_disp = SPLIT_A_R;
            display_commit();
            bsp_printf("  split=%d (半幅几何已生效, 显示已钉到 A)\r\n", lineBuffer_isSplit());
#if SPLIT_MODE_LADDER
            g_st = ST_LV_SETUP;             /* 加压档: 直接进第一档 */
#else
            g_st = ST_PAINT_L;
#endif
            break;

        /* ---------------- 开局: 左半两块整帧画好 ---------------- */
        case ST_PAINT_L:
        {
            u32 t = gpu2d_getCmdDone();
            gpu2d_exclusive_begin();
            left_paint_full(SPLIT_A_L, 0);
            left_paint_full(SPLIT_B_L, 1);
            /* 每条 left_paint_full = 1 条铺底 + N 条方块 (FENCE 不计入 CMD_DONE,
             * 口径同 game_demo.c) => 两次共 2*(N+1) 条 */
            (void)gpu2d_exclusive_end(t + 2u * (g_blk_n + 1u));
            bsp_printf("  左半 A_L/B_L 已画好\r\n");
            g_st = ST_PAINT_R;
            break;
        }

        /* ---------------- 开局: 右半两块整帧画好(CPU, 一次性) ---------------- */
        case ST_PAINT_R:
            right_paint_full(SPLIT_A_R, 0);
            right_paint_full(SPLIT_B_R, 1);
            bsp_printf("  右半 A_R/B_R 已画好(CPU 逐字节)\r\n");
            /* A 正在显示; 两边都先画 B, 第一个帧边界之后才轮到 B */
            g_l_idx = 1;
            g_r_idx = 1;
            g_cs    = CS_ERASE;
            g_cs_cnt = 0;
            {
                int i;
                for (i = 0; i < g_blk_n; i++) {
                    g_b[i].sx = g_b[i].x;
                    g_b[i].sy = g_b[i].y;
                }
            }
            g_t_prev = clint_getTimeLow(BSP_CLINT);
            g_show_t0 = g_t_prev;
            g_show_win = 0u;
            g_show_r_prev = g_r_frames;
            bsp_printf("  开始跑 %d 帧 ...\r\n", SPLIT_FRAMES);
            g_st = ST_MOVE;             /* 数字已经并进左右两半各自的绘制里 */
            break;

        /* ---------------- 每显示帧 ---------------- */
        case ST_MOVE:
            if (!g_freeze) blocks_step();   /* 模型走一步(左右共用同一份位置) */
            g_st = ST_L_FRAME;              /* 数字已经并进左右两半各自的绘制里了 */
            break;

        case ST_L_FRAME:                    /* 左半: 发这一帧的命令 + 交帧 */
            left_frame();
            g_st = ST_R_SLICE;
            break;

        case ST_R_SLICE:                    /* 右半: CPU 推一片 */
#if SPLIT_MODE_LADDER
            g_st = ST_WAIT;                 /* 加压档: 右半不跑 CPU, 免得占掉帧预算 */
#else
        {
            u32 t0 = clint_getTimeLow(BSP_CLINT);
            right_slice();
            {
                u32 dt = (clint_getTimeLow(BSP_CLINT) - t0) / TICKS_PER_US;  /* us */
                g_cpu_sum += dt;
                if (dt > g_cpu_max) g_cpu_max = dt;
            }
            g_st = ST_WAIT;
            break;
        }
#endif

        case ST_WAIT:                       /* 锁到显示帧边界 */
            wait_display_frame();
            g_frame++;
            g_win++;
            g_lv_frames++;

            /* ---- 屏幕上的帧率数字: 按固定窗口刷新(别只在换档时更新) ---- */
            g_show_win++;
            if (g_show_win >= SPLIT_SHOW_EVERY) {
                u32 now = clint_getTimeLow(BSP_CLINT);
                u32 ms  = (now - g_show_t0) / TICKS_PER_MS;
                u32 dn  = g_r_frames - g_show_r_prev;

                if (ms == 0u) ms = 1u;
                g_show_l = (g_show_win * 1000u) / ms;
                g_show_r = (dn * 1000u) / ms;
                g_show_r_prev = g_r_frames;
                g_show_t0 = now;
                g_show_win = 0u;
            }
#if SPLIT_MODE_LADDER
            g_st = (g_lv_frames >= SPLIT_LEVEL_FRAMES) ? ST_LV_REPORT : ST_KEY;
#else
            if (g_win >= SPLIT_REPORT_EVERY)
                g_st = ST_REPORT;
            else if (g_frame >= SPLIT_FRAMES)
                g_st = ST_DONE;
            else
                g_st = ST_KEY;
#endif
            break;

        case ST_REPORT:
        {
            u32 now = clint_getTimeLow(BSP_CLINT);
            u32 ms  = (now - g_t_prev) / TICKS_PER_MS;
            u32 fps_l, fps_r;

            g_t_prev = now;
            g_t_total += ms;
            fps_l = (g_win * 1000u) / (ms ? ms : 1u);
            fps_r = (g_win_r_frames * 1000u) / (ms ? ms : 1u);

            bsp_printf("  帧 %4d: 硬件 %2d fps(FPS_ACC=%2d, %d 条命令/帧) | "
                       "CPU %2d fps(FPS_CPU=%2d, 累计 %d 帧) | CPU 每帧 %d us(峰值 %d)\r\n",
                       (int)g_frame, (int)fps_l, (int)lineBuffer_getFpsAcc(),
                       SPLIT_L_CMDS_PER_FRAME,
                       (int)fps_r, (int)lineBuffer_getFpsCpu(), (int)g_r_frames,
                       (int)(g_cpu_sum / g_win), (int)g_cpu_max);

            g_win = 0u;
            g_win_r_frames = 0u;
            g_cpu_sum = 0u;
            g_cpu_max = 0u;
            g_st = ST_KEY;
            break;
        }

        /* ---------------- 阶梯加压(测量台架, 默认关) ---------------- */
#if SPLIT_MODE_LADDER
        case ST_LV_SETUP:                   /* 换一档: 重新撒点 + 两块重画 + 计数快照 */
        {
            gpu2d_stats_t s;
            u32 t;
            int i;

            blocks_setup(g_ladder[g_lv].n, g_ladder[g_lv].w, g_ladder[g_lv].h);

            t = gpu2d_getCmdDone();
            gpu2d_exclusive_begin();
            left_paint_full(SPLIT_A_L, 0);
            left_paint_full(SPLIT_B_L, 1);
            /* 右半保持静态(这半不参与测量), 直接用加速器铺底色 */
            CI_gpu2d_fillRect(SPLIT_A_R, 0u, 0u, SPLIT_HALF_W, SPLIT_HALF_H, SPLIT_BG_COLOR);
            CI_gpu2d_fillRect(SPLIT_B_R, 0u, 0u, SPLIT_HALF_W, SPLIT_HALF_H, SPLIT_BG_COLOR);
            (void)gpu2d_exclusive_end(t + 2u * (g_blk_n + 1u) + 2u);

            g_l_idx = 1;                    /* A 正在显示, 先画 B */
            g_r_idx = 1;
            g_cs    = CS_ERASE;
            g_cs_cnt = 0;
            for (i = 0; i < g_blk_n; i++) {
                g_b[i].sx = g_b[i].x;
                g_b[i].sy = g_b[i].y;
            }

            gpu2d_getStats(&s);             /* 本档计数快照 */
            g_lv_cmd0  = s.cmd_done;
            g_lv_wr0   = s.wr_beats;
            g_lv_busy0 = s.busy_cyc;
            g_lv_t0    = clint_getTimeLow(BSP_CLINT);
            g_lv_frames = 0u;

            /* 屏幕数字也重新开一窗: 换档后立刻归零, 0.5 秒内填上本档的真实值 */
            g_show_l   = 0u;
            g_show_r   = 0u;
            g_show_win = 0u;
            g_show_t0  = g_lv_t0;
            g_show_r_prev = g_r_frames;

            bsp_printf("---- 档 %d/%d: %3d 个 %dx%d ----\r\n",
                       g_lv + 1, SPLIT_LADDER_NUM, g_blk_n, g_blk_w, g_blk_h);
            g_st = ST_MOVE;
            break;
        }

        case ST_LV_REPORT:                  /* 一档跑完: 出数 */
        {
            gpu2d_stats_t s;
            u32 now = clint_getTimeLow(BSP_CLINT);
            u32 ms  = (now - g_lv_t0) / TICKS_PER_MS;
            u32 n   = g_lv_frames ? g_lv_frames : 1u;
            u32 fps, cmd_pf, busy_us, wr_bpf;

            gpu2d_getStats(&s);
            fps     = (g_lv_frames * 1000u) / (ms ? ms : 1u);
            cmd_pf  = (s.cmd_done - g_lv_cmd0) / n;
            busy_us = (s.busy_cyc - g_lv_busy0) / 100u / n;
            wr_bpf  = (s.wr_beats - g_lv_wr0) * 16u / n;

            bsp_printf("    %3d个 %dx%d: %2d fps | %4d 条/帧 | 忙 %5d us/帧 | 写 %6d B/帧 | %s\r\n",
                       g_blk_n, g_blk_w, g_blk_h,
                       (int)fps, (int)cmd_pf, (int)busy_us, (int)wr_bpf,
                       (fps >= 59u) ? "OK" : "掉帧 <<<");

            g_t_total += ms;
            g_lv++;
            g_st = (g_lv >= SPLIT_LADDER_NUM) ? ST_DONE : ST_LV_SETUP;
            break;
        }
#endif  /* SPLIT_MODE_LADDER */

        case ST_KEY:                        /* 现场开关(诊断): 按下即切, 串口打印 */
        {
            u32 ks   = key_getData();
            u32 down = ks & ~g_key_prev;
            g_key_prev = ks;

            if (down & 0x1u) {              /* KEY0: 打乱元素表(层号验证) */
                blocks_shuffle();
                bsp_printf("  ★ KEY0: 元素表已打乱 —— 画面应当【逐像素不变】"
                           "(绘制顺序由 层号+id 决定, 与数组怎么摆无关)\r\n");
            }
            if (down & 0x2u) {              /* KEY1 */
                g_info_on = !g_info_on;
                bsp_printf("  ★ KEY1: 信息带(帧率数字) %s\r\n", g_info_on ? "开" : "关");
            }
            if (down & 0x8u) {              /* KEY3 */
                g_freeze = !g_freeze;
                bsp_printf("  ★ KEY3: 方块 %s (冻住后画面应该完全静止; 还闪就说明是显示端/带宽)\r\n",
                           g_freeze ? "冻住" : "继续跑");
            }
            g_st = ST_MOVE;
            break;
        }

        case ST_DONE:
        default:
            break;
        }
    }

    bsp_printf("---- 分屏对比结束 ----\r\n");
    bsp_printf("  左半(硬件)交帧 %d 次, 右半(CPU)交帧 %d 次; 墙钟 %d ms\r\n",
               (int)g_l_frames, (int)g_r_frames, (int)g_t_total);
    bsp_printf("  FPS_ACC=%d (0x2060)   FPS_CPU=%d (0x2064)\r\n",
               (int)lineBuffer_getFpsAcc(), (int)lineBuffer_getFpsCpu());
}
