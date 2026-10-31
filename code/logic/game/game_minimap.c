//==============================================================================
//  game_minimap.c —— 右上角固定 7×7 小地图
//------------------------------------------------------------------------------
//  干什么      :
//    · 地图框固定，不随人物滚动；人物位置用白块表示。
//    · 初始只点亮【起点 + 上下左右邻居】。
//    · 进入新房间时点亮当前房间 + 上下左右邻居；进入下一层重置。
//    · 普通房灰 / 宝箱房黄 / Boss 房红 / 商店蓝；当前房间白。
//  上一级是谁  : game/game_main.c（每帧 hud_minimap）、game/game_room.c（换房刷新）
//  接口契约    :
//    · refresh 只点亮数据，不画；draw 每帧只重画缓存。
//    · 地图坐标 = game_room.c 的 7×7 网格坐标，方向与房间布局一致。
//==============================================================================
#include "game.h"
#include "CI_gpu2d.h"

#define MM_CELL      10                /* ★ 7×7：7*10 + 6*2 = 82 px 方形 */
#define MM_GAP       2
#define MM_STEP      (MM_CELL + MM_GAP)
#define MM_BG_W      (ROOM_GRID_W * MM_CELL + (ROOM_GRID_W - 1) * MM_GAP)   /* 82：正好等于 7×7 格子区域 */
#define MM_BG_H      (ROOM_GRID_H * MM_CELL + (ROOM_GRID_H - 1) * MM_GAP)
#define MM_BG_X      (SCR_W - MM_BG_W) /* 1198，右上角 */
#define MM_BG_Y      4
#define MM_CELL_X    MM_BG_X
#define MM_CELL_Y    MM_BG_Y
#define MM_BG_COLOR  0x00101018u

static unsigned char s_seen[ROOM_GRID_H][ROOM_GRID_W];
static int s_cur_x, s_cur_y;
static int s_mm_valid;
static int s_mm_dirty = 1;

void game_minimap_reset(void)
{
    int x, y;

    for (y = 0; y < ROOM_GRID_H; ++y)
        for (x = 0; x < ROOM_GRID_W; ++x)
            s_seen[y][x] = 0;
    s_mm_valid = 0;
    s_mm_dirty = 1;
}

void game_minimap_refresh(void)
{
    static const signed char dx[4] = { 0, 1, 0, -1 };
    static const signed char dy[4] = { -1, 0, 1, 0 };
    int gx, gy, d;

    game_room_pos(&gx, &gy);
    if (gx < 0 || gy < 0) return;

    s_cur_x = gx;
    s_cur_y = gy;

    /*  ★ 多格房（Boss 2×2 / L 形）：当前房间占的每一格都点亮，
     *    再把它们的 4 邻居点亮 —— 否则 L 里离锚点远的那几格会一直是黑的。  */
    {
        int x, y;
        int self = game_room_owner_at(gx, gy);

        for (y = 0; y < ROOM_GRID_H; ++y)
            for (x = 0; x < ROOM_GRID_W; ++x)
                if (self >= 0 && game_room_owner_at(x, y) == self) s_seen[y][x] = 1;

        for (y = 0; y < ROOM_GRID_H; ++y)
            for (x = 0; x < ROOM_GRID_W; ++x) {
                if (!s_seen[y][x]) continue;
                if (self >= 0 && game_room_owner_at(x, y) != self) continue;
                for (d = 0; d < 4; ++d) {
                    int nx = x + dx[d];
                    int ny = y + dy[d];
                    if (nx < 0 || nx >= ROOM_GRID_W || ny < 0 || ny >= ROOM_GRID_H) continue;
                    /*  ★ 隐藏房不靠"邻居点亮"暴露：只有真走进去过才亮
                     *    （进房时由上面"当前房间每一格"那段点亮）。  */
                    if (game_room_type_at(nx, ny) == ROOM_HIDDEN) continue;
                    if (game_room_type_at(nx, ny) != ROOM_NONE) s_seen[ny][nx] = 1;
                }
            }
    }

    /*  已经被看见的多格房：只要看到其中一格，整间（2×2 / L）一起点亮。
     *  ★ 以前这里挑的是"第一个被看见的房间"，1×1 房也算 —— 于是永远只点亮它自己，
     *    Boss 2×2 在没进去之前只显示挨着当前房间的那一格（看着像个 1×1 图标）。  */
    {
        int x, y;

        for (y = 0; y < ROOM_GRID_H; ++y)
            for (x = 0; x < ROOM_GRID_W; ++x) {
                int o, xx, yy;

                if (!s_seen[y][x]) continue;
                if (game_room_type_at(x, y) == ROOM_NONE) continue;
                o = game_room_owner_at(x, y);
                if (o < 0 || o == y * ROOM_GRID_W + x) continue;    /* 1×1 房：不用补 */
                for (yy = 0; yy < ROOM_GRID_H; ++yy)
                    for (xx = 0; xx < ROOM_GRID_W; ++xx)
                        if (game_room_owner_at(xx, yy) == o) s_seen[yy][xx] = 1;
            }
    }
    s_mm_valid = 1;
    s_mm_dirty = 1;
}

