//==============================================================================
//  demo_anim.c —— 单张【spirte 素材】跑动(两帧动画 + 脏区局部重放)
//------------------------------------------------------------------------------
//  素材来自 res/func/pic2pram.py 对 res/pic/character 的转换:
//      0  run_left_00_contact  63x89  stride 16  base    0
//      1  run_left_01_passing  65x86  stride 17  base 1424
//  两帧尺寸不同 => 统一放进一个 SPR_BOX_W x SPR_BOX_H 的框里(取较大值), 左上对齐。
//  ★ 画的时候宽度用【素材按 4 像素对齐后的宽度】(stride_word × 4)：镜像要求 4 对齐，
//    行尾补的白像素被 ck 抠掉；镜像后它们跑到左边，所以 dx 要减掉 pad、擦除框往左留 3 像素。
//  ★ 朝向: 往右跑时水平镜像(flags[5]=MX)，素材是 run_left 所以往左跑不用翻。
//  ★★ 【本轮】上下两条跑道, 各跑一个来回, 专门看【垂直镜像 MY(flags[6])】:
//        下方跑道(贴底): 只翻左右(MX)          => 正常姿势
//        上方跑道(贴顶): 左右(MX) + 垂直(MY)   => 应该是【头朝下】的倒立
//      上方没倒立 / 两条一模一样 => MY 没生效(这颗 bitstream 里没带镜像那次改动)。
//      跑完一个来回(回到左端)就换跑道, 串口会打印现在在哪条。
//  透明背景在转换时变成了白色(调色板第 0 项 = ffffff)。
//  ★ 现在开【颜色键控(ck)】画: 等于白色的像素【根本不写】—— 精灵外面那圈不再是
//    白框, 而是帧缓存里原有的背景/参考线。所以也不用"先补背景再压精灵"了。
//
//  绘制: CI_gpu2d_spriteRect(base_word, stride_word, fb, x, y, w, h)
//        —— 走【片上 PRAM 索引 + 调色板】那一路, 全程不读 DDR ✓
//
//  脏区: 后台缓冲里装的是【两帧前】那一幅(每块缓冲各记一份位置历史),
//        所以每次要把"旧位置整块"擦掉 + 补参考线, 再把 sprite 画到新位置。
//  自检: 只查【空出来的那一片】(旧位置减去新位置) —— sprite 内部逐像素的颜色
//        模型在 C 这边没有(素材在 PRAM 里), 所以不查它。
//==============================================================================
#include "demo_anim.h"
#include "CI_gpu2d.h"
#include "user_gpu2d.h"
#include "user_lineBuffer.h"
#include "assets_desc.h"        /* 素材表: g_assets[] (pic2pram.py 生成) */
#include "IRQ_frame.h"
#include "user_key.h"           /* KEY2: 暂停/继续 */
#include "bsp.h"

#define LB_FRAME_C      0x08000000u
#define LB_FRAME_D      0x08800000u

#define BG_COLOR        0x00FFFFFFu

/* sprite 的框: 取两帧里较大的尺寸, 左上对齐
 *   ★ 画的时候宽度取【素材按 4 像素对齐后的宽度】(= stride_word × 4)：
 *     镜像(MX)要求跨度是 4 的倍数（见 docs/spec/硬件_加速器.md §1.6），
 *     行尾补出来的那几个像素是索引 0 = 白，开着 ck 自动透明。
 *   ★ 镜像后 padding 会跑到【左边】，所以框要往左多留 SPR_PADMAX 个像素。 */
#define SPR_WMAX        68              /* 素材行按 4 对齐后的最大宽度（64 / 68） */
#define SPR_HMAX        89              /* 两帧里较高的那个 */
#define SPR_PADMAX      3               /* 最多补几个像素（68 - 65） */
#define SPR_BOX_W       (SPR_WMAX + SPR_PADMAX)   /* 71：两个方向都盖得住 */
#define SPR_BOX_H       SPR_HMAX
#define SPR_BOX_DX      (-SPR_PADMAX)   /* 框的左边缘 = x + SPR_BOX_DX */

