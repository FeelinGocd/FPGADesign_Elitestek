//==============================================================================
//  game_room.c —— 房间系统：2×2 四间房 / 门的触发格 / 切换
//------------------------------------------------------------------------------
//  干什么      :
//    · 4 间房按 2×2 摆放：左上=初始房  右上=宝箱房  左下=空房  右下=Boss房
//    · 每间的四扇门里，朝邻居的通、靠外的锁死
//    · 触发规则：玩家【中心点】走进门底下那个 32×32 格 才换房间
//    · 进入新房间：出现在"进来那扇门"的内侧格
//  上一级是谁  : game/game_main.c（每帧 game_room_step / 待切换时 commit）
//  接口契约    :
//    · 门的朝向 = 地图里那格的旋转值（0 上 / 1 右 / 2 下 / 3 左）
//    · 4 间房现在共用同一张地图；以后单独画，改 game_world.c 里两张指针表即可
//  为什么这么做:
//    · 走格子触发比"矩形相交"稳：玩家比格子大，用中心点不会在门口反复横跳
//    · 换房要重铺两块缓冲 ⇒ 用独占模式一次性铺完，和开机的做法一致
//==============================================================================
#include "game.h"
#include "CI_gpu2d.h"
#include "user_gpu2d.h"
#include "user_lineBuffer.h"

#define ROOM_GRID_W 2
#define ROOM_GRID_H 2

/*  当前房间里那 4 扇门：朝向 + 触发格  */
typedef struct {
    int cx;         /* 触发格（中心点落在这里就换房）  */
    int cy;
    int dir;        /* 0 上 / 1 右 / 2 下 / 3 左  */
} door_info_t;

static int s_room;              /* 0..3 */
static int s_pending;           /* 有切换在等：主循环看到就 commit */
static int s_target;
static int s_entry_dir;
static int s_lock;              /* 还站在触发格里 ⇒ 不重复触发 */

static door_info_t s_doors[4];
static int s_door_count;

/*----------------------------------------------------------------------------*/
/*  从地图里扫出 4 扇门，算出它们的触发格  */
static void doors_build(void)
{
    const char (*m)[MAP_COLS + 1] = game_world_map();
    const unsigned char (*r)[MAP_COLS] = game_world_rot();
    int x, y, n = 0;

    s_door_count = 0;
    for (x = 0; x < 4; ++x) {
        s_doors[x].cx = 0;
        s_doors[x].cy = 0;
        s_doors[x].dir = 0;
    }

    for (y = 0; y < MAP_ROWS && n < 4; ++y)
        for (x = 0; x < MAP_COLS && n < 4; ++x) {
            int dir = r[y][x] & 3;

            if (m[y][x] != 'D') continue;

            s_doors[n].dir = dir;
            if (dir == 0)      { s_doors[n].cx = x;     s_doors[n].cy = y + 2; }  /* 上：往下两格  */
            else if (dir == 2) { s_doors[n].cx = x;     s_doors[n].cy = y - 1; }  /* 下：往上一格  */
            else if (dir == 1) { s_doors[n].cx = x - 2; s_doors[n].cy = y;     }  /* 右：往左两格  */
            else               { s_doors[n].cx = x + 2; s_doors[n].cy = y;     }  /* 左：往右两格  */
            ++n;
        }

    s_door_count = n;
}

/*----------------------------------------------------------------------------*/
/*  进门规则（透视）：
 *    · 上墙（整条）：人物可以【走进去】，最多一个身高（PH = 56 px）
 *      → y 最小到 ARENA_Y0 - PH = 72。这是"后墙"纵深，人物能整个躲进去。
 *    · 左 / 右 / 下：只能贴到墙根，进不去。  */
void game_room_door_bounds(int px, int py, int *x0, int *y0, int *x1, int *y1)
{
    (void)px; (void)py; (void)x0; (void)x1; (void)y1;

    if (*y0 > ARENA_Y0 - PH) *y0 = ARENA_Y0 - PH;
}

/*  当前房间【某一方向】的门通不通邻居（锁死的门不渲染、也走不出去）  */
int game_room_door_open(int dir)
{
    int gx = s_room % ROOM_GRID_W;
    int gy = s_room / ROOM_GRID_W;

    if (dir == 0) return gy > 0;                    /* 上 */
    if (dir == 2) return gy < ROOM_GRID_H - 1;      /* 下 */
    if (dir == 3) return gx > 0;                    /* 左 */
    return gx < ROOM_GRID_W - 1;                    /* 右 */
}

int game_room_mouth(int *x0, int *x1, int *y0)
{
    int i;

    for (i = 0; i < 4; ++i) {
        int dx0, dx1;

        if (s_doors[i].dir != 0) continue;
        dx0 = s_doors[i].cx * TILE;
        dx1 = dx0 + TILE;
        if (x0) *x0 = dx0 - PW + 1;
        if (x1) *x1 = dx1 - 1;
        if (y0) *y0 = ARENA_Y0 - PH;
        return 1;
    }
    return 0;
}

int game_room_id(void)
{
    return s_room;
}

int game_room_pending(void)
{
    return s_pending;
}

