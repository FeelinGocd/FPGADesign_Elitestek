//==============================================================================
//  demo_glass.c —— 玻璃当背景层 + 平滑移动的元素（分层 z 序 + alpha 混合）
//------------------------------------------------------------------------------
//  分层怎么做的（★ 先看这条：本工程没有 z-buffer，z 序 = 【绘制顺序】）
//      硬件只按命令先后画，后画的盖前面的 —— 见 docs/spec/软件_驱动与API.md §2
//      和 软件_驱动与API.md §5 的"A 层编号"。所以这里把玻璃当成【中间一层】：
//
//          ① 底色色带            （最底）
//          ② z = 0 的元素        ⇒ 在玻璃【下面】：透过来看，被玻璃的格线/亮纹压住
//          ③ 玻璃瓦片            （中间层）
//          ④ z >= 2 的元素       ⇒ 在玻璃【上面】：清清楚楚，一点不被染色
//
//      同一层里再按 id 排（和 demo_split.c 的 g_order 一个道理：稳定排序）。
//
//  元素
//      · z=0 的色块若干个：要能看见"玻璃盖在它上面"
//      · z=2 的色块若干个：同一批颜色，但不受玻璃影响
//      · 跑步角色（PRAM 精灵 + ck + 左右镜像）画在最上面
//      ⇒ 对照着看：同一块颜色，一层写着"在里面"、一层写着"在外面"。
//
//  脏区怎么重画（★ 上板踩过，务必看懂）
//      重画一块矩形 = 把上面那四步按顺序再来一遍（repaint_rect）：
//          底色 FILL → z=0 元素 → 整块玻璃瓦片 → z>=2 元素
//      两个要点：
//        · 底色必须【先铺且不透明】—— 玻璃是半透的，直接重画玻璃压不住旧内容 ⇒ 拖影；
//        · 擦的范围要【对齐到 32 像素格子】—— 玻璃瓦片只有对齐才拼得上，
//          对不齐会叠出第二层（半透叠半透 = 更实，出接缝）。
//      ★ 元素之间【不许重叠】（move_model 里会检查、撞上就掉头）——
//        这样一小块脏区只要重画"和它相交的元素"就是精确的；
//        允许重叠的话就得把整屏当一个簇重画（代价见 软件_驱动与API.md §6）。
//
//  每帧节奏（照 demo_anim.c，别自己发明）
//      只画【不在显示的那一块】→ fence → 先交帧 setFullBase(back,back) → waitFrame → 等翻页。
//      每块缓冲【各记一份旧位置】（软件_驱动与API.md §6.2）。
//
//  自检（不依赖调色板）：铺完背景、画元素之前跑一次
//      同一局部格点 (dx,dy)：alpha 区 A、ck 对照条 S。
//      · A != S 的点 = 玻璃里【半透明】的像素 ⇒ 数出来应该 = 84
//      · 透明处 A 必须原样等于底色；所有点逐通道查"混合值落在底色与实色之间"
// ==============================================================================
#include "demo_glass.h"
#include "CI_gpu2d.h"
#include "user_gpu2d.h"
#include "user_lineBuffer.h"
#include "user_key.h"
#include "assets_desc.h"          /* g_assets[] (pic2pram.py 生成) */
#include "vexriscv.h"             /* data_cache_invalidate_all */
#include "bsp.h"

/* ---- 屏幕几何 ------------------------------------------------------------
 *   ★ 这两块地址必须和 main.c 里的 LB_FRAME_C / LB_FRAME_D 一致 */
#define GLASS_LB_C       0x08000000u
#define GLASS_LB_D       0x08800000u
#define SCR_W            1920
#define SCR_H            1080
#define SCR_LINE_BYTES   5760u

/* ---- 素材 ---------------------------------------------------------------- */
#define GLASS_IDX        12             /* minecraft_glass.png（32x32，带半透）*/
#define CELL             32
#define RUN_IDX0         0              /* 跑步帧 0（63x89，PRAM + ck）*/
#define RUN_IDX1         1
#define RUN_W            60             /* 取到 4 的倍数：MX 要求 w 是 4 的倍数 */
#define RUN_H            89