#define SPR_ANIM_N      2               /* ★ 跑步动作帧数: 素材表里 0/1 是两帧跑步
                                         *   (加了数字素材后 ASSET_NUM 变成 12,
                                         *    换帧不能再用 ASSET_NUM 取模, 否则会画到数字上) */
#define SPR_MOVE_X      8               /* 每帧横向移动(★ 调快了: 一个来回约 7 秒, 方便看换道) */
#define SPR_ANIM_EVERY  8               /* 每多少帧换一次动作帧 */
#define SPR_BOUNCE_MIN  60              /* 活动区左边界(留点边距) */
#define SPR_X_MAX       (1920 - SPR_WMAX - SPR_BOUNCE_MIN)  /* 右端 */

/* 上下两条跑道(左上角 y) —— 专门对比"正常"和"垂直镜像(MY)" */
#define LANE_Y_LOW      (1080 - SPR_BOX_H - 4)  /* 下方: 贴底 */
#define LANE_Y_HIGH     8                       /* 上方: 贴顶 */

#define BASE_Y          1079            /* 底部横线 */
#define BASE_COLOR      0x00FF8000u
#define RULER_W         8
#define RULER_N         5

#define FRAMES          2000
#define ASSUME_FRONT_C  1
#define TICKS_PER_MS    (BSP_CLINT_HZ / 1000u)

typedef struct { int x; u32 color; } ruler_t;
static const ruler_t g_ruler[RULER_N] = {
    {    0, 0x00FF0000u }, {  480, 0x0000FF00u }, {  960, 0x000000FFu },
    { 1440, 0x00FFFF00u }, { 1912, 0x00000000u }
};

static int   g_x, g_y, g_vx;            /* 位置与速度(只水平跑) */
static int   g_lane;                    /* 当前跑道: 0 = 下方(不翻), 1 = 上方(垂直翻转) */
static int   g_asset;                   /* 当前动作帧 0/1 */
static int   g_flip;                    /* 朝向: bit0(MX) 往右跑时置; bit1(MY) 上方跑道置 */
static int   g_fb_x[2], g_fb_y[2];      /* 每块缓冲各记一份: 上次画进去时它在哪 */
static u32   g_cmd_cnt, g_px;

static void fill_rect(u32 base, int x, int y, int w, int h, u32 color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > 1920) w = 1920 - x;
    if (y + h > 1080) h = 1080 - y;
    if (w <= 0 || h <= 0) return;

    CI_gpu2d_fillRect(base, (u32)x, (u32)y, (u32)w, (u32)h, color);
    g_cmd_cnt++;
    g_px += (u32)w * (u32)h;
}

/* 参考线(画在 sprite 下面) */
static void draw_refs_rect(u32 base, int x, int y, int w, int h)
{
    int k, x0, x1;

    if (BASE_Y >= y && BASE_Y < y + h)
        fill_rect(base, x, BASE_Y, w, 1, BASE_COLOR);

    for (k = 0; k < RULER_N; k++) {
        x0 = (g_ruler[k].x > x) ? g_ruler[k].x : x;
        x1 = (g_ruler[k].x + RULER_W < x + w) ? (g_ruler[k].x + RULER_W) : (x + w);
        if (x1 > x0)
            fill_rect(base, x0, y, x1 - x0, h, g_ruler[k].color);
    }
}

/* 把当前动作帧画到 (x, y) —— ★ 开 ck(白色像素不写) + 按朝向镜像。
 *   wpad = stride_word × 4 ：素材行按 4 像素打包后的宽度（镜像要求 4 对齐）
 *   pad  = wpad − 素材真实宽 ：行尾补的白像素个数
 *   ★ 只有【水平镜像 MX】时那几个白像素才跑到【最左边】，所以只有 MX 要把 dx 减掉 pad，
 *     画出来的图形位置才和正着画时严格对称（白像素被 ck 抠掉，看不见）。
 *     ★ 垂直镜像 MY 不改行内布局 ⇒ dx 一个像素都不挪。
 *   g_px 记的是【命令覆盖的像素数】, 透明的那部分没落盘, 所以它是上界。 */
