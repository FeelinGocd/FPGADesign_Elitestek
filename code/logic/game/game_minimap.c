//==============================================================================
//  game_minimap.c —— 右上角固定 6×6 小地图
//------------------------------------------------------------------------------
//  干什么      :
//    · 地图框固定，不随人物滚动；人物位置用白块表示。
//    · 初始只点亮【起点 + 上下左右邻居】。
//    · 进入新房间时点亮当前房间 + 上下左右邻居；进入下一层重置。
//    · 普通房灰 / 宝箱房黄 / Boss 房红 / 商店蓝；当前房间白。
//  上一级是谁  : game/game_main.c（每帧 hud_minimap）、game/game_room.c（换房刷新）
//  接口契约    :
//    · refresh 只点亮数据，不画；draw 每帧只重画缓存。
//    · 地图坐标 = game_room.c 的 6×6 网格坐标，方向与房间布局一致。
//==============================================================================
#include "game.h"
#include "CI_gpu2d.h"

#define MM_BG_X      (SCR_W - 84)      /* 1196，右上角，6×6 方形地图 */
#define MM_BG_Y      4
#define MM_BG_W      76
#define MM_BG_H      76
#define MM_CELL_X    (MM_BG_X + 3)     /* 1199 */
#define MM_CELL_Y    (MM_BG_Y + 3)     /* 7 */
#define MM_CELL      10                /* ★ 6×6：6*10 + 5*2 = 70 px 方形 */
#define MM_GAP       2
#define MM_STEP      (MM_CELL + MM_GAP)

static unsigned char s_seen[ROOM_GRID_H][ROOM_GRID_W];
static int s_cur_x, s_cur_y;
static int s_mm_valid;

void game_minimap_reset(void)
{
    int x, y;

    for (y = 0; y < ROOM_GRID_H; ++y)
        for (x = 0; x < ROOM_GRID_W; ++x)
            s_seen[y][x] = 0;
    s_mm_valid = 0;
}

void game_minimap_refresh(void)
{
    static const int dx[4] = { 0, 1, 0, -1 };
    static const int dy[4] = { -1, 0, 1, 0 };
    int gx, gy, d;

    game_room_pos(&gx, &gy);
    if (gx < 0 || gy < 0) return;

    s_cur_x = gx;
    s_cur_y = gy;
    if (game_room_type_at(gx, gy) != ROOM_NONE) s_seen[gy][gx] = 1;
    for (d = 0; d < 4; ++d) {
        int nx = gx + dx[d];
        int ny = gy + dy[d];
        if (game_room_type_at(nx, ny) != ROOM_NONE) s_seen[ny][nx] = 1;
    }
    s_mm_valid = 1;
}

static u32 mm_color(int type)
{
    switch (type) {
    case ROOM_ITEM: return 0x00FFD800u;   /* 宝箱：黄 */
    case ROOM_BOSS: return 0x00E02020u;   /* Boss：红 */
    case ROOM_SHOP: return 0x003060C0u;   /* 商店：蓝 */
    default:        return 0x00808080u;   /* 普通：灰 */
    }
}

void game_minimap_draw(u32 fb)
{
    int x, y;

    if (!s_mm_valid) return;

    /* 固定底板 */
    CI_gpu2d_fillRect(fb, MM_BG_X, MM_BG_Y, MM_BG_W, MM_BG_H, 0x00101018u);

    /* 已点亮房间之间的连接线（2 px，填在 2 px 缝里） */
    for (y = 0; y < ROOM_GRID_H; ++y) {
        for (x = 0; x < ROOM_GRID_W; ++x) {
            if (!s_seen[y][x]) continue;
            if (x + 1 < ROOM_GRID_W && s_seen[y][x + 1])
                CI_gpu2d_fillRect(fb, (u32)(MM_CELL_X + x * MM_STEP + MM_CELL),
                                  (u32)(MM_CELL_Y + y * MM_STEP + 4),
                                  MM_GAP, MM_GAP, 0x00505050u);
            if (y + 1 < ROOM_GRID_H && s_seen[y + 1][x])
                CI_gpu2d_fillRect(fb, (u32)(MM_CELL_X + x * MM_STEP + 4),
                                  (u32)(MM_CELL_Y + y * MM_STEP + MM_CELL),
                                  MM_GAP, MM_GAP, 0x00505050u);
        }
    }

    /* 房间块；当前房间白块 */
    for (y = 0; y < ROOM_GRID_H; ++y) {
        for (x = 0; x < ROOM_GRID_W; ++x) {
            int t;
            u32 c;

            if (!s_seen[y][x]) continue;
            t = game_room_type_at(x, y);
            if (t == ROOM_NONE) continue;
            c = (x == s_cur_x && y == s_cur_y) ? 0x00FFFFFFu : mm_color(t);
            CI_gpu2d_fillRect(fb, (u32)(MM_CELL_X + x * MM_STEP),
                                  (u32)(MM_CELL_Y + y * MM_STEP),
                                  MM_CELL, MM_CELL, c);
        }
    }
}