/* ---- 底色：4 道横向色带（暗 → 亮）----------------------------------------- */
#define BAND_H           270
#define BAND_N           4
static const u32 g_band[BAND_N] = { 0x182028u, 0x2A3440u, 0x3E4A58u, 0x55636Fu };

/* ---- ck 硬边对照条（最左 256 像素 = 8 格）--------------------------------- */
#define REF_W            256

/* ---- 玻璃里 α=128 的像素数（从素材数出来的；重画素材要改这里）-------------- */
#define GLASS_HALF_PX    84

/* ---- 图层 ----------------------------------------------------------------
 *   Z_GLASS 不存在实体里，它就是"玻璃瓦片画在中间"这件事；
 *   元素只分"玻璃之下"和"玻璃之上"两类，中间那一层永远是玻璃。 */
#define Z_UNDER          0              /* 玻璃之下：会被玻璃的格线/亮纹压住 */
#define Z_OVER           2              /* 玻璃之上：清清楚楚 */

/* ---- 元素 ---------------------------------------------------------------- */
#define EL_MAX           18
#define UNDER_N          6              /* 玻璃下的色块 */
#define OVER_N           8              /* 玻璃上的色块 */
#define RUN_N            2              /* 跑步角色（也在玻璃上）*/
#define SPR_ANIM_EVERY   4              /* 每几帧换一个跑步动作 */
#define FRAMES           1800           /* 跑多少帧（60fps 约 30 秒）*/
#define TICKS_PER_MS     (BSP_CLINT_HZ / 1000u)

#define K_BLOCK          0
#define K_RUN            1

typedef struct {
    int x, y, w, h;         /* 矩形（像素）*/
    int vx, vy;             /* 每帧走几像素 */
    int z;                  /* Z_UNDER / Z_OVER */
    int id;                 /* 同层里按它排（稳定顺序）*/
    int kind;               /* K_BLOCK / K_RUN */
    int flip;               /* K_RUN：要不要水平镜像 */
    u32 color;              /* K_BLOCK 颜色 */
    int asset;              /* K_RUN 素材下标 */
} el_t;

static el_t  g_el[EL_MAX];
static int   g_oldx[2][EL_MAX];         /* ★ 每块缓冲各记一份"上次画在哪" */
static int   g_oldy[2][EL_MAX];
static int   g_nel;
static int   g_disp = 0;                /* 正在显示哪一块（main.c 开局交的是 C）*/
static u32   g_rnd = 0x1234ABCDu;       /* 固定种子 ⇒ 每次上电画面一样 */
static u32   g_cmd;
static u32   g_anim;

static const u32 g_pal[8] = {
    0x00FF4040u, 0x0040FF40u, 0x004040FFu, 0x00FFFF40u,
    0x00FF40FFu, 0x0040FFFFu, 0x00FFA040u, 0x00A0A0FFu
};

static u32 band_at(int y)
{
    int i = y / BAND_H;

    if (i < 0) i = 0;
    if (i >= BAND_N) i = BAND_N - 1;
    return g_band[i];
}

static u32 rnd_next(void)
{
    g_rnd = g_rnd * 1103515245u + 12345u;
    return (g_rnd >> 16) & 0x7FFFu;
}

static u32 rd_px(u32 fb, int x, int y)
{
    volatile u8 *p = (volatile u8 *)(fb + (u32)y * SCR_LINE_BYTES + (u32)x * 3u);

    return (((u32)p[2] << 16) | ((u32)p[1] << 8) | (u32)p[0]);
}

static int rect_hit(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh)
{
    return !(ax + aw <= bx || bx + bw <= ax || ay + ah <= by || by + bh <= ay);
}

/*------------------------------------------------------------------------------
 * 底色：矩形按色带切段，全用不透明 FILL
 *----------------------------------------------------------------------------*/
static void band_fill(u32 fb, int x, int y, int w, int h)
{
    int yy = y, ye;

    while (yy < y + h) {
        ye = ((yy / BAND_H) + 1) * BAND_H;
        if (ye > y + h) ye = y + h;
        CI_gpu2d_fillRect(fb, (u32)x, (u32)yy, (u32)w, (u32)(ye - yy), band_at(yy));
        g_cmd++;
        yy = ye;
    }
}