static void draw_sprite(u32 base, int x, int y, int a, int flip)
{
    int wpad = (int)(g_assets[a].stride_word * 4u);
    int pad  = wpad - (int)g_assets[a].width;
    /*  ★ 只有【水平镜像 MX】才要减 pad: 行尾的白像素被翻到了左边。
     *    垂直镜像 MY 行内布局不变, 一个像素都不该挪 ——
     *    老写法是 flip ? (x-pad) : x, 一旦传进 MY 就会平白左移 pad 个像素。 */
    int dx   = (flip & GPU2D_FLIP_X) ? (x - pad) : x;

    CI_gpu2d_spriteFlipCK(g_assets[a].base_word, g_assets[a].stride_word,
                          base, (u32)dx, (u32)y,
                          (u32)wpad, g_assets[a].height,
                          (u32)flip);
    g_cmd_cnt++;
    g_px += (u32)wpad * g_assets[a].height;
}

static void repaint_all(void)
{
    const u32 fb[2] = { LB_FRAME_C, LB_FRAME_D };
    int k;

    for (k = 0; k < 2; k++) {
        fill_rect(fb[k], 0, 0, 1920, 1080, BG_COLOR);
        draw_refs_rect(fb[k], 0, 0, 1920, 1080);
        draw_sprite(fb[k], g_x, g_y, g_asset, g_flip);
        g_fb_x[k] = g_x;
        g_fb_y[k] = g_y;
    }
}

/* 一帧: 把"这块缓冲里的旧位置"擦掉补线, 再把 sprite 画到新位置 */
static void frame_draw(u32 back, int idx)
{
    int ox = g_fb_x[idx], oy = g_fb_y[idx];

    /*   ★ 擦的框覆盖【两种朝向 + 两帧】：镜像时图形会占满 x−pad..x+wpad */
    fill_rect(back, ox + SPR_BOX_DX, oy, SPR_BOX_W, SPR_BOX_H, BG_COLOR);   /* ① 擦旧位置 */
    draw_refs_rect(back, ox + SPR_BOX_DX, oy, SPR_BOX_W, SPR_BOX_H);        /* ② 旧位置补参考线 */

    /*  ★ 这里原来还有一步"给新位置补参考线, 再让 sprite 压上去" ——
     *    ck 开了之后不需要了: 透明处根本不写, 新位置原有的参考线/背景自动保留。
     *    （老代码留档在这里: draw_refs_rect(back, g_x, g_y, SPR_W, SPR_H); ） */

    draw_sprite(back, g_x, g_y, g_asset, g_flip);            /* ③ 画 sprite(白不写 + 按朝向镜像) */

    g_fb_x[idx] = g_x;                                      /* ④ 记下这块缓冲现在是什么 */
    g_fb_y[idx] = g_y;
}

/*------------------------------------------------------------------------------
 * 自检: 只查"空出来的那一片"(旧位置 减去 新位置) —— 那里应该被擦成背景/参考线。
 * sprite 内部的颜色模型在 C 这边没有(素材在 PRAM 里), 所以不查。
 *----------------------------------------------------------------------------*/
static u32 expect_pixel(int px, int py)
{
    int k;

    for (k = 0; k < RULER_N; k++)
        if (px >= g_ruler[k].x && px < g_ruler[k].x + RULER_W)
            return g_ruler[k].color;
    if (py == BASE_Y) return BASE_COLOR;
    return BG_COLOR;
}

