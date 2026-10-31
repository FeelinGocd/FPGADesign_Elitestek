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
//      所以把"一帧的 2N 个矩形"拆开，每个显示帧只做 K 个。
//      以 48 个为例：擦 48 个 = 6 个显示帧，画 48 个 = 6 个显示帧，
//      一整帧约 12 个显示帧 => 右半约 5 fps，左半仍按显示节拍运行。
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
const asset_desc_t *game_asset_table(void);
#include "vexriscv.h"           /* data_cache_invalidate_all: 写完帧缓存要写回 cache */
#include "bsp.h"
#include "framebuffer_layout.h"
#include "demo_control.h"
#include "game_log.h"

/*------------------------------------------------------------------------------
 * 分屏半幅几何与地址(★ 和 main.c 的 LB_A_L/LB_A_R/LB_B_L/LB_B_R 必须一致)
 *----------------------------------------------------------------------------*/
#define SPLIT_HALF_W        640                 /* 可见宽：1280 / 2（720p） */
#define SPLIT_HALF_H        720                 /* 可见高：720p 的 720 行 */
#define SPLIT_LINE_BYTES    2880u               /* 画布行距仍是 960*3（RTL 分屏段跨距 = 5760/2） */
#define SPLIT_A_L           DDR_FB_A_LEFT
#define SPLIT_A_R           DDR_FB_A_RIGHT
#define SPLIT_B_L           DDR_FB_B_LEFT
#define SPLIT_B_R           DDR_FB_B_RIGHT

/*------------------------------------------------------------------------------
 * 画面参数
 *----------------------------------------------------------------------------*/
/* ★★ 元素个数/尺寸改成【运行时变量】(g_blk_n / g_blk_w / g_blk_h),
 *    这样同一份代码能一档一档往上加, 不用来回编译。数组按 MAX 开。 */
#define SPLIT_BLK_MAX       96                  /* 多档对比最大元素数 */
#define SPLIT_CMP_W         64                  /* 对比模式: 元素尺寸 */
#define SPLIT_CMP_H         64
#define SPLIT_SPEED_MIN     2                   /* 每帧位移 2~4 像素 */
#define SPLIT_SPEED_MAX     4
#define SPLIT_BG_COLOR      0x00101418u         /* 底色(深灰蓝) */

/* 右半每显示帧做几个矩形（擦/画各算一个），固定分片粒度便于各负载档对比。
 * 方块总数变化时，CPU 一帧占用的显示帧数随之变化，帧率自然拉开。 */
#define SPLIT_CPU_OPS_PER_FRAME 8

/*------------------------------------------------------------------------------
 * 屏幕上的帧率数字(两位, 用 PRAM 里的数字素材画, 每位 1 条 sprite 命令)
 *   ★ 素材编号: pic2pram.py 按【--dir 给的顺序】编号 —— 先 character 后 base/num,
 *     所以 0/1 是跑步帧, 2..11 是数字 '0'..'9'。
 *   ★ 数字在元素之后绘制，每帧固定四步：
 *         ① 擦全部元素的旧位置   ② 擦数字框(层 0 的背景)
 *         ③ 按统一数组顺序画全部元素   ④ 画数字(永远最后)
 *     ⇒ HUD 被擦掉也无所谓, 第 ④ 步每帧都会补回来 —— 所以
 *       【方块/子弹可以从数字下面随便穿过, 不再需要"避开活动区"】。
 *     代价只有每帧 4 条命令, 可以忽略。
 *----------------------------------------------------------------------------*/
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
/* 左半每帧的命令数 = 擦 N + 擦数字框 2 + 画 N + 画数字 2 + FENCE 1 */
#define SPLIT_L_CMDS_PER_FRAME  (2 * g_blk_n + 5)

/* 屏幕上两个帧率数字每秒刷新一次。
 * 固定窗口避免每个显示帧都刷新数字；换档时会清空窗口，防止显示上一档结果。 */
#define SPLIT_SHOW_EVERY    60

#define SPLIT_REPORT_EVERY  60                  /* 每多少帧打一行统计 */

/*------------------------------------------------------------------------------
 * 多档方块对比：自动循环 24/48/72/96 个 64×64 方块，每档约 5 秒。
 * 两侧始终使用同一份方块位置、尺寸和速度，只改变“谁在画”。
 * 串口每秒打印当前数量和两侧实测结果。
 *----------------------------------------------------------------------------*/