/*  一个格子的玻璃瓦片（左边那条用 ck 硬边当对照）
 *   ★ 1080 不是 32 的整数倍（33 行差 24 像素）⇒ 最后一行按实际高度裁剪 */
static void glass_tile(u32 fb, int tx, int ty)
{
    int h = CELL;

    if (ty + h > SCR_H) h = SCR_H - ty;
    if (h <= 0) return;
    if (tx < REF_W)
        CI_gpu2d_spriteRectCK(g_assets[GLASS_IDX].base_word,
                              g_assets[GLASS_IDX].stride_word,
                              fb, (u32)tx, (u32)ty, CELL, (u32)h);
    else
        CI_gpu2d_spriteAlphaRect(g_assets[GLASS_IDX].base_word,
                                 g_assets[GLASS_IDX].stride_word,
                                 fb, (u32)tx, (u32)ty, CELL, (u32)h, 0u);
    g_cmd++;
}

/*  画一个元素 */
static void draw_elem(u32 fb, int i)
{
    el_t *e = &g_el[i];

    if (e->kind == K_BLOCK) {
        CI_gpu2d_fillRect(fb, (u32)e->x, (u32)e->y, (u32)e->w, (u32)e->h, e->color);
        g_cmd++;
    } else {
        CI_gpu2d_spriteFlipCK(g_assets[e->asset].base_word,
                              g_assets[e->asset].stride_word,
                              fb, (u32)e->x, (u32)e->y, (u32)e->w, (u32)e->h,
                              e->flip ? GPU2D_FLIP_X : 0u);
        g_cmd++;
    }
}

/*  按 z 序画【和这个矩形相交】的元素（同层按 id）*/
static void draw_layer(u32 fb, int x, int y, int w, int h, int zlo, int zhi)
{
    int z, i;

    for (z = 0; z <= 8; z++) {
        if (z < zlo || z > zhi) continue;
        for (i = 0; i < g_nel; i++) {
            el_t *e = &g_el[i];

            if (e->z != z) continue;
            if (!rect_hit(x, y, w, h, e->x, e->y, e->w, e->h)) continue;
            draw_elem(fb, i);
        }
    }
}

/*------------------------------------------------------------------------------
 * ★ 重画一块矩形：把"这一块该长什么样"按分层顺序重新画一遍
 *      ① 不透明底色  ② 玻璃之下的元素  ③ 玻璃瓦片  ④ 玻璃之上的元素
 *   范围先对齐到 32 像素格子（玻璃瓦片只有对齐才拼得上）
 *----------------------------------------------------------------------------*/
static void repaint_rect(u32 fb, int x, int y, int w, int h)
{
    int x0 = (x / CELL) * CELL,                       y0 = (y / CELL) * CELL;
    int x1 = ((x + w + CELL - 1) / CELL) * CELL,      y1 = ((y + h + CELL - 1) / CELL) * CELL;
    int tx, ty;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > SCR_W) x1 = SCR_W;
    if (y1 > SCR_H) y1 = SCR_H;
    if (x1 <= x0 || y1 <= y0) return;

    band_fill(fb, x0, y0, x1 - x0, y1 - y0);                  /* ① 不透明底色 */
    draw_layer(fb, x0, y0, x1 - x0, y1 - y0, Z_UNDER, Z_UNDER);/* ② 玻璃之下 */
    for (ty = y0; ty < y1; ty += CELL)                        /* ③ 玻璃这一层 */
        for (tx = x0; tx < x1; tx += CELL)
            glass_tile(fb, tx, ty);
    draw_layer(fb, x0, y0, x1 - x0, y1 - y0, Z_OVER, 8);      /* ④ 玻璃之上 */
}

/*------------------------------------------------------------------------------
 * 模型走一步：位置 + 边界反弹 + 不许互相重叠 + 跑步换帧
 *   ★ 不许重叠是这套脏区重画的【前提】（见文件头）：撞上就掉头，不硬挤。
 *----------------------------------------------------------------------------*/