/*  把玩家摆进"某一扇门内侧"  */
static void place_at_door(int dir)
{
    int i;

    for (i = 0; i < 4; ++i) {
        int x, y;

        if (s_doors[i].dir != dir) continue;

        x = s_doors[i].cx * TILE;
        y = s_doors[i].cy * TILE;

        /*  ★ 落点：上门落在门洞里（还没进房间，走出来就是纵深）
         *    其余落在墙内侧 8 px。  */
        if (dir == 0)      y = ARENA_Y0 - PH + 8;
        else if (dir == 2) y = ARENA_Y1 - PH - 8;
        else if (dir == 3) x = ARENA_X0 + 8;
        else               x = ARENA_X1 - PW - 8;

        /*  ★ 上门允许停在门洞里（ARENA_Y0 - PH 起），其余夹在场地内  */
        if (x < ARENA_X0) x = ARENA_X0;
        if (x > ARENA_X1 - PW) x = ARENA_X1 - PW;
        if (dir == 0) { if (y < ARENA_Y0 - PH + 8) y = ARENA_Y0 - PH + 8; }
        else          { if (y < ARENA_Y0) y = ARENA_Y0; }
        if (y > ARENA_Y1 - PH) y = ARENA_Y1 - PH;

        g.x = x;
        g.y = y;
        return;
    }
}

/*----------------------------------------------------------------------------*/
/*  开局：回初始房，人物摆中间  */
void game_room_reset(void)
{
    s_room = 0;
    s_pending = 0;
    s_lock = 0;
    game_world_select(0);
    doors_build();

    g.x = (ARENA_X0 + ARENA_X1) / 2 - PW / 2;
    g.y = (ARENA_Y0 + ARENA_Y1) / 2 - PH / 2;
}

/*----------------------------------------------------------------------------*/
/*  每帧：玩家是否走到某个门底下那一格。
 *  ★ 判定 = 玩家矩形和触发格【有重叠】+ 玩家【贴住那一面墙】。
 *    只用中心点太苛刻：玩家 40×56、格子 32×32，中心要压进 32 格得贴到墙根 4 px 内。  */
static int trigger_hit(int i)
{
    int dir = s_doors[i].dir;
    int dx0 = s_doors[i].cx * TILE;
    int dx1 = dx0 + TILE;

    if (!game_room_door_open(dir)) return 0;    /* 锁死的门完全不触发 */

    /*  ★ 上门：走进墙里（y 比地板再小 8px）+ 横向对齐门那一格。
     *     不能用"和触发格重叠"：人整个走进墙后，矩形在触发格上方，重叠为 0。  */
    if (dir == 0)
        return (g.x < dx1 && g.x + PW > dx0) && (g.y <= ARENA_Y0 - 8);

    {
        int x0 = s_doors[i].cx * TILE;
        int y0 = s_doors[i].cy * TILE;
        int overlap = (g.x < x0 + TILE) && (g.x + PW > x0) &&
                      (g.y < y0 + TILE) && (g.y + PH > y0);

        if (!overlap) return 0;
        if (dir == 2) return g.y + PH >= ARENA_Y1 - 2;   /* 下：贴住 */
        if (dir == 3) return g.x <= ARENA_X0 + 2;        /* 左：贴住 */
        return g.x + PW >= ARENA_X1 - 2;                 /* 右：贴住 */
    }
}

void game_room_step(void)
{
    int i, hit = -1;

    if (s_pending) return;

    for (i = 0; i < 4; ++i)
        if (trigger_hit(i)) { hit = i; break; }

    if (hit < 0) { s_lock = 0; return; }     /* 离开触发格 ⇒ 解锁 */
    if (s_lock) return;
    s_lock = 1;

    {
        int gx = s_room % ROOM_GRID_W;
        int gy = s_room / ROOM_GRID_W;
        int nx = gx, ny = gy;

        if (s_doors[hit].dir == 0)      --ny;
        else if (s_doors[hit].dir == 2) ++ny;
        else if (s_doors[hit].dir == 3) --nx;
        else                            ++nx;

        if (nx < 0 || nx >= ROOM_GRID_W || ny < 0 || ny >= ROOM_GRID_H)
            return;                          /* 靠外的门：锁死 */

        s_target = ny * ROOM_GRID_W + nx;
        s_entry_dir = s_doors[hit].dir ^ 1;  /* 上↔下 / 左↔右 */
        s_pending = 1;
    }
}

/*----------------------------------------------------------------------------*/
/*  真正换房：状态 + 两块缓冲重铺  */
int game_room_commit(void)
{
    static const u32 fb[2] = { FB_C, FB_D };
    u32 cmd0;
    int bi;

    s_room = s_target;
    s_pending = 0;
    game_world_select(s_room);
    doors_build();

    /*  ★ 换房整屏重铺会把 HUD 数字/血量/道具池盖掉，作废它们的缓存，
     *     否则 STAGE/DMG/MOVE… 这些静态值会一直不重画（和重开同款处理）。  */
    game_hud_invalidate();
    game_vital_invalidate();
    game_item_invalidate();

    game_bullets_clear();                    /* 弹幕不跟着过门 */
    g.vx8 = 0; g.vy8 = 0; g.rx = 0; g.ry = 0;
    place_at_door(s_entry_dir);

    /*  ★ 换房 = 整屏重铺：作废旧帧的精灵位置跟踪（否则第一帧会去"恢复"旧房门口），
     *    并把 front 归零 —— commit 固定 present(FB_C)，和重开保持一致，
     *    否则下一帧可能画进正在显示的那块缓冲，闪一下。  */
    {
        int i;
        for (i = 0; i < N_BULLETS; ++i) {
            g.old_bullets[0][i].active = 0;
            g.old_bullets[1][i].active = 0;
        }
        g.old_item[0].active = 0;   g.old_item[1].active = 0;
        g.old_boss[0].active = 0;   g.old_boss[1].active = 0;
        g.old_player[0].active = 0; g.old_player[1].active = 0;
        g.front = 0;
    }

    cmd0 = gpu2d_getCmdDone();
    gpu2d_exclusive_begin();
    for (bi = 0; bi < 2; ++bi) game_draw_floor(fb[bi]);
    lineBuffer_setFullBase(FB_C, FB_D);
    if (gpu2d_exclusive_end(cmd0 + 2u * game_floor_cmd_count()) != 0) return 0;
    return game_present(FB_C);
}
