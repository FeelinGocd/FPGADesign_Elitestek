//==============================================================================
//  game_stone.c —— 房间里的石头 / 木箱 / 板条箱（挡人、挡弹，石头可炸碎）
//------------------------------------------------------------------------------
//  数据模型：
//    · 【没有实体池】。位置和种类都在房间蓝图（game_layout.c 的 obj_place_t）里，
//      这里只按"当前房间用的是哪张布局"去遍历；
//    · 唯一会变的状态是"碎没碎"：game_room.c 里一张 (房间, 对象下标) 的小清单
//      （MAX_BROKEN 条），重进房间/死亡回退都按它跳过；
//    · 石头会被炸弹炸碎；木箱 / 板条箱只挡人挡弹，不会碎。
//  碰撞：
//    · 当前房间一张 32 px 位图（SG_*），换房或炸碎后重建；
//    · 玩家碰撞按"石头在上用脚、在下用眼睛"只挡迎面那条边。
//  画法：Z = ACTOR_Z(底边 y)，和人物同一把深度尺子，站在石头后面会被挡。
//==============================================================================
#include "game.h"
#include "CI_gpu2d.h"

#define STONE_SIZE 32
/* 直接使用绝对地图格，避免重复做竞技场原点换算，并覆盖全部合法 45×35 地图。
 * 旧的 24 行位图在 L 形房中可能越界。 */

/*  当前房间的石头占用表：32 px 格对齐，碰撞只查 1 个格 / 身体覆盖的几格。
 *  s_grid_room 与当前房间不一致时重建；换房/炸碎后置 -1。  */
#define SG_CELLS (MAP_MAX_COLS * MAP_MAX_ROWS)
#define SG_BITS  ((SG_CELLS + 7) / 8)
#define SG_IDX(i, j) ((j) * MAP_MAX_COLS + (i))
#define SG_GET(a, i) (((a)[(i) >> 3] >> ((i) & 7)) & 1u)
#define SG_SET(a, i) ((a)[(i) >> 3] |= (unsigned char)(1u << ((i) & 7)))

static unsigned char s_stone_grid[SG_BITS];
static signed char   s_grid_room = -1;
static unsigned char s_grid_has_blockers;

/*  当前房间的布局（没有布局 = 这间房没有石头）  */
static const room_layout_t *stone_layout(void)
{
    return game_layout(game_room_layout());
}

static int is_blocker(int kind)
{
    return kind == OBJ_STONE || kind == OBJ_BOX || kind == OBJ_CRATE;
}

/*  重建当前房间的石头占用表（换房后第一次碰撞/绘制查询时做一次）  */
static void stone_grid_build(void)
{
    const room_layout_t *L = stone_layout();
    signed char room = (signed char)game_room_id();
    int i, b;

    for (b = 0; b < SG_BITS; ++b) s_stone_grid[b] = 0;
    s_grid_has_blockers = 0u;

    if (L) {
        for (i = 0; i < (int)L->n_objs; ++i) {
            const obj_place_t *o = &L->objs[i];
            int gx, gy;

            if (!is_blocker((int)o->kind)) continue;
            if (game_room_cell_gone(room, (int)o->x, (int)o->y)) continue;      /* 已经炸掉的不占格 */
            game_layout_obj_cell(L, o, game_room_flip(), &gx, &gy);
            if (gx >= 0 && gx < MAP_MAX_COLS && gy >= 0 && gy < MAP_MAX_ROWS)
            {
                SG_SET(s_stone_grid, SG_IDX(gx, gy));
                s_grid_has_blockers = 1u;
            }
        }
    }
    s_grid_room = room;
}

static void stone_grid_ensure(void)
{
    if (s_grid_room != (signed char)game_room_id()) stone_grid_build();
}

void game_stone_invalidate(void)
{
    s_grid_room = -1;
}

int game_stone_has_blockers(void)
{
    stone_grid_ensure();
    return s_grid_has_blockers != 0u;
}

/*  石头按"相对上下"只挡迎面的那条边：
 *    · 石头在上、人物在下 → 用脚底（向上走时脚先碰到石头下沿）；
 *    · 反之石头在下、人物在上 → 用最上一排（眼睛，向下走时先碰到石头上沿）。
 *  这样人物上半身可以叠进石头，不会因为整条身体矩形擦到石头就被卡住。  */
static int stone_block_overlap(int px, int py, int sx, int sy)
{
    int ex, ey, ew, eh;
    game_creature_foot_rect(px, py, PW, PH, &ex, &ey, &ew, &eh);

    return ex < sx + STONE_SIZE && ex + ew > sx &&
           ey < sy + STONE_SIZE && ey + eh > sy;
}