static void move_model(void)
{
    int i, j;

    g_anim++;
    for (i = 0; i < g_nel; i++) {
        el_t *e = &g_el[i];
        int nx, ny, bad;

        nx = e->x + e->vx;
        ny = e->y + e->vy;

        /* 撞屏幕边就反弹 */
        if (nx < 0)             { nx = 0;             e->vx = -e->vx; }
        if (nx > SCR_W - e->w)  { nx = SCR_W - e->w;  e->vx = -e->vx; }
        if (ny < 0)             { ny = 0;             e->vy = -e->vy; }
        if (ny > SCR_H - e->h)  { ny = SCR_H - e->h;  e->vy = -e->vy; }

        /* 撞别的元素就掉头（本帧不动）*/
        bad = 0;
        for (j = 0; j < g_nel; j++) {
            if (j == i) continue;
            if (!rect_hit(nx, ny, e->w, e->h, g_el[j].x, g_el[j].y, g_el[j].w, g_el[j].h)) continue;
            if (!rect_hit(e->x, e->y, e->w, e->h, g_el[j].x, g_el[j].y, g_el[j].w, g_el[j].h)) break;
            bad = 1;
            break;
        }
        if (bad) { e->vx = -e->vx; e->vy = -e->vy; continue; }

        e->x = nx;
        e->y = ny;
        if (e->kind == K_RUN) {
            e->flip  = (e->vx > 0) ? 1 : 0;             /* 往右跑就水平镜像 */
            e->asset = ((g_anim / SPR_ANIM_EVERY) & 1) ? RUN_IDX1 : RUN_IDX0;
        }
    }
}

static void init_model(void)
{
    int i, n = 0;

    g_rnd = 0x1234ABCDu;
    for (i = 0; i < UNDER_N; i++, n++) {            /* 玻璃之下 */
        el_t *e = &g_el[n];
        e->w = 32; e->h = 32; e->kind = K_BLOCK; e->z = Z_UNDER; e->id = i;
        e->x = 16 + (int)(rnd_next() % (u32)(SCR_W - 96));
        e->y = 96 + (int)(rnd_next() % (u32)(SCR_H - 192));
        e->vx = ((int)(rnd_next() % 7u)) - 3;
        e->vy = ((int)(rnd_next() % 7u)) - 3;
        if (e->vx == 0 && e->vy == 0) e->vx = 2;
        e->color = g_pal[i & 7u];
    }
    for (i = 0; i < OVER_N; i++, n++) {             /* 玻璃之上 */
        el_t *e = &g_el[n];
        e->w = 32; e->h = 32; e->kind = K_BLOCK; e->z = Z_OVER; e->id = i;
        e->x = 16 + (int)(rnd_next() % (u32)(SCR_W - 96));
        e->y = 96 + (int)(rnd_next() % (u32)(SCR_H - 192));
        e->vx = ((int)(rnd_next() % 7u)) - 3;
        e->vy = ((int)(rnd_next() % 7u)) - 3;
        if (e->vx == 0 && e->vy == 0) e->vy = 2;
        e->color = g_pal[(i + 4) & 7u];
    }
    for (i = 0; i < RUN_N; i++, n++) {              /* 跑步角色：玻璃之上 */
        el_t *e = &g_el[n];
        e->w = RUN_W; e->h = RUN_H; e->kind = K_RUN; e->z = Z_OVER; e->id = 100 + i;
        e->x = 40 + i * 300;
        e->y = 300 + i * 200;
        e->vx = (i & 1) ? 3 : -3;
        e->vy = 0;
        e->asset = RUN_IDX0;
        e->flip  = (e->vx > 0) ? 1 : 0;
    }
    g_nel = n;
}

/*------------------------------------------------------------------------------
 * 自检：拿 ck 对照条当参照，验混合
 *----------------------------------------------------------------------------*/