#define SPLIT_LEVEL_FRAMES  300                 /* 每档显示帧数（60 Hz 约 5 秒） */
#define SPLIT_LEVEL_COUNT   4
static const unsigned char g_level_blocks[SPLIT_LEVEL_COUNT] = { 24u, 48u, 72u, 96u };
#define TICKS_PER_MS        (BSP_CLINT_HZ / 1000u)
#define TICKS_PER_US        (BSP_CLINT_HZ / 1000000u)

/*==============================================================================
 * 状态机
 *==============================================================================*/
/* 顶层: 一个状态做一件事, 做完设下一个状态 */
typedef enum {
    ST_INIT = 0,        /* 建模型 + 切分屏 + 等几何生效 */
    ST_LEVEL_SETUP,     /* 换档: 重建方块并整帧重画左右四块半幅 */
    ST_MOVE,            /* 每显示帧: 模型走一步(左右共用同一份位置) */
    ST_L_FRAME,         /* 左半: 发这一帧的 2N+1 条命令 + 交帧 */
    ST_R_SLICE,         /* 右半: 推进 CPU 侧子状态机一片 */
    ST_WAIT,            /* 等这一个显示帧过去(锁 60Hz 节拍) */
    ST_REPORT,          /* 每 SPLIT_REPORT_EVERY 帧统计一次 */
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
typedef struct {
    short x, y;             /* 当前位置(半幅坐标，0..720) */
    signed char vx, vy;     /* 速度(像素/帧, 撞边反向) */
    unsigned char color;    /* g_pal[] 索引 */

    /* 每块缓冲各记一份上次绘制的位置，用于恢复背景。 */
    short fx[2], fy[2];     /* 左半: 每块缓冲各记一份"上次画它时它在哪" */
    short rx[2], ry[2];     /* 右半: 同上 */
    short sx, sy;           /* 右半: 这一帧开始时冻结的位置快照(分片期间不许变) */
} blk_t;
typedef char blk_t_size_check[(sizeof(blk_t) <= 28) ? 1 : -1];

static blk_t g_b[SPLIT_BLK_MAX];

/* 运行时状态 */
static split_state_t g_st;
static cpu_state_t   g_cs;
static int   g_l_idx;                   /* 左半正在画哪块(0=A_L 1=B_L) */
static int   g_r_idx;                   /* 右半正在画哪块(0=A_R 1=B_R) */
static int   g_cs_cnt;                  /* 本子状态里已经做了几个矩形 */
static int   g_blk_n;                   /* 当前这一档的元素个数 */
static int   g_blk_w, g_blk_h;          /* 当前这一档的元素尺寸 */
static int   g_level;                   /* 当前方块数量档位 */
static u32   g_level_frames;            /* 本档已经跑了几帧 */

static u32   g_rnd = 0x1234ABCDu;
static u32   g_frame;                   /* 顶层帧计数(每显示帧 +1) */
static u32   g_win;                     /* 本统计窗内的帧数 */
static u32   g_r_frames;                /* 右半累计交帧次数 */
static u32   g_win_r_frames;
static u32   g_t_prev;
static u32   g_cpu_sum, g_cpu_max;      /* CPU 每帧耗时(us) */
static u32   g_show_l, g_show_r;        /* 屏幕上正在显示的两个帧率值 */
static u32   g_show_t0;                 /* 屏幕数字这一窗的起点(CLINT) */
static u32   g_show_win;                /* 屏幕数字这一窗已经过了几帧 */
static u32   g_show_r_prev;             /* 上一窗末尾的 g_r_frames(算右半帧率用) */

/* ---- 显示给哪一块(相位免疫用) ----
 *   ★★ 为什么需要: 硬件每帧按内部索引 acc_idx/cpu_idx 在 A/B 之间二选一,
 *      而这两个索引【软件读不到】, 开机初值还可能被上一次运行弄脏 ——
 *      一旦和软件的假设差一位, 软件就会一直在画
 *      【正在显示的那一块】, 表现是方块上半截被擦掉还没重画就扫过去了
 *      (而且重新下载 bitstream 后又会"自己好", 因为 FPGA 复位把索引清 0 了)。
 *   ★ 解法: 把 A/B 两个基址槽【都写成同一块】, 索引取哪个都一样 ——
 *      显示哪一块完全由软件说了算, 与开机初值无关。 */
static u32   g_l_disp, g_r_disp;        /* 左边/右边下一步要显示的那块 */

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
                          g_blk_w, g_blk_h, g_pal[g_b[i].color]);
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
        CI_gpu2d_fillRect(back,
                          (u32)g_b[i].fx[g_l_idx], (u32)g_b[i].fy[g_l_idx],
                          g_blk_w, g_blk_h, SPLIT_BG_COLOR);
    }

    info_erase(back);                                   /* ② 擦数字框(层 0 的背景) */

    /* ③ 按同一数组顺序画全部元素，两侧顺序保持一致 */
    for (i = 0; i < g_blk_n; i++) {
        CI_gpu2d_fillRect(back,
                          (u32)g_b[i].x, (u32)g_b[i].y,
                          g_blk_w, g_blk_h, g_pal[g_b[i].color]);
        g_b[i].fx[g_l_idx] = g_b[i].x;                      /* ③ 记住这块缓冲现在是什么 */
        g_b[i].fy[g_l_idx] = g_b[i].y;
    }

    info_digits(back, g_show_l);                        /* ④ 画数字(层 192, 永远最上) */

    CI_gpu2d_fence();
    /* ★ 绝不在这里写 lineBuffer_doneGpu2d(): FENCE 完成时【硬件自己置位 DONE_ACC】
     *   (APB0_GPU_2D.v: acc_evt = acc_frame_done | 软件写1)。软件抢在前面写 1,
     *   显示会在帧边界翻到"还没画完的那块" => 撕裂。 */

    g_l_disp = back;                        /* ★ 下一帧边界显示这块(两个槽都写它) */
    display_commit();
    g_l_idx ^= 1;                                           /* 下一帧画另一块 */
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
        cpu_fill(base, g_b[i].x, g_b[i].y, g_blk_w, g_blk_h,
                 g_pal[g_b[i].color]);
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
                e = g_cs_cnt;
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
                e = g_cs_cnt;
                cpu_fill(base,
                         g_b[e].sx, g_b[e].sy,
                         g_blk_w, g_blk_h, g_pal[g_b[e].color]);
                g_cs_cnt++;
            }
            /* ★ 画完最后一片的【当帧】就把交帧做掉(以前它是单独一片 CS_FLUSH):
             *     擦 N 个和画 N 个各占若干显示帧，CPU 帧率由此自然下降。
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
    const asset_desc_t *assets = game_asset_table();

    a1 = &assets[s_split_digits[(int)((v / 10u) % 10u)]];
    a0 = &assets[s_split_digits[(int)(v % 10u)]];

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
        g_b[i].color = (unsigned char)(i & 7);
    }
}

/* 设定这一档的元素个数/尺寸, 然后重新随机撒一遍(固定种子 => 可复现) */
static void blocks_setup(int n, int w, int h)
{
    g_blk_n = n;
    g_blk_w = w;
    g_blk_h = h;
    blocks_init();
}