/* 玩家当前位置是否与任一有效石头、木箱或板条箱相交。 */
int game_stone_block(int px, int py)
{
    const room_layout_t *L = stone_layout();
    signed char room = (signed char)game_room_id();
    int i;

    if (!L) return 0;
    for (i = 0; i < (int)L->n_objs; ++i) {
        const obj_place_t *o = &L->objs[i];
        int sx, sy;

        if (!is_blocker((int)o->kind)) continue;
        if (game_room_cell_gone(room, (int)o->x, (int)o->y)) continue;
        game_layout_obj_cell(L, o, game_room_flip(), &sx, &sy);
        sx *= TILE;
        sy *= TILE;
        if (stone_block_overlap(px, py, sx, sy)) return 1;
    }
    return 0;
}

/* 判断人物从旧位置移动到新位置时是否穿过阻挡物边界。 */
int game_stone_block_move(int old_px, int old_py, int px, int py)
{
    const room_layout_t *L = stone_layout();
    signed char room = (signed char)game_room_id();
    int old_l, old_t, old_w, old_h, old_r, old_b;
    int new_l, new_t, new_w, new_h, new_r, new_b;
    int dx = px - old_px, dy = py - old_py;
    int i;

    if (!L || (dx == 0 && dy == 0)) return 0;
    game_creature_foot_rect(old_px, old_py, PW, PH, &old_l, &old_t, &old_w, &old_h);
    game_creature_foot_rect(px, py, PW, PH, &new_l, &new_t, &new_w, &new_h);
    old_r = old_l + old_w; old_b = old_t + old_h;
    new_r = new_l + new_w; new_b = new_t + new_h;
    for (i = 0; i < (int)L->n_objs; ++i) {
        const obj_place_t *o = &L->objs[i];
        int sx, sy;

        if (!is_blocker((int)o->kind)) continue;
        if (game_room_cell_gone(room, (int)o->x, (int)o->y)) continue;
        game_layout_obj_cell(L, o, game_room_flip(), &sx, &sy);
        sx *= TILE;
        sy *= TILE;
        if (!stone_block_overlap(px, py, sx, sy)) continue;
        if (dx > 0 && old_r <= sx && new_r > sx) return 1;
        if (dx < 0 && old_l >= sx + STONE_SIZE && new_l < sx + STONE_SIZE) return 1;
        if (dy > 0 && old_b <= sy && new_b > sy) return 1;
        if (dy < 0 && old_t >= sy + STONE_SIZE && new_t < sy + STONE_SIZE) return 1;
    }
    return 0;
}

int game_stone_hit_at(int x, int y)
{
    int gx, gy;

    if (x < ARENA_X0 || x >= ARENA_X1 ||
        y < ARENA_Y0 || y >= ARENA_Y1) return 0;

    /* STONE_SIZE 固定为 32，且竞技场坐标非负；
     * 碰撞热路径使用移位代替 RISC-V 上的软件除法。 */
    gx = x >> 5;
    gy = y >> 5;
    stone_grid_ensure();
    return SG_GET(s_stone_grid, SG_IDX(gx, gy)) != 0u;
}

static void erase_push(int x, int y);   /* 炸碎后把格子推进"待擦队列"（定义在下面） */

/*  炸弹冲击波：把半径 r 内的【石头】炸碎（箱子/板条箱不动）。
 *  · 记 (room, 格子 x, y) ⇒ 这一局重进房间不再生；
 *  · 碰撞格点失效，下一帧重建；
 *  · 像素由"待擦队列"在随后两帧里补回地板。  */
int game_stones_blast(int cx, int cy, int r)
{
    const room_layout_t *L = stone_layout();
    signed char room = (signed char)game_room_id();
    int r2 = r * r;
    int i, n = 0;

    if (!L) return 0;
    for (i = 0; i < (int)L->n_objs; ++i) {
        const obj_place_t *o = &L->objs[i];
        int sx, sy, d2;

        if (o->kind != OBJ_STONE) continue;              /* 只有石头会被炸碎 */
        if (game_room_cell_gone(room, (int)o->x, (int)o->y)) continue;

        game_layout_obj_cell(L, o, game_room_flip(), &sx, &sy);
        sx = sx * TILE + STONE_SIZE / 2;
        sy = sy * TILE + STONE_SIZE / 2;
        d2 = (sx - cx) * (sx - cx) + (sy - cy) * (sy - cy);
        if (d2 > r2) continue;

        game_room_mark_cell_gone(room, (int)o->x, (int)o->y);
        erase_push(sx - STONE_SIZE / 2, sy - STONE_SIZE / 2);
        ++n;
    }

    if (n) s_grid_room = -1;
    return n;
}