static u32 chk_pixel(u32 base, int x, int y)
{
    volatile u8 *p = (volatile u8 *)(base + (u32)y * 5760u + (u32)x * 3u);
    return ((u32)p[2] << 16) | ((u32)p[1] << 8) | (u32)p[0];
}

static u32 check_frame(u32 back, int ox, int oy, u32 *nchk)
{
    u32 bad = 0;
    int xx, yy;

    data_cache_invalidate_all();

    for (yy = oy; yy < oy + SPR_BOX_H; yy++) {
        if (yy < 0 || yy >= 1080) continue;
        for (xx = ox + SPR_BOX_DX; xx < ox + SPR_BOX_DX + SPR_BOX_W; xx++) {
            u32 got;

            if (xx < 0 || xx >= 1920) continue;
            /*   ★ 跳过"新位置那一块"（框覆盖两种朝向）—— 那里是 sprite 自己 */
            if (xx >= g_x + SPR_BOX_DX && xx < g_x + SPR_BOX_DX + SPR_BOX_W &&
                yy >= g_y && yy < g_y + SPR_BOX_H) continue;

            got = chk_pixel(back, xx, yy);
            (*nchk)++;
            if (got != expect_pixel(xx, yy)) {
                bad++;
                if (bad <= 6)
                    bsp_printf("  [ERR 残留] (%d,%d) got=0x%06x exp=0x%06x\r\n",
                               xx, yy, got, expect_pixel(xx, yy));
            }
        }
    }
    return bad;
}

static u32 raw_pixel(int x, int y)
{
    volatile u8 *p = (volatile u8 *)(LB_FRAME_C + (u32)y * 5760u + (u32)x * 3u);
    return ((u32)p[2] << 16) | ((u32)p[1] << 8) | (u32)p[0];
}

static void raw_probe(void)
{
    static const int xs[5] = { 200, 700, 1200, 1700, 1900 };
    int i, ok = 1;

    for (i = 0; i < 5; i++) if (raw_pixel(xs[i], BASE_Y) != BASE_COLOR) ok = 0;
    for (i = 0; i < RULER_N; i++) if (raw_pixel(g_ruler[i].x, 540) != g_ruler[i].color) ok = 0;
    bsp_printf("  直读帧缓冲(硬编码 y*5760+x*3): 底部横线 5 点 + 5 条竖线 => %s\r\n",
               ok ? "全部 OK" : "!! 有不对");
}