/*  一维弹跳：走一步，撞边就贴边并反向（x / y 两个轴共用）。  */
static void bounce(short *pos, signed char *vel, int lo, int hi)
{
    int next = (int)*pos + (int)*vel;

    *pos = (short)next;
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
static int demo_switch_requested(void)
{
    (void)game_keys_read();
    return game_take_demo_switch();
}

/* 建立一个方块数量档。
 * 首次进入初始化 A/B 两块；后续先画备用半幅并切页，再补画刚释放的旧半幅。
 * 切档期间显示屏始终有完整画面，不会黑闪，也不会残留上一档的多余方块。 */
static int compare_level_setup(int first)
{
    u32 t0, target;
    int i, cpu_slices, cpu_frame_frames;
    int left_idx = g_l_idx, right_idx = g_r_idx;
    u32 left_back = left_idx ? SPLIT_B_L : SPLIT_A_L;
    u32 right_back = right_idx ? SPLIT_B_R : SPLIT_A_R;

    g_blk_n = (int)g_level_blocks[g_level];
    g_blk_w = SPLIT_CMP_W;
    g_blk_h = SPLIT_CMP_H;
    blocks_setup(g_blk_n, g_blk_w, g_blk_h);

    t0 = gpu2d_getCmdDone();
    if (first) {
        /* 首档两套缓冲都要有完整的底色、方块和历史位置。 */
        left_paint_full(SPLIT_A_L, 0);
        left_paint_full(SPLIT_B_L, 1);
        right_paint_full(SPLIT_A_R, 0);
        right_paint_full(SPLIT_B_R, 1);
        g_l_disp = SPLIT_B_L;
        g_r_disp = SPLIT_B_R;
        g_l_idx = 0;
        g_r_idx = 0;
        target = t0 + 2u * ((u32)g_blk_n + 1u);
    } else {
        /* 先把新档画进当前不可见的 A 或 B，完成后切换显示基址。 */
        left_paint_full(left_back, left_idx);
        right_paint_full(right_back, right_idx);
        g_l_disp = left_back;
        g_r_disp = right_back;
        target = t0 + (u32)g_blk_n + 1u;
    }
    if (gpu2d_waitCmdDone(target, 3000000u) != 0) {
        GAME_LOG("COMPARE ERROR: level setup timeout\r\n");
        lineBuffer_enable(1);
        return 0;
    }

    display_commit();
    if (first) {
        lineBuffer_enable(1);
    } else {
        /* 等基址在帧边界生效，再把刚释放的另一块也铺成新档。
         * 这一阶段显示的是已经完整的备用缓冲，因此不会黑屏。 */
        int other_left = left_idx ^ 1;
        int other_right = right_idx ^ 1;
        u32 other_left_base = other_left ? SPLIT_B_L : SPLIT_A_L;
        u32 other_right_base = other_right ? SPLIT_B_R : SPLIT_A_R;

        bsp_uDelay(20000u);
        t0 = gpu2d_getCmdDone();
        left_paint_full(other_left_base, other_left);
        right_paint_full(other_right_base, other_right);
        target = t0 + (u32)g_blk_n + 1u;
        if (gpu2d_waitCmdDone(target, 3000000u) != 0) {
            GAME_LOG("COMPARE ERROR: second buffer setup timeout\r\n");
            return 0;
        }
        g_l_idx = other_left;
        g_r_idx = other_right;
    }
    g_cs = CS_ERASE;
    g_cs_cnt = 0;
    for (i = 0; i < g_blk_n; ++i) {
        g_b[i].sx = g_b[i].x;
        g_b[i].sy = g_b[i].y;
    }

    g_frame = 0u;
    g_win = 0u;
    g_win_r_frames = 0u;
    g_cpu_sum = g_cpu_max = 0u;
    g_level_frames = 0u;
    g_show_l = g_show_r = 0u;
    g_show_win = 0u;
    g_t_prev = g_show_t0 = clint_getTimeLow(BSP_CLINT);
    g_show_r_prev = g_r_frames;
    g_st = ST_MOVE;
    cpu_slices = (g_blk_n + SPLIT_CPU_OPS_PER_FRAME - 1) / SPLIT_CPU_OPS_PER_FRAME;
    cpu_frame_frames = 2 * cpu_slices;
    GAME_LOG("---- LEVEL %d/%d: %d blocks, %dx%d ----\r\n",
               g_level + 1, SPLIT_LEVEL_COUNT, g_blk_n, g_blk_w, g_blk_h);
    GAME_LOG("  hardware=%d cmds/frame | CPU=%d slices/frame => about %d fps\r\n",
               SPLIT_L_CMDS_PER_FRAME, cpu_frame_frames,
               60 / (cpu_frame_frames ? cpu_frame_frames : 1));
    return 1;
}

int compare_demo_run(void)
{
    GAME_LOG("\r\n==== 分屏帧率对比 Demo: 左半硬件加速 / 右半纯 CPU ====\r\n");
    GAME_LOG("  半幅 %dx%d, 行距 %d; 多档 24/48/72/96 个 %dx%d, 速度 %d~%d 像素/帧\r\n",
               SPLIT_HALF_W, SPLIT_HALF_H, (int)SPLIT_LINE_BYTES,
               SPLIT_CMP_W, SPLIT_CMP_H, SPLIT_SPEED_MIN, SPLIT_SPEED_MAX);
    GAME_LOG("  两边【同一组方块、同一条轨迹】, 只有画的人不同(见 src/compare_demo.c)\r\n");
    GAME_LOG("  屏幕上: 每条半幅左上角 (%d,%d) 显示该半幅的帧率(两位, PRAM 数字素材; 素材 %d 张)\r\n",
               SPLIT_INFO_X, SPLIT_INFO_Y, ASSET_NUM);
    GAME_LOG("  多档对比: 每档约 5 秒；M = 切换游戏 Demo\r\n");

    if (CI_gpu2d_checkId() != GPU2D_CI_ID) {
        GAME_LOG("  !! CI 通路不对, 停\r\n");
        return 0;
    }

    g_level = 0;
    g_st = ST_INIT;
    g_frame = 0u;
    g_level_frames = 0u;
    {
        int first_level = 1;

        for (;;) {
        if (demo_switch_requested()) {
            GAME_LOG("DEMO SWITCH: compare -> game\r\n");
            return 1;
        }
        switch (g_st) {

        /* ---------------- 初始化 ---------------- */
        case ST_INIT:
            lineBuffer_enable(0);
            lineBuffer_setSplit(1);         /* 分屏: 帧边界才生效 */
            bsp_uDelay(20000);              /* ★ 等 >1 帧, 让硬件把 2880 跨距锁进去 */
            g_l_idx = 1;                    /* 首次只画未显示的 B 半幅 */
            g_r_idx = 1;
            GAME_LOG("  split=%d (半幅几何生效; back buffer ready before display)\r\n",
                       lineBuffer_isSplit());
            g_st = ST_LEVEL_SETUP;
            break;

            case ST_LEVEL_SETUP:
                if (!compare_level_setup(first_level)) return 0;
                first_level = 0;
                break;

        /* ---------------- 每显示帧 ---------------- */
        case ST_MOVE:
            blocks_step();                  /* 模型走一步(左右共用同一份位置) */
            g_st = ST_L_FRAME;              /* 数字已经并进左右两半各自的绘制里了 */
            break;

        case ST_L_FRAME:                    /* 左半: 发这一帧的命令 + 交帧 */
            left_frame();
            g_st = ST_R_SLICE;
            break;

        case ST_R_SLICE:                    /* 右半: CPU 推一片 */
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

        case ST_WAIT:                       /* 锁到显示帧边界 */
            wait_display_frame();
            g_frame++;
            g_win++;
            g_level_frames++;

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
            if (g_win >= SPLIT_REPORT_EVERY)
                g_st = ST_REPORT;
            else
                g_st = ST_MOVE;
            break;

        case ST_REPORT:
        {
            u32 now = clint_getTimeLow(BSP_CLINT);
            u32 ms  = (now - g_t_prev) / TICKS_PER_MS;
            u32 fps_l, fps_r;

            g_t_prev = now;
            fps_l = (g_win * 1000u) / (ms ? ms : 1u);
            fps_r = (g_win_r_frames * 1000u) / (ms ? ms : 1u);

            GAME_LOG("  LV%d %2d块 帧%4d: 硬件 %2d fps(FPS_ACC=%2d, %d 条命令/帧) | "
                       "CPU %2d fps(FPS_CPU=%2d, 累计 %d 帧) | CPU 每帧 %d us(峰值 %d)\r\n",
                       g_level + 1, g_blk_n, (int)g_frame,
                       (int)fps_l, (int)lineBuffer_getFpsAcc(),
                       SPLIT_L_CMDS_PER_FRAME,
                       (int)fps_r, (int)lineBuffer_getFpsCpu(), (int)g_r_frames,
                       (int)(g_cpu_sum / g_win), (int)g_cpu_max);

            g_win = 0u;
            g_win_r_frames = 0u;
            g_cpu_sum = 0u;
            g_cpu_max = 0u;
            if (g_level_frames >= SPLIT_LEVEL_FRAMES) {
                g_level = (g_level + 1) % SPLIT_LEVEL_COUNT;
                g_st = ST_LEVEL_SETUP;
            } else {
                g_st = ST_MOVE;
            }
            break;
        }

            default:
                g_st = ST_MOVE;
                break;
            }
        }
    }
}