static void self_check(u32 fb)
{
    int dx, dy, shown = 0;
    u32 n_diff = 0, n_tok = 0, n_tbad = 0, n_range = 0;

    data_cache_invalidate_all();

    for (dy = 0; dy < CELL; dy++) {
        for (dx = 0; dx < CELL; dx++) {
            u32 A = rd_px(fb, REF_W + dx, dy);
            u32 S = rd_px(fb, dx, dy);
            u32 B = band_at(dy);
            int c;

            if (A != S) {
                n_diff++;
                if (shown < 3) {
                    shown++;
                    bsp_printf("    [样本] 局部(%2d,%2d) 底色=%06x ck=%06x alpha=%06x\r\n",
                               dx, dy, B, S, A);
                }
            }
            if (S == B) {
                if (A == B) n_tok++; else n_tbad++;
            } else {
                for (c = 0; c < 3; c++) {
                    u32 ab = (A >> (c * 8)) & 0xFFu;
                    u32 bb = (B >> (c * 8)) & 0xFFu;
                    u32 sb = (S >> (c * 8)) & 0xFFu;
                    u32 lo = (bb < sb) ? bb : sb;
                    u32 hi = (bb < sb) ? sb : bb;

                    if (ab < lo || ab > hi) { n_range++; break; }
                }
            }
        }
    }
    bsp_printf("  [自检] 一格 %dx%d: alpha 与 ck 不同的点 = %d (素材里 alpha=128 的像素 = %d) %s\r\n",
               CELL, CELL, (int)n_diff, GLASS_HALF_PX,
               (n_diff == GLASS_HALF_PX) ? "OK" : "!! 不对");
    bsp_printf("         透明处(应原样留底色): 对 %d / 错 %d ; 混合值越界的点: %d  %s\r\n",
               (int)n_tok, (int)n_tbad, (int)n_range,
               (n_tbad == 0u && n_range == 0u) ? "OK" : "!! 不对");
}

/*==============================================================================
 * 入口
 *============================================================================*/