/*  颜色和"是不是特殊房"都在 room_kind_t 表里（game_layout.c），这里只查表  */
static u32 mm_color(int type)
{
    return (u32)game_kind(type)->color;
}

void game_minimap_rect(int *x, int *y, int *w, int *h)
{
    *x = MM_BG_X; *y = MM_BG_Y; *w = MM_BG_W; *h = MM_BG_H;
}

static void minimap_draw_contents(u32 fb)
{
    int x, y;

    if (!s_mm_valid) return;

    /* 固定底板 */
    CI_gpu2d_fillRect(fb, MM_BG_X, MM_BG_Y, MM_BG_W, MM_BG_H, MM_BG_COLOR);

    /* 已点亮房间之间的连接线（2 px，填在 2 px 缝里）。
     *  ★ 判据 = 【两边格子所属的房间号不同】：
     *      · 同一间多格房（L / Boss）的内部缝，两边 o 相同 ⇒ 不画，
     *        所以 L 方块中间不会再冒出灰点（2026-10-24 修过一次的方向）；
     *      · 以前写的是"两边都必须是 1×1 房的锚点格"，于是 L 的覆盖格
     *        (锚点右边那格 / 下边那格) 往外的连线一条都画不出来 ——
     *        L 跟东边、南边那间房明明有门，小地图上却像没连通。
     *    ★ Boss 2×2 的四格也是同一个 o，同样受益。  */
    for (y = 0; y < ROOM_GRID_H; ++y) {
        for (x = 0; x < ROOM_GRID_W; ++x) {
            int o1, o2;

            if (!s_seen[y][x]) continue;
            o1 = game_room_owner_at(x, y);
            if (o1 < 0) continue;

            if (x + 1 < ROOM_GRID_W && s_seen[y][x + 1]) {
                o2 = game_room_owner_at(x + 1, y);
                if (o2 >= 0 && o2 != o1)
                    CI_gpu2d_fillRect(fb, (u32)(MM_CELL_X + x * MM_STEP + MM_CELL),
                                      (u32)(MM_CELL_Y + y * MM_STEP + 4),
                                      MM_GAP, MM_GAP, 0x00505050u);
            }
            if (y + 1 < ROOM_GRID_H && s_seen[y + 1][x]) {
                o2 = game_room_owner_at(x, y + 1);
                if (o2 >= 0 && o2 != o1)
                    CI_gpu2d_fillRect(fb, (u32)(MM_CELL_X + x * MM_STEP + 4),
                                      (u32)(MM_CELL_Y + y * MM_STEP + MM_CELL),
                                      MM_GAP, MM_GAP, 0x00505050u);
            }
        }
    }

    /*  ★ 房间块：一间房只画一次。
     *    · 外接框被占满（1×1 / Boss 2×2）→ 整块矩形一次填，中心不会留缝；
     *    · 外接框没占满（L 形）→ 逐格画 + 只补"同一间房相邻两格之间"的缝隙，
     *      缺口那一侧一个像素都不碰，图上就是干净的 L。  */
    {
        unsigned char drawn[ROOM_COUNT];
        int i;

        for (i = 0; i < ROOM_COUNT; ++i) drawn[i] = 0;

        for (y = 0; y < ROOM_GRID_H; ++y) {
            for (x = 0; x < ROOM_GRID_W; ++x) {
                int t, o, x0, y0, x1, y1, xx, yy, cnt = 0, full;
                u32 c;

                if (!s_seen[y][x]) continue;
                t = game_room_type_at(x, y);
                if (t == ROOM_NONE) continue;
                o = game_room_owner_at(x, y);
                if (o < 0 || o >= ROOM_COUNT || drawn[o]) continue;
                drawn[o] = 1;

                x0 = x1 = x; y0 = y1 = y;
                for (yy = 0; yy < ROOM_GRID_H; ++yy)
                    for (xx = 0; xx < ROOM_GRID_W; ++xx) {
                        if (game_room_owner_at(xx, yy) != o) continue;
                        ++cnt;
                        if (xx < x0) x0 = xx;
                        if (xx > x1) x1 = xx;
                        if (yy < y0) y0 = yy;
                        if (yy > y1) y1 = yy;
                    }
                if (cnt <= 0) continue;

                c = mm_color(t);
                full = (cnt == (x1 - x0 + 1) * (y1 - y0 + 1));

                if (full) {
                    CI_gpu2d_fillRect(fb, (u32)(MM_CELL_X + x0 * MM_STEP),
                                          (u32)(MM_CELL_Y + y0 * MM_STEP),
                                          (u32)((x1 - x0 + 1) * MM_CELL + (x1 - x0) * MM_GAP),
                                          (u32)((y1 - y0 + 1) * MM_CELL + (y1 - y0) * MM_GAP), c);
                } else {
                    for (yy = y0; yy <= y1; ++yy)
                        for (xx = x0; xx <= x1; ++xx) {
                            int sr, sd;

                            if (game_room_owner_at(xx, yy) != o) continue;
                            CI_gpu2d_fillRect(fb, (u32)(MM_CELL_X + xx * MM_STEP),
                                                  (u32)(MM_CELL_Y + yy * MM_STEP),
                                                  MM_CELL, MM_CELL, c);

                            /*  ★ 补缝只能补【缝本身】：长度写一格（MM_CELL），不能写
                             *    MM_CELL + MM_GAP。多出来的那 2 px 是往垂直方向长的，
                             *    会在内缝的端头、格子外面多画一个 2×2 方块 —— L 形房因此
                             *    有两个凸起：一个在 A/B/C/缺口 的十字交叉点（中心），一个
                             *    越过 B 的右边缘（中心偏右）。2026-10-24 上板拍照抓到。  */
                            sr = (xx < x1) && game_room_owner_at(xx + 1, yy) == o;
                            sd = (yy < y1) && game_room_owner_at(xx, yy + 1) == o;

                            if (sr)   /* 竖缝：宽 2，高正好一格 */
                                CI_gpu2d_fillRect(fb, (u32)(MM_CELL_X + xx * MM_STEP + MM_CELL),
                                                      (u32)(MM_CELL_Y + yy * MM_STEP),
                                                      MM_GAP, MM_CELL, c);
                            if (sd)   /* 横缝：高 2，宽正好一格 */
                                CI_gpu2d_fillRect(fb, (u32)(MM_CELL_X + xx * MM_STEP),
                                                      (u32)(MM_CELL_Y + yy * MM_STEP + MM_CELL),
                                                      MM_CELL, MM_GAP, c);

                            /*  只有四格真同属这一间房（真十字）才补那个 2×2 交叉点；
                             *    L 形房不满足 ⇒ 凹角正好是缺口边界：不凸出去，也不会留黑点。  */
                            if (sr && sd && game_room_owner_at(xx + 1, yy + 1) == o)
                                CI_gpu2d_fillRect(fb, (u32)(MM_CELL_X + xx * MM_STEP + MM_CELL),
                                                      (u32)(MM_CELL_Y + yy * MM_STEP + MM_CELL),
                                                      MM_GAP, MM_GAP, c);
                        }
                }
            }
        }
    }

    /*  当前房间的白块【最后画】。
     *  ★ 以前它画在上面那个循环里，同一间房后画的格子会把它盖掉：
     *    L 形房的标记落在另一格上就整个消失；Boss 2×2 的标记被右边/下边那两格
     *    削掉两条边，看着就"很小"。  */
    {
        int self = game_room_owner_at(s_cur_x, s_cur_y);
        int rw = 1, rh = 1, cnt = 0, xx, yy, mx = s_cur_x, my = s_cur_y;
        int sumx = 0, sumy = 0, wx, wy, mk;
        int spanx, spany;

        if (self < 0) return;
        game_room_span(&rw, &rh);
        for (yy = 0; yy < ROOM_GRID_H; ++yy)
            for (xx = 0; xx < ROOM_GRID_W; ++xx) {
                if (game_room_owner_at(xx, yy) != self) continue;
                sumx += xx;
                sumy += yy;
                ++cnt;
            }
        if (cnt <= 0) return;

        if (cnt > 1 && cnt == rw * rh) {
            /*  整块都占（Boss 2×2）：白块画在外接框正中，尺寸按块放大  */
            spanx = rw * MM_CELL + (rw - 1) * MM_GAP;
            spany = rh * MM_CELL + (rh - 1) * MM_GAP;
            mk = 8;
            wx = MM_CELL_X + s_cur_x * MM_STEP + spanx / 2 - mk / 2;
            wy = MM_CELL_Y + s_cur_y * MM_STEP + spany / 2 - mk / 2;
        } else if (cnt == 1) {
            mk = 4;                                  /* 1×1 房：保持原来的小白点 */
            wx = MM_CELL_X + s_cur_x * MM_STEP + (MM_CELL - mk) / 2;
            wy = MM_CELL_Y + s_cur_y * MM_STEP + (MM_CELL - mk) / 2;
        } else {
            /*  L 形（缺一格）：画在离质心最近的那一格，别落到缺口上  */
            int best = 0x7FFFFFFF;

            for (yy = 0; yy < ROOM_GRID_H; ++yy)
                for (xx = 0; xx < ROOM_GRID_W; ++xx) {
                    int d;

                    if (game_room_owner_at(xx, yy) != self) continue;
                    d = (xx * cnt - sumx) * (xx * cnt - sumx) +
                        (yy * cnt - sumy) * (yy * cnt - sumy);
                    if (d < best) { best = d; mx = xx; my = yy; }
                }
            mk = 6;
            wx = MM_CELL_X + mx * MM_STEP + (MM_CELL - mk) / 2;
            wy = MM_CELL_Y + my * MM_STEP + (MM_CELL - mk) / 2;
        }
        CI_gpu2d_fillRect(fb, (u32)wx, (u32)wy, (u32)mk, (u32)mk, 0x00FFFFFFu);
    }
}

void game_minimap_draw(u32 fb)
{
    if (!s_mm_valid) return;
    if (s_mm_dirty) {
        /* Same painter and same pixels; only the destination is cached.
         * FIFO ordering completes these writes before the following copy. */
        minimap_draw_contents(DDR_MINIMAP_CACHE -
                              MM_BG_Y * 5760u - MM_BG_X * 3u);
        s_mm_dirty = 0;
    }
    CI_gpu2d_blitRect(DDR_MINIMAP_CACHE, 0u, 0u,
                      game_view_fb(fb), MM_BG_X, MM_BG_Y, MM_BG_W, MM_BG_H);
}
