//==============================================================================
//  demo_combine.c —— 图片合成 Demo：两张 PRAM 素材图合成一张
//------------------------------------------------------------------------------
//  硬件那边是什么（看 RTL 的人对照 docs/spec/硬件_加速器.md §5.5）
//      一条合成命令 = 3 条 CI：命令开始(宽高+目的) + 图一(地址/模式/跨距, more=1)
//                                              + 图二(地址/模式/跨距, more=0)
//      两张图都在同一个 PRAM 里 ⇒ 硬件把两行【串行】读出来，在 process/imageCombine.v
//      里缓存图一那一行、与图二逐拍合成，再按图二那一步的模式去和背景 AB / 写回 DDR。
//
//  屏幕上排四格（同一块背景上，肉眼就能比出来）
//      ① 只画图一（底图）         → 读回存 P1
//      ② 只画图二（特效图）        → 读回存 P2
//      ③ 合成（图一, 图二）        → 读回存 Pc
//      ④ 合成（两张图都开 ck）     → 读回存 Pk
//      ★ 四格都先铺同一块【不透明底色】再画 —— 不然"透明处"看到的是旧内容，比对没意义
//
//  ck 在哪一级（★ 板上定稿：判在【合成之前】）
//      素材源那两趟都照常比键色（Decoder 恒定给"这张图哪几个字节透明"）；
//      "这次要不要抠"由 imageCombine 按【命令级】使能（最后一格的 flags[4]）一处 gate；
//      然后按"上面那张(图二)的掩码"逐字节选：要写就用图二，被抠掉就用缓冲里的图一。
//      ⇒ ③ 两张都不开 ck：上面那张全赢（图二的 1 把图一的 0 盖住 —— 这是预期的）；
//         ④ 两张都开 ck：图二的白不写、图一的白也不写 ⇒ 露图一 / 两张都白处落底色。
//      （先说"挪到合成之后"试过：那样图一整个看不见、只剩图二 —— 板上看到只有 1，已改回。）
//
//  自检（★ 为什么只能查"结构"）
//      调色板和素材都在片上 PRAM，软件【读不到】 ⇒ 没法算期望像素。
//      所以只查三条与素材内容无关、但足以证明"真的合成了"的判据：
//        · ③ 每个像素都必须等于 P2（两张都没开 ck ⇒ 上面那张全赢）
//        · ③ 里等于 P2 的像素数要够多 (>= MIN_MIX_PIX)
//        · ④：图二=白的像素 ⇒ 期望 = 图一（图一也白 ⇒ 底色）；图二≠白 ⇒ 期望 = 图二；
//          并且"露出图一"的像素数必须 > 0（不然就是合成没起作用）
//
//  素材
//      图一 / 图二都取 32x48 的两个数字素材（尺寸一样，目标矩形就取这个尺寸）。
//      ★ 硬性约定：两张素材都要 ≥ 目标矩形尺寸（宽不够会在行尾读到相邻行）。
//==============================================================================
#include "demo_combine.h"
#include "CI_gpu2d.h"
#include "user_gpu2d.h"
#include "user_lineBuffer.h"
#include "assets_desc.h"          /* g_assets[] (pic2pram.py 生成) */
#include "vexriscv.h"             /* data_cache_invalidate_all */
#include "bsp.h"

/* ---- 屏幕几何（★ 和 main.c 里的 LB_FRAME_C / LB_FRAME_D 一致）---------------- */
#define CB_LB_C          0x08000000u
#define CB_LB_D          0x08800000u
#define SCR_LINE_BYTES   5760u

/* ---- 素材：两个 32x48 的数字（尺寸一样，好当"目标矩形"）------------------------ */
#define IMG_A            2              /* "num0" 32x48 */
#define IMG_B            3              /* "num1" 32x48 */

/* ---- 四个格子的位置/尺寸 ---------------------------------------------------- */
#define CELL_W           32
#define CELL_H           48
#define CELL_N           (CELL_W * CELL_H)
#define CELL_GAP         24
#define ORIGIN_X         40
#define ORIGIN_Y         40
#define BG_COLOR         0x203040u      /* ①②③ 的不透明底色（深蓝灰） */
//   ★ 第 ④ 格单独用【纯黑】：ck 抠掉的地方是"谁都不写"，露出来的就是铺在这儿的底色。
//     和屏幕背景（启动时 C 帧铺的纯黑）一个颜色，一眼就能看出"哪些像素真的没被写"。
#define BG_COLOR_K       0x000000u      /* ④ 的底色（纯黑） */