//==============================================================================
// 主流程
//==============================================================================
void demo_anim_run(void)
{
    u32 c0, t0, t_prev, t_total, f0, back;
    u32 frame, done = 0, err = 0, nchk = 0;
    u32 cmd_win = 0, busy_win = 0, px_win = 0, win_n = 0;
    u32 split_prev;
    int idx = 0, paused = 0, key_prev = 0;
    u32 pause_cnt = 0, pause_flip = 0;
    gpu2d_stats_t s0, s1;

    bsp_printf("\r\n==== 动图 Demo: sprite 素材跑动(PRAM 索引 + 调色板) ====\r\n");
    bsp_printf("  素材 %d 张: %s(%dx%d) / %s(%dx%d)\r\n", ASSET_NUM,
               g_assets[0].name, g_assets[0].width, g_assets[0].height,
               g_assets[1].name, g_assets[1].width, g_assets[1].height);
    bsp_printf("  每帧横移 %d 像素, 每 %d 帧换一次动作帧; 框 %dx%d\r\n",
               SPR_MOVE_X, SPR_ANIM_EVERY, SPR_BOX_W, SPR_BOX_H);
    bsp_printf("  朝向: 往右跑时水平镜像(flags[5]=MX) —— 素材是 run_left, 所以右边自动朝右\r\n");
    bsp_printf("  ★ 换道演示: 下方跑道 y=%d(只翻左右 MX), 上方跑道 y=%d(左右 MX + 垂直 MY)\r\n",
               LANE_Y_LOW, LANE_Y_HIGH);
    bsp_printf("    上方那个应该是【头朝下】的; 两条看起来一样 = MY 没生效\r\n");

    if (CI_gpu2d_checkId() != GPU2D_CI_ID) { bsp_printf("  !! CI 通路不对, 停\r\n"); return; }
    bsp_printf("  KEY2 = 暂停/继续(暂停时不写任何东西; 每 0.5 秒换一块缓冲显示)\r\n");
    bsp_printf("  开局: enable=%d split=%d done_gpu2d=%d\r\n",
               lineBuffer_isEnabled(), lineBuffer_isSplit(), lineBuffer_getDoneGpu2d());

    g_lane  = 0;                            /* 0 = 从下方跑道开始 */
    g_x     = SPR_BOUNCE_MIN;               /* 从左端出发, 先往右跑 */
    g_y     = LANE_Y_LOW;
    g_vx    = SPR_MOVE_X;
    g_asset = 0;
    g_flip  = GPU2D_FLIP_X;                 /* 往右 = 水平镜像（素材是 run_left） */

    gpu2d_exclusive_begin();
    c0 = gpu2d_getCmdDone();
    g_cmd_cnt = 0;
    repaint_all();
    if (gpu2d_exclusive_end(c0 + g_cmd_cnt) != 0)
        bsp_printf("  !! 首帧整屏绘制超时\r\n");
    bsp_printf("  首帧已就绪(%d 条命令), 开始跑 %d 帧 ...\r\n", g_cmd_cnt, FRAMES);
    raw_probe();

    back = ASSUME_FRONT_C ? LB_FRAME_C : LB_FRAME_D;
    idx  = ASSUME_FRONT_C ? 0 : 1;
    t0 = clint_getTimeLow(BSP_CLINT);
    t_prev = t0;
    t_total = 0u;
    split_prev = lineBuffer_isSplit();

    for (frame = 0; frame < FRAMES; frame++) {
        u32 kd = key_getData() & 0x4u;              /* KEY2 */
        if (kd && !key_prev) {
            paused = !paused;
            bsp_printf(paused ? "  ★ 暂停: 停止一切绘制, C/D 照旧翻页\r\n" : "  ★ 继续\r\n");
        }
        key_prev = kd;

        if (paused) {
            pause_cnt++;
            if (pause_cnt >= 30u) {
                u32 f0p = gpu2d_getFrameDone();
                pause_cnt = 0; pause_flip++;
                CI_gpu2d_fence();
                (void)gpu2d_waitFrame(f0p, 500000u);
                { u32 w = 0u; while (lineBuffer_getDoneGpu2d() && w < 40000u) { bsp_uDelay(1u); w++; } }
                bsp_printf("  ★ 暂停中: 第 %d 次翻页 => 屏幕上刚换到【另一块缓冲】\r\n", pause_flip);
            } else {
                bsp_uDelay(1000u);
            }
            continue;
        }

        if (lineBuffer_isSplit() != split_prev) {
            u32 tcmd;
            split_prev = lineBuffer_isSplit();
            gpu2d_exclusive_begin();
            tcmd = gpu2d_getCmdDone();
            g_cmd_cnt = 0;
            repaint_all();
            (void)gpu2d_exclusive_end(tcmd + g_cmd_cnt);
            bsp_printf("  ★ 显示模式变了 => 两块缓冲整屏重建\r\n");
        }

        back = (back == LB_FRAME_C) ? LB_FRAME_D : LB_FRAME_C;   /* 本帧画另一块 */
        idx  = (idx == 0) ? 1 : 0;

        g_x += g_vx;                                             /* 走一步 */
        if (g_x >= SPR_X_MAX) {                                  /* 到右端 -> 掉头往左 */
            g_x = SPR_X_MAX;
            g_vx = -SPR_MOVE_X;
        } else if (g_x <= SPR_BOUNCE_MIN) {                      /* 回到左端 = 跑完一个来回 */
            g_x = SPR_BOUNCE_MIN;
            g_vx = SPR_MOVE_X;
            g_lane ^= 1;                                         /* ★ 换跑道 */
            g_y = g_lane ? LANE_Y_HIGH : LANE_Y_LOW;
            bsp_printf("  ★ 换跑道: %s (y=%d, %s)\r\n",
                       g_lane ? "上方" : "下方", g_y,
                       g_lane ? "叠 MY 垂直翻转" : "只翻左右 MX");
        }
        /*   ★ 朝向 = 速度方向(MX) + 跑道(MY)：素材是 run_left(朝左)，往右跑就水平镜像；
         *     上方跑道再叠一个垂直翻转。
         *     ★ 两个镜像的【白边补偿】都只在 MX 时做 —— 见 draw_sprite。 */
        g_flip = ((g_vx > 0) ? GPU2D_FLIP_X : 0) | (g_lane ? GPU2D_FLIP_Y : 0);

        if ((frame % SPR_ANIM_EVERY) == 0u)                      /* 换动作帧 */
            g_asset = (g_asset + 1) % SPR_ANIM_N;

        g_cmd_cnt = 0;
        g_px = 0;
        gpu2d_getStats(&s0);
        {
            int ox = g_fb_x[idx], oy = g_fb_y[idx];
            frame_draw(back, idx);
            f0 = gpu2d_getFrameDone();
            CI_gpu2d_fence();
            /* ★★ 相位免疫(和 demo_split.c 同一个做法): 显示通道每帧按内部索引
             *   在 C/D 之间二选一, 而那个索引【软件读不到】, 开机初值还可能被上一次
             *   运行弄脏 —— 一旦和 ASSUME_FRONT_C 的假设差一位, 就会一直在画
             *   【正在显示的那一块】(撕裂, 而且自检查不出来, 因为它读的是 DDR)。
             *   把 C/D 两个基址槽都写成"本帧刚画好的这块", 索引取哪个都一样。 */
            lineBuffer_setFullBase(back, back);
            if (gpu2d_waitFrame(f0, 500000) != 0) { bsp_printf("  !! FENCE 超时, 停\r\n"); break; }
            done++;
            gpu2d_getStats(&s1);
            err += check_frame(back, ox, oy, &nchk);
        }
        {   /* 锁到显示帧边界 */
            u32 w = 0u;
            while (lineBuffer_getDoneGpu2d() && w < 40000u) { bsp_uDelay(1u); w++; }
        }

        cmd_win  += g_cmd_cnt + 1u;
        busy_win += (s1.busy_cyc - s0.busy_cyc) / 100u;
        px_win   += g_px;
        win_n++;

        if (win_n >= 60u) {
            u32 now = clint_getTimeLow(BSP_CLINT);
            u32 dt  = (now - t_prev) / TICKS_PER_MS;
            t_prev = now;
            t_total += dt;
            bsp_printf("  帧 %4d: 动作帧 %d, 命令 %2d, 填充 %5d 像素/帧, 忙 %3d us/帧, %d fps, 错 %d, IRQ帧 %d(最大间隔 %d us)\r\n",
                       frame, g_asset, cmd_win / win_n, px_win / win_n, busy_win / win_n,
                       (int)((win_n * 1000u) / (dt ? dt : 1u)), err,
                       (int)IRQ_frame_count(), (int)(IRQ_frame_maxGap() / 100u));
            cmd_win = 0; busy_win = 0; px_win = 0; win_n = 0;
        }
    }

    bsp_printf("---- 结束 ----\r\n");
    bsp_printf("  完成 %d 帧, 墙钟 %d ms -> 平均 %d fps\r\n",
               done, t_total, (int)((done * 1000u) / (t_total ? t_total : 1u)));
    bsp_printf("  自检: 采样 %d 点(只查空出来的那一片), 不一致 %d -> %s\r\n",
               nchk, err, err ? "FAIL" : "PASS");
}
