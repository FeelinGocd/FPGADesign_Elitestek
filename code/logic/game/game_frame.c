//==============================================================================
//  game_frame.c —— 标准矩形房间框架的生成规则（纯函数，无硬件依赖）
//------------------------------------------------------------------------------
//  规则见 game_frame.h。这里只做几何，不碰素材 / GPU / 地图池。
//==============================================================================
#include "game_frame.h"

/*  某条边的门格和"另一半"：
 *  返回 0 = 这条边没有门；否则 (*dx,*dy) 是 'D'，(*px,*py) 是门洞的空白格。  */
static int door_cells(const frame_geom_t *g, int side,
                      int *dx, int *dy, int *px, int *py)
{
    if (g->door[side] == FRAME_DOOR_NONE) return 0;

    switch (side) {
    case 0:                                     /* N：上边，门洞朝下 */
        *dx = (int)g->door[0];
        *dy = (int)g->ay0 - 2;
        *px = *dx;
        *py = *dy + 1;
        return 1;
    case 1:                                     /* E：右边，门洞朝左 */
        *dy = (int)g->door[1];
        *dx = (int)g->ax1 + 1;
        *px = *dx + 1;
        *py = *dy;
        return 1;
    case 2:                                     /* S：下边，门洞朝上 */
        *dx = (int)g->door[2];
        *dy = (int)g->ay1 + 1;
        *px = *dx;
        *py = *dy + 1;
        return 1;
    default:                                    /* W：左边，门洞朝右 */
        *dy = (int)g->door[3];
        *dx = (int)g->ax0 - 2;
        *px = *dx + 1;
        *py = *dy;
        return 1;
    }
}

/*  四角的角块：外圈四个 + 内圈四个。返回 0 = 这格不是角块。  */
static char corner_char(const frame_geom_t *g, int x, int y)
{
    int ox0 = (int)g->ax0 - 2, oy0 = (int)g->ay0 - 2;
    int ox1 = (int)g->ax1 + 2, oy1 = (int)g->ay1 + 2;
    int ix0 = (int)g->ax0 - 1, iy0 = (int)g->ay0 - 1;
    int ix1 = (int)g->ax1 + 1, iy1 = (int)g->ay1 + 1;

    if (y == oy0) { if (x == ox0) return 'X'; if (x == ox1) return 'C'; }
    if (y == oy1) { if (x == ox0) return 'Z'; if (x == ox1) return 'Y'; }
    if (y == iy0) { if (x == ix0) return 'X'; if (x == ix1) return 'C'; }
    if (y == iy1) { if (x == ix0) return 'Z'; if (x == ix1) return 'Y'; }
    return 0;
}

static int outside(const frame_geom_t *g, int x, int y)
{
    return x < 0 || y < 0 || x >= (int)g->cols || y >= (int)g->rows;
}

static int in_arena(const frame_geom_t *g, int x, int y)
{
    return x >= (int)g->ax0 && x <= (int)g->ax1 &&
           y >= (int)g->ay0 && y <= (int)g->ay1;
}

/*  黑边之外（墙带外扩 2 格以外的部分）  */
static int in_ring(const frame_geom_t *g, int x, int y)
{
    return x >= (int)g->ax0 - 2 && x <= (int)g->ax1 + 2 &&
           y >= (int)g->ay0 - 2 && y <= (int)g->ay1 + 2;
}

char frame_tile(const frame_geom_t *g, int x, int y)
{
    int d, dx, dy, px, py;

    if (!g || outside(g, x, y)) return '.';

    for (d = 0; d < 4; ++d) {
        if (!door_cells(g, d, &dx, &dy, &px, &py)) continue;
        if (x == dx && y == dy) return (char)(g->door_ch ? g->door_ch : 'D');
        if (x == px && y == py) return '.';
    }

    if (in_arena(g, x, y)) return (char)(g->floor_ch ? g->floor_ch : 'B');
    if (!in_ring(g, x, y)) return '.';

    if (g->corner) {
        char c = corner_char(g, x, y);
        if (c) return c;
    }
    return (char)(g->wall_ch ? g->wall_ch : 'W');
}

unsigned char frame_rot(const frame_geom_t *g, int x, int y)
{
    int d, dx, dy, px, py;
    int in_n, in_s, in_w, in_e;

    if (!g || outside(g, x, y)) return 0;

    for (d = 0; d < 4; ++d) {
        if (!door_cells(g, d, &dx, &dy, &px, &py)) continue;
        if (x == dx && y == dy) return (unsigned char)d;   /* 门的方向：N0 E1 S2 W3 */
        if (x == px && y == py) return 0;
    }

    if (in_arena(g, x, y)) return 0;
    if (!in_ring(g, x, y)) return 0;

    if (g->corner) {
        char c = corner_char(g, x, y);
        if (c) return 0;
    }

    /*  墙的木纹角度：先看"外圈那一行/列"，再看所在的边。
     *  这个顺序是从现有两张图里反推出来的（角落那一格的木纹属于外圈）。  */
    in_n = y <= (int)g->ay0 - 1;
    in_s = y >= (int)g->ay1 + 1;
    in_w = x <= (int)g->ax0 - 1;
    in_e = x >= (int)g->ax1 + 1;

    if (in_n && y == (int)g->ay0 - 2) return g->band_rot[0];
    if (in_s && y == (int)g->ay1 + 2) return g->band_rot[2];
    if (in_w) return g->band_rot[3];
    if (in_e) return g->band_rot[1];
    if (in_n) return g->band_rot[0];
    return g->band_rot[2];
}