/*  矩形 (x,y,w,h) 和当前房间的石头相交？—— 金币落点避让用。
 *  石头占满 32 px 格，所以只要矩形覆盖到任一标记格就相交。  */
int game_stone_overlap(int x, int y, int w, int h)
{
    int x0 = x, x1 = x + w - 1;
    int y0 = y, y1 = y + h - 1;
    int i0, i1, j0, j1, i, j;

    if (w <= 0 || h <= 0) return 0;
    if (x1 < ARENA_X0 || x0 >= ARENA_X1 ||
        y1 < ARENA_Y0 || y0 >= ARENA_Y1) return 0;

    if (x0 < ARENA_X0) x0 = ARENA_X0;
    if (y0 < ARENA_Y0) y0 = ARENA_Y0;
    if (x1 >= ARENA_X1) x1 = ARENA_X1 - 1;
    if (y1 >= ARENA_Y1) y1 = ARENA_Y1 - 1;

    i0 = x0 / STONE_SIZE;
    i1 = x1 / STONE_SIZE;
    j0 = y0 / STONE_SIZE;
    j1 = y1 / STONE_SIZE;

    stone_grid_ensure();
    for (j = j0; j <= j1; ++j)
        for (i = i0; i <= i1; ++i)
            if (SG_GET(s_stone_grid, SG_IDX(i, j))) return 1;
    return 0;
}


/*----------------------------------------------------------------------------*/
/*  待擦队列：炸碎一格石头后，接下来 2 帧（两块缓冲各一次）要把那格补回地板。
 *  ★ 静态物平时不标脏 —— 它不动，背景本来就不需要重铺；只有"没了"才需要。  */
#define ERASE_MAX 16
static struct { short x, y; unsigned char ttl; } s_erase[ERASE_MAX];
static int s_erase_n;

static void erase_push(int x, int y)
{
    if (s_erase_n >= ERASE_MAX) return;
    s_erase[s_erase_n].x = (short)x;
    s_erase[s_erase_n].y = (short)y;
    s_erase[s_erase_n].ttl = GAME_FB_COUNT;
    ++s_erase_n;
}

void game_stone_erase_cell(int x, int y)
{
    erase_push(x, y);
}

int game_stones_erase_at(int i, int *x, int *y)
{
    int k, n = 0;

    if (i < 0) return 0;
    for (k = 0; k < s_erase_n; ++k) {
        if (s_erase[k].ttl == 0u) continue;
        if (n++ != i) continue;
        *x = (int)s_erase[k].x;
        *y = (int)s_erase[k].y;
        return 1;
    }
    return 0;
}

void game_stones_erase_tick(void)
{
    int i, w = 0;

    for (i = 0; i < s_erase_n; ++i) {
        if (s_erase[i].ttl == 0u) continue;
        --s_erase[i].ttl;
        if (s_erase[i].ttl == 0u) continue;
        s_erase[w++] = s_erase[i];
    }
    s_erase_n = w;
}

/*  新一局 / 新一层：布局本身不变，只是碰撞格要按新的"已打碎"清单重建。  */
void game_stones_reset(void)
{
    s_grid_room = -1;
    s_erase_n = 0;
}

/*----------------------------------------------------------------------------*/
/*  obj kind → 贴图：箱子和板条箱共用石头的碰撞，只换图  */
static int stone_asset(int kind)
{
    /*  ★ 素材表里箱子拆成了"关/开"两张（2026-10-24 重跑 pic2pram），
     *    地上的箱子永远画"关"的那张；"开"的那张留给打开动画。  */
    if (kind == OBJ_BOX)   return ASSET_background_interact_box_close;
    if (kind == OBJ_CRATE) return ASSET_background_interact_crate;
    return ASSET_background_interact_stone;
}

static void stone_draw_one(u32 fb, u32 now, int buf, int i)
{
    const room_layout_t *L = stone_layout();
    const obj_place_t *o;
    const asset_desc_t *a;
    int x, y;

    (void)buf;
    if (!L || i < 0 || i >= (int)L->n_objs) return;
    o = &L->objs[i];
    if (!is_blocker((int)o->kind)) return;
    if (game_room_cell_gone((int)game_room_id(), (int)o->x, (int)o->y) &&
        !(o->kind == OBJ_BOX && game_chest_is_opening(game_room_id(), o->x, o->y, now))) return;

    game_layout_obj_cell(L, o, game_room_flip(), &x, &y);
    if (o->kind == OBJ_BOX && game_chest_is_opening(game_room_id(), o->x, o->y, now))
        a = &g_assets[ASSET_background_interact_box_open];
    else
        a = &g_assets[stone_asset((int)o->kind)];
    CI_gpu2d_spriteAlphaRect(a->base_word, a->stride_word, fb,
                             (u32)(x * TILE), (u32)(y * TILE),
                             STONE_SIZE, STONE_SIZE, 0u);
}