void demo_glass_run(void)
{
    u32 c0, cmds, f0, t0, t_prev, cmd_sum, cmd_max;
    int frame, back, bi, i;
    int split_prev;
    u32 key_prev = 0u;

    bsp_printf("\r\n==== alpha 玻璃背景 Demo（分层 z 序 + 平滑移动）====\r\n");

    if (CI_gpu2d_checkId() != GPU2D_CI_ID) { bsp_printf("  !! CI 通路不对, 停\r\n"); return; }
    if (ASSET_NUM <= GLASS_IDX) {
        bsp_printf("  !! 素材表只有 %d 张, 拿不到第 %d 张(玻璃) —— 先跑 pic2pram.py\r\n", ASSET_NUM, GLASS_IDX);
        return;
    }
    if (g_assets[GLASS_IDX].width != CELL || g_assets[GLASS_IDX].height != CELL) {
        bsp_printf("  !! 第 %d 张素材不是 %dx%d\r\n", GLASS_IDX, CELL, CELL);
        return;
    }
    bsp_printf("  分层(靠绘制顺序, 硬件没有 z-buffer): 底色 → 玻璃下 %d 块 → 玻璃 → 玻璃上 %d 块 + %d 跑步\r\n",
               UNDER_N, OVER_N, RUN_N);
    bsp_printf("  ★ 同一批颜色: 玻璃下的会被玻璃格线压住(看起来在里面), 玻璃上的干干净净\r\n");

    init_model();

    /* ---- 关显示 → 两块缓冲各铺一遍背景(不画元素) → 开显示 ---- */
    gpu2d_exclusive_begin();
    c0 = gpu2d_getCmdDone();
    g_cmd = 0u;
    repaint_rect(GLASS_LB_C, 0, 0, SCR_W, SCR_H);
    repaint_rect(GLASS_LB_D, 0, 0, SCR_W, SCR_H);
    CI_gpu2d_fence();
    cmds = g_cmd + 1u;
    if (gpu2d_exclusive_end(c0 + cmds) != 0) bsp_printf("  !! 铺背景超时(FENCE 没回来)\r\n");
    lineBuffer_setFullBase(GLASS_LB_C, GLASS_LB_D);
    bsp_printf("  背景铺完: %d 条命令（两块各 %d 条）\r\n", (int)cmds, (int)(g_cmd / 2u));

    self_check(GLASS_LB_C);                           /* 这时是纯背景，先验混合 */

    /* ---- 两块缓冲各把元素画上（位置一样），并记下"画在哪" ---- */
    gpu2d_exclusive_begin();
    c0 = gpu2d_getCmdDone();
    g_cmd = 0u;
    for (bi = 0; bi < 2; bi++) {
        u32 fb = bi ? GLASS_LB_D : GLASS_LB_C;

        for (i = 0; i < g_nel; i++) {
            draw_elem(fb, i);
            g_oldx[bi][i] = g_el[i].x;
            g_oldy[bi][i] = g_el[i].y;
        }
    }
    CI_gpu2d_fence();
    if (gpu2d_exclusive_end(c0 + g_cmd + 1u) != 0) bsp_printf("  !! 首帧元素超时\r\n");
    bsp_printf("  首帧元素画完(%d 条), 开始跑 %d 帧 ...\r\n", (int)g_cmd, FRAMES);

    split_prev = lineBuffer_isSplit();
    t0 = clint_getTimeLow(BSP_CLINT);
    t_prev = t0;
    cmd_sum = 0u;
    cmd_max = 0u;

    for (frame = 0; frame < FRAMES; frame++) {
        u32 kd = key_getData() & 0x4u;              /* KEY2 = 暂停 */

        if (kd && !key_prev) {
            bsp_printf("  ★ KEY2 暂停（松开继续）\r\n");
            while (key_getData() & 0x4u) bsp_uDelay(1000u);
        }
        key_prev = kd;

        if (lineBuffer_isSplit() != split_prev) {
            split_prev = lineBuffer_isSplit();
            bsp_printf("  !! 显示模式变了(KEY0) —— 本 Demo 只在全屏下跑, 停\r\n");
            break;
        }

        back = (g_disp == 0) ? 1 : 0;               /* ★ 只画不在显示的那一块 */
        bi   = back;

        move_model();

        g_cmd = 0u;
        /*  重画脏区：每个元素"上次画在哪 + 现在在哪"两块都重画一遍（分层顺序见 repaint_rect）*/
        for (i = 0; i < g_nel; i++) {
            el_t *e = &g_el[i];
            u32 fb = (back == 0) ? GLASS_LB_C : GLASS_LB_D;

            if (g_oldx[bi][i] == e->x && g_oldy[bi][i] == e->y) continue;   /* 没动过 */
            repaint_rect(fb, g_oldx[bi][i], g_oldy[bi][i], e->w, e->h);
            repaint_rect(fb, e->x, e->y, e->w, e->h);
        }
        for (i = 0; i < g_nel; i++) { g_oldx[bi][i] = g_el[i].x; g_oldy[bi][i] = g_el[i].y; }

        cmd_sum += g_cmd;
        if (g_cmd > cmd_max) cmd_max = g_cmd;

        /*  交帧：照 demo_anim.c 的顺序（先 fence + 交帧，再等翻页）*/
        f0 = gpu2d_getFrameDone();
        CI_gpu2d_fence();
        lineBuffer_setFullBase((back == 0) ? GLASS_LB_C : GLASS_LB_D,
                               (back == 0) ? GLASS_LB_C : GLASS_LB_D);
        if (gpu2d_waitFrame(f0, 500000u) != 0) { bsp_printf("  !! FENCE 超时, 停\r\n"); break; }
        {
            u32 w = 0u;
            while (lineBuffer_getDoneGpu2d() && w < 40000u) { bsp_uDelay(1u); w++; }
        }
        g_disp = back;

        if ((frame % 300) == 299) {
            u32 now = clint_getTimeLow(BSP_CLINT);
            u32 d   = (now - t_prev) / TICKS_PER_MS;

            t_prev = now;
            bsp_printf("  帧 %4d: %d 条命令/帧(峰值 %d) | 300 帧 %d ms = %d fps\r\n",
                       frame + 1, (int)(cmd_sum / 300u), (int)cmd_max,
                       (int)d, (int)(300u * 1000u / (d ? d : 1u)));
            cmd_sum = 0u;
            cmd_max = 0u;
        }
    }

    {
        u32 ms2 = (clint_getTimeLow(BSP_CLINT) - t0) / TICKS_PER_MS;

        bsp_printf("  ★ 结束: %d 帧 / %d ms = %d fps\r\n",
                   FRAMES, (int)ms2, (int)((u32)FRAMES * 1000u / (ms2 ? ms2 : 1u)));
    }
}