/* ---- 自检阈值 --------------------------------------------------------------- */
#define MIN_MIX_PIX      32             /* "来自图二"和"来自图一"的像素至少这么多 */

/* ---- ck 开关是【命令级】的：看最后一格（合成 = 图二那格）的 flags[4] ---- */
#define FLAGS_A          (GPU2D_MODE_REPLACE | GPU2D_MODE_SPRITE)
#define FLAGS_B_PLAIN    (GPU2D_MODE_REPLACE | GPU2D_MODE_SPRITE)
#define FLAGS_B_CK       (GPU2D_MODE_REPLACE | GPU2D_MODE_SPRITE | GPU2D_MODE_CKEY_EN)

/* 读回来的四格（一格 32x48 = 1536 像素 → 每格 6KB，四格 24KB，片上 RAM 放得下）*/
static u32 s_p1[CELL_N], s_p2[CELL_N], s_pc[CELL_N], s_pk[CELL_N];

/* 一个矩形的左上角地址 */
static u32 cell_addr(u32 fb, u32 x, u32 y)
{
    return fb + y * SCR_LINE_BYTES + x * LINEBUFFER_PIXEL_BYTES;
}

/* 读回一个像素（帧缓存里是 B,G,R；这里统一成 0xRRGGBB 好比） */
static u32 read_pixel(u32 addr)
{
    volatile u8 *p = (volatile u8 *)addr;

    return ((u32)p[2] << 16) | ((u32)p[1] << 8) | (u32)p[0];
}

/* 把一格读进数组 */
static void read_cell(u32 addr, u32 *dst)
{
    u32 i, j;

    for (j = 0; j < CELL_H; j++)
        for (i = 0; i < CELL_W; i++)
            dst[j * CELL_W + i] = read_pixel(addr + j * SCR_LINE_BYTES
                                                  + i * LINEBUFFER_PIXEL_BYTES);
}