#if GAME_STATIC_CULL
/* 新进入的实体可能盖住一块保留在地面的石头，即使旧精灵覆盖区没有标脏该格。
 * 这块石头仍要进入 z 排序列表，才能保持正确的遮挡关系。 */
static dirty_map_t s_stone_dynamic;

static void stone_dynamic_mark(int x, int y, int w, int h)
{
    int tx, ty, x0, y0, x1, y1;
    if (x + w <= 0 || y + h <= 0) return;
    x0 = x < 0 ? 0 : x / TILE;
    y0 = y < 0 ? 0 : y / TILE;
    x1 = (x + w - 1) / TILE;
    y1 = (y + h - 1) / TILE;
    if (x1 >= GRID_W) x1 = GRID_W - 1;
    if (y1 >= GRID_H) y1 = GRID_H - 1;
    for (ty = y0; ty <= y1; ++ty)
        for (tx = x0; tx <= x1; ++tx) dirty_set(s_stone_dynamic, tx, ty);
}

/* 按需构建一次，避免为每块石头扫描全部实体池。
 * 石头总是占满对齐格，因此位图可以精确保留重叠关系。 */
static void stone_dynamic_build(signed char room)
{
    int i;
    dirty_clear(s_stone_dynamic);
    if (g.phase != PHASE_GAMEOVER) stone_dynamic_mark(g.x, g.y, PW, PH);
    for (i = 0; i < N_MOBS; ++i)
        if (g.mobs[i].active) stone_dynamic_mark(g.mobs[i].x, g.mobs[i].y - 8, 32, 40);
    if (game_bullets_alive())
        for (i = 0; i < N_BULLETS; ++i)
            if (g.bullets[i].active) stone_dynamic_mark(g.bullets[i].x, g.bullets[i].y, BW, BH);
    if (g.item.active) stone_dynamic_mark(g.item.x, g.item.y, ITEM_W, ITEM_H);
    for (i = 0; i < N_COINS; ++i)
        if (g.coin_items[i].active && g.coin_items[i].room == room)
            stone_dynamic_mark(g.coin_items[i].x, g.coin_items[i].y, 24, 24);
    for (i = 0; i < N_BOMBS; ++i)
        if (g.bombs[i].active && g.bombs[i].room == room)
            stone_dynamic_mark(g.bombs[i].x, g.bombs[i].y, BOMB_W, BOMB_H);
    for (i = 0; i < N_BOMB_PICKUPS; ++i)
        if (g.bomb_pickups[i].active && g.bomb_pickups[i].room == room)
            stone_dynamic_mark(g.bomb_pickups[i].x, g.bomb_pickups[i].y,
                               BOMB_PICK_W, BOMB_PICK_H);
    for (i = 0; i < N_BOMB_AIRS; ++i)
        if (g.bomb_airs[i].active && g.bomb_airs[i].room == room)
            stone_dynamic_mark(g.bomb_airs[i].x, g.bomb_airs[i].y, BOMB_PICK_W, BOMB_PICK_H);
}

#endif

void game_stones_submit(int buf)
{
    const room_layout_t *L = stone_layout();
    signed char room = (signed char)game_room_id();
    u32 now = game_ms();
    int i;

    (void)buf;
#if GAME_STATIC_CULL
    int dynamic_ready = 0;
#endif

    if (!L) return;
    for (i = 0; i < (int)L->n_objs; ++i) {
        const obj_place_t *o = &L->objs[i];
        int x, y;

        if (!is_blocker((int)o->kind)) continue;
        if (game_room_cell_gone(room, (int)o->x, (int)o->y) &&
            !(o->kind == OBJ_BOX && game_chest_is_opening(room, o->x, o->y, now))) continue;
        game_layout_obj_cell(L, o, game_room_flip(), &x, &y);
        x *= TILE;
        y *= TILE;
        if (!game_visible_world(x, y, STONE_SIZE, STONE_SIZE)) continue;
#if GAME_STATIC_CULL
        if (!game_static_draw_force() &&
            !game_dirty_touches(x / TILE, y / TILE,
                                (x + STONE_SIZE - 1) / TILE + 1,
                                (y + STONE_SIZE - 1) / TILE + 1)) {
            if (!dynamic_ready) { stone_dynamic_build(room); dynamic_ready = 1; }
            if (!dirty_get(s_stone_dynamic, x / TILE, y / TILE)) continue;
        }
#endif
        rl_push(ACTOR_Z(y + STONE_SIZE), stone_draw_one, i);
    }
}