void demo_combine_run(void)
{
    u32 fb  = CB_LB_C;
    u32 x0  = ORIGIN_X;
    u32 y0  = ORIGIN_Y;
    u32 xa  = x0;
    u32 xb  = x0 + CELL_W + CELL_GAP;
    u32 xc  = x0 + 2u * (CELL_W + CELL_GAP);
    u32 xk  = x0 + 3u * (CELL_W + CELL_GAP);
    u32 a   = g_assets[IMG_A].base_word, a_stride = g_assets[IMG_A].stride_word;
    u32 b   = g_assets[IMG_B].base_word, b_stride = g_assets[IMG_B].stride_word;
    u32 i, n_c, n_a, n_ex, n_bg, bad, c0, n_ck_bad, exp;

    bsp_printf("\r\n==== 图片合成 Demo（两张 PRAM 素材图合成一张）====\r\n");

    /* 0) CI 通路自检：发一条 NOP 读回固定标识 */
    if (CI_gpu2d_checkId() != GPU2D_CI_ID) {
        bsp_printf("  !! CI 通路不对, 停\r\n");
        return;
    }
    bsp_printf("  CI 通路 OK;  图一(asset %0d) 图二(asset %0d)\r\n", IMG_A, IMG_B);
    if ((g_assets[IMG_A].width < CELL_W) || (g_assets[IMG_A].height < CELL_H) ||
        (g_assets[IMG_B].width < CELL_W) || (g_assets[IMG_B].height < CELL_H)) {
        bsp_printf("  !! 素材比目标矩形小, 停\r\n");
        return;
    }

    /* 1) 一批命令把四格画出来（每格：先铺底色, 再画它那一格的东西）*/
    gpu2d_exclusive_begin();
    c0 = gpu2d_getCmdDone();

    /* ① 只画图一 */
    CI_gpu2d_fillRect(fb, xa, y0, CELL_W, CELL_H, BG_COLOR);
    CI_gpu2d_spriteRect(a, a_stride, fb, xa, y0, CELL_W, CELL_H);

    /* ② 只画图二 */
    CI_gpu2d_fillRect(fb, xb, y0, CELL_W, CELL_H, BG_COLOR);
    CI_gpu2d_spriteRect(b, b_stride, fb, xb, y0, CELL_W, CELL_H);

    /* ③ 合成：图一（底）+ 图二（上） */
    CI_gpu2d_fillRect(fb, xc, y0, CELL_W, CELL_H, BG_COLOR);
    CI_gpu2d_combineRect(a, a_stride, FLAGS_A,
                         b, b_stride, FLAGS_B_PLAIN,
                         fb, xc, y0, CELL_W, CELL_H);

    /* ④ 合成 + 开 ck（★ ck 判在【合成之前】：图二的白不写 ⇒ 不盖住图一；图一的白同样不写
     *    ⇒ 两张都是白的地方谁都不写，露出底色。开关只写在最后一格上）*/
    CI_gpu2d_fillRect(fb, xk, y0, CELL_W, CELL_H, BG_COLOR_K);
    CI_gpu2d_combineRect(a, a_stride, FLAGS_A,
                         b, b_stride, FLAGS_B_CK,
                         fb, xk, y0, CELL_W, CELL_H);

    /*   ★ 记数按【命令】算, 不是 CI：四格各两条命令(FILL + 画/合成) = 8 条。
     *     FENCE 不是"绘制命令"，它只置 frame_done、不进 cmd_done 计数 ⇒ 这里不用它，
     *     静态一张图也不需要交帧。 */
    if (gpu2d_exclusive_end(c0 + 8u) != 0) {
        bsp_printf("  !! 画四格超时(命令没做完), 停\r\n");
        return;
    }

    /* 2) 读回四格（DDR 是 CPU 可能缓存过的地址 ⇒ 先失效 cache）*/
    data_cache_invalidate_all();
    read_cell(cell_addr(fb, xa, y0), s_p1);
    read_cell(cell_addr(fb, xb, y0), s_p2);
    read_cell(cell_addr(fb, xc, y0), s_pc);
    read_cell(cell_addr(fb, xk, y0), s_pk);

    /* 3) 自检（★ 定稿契约：ck 判在【合成之前】——每张图各判各的；合成按"上面那张的掩码"选图层）*/
    bad = 0; n_c = 0; n_a = 0; n_ex = 0; n_bg = 0; n_ck_bad = 0;
    for (i = 0; i < CELL_N; i++) {
        /* ③ 两张图都没开 ck ⇒ 上面那张掩码全 1 ⇒ 处处是图二（1 盖住 0，这是预期的）*/
        if (s_pc[i] == s_p2[i]) n_c++;
        else                    bad++;
        if (s_pc[i] == s_p1[i]) n_a++;      /* 两张图底色相同的地方必然相等，只做参考 */
    }
    for (i = 0; i < CELL_N; i++) {
        if (s_p2[i] == 0x00FFFFFFu) {       /* 图二这一点是键色(白)：图二不写 */
            if (s_p1[i] == 0x00FFFFFFu) {   /* 图一也是白：两级都抠 ⇒ 谁都不写 ⇒ 底色 */
                n_bg++;
                exp = BG_COLOR_K;           /* ★ ④ 的底色是纯黑，不是 0x203040 */
            } else {                        /* 图一有东西 ⇒ 露出来（这就是 ck 要提前的原因）*/
                n_ex++;
                exp = s_p1[i];
            }
        } else {
            exp = s_p2[i];                  /* 图二不是白 ⇒ 上面那张赢 */
        }
        if (s_pk[i] != exp) n_ck_bad++;
    }

    bsp_printf("  ① 图一 / ② 图二 / ③ 合成 / ④ 合成+ck  (每格 %0d 像素)\r\n", CELL_N);
    bsp_printf("  合成: = 图二 %0d, 不是图二 %0d  <== 期望 不是图二=0（③ 没开 ck ⇒ 上面那张全赢）\r\n",
               n_c, bad);
    bsp_printf("  ck  : 抠错 %0d, 露出图一 %0d, 两张都是白(落底色 %06x) %0d\r\n",
               n_ck_bad, n_ex, BG_COLOR_K, n_bg);
    bsp_printf("        <== 期望 抠错=0 且 露出图一>0（ck 在合成之前 ⇒ 图二的白不盖图一）\r\n");
    bsp_printf("        （③ 里等于图一的地方 %0d 个像素，仅参考）\r\n", n_a);

    if ((bad == 0) && (n_c >= MIN_MIX_PIX) && (n_ck_bad == 0) && (n_ex > 0)) {
        bsp_printf("  ==== 图片合成自检通过 ====\r\n");
    } else {
        bsp_printf("  !! 合成自检不过（看上面三行）\r\n");
    }
}
