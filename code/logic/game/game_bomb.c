//==============================================================================
//  game_bomb.c —— 可推动炸弹：拾取 / 放置 / 滑动 / 引信 / 爆炸伤害
//------------------------------------------------------------------------------
//  规则：
//    · B 键在面前放一颗；数量来自 g.bomb_count（地面炸弹拾取 +1）
//    · 玩家撞到炸弹时把速度传给它；玩家被挡在炸弹后面
//    · 炸弹带摩擦滑动，撞墙/石头/其它炸弹/玩家会停
//    · 引信 3 s；爆炸对范围内玩家、小怪、Boss 造成固定伤害；可连锁
//    · 引信最后 0.6 s 整颗往白里闪（颜色合成，不是隔帧不画）
//    · 爆炸后本体消失，改在爆心周围随机撒 3~5 个 air 火花
//    · 炸弹按 room 持久化；离开房间后引信继续走
//==============================================================================
#include "game.h"
#include "CI_gpu2d.h"
#include "bsp.h"

/*----------------------------------------------------------------------------*/
static int bomb_rect_overlap(int ax, int ay, int aw, int ah,
                             int bx, int by, int bw, int bh)
{
    return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}

/*  炸弹碰撞体 vs 【指定位置】的玩家身体（脚到眼睛那块 16×32）。
 *  ★ 必须能传玩家坐标：判断"这一帧走到这里会不会撞上"时玩家还没写回 g，
 *    用 g 就等于永远在拿上一帧的位置做判定（踩过：人卡在炸弹里出不来）。  */
static int bomb_hits_player_at(int bx, int by, int px, int py)
{
    return bomb_rect_overlap(bx + BOMB_HIT_OFF_X, by + BOMB_HIT_OFF_Y,
                             BOMB_HIT_W, BOMB_HIT_H,
                             px + PB_OFF_X, py + PB_OFF_Y, PB_W, PB_H);
}

/*  炸弹碰撞体 vs 玩家【当前位置】（炸弹自己滑动时用，那时 g 已经是新的）  */
static int bomb_hits_player(int bx, int by)
{
    return bomb_hits_player_at(bx, by, g.x, g.y);
}

static int bomb_can_move(int x, int y, int ignore)
{
    int i;
    int hx = x + BOMB_HIT_OFF_X;
    int hy = y + BOMB_HIT_OFF_Y;

    if (hx < ARENA_X0 || hx + BOMB_HIT_W > ARENA_X1 ||
        hy < ARENA_Y0 || hy + BOMB_HIT_H > ARENA_Y1) return 0;
    if (game_stone_overlap(hx, hy, BOMB_HIT_W, BOMB_HIT_H)) return 0;

    for (i = 0; i < N_BOMBS; ++i) {
        const bomb_t *b = &g.bombs[i];
        int bx, by;

        if (i == ignore || !b->active) continue;
        if (b->room != (signed char)game_room_id()) continue;
        bx = b->x + BOMB_HIT_OFF_X;
        by = b->y + BOMB_HIT_OFF_Y;
        if (bomb_rect_overlap(hx, hy, BOMB_HIT_W, BOMB_HIT_H,
                              bx, by, BOMB_HIT_W, BOMB_HIT_H)) return 0;
    }

    return 1;
}

/*----------------------------------------------------------------------------*/
/*  爆炸火花  */
static u32 s_air_seed;

static u32 bomb_rand(void)
{
    if (s_air_seed == 0u) s_air_seed = game_ms() | 1u;
    s_air_seed = s_air_seed * 1664525u + 1013904223u;
    return s_air_seed >> 16;
}

/*  在爆心周围撒 3~5 个火花；池满就少撒，绝不覆盖还活着的。  */
static void bomb_air_burst(int cx, int cy, u32 now)
{
    int n = BOMB_AIR_MIN + (int)(bomb_rand() % (u32)(BOMB_AIR_MAX - BOMB_AIR_MIN + 1));
    int k, i;

    for (k = 0; k < n; ++k) {
        bomb_air_t *p = 0;
        int a, d, cosv, sinv;

        for (i = 0; i < N_BOMB_AIRS; ++i)
            if (!g.bomb_airs[i].active) { p = &g.bomb_airs[i]; break; }
        if (!p) break;

        a    = (int)(bomb_rand() & 63u);                 /* 一圈 64 格，随机方向 */
        d    = 4 + (int)(bomb_rand() % (u32)(BOMB_AIR_SPREAD - 4));
        cosv = g_sin_tab[(a + ANGLE_STEPS / 4) & 63];
        sinv = g_sin_tab[a & 63];

        p->active = 1;
        p->room   = (signed char)game_room_id();
        p->x      = (short)(cx + ((cosv * d) >> 6) - BOMB_PICK_W / 2);
        p->y      = (short)(cy + ((sinv * d) >> 6) - BOMB_PICK_H / 2);
        p->vx8    = (short)((cosv * BOMB_AIR_V8) >> 6);
        p->vy8    = (short)((sinv * BOMB_AIR_V8) >> 6);
        p->rx     = 0;
        p->ry     = 0;
        p->until  = now + BOMB_AIR_MS;
        for (int b = 0; b < GAME_FB_COUNT; ++b) g.old_bomb_air[b][i].active = 0;
    }
}

/*----------------------------------------------------------------------------*/
void game_bombs_reset(void)
{
    int i, b;

    for (i = 0; i < N_BOMBS; ++i) {
        g.bombs[i].active = 0;
        g.bombs[i].state = 0;
        g.bombs[i].room = -1;
        g.bombs[i].rx = 0;
        g.bombs[i].ry = 0;
        g.bombs[i].vx8 = 0;
        g.bombs[i].vy8 = 0;
        g.bombs[i].fuse_until = 0u;
        g.bombs[i].boom_until = 0u;
        for (b = 0; b < GAME_FB_COUNT; ++b) g.old_bomb[b][i].active = 0;
    }
    g.bomb_pickup.active = 0;
    g.bomb_pickup.room = -1;
    for (int b = 0; b < GAME_FB_COUNT; ++b) g.old_bomb_pickup[b].active = 0;
    for (i = 0; i < N_BOMB_AIRS; ++i) {
        g.bomb_airs[i].active = 0;
        g.bomb_airs[i].room = -1;
        g.bomb_airs[i].vx8 = 0;
        g.bomb_airs[i].vy8 = 0;
        g.bomb_airs[i].until = 0u;
        for (b = 0; b < GAME_FB_COUNT; ++b) g.old_bomb_air[b][i].active = 0;
    }
    g.bomb_count = 0;
}

void game_bomb_spawn_pickup(int x, int y, int room)
{
    g.bomb_pickup.active = 1;
    g.bomb_pickup.room = (signed char)room;
    g.bomb_pickup.x = (short)x;
    g.bomb_pickup.y = (short)y;
    for (int b = 0; b < GAME_FB_COUNT; ++b) g.old_bomb_pickup[b].active = 0;
}

void game_bomb_pickup_try(u32 now)
{
    (void)now;

    if (!g.bomb_pickup.active) return;
    if (g.bomb_pickup.room != (signed char)game_room_id()) return;
    if (!bomb_rect_overlap(g.bomb_pickup.x, g.bomb_pickup.y, BOMB_PICK_W, BOMB_PICK_H,
                           g.x + PB_OFF_X, g.y + PB_OFF_Y, PB_W, PB_H)) return;

    g.bomb_pickup.active = 0;
    ++g.bomb_count;
    bsp_printf("BOMB: picked up, count=%d\r\n", g.bomb_count);
}

/*----------------------------------------------------------------------------*/
int game_bomb_place(u32 now)
{
    int i, x, y;
    bomb_t *b = 0;

    if (g.bomb_count <= 0) return 0;

    x = g.x + PW / 2 - BOMB_W / 2;
    /*  放在脚下：贴图底边落在脚底，实际 16×16 碰撞体在脚底正下方。  */
    y = g.y + PB_OFF_Y + PB_H - BOMB_HIT_OFF_Y;
    if (!bomb_can_move(x, y, -1)) return 0;

    for (i = 0; i < N_BOMBS; ++i) {
        if (!g.bombs[i].active) { b = &g.bombs[i]; break; }
    }
    if (!b) return 0;

    b->active = 1;
    b->state = 0;
    b->room = (signed char)game_room_id();
    b->x = (short)x;
    b->y = (short)y;
    b->rx = 0;
    b->ry = 0;
    b->vx8 = 0;
    b->vy8 = 0;
    b->fuse_until = now + BOMB_FUSE_MS;
    b->boom_until = 0u;
    --g.bomb_count;
    for (int bidx = 0; bidx < GAME_FB_COUNT; ++bidx)
        g.old_bomb[bidx][i].active = 0;
    bsp_printf("BOMB: placed, count=%d\r\n", g.bomb_count);
    return 1;
}

/*----------------------------------------------------------------------------*/
/*  玩家撞到炸弹：推得动就推着走，推不动就停在炸弹前。
 *------------------------------------------------------------------------------
 *  ★ 三条必须守住的规则（以前的写法踩过第一条，表现是"人钉住、动的是炸弹"）：
 *   1) 只用【本帧候选位置】判定，玩家矩形必须用 *nx/*ny，不能读 g.x/g.y。
 *      用旧位置判定时，一旦脚陷进碰撞体 1 px，就会永远"重叠 → 回退到原地"，
 *      而"原地"本身还是重叠的 ⇒ 除了往上，按哪个方向都是在推炸弹。
 *   2) 玩家【本来就压着】炸弹时（被挤进来 / 以后加了新放置方式），
 *      这颗炸弹整帧不参与碰撞，先让人自由走出去；等彻底离开才恢复实体。
 *      —— 只当兜底，正常运行不会进这个分支。
 *   2b) 以前这里有个 300 ms 的"刚放下不挡人"宽限期，已删除：炸弹放在脚底
 *      下一格，本来就不与玩家重叠，不需要宽限；而宽限期里炸弹等于空气，
 *      玩家会直接走穿过去，宽限一结束还可能正好站在炸弹里。
 *   3) 分轴解算：X 挡不住还能沿 Y 滑，贴着炸弹走不会发涩。
 *------------------------------------------------------------------------------*/
int game_bomb_push(int *nx, int *ny, int oldx, int oldy, u32 now)
{
    int i;
    int blocked = 0;

    (void)now;                      /* 推动本身不看时间；参数留给以后的动作节奏 */

    for (i = 0; i < N_BOMBS; ++i) {
        bomb_t *b = &g.bombs[i];
        int dx, dy;

        if (!b->active || b->state != 0) continue;
        if (b->room != (signed char)game_room_id()) continue;

        /* 规则 2：起点就已经重叠 ⇒ 这颗当空气，让人先走出去。  */
        if (bomb_hits_player_at(b->x, b->y, oldx, oldy)) continue;

        /* X 轴：只有"这一帧确实要往 X 走，且走过去会压到炸弹"才处理  */
        dx = *nx - oldx;
        if (dx != 0 && bomb_hits_player_at(b->x, b->y, *nx, oldy)) {
            if (bomb_can_move(b->x + dx, b->y, i)) {
                b->x = (short)(b->x + dx);
                /*  ★ 纯推：炸弹只跟着这一帧的位移走，松手就停。
                 *    以前这里还塞了 vx8/rx 当"初速"，松手后炸弹会自己再滑 4~6 帧
                 *    （贴着玩家时还会被 bomb_move 的 dot 判据挡一下），手感就是
                 *    "推一下窜出去"，很难把它精准推到想要的位置。  */
                b->vx8 = 0;
                b->rx  = 0;
            } else {
                *nx = oldx;
                g.vx8 = 0;
                g.rx = 0;
                blocked = 1;
            }
        }

        /* Y 轴：用【X 解算之后】的 *nx 判，斜着走也不会从角上穿进去  */
        dy = *ny - oldy;
        if (dy != 0 && bomb_hits_player_at(b->x, b->y, *nx, *ny)) {
            if (bomb_can_move(b->x, b->y + dy, i)) {
                b->y = (short)(b->y + dy);
                b->vy8 = 0;
                b->ry  = 0;
            } else {
                *ny = oldy;
                g.vy8 = 0;
                g.ry = 0;
                blocked = 1;
            }
        }
    }

    return blocked;
}

/*----------------------------------------------------------------------------*/
static void bomb_damage_at(bomb_t *b, u32 now, int idx)
{
    int cx = b->x + BOMB_HIT_OFF_X + BOMB_HIT_W / 2;
    int cy = b->y + BOMB_HIT_OFF_Y + BOMB_HIT_H / 2;
    int r2 = BOMB_RADIUS * BOMB_RADIUS;
    int i;

    /*  玩家自己也会被炸  */
    {
        int px = g.x + PW / 2;
        int py = g.y + PH / 2;
        int d2 = (px - cx) * (px - cx) + (py - cy) * (py - cy);

        if (d2 <= r2) game_vital_hit(now);
    }

    for (i = 0; i < N_MOBS; ++i) {
        mob_t *m = &g.mobs[i];
        int mx, my, d2;

        if (!m->active) continue;
        mx = m->x + 16;
        my = m->y + 16;
        d2 = (mx - cx) * (mx - cx) + (my - cy) * (my - cy);
        if (d2 > r2) continue;

        if ((int)m->hp <= BOMB_DMG) {
            m->active = 0;
            g.score += 10;
        } else {
            m->hp = (unsigned char)(m->hp - BOMB_DMG);
            m->hit_until = now + HIT_FLASH_MS;
        }
    }

    /*  冲击波炸碎石头（箱子不受影响）  */
    (void)game_stones_blast(cx, cy, BOMB_RADIUS);

    /*  冲击波炸开隐藏门：只打开命中的那一条连接（隐藏房其它几面保持封着）  */
    (void)game_room_blast_hidden(cx, cy, BOMB_RADIUS);

    if (g.phase == PHASE_FIGHT && g.boss_hp > 0) {
        int bx = g.boss_x + BOSS_W / 2;
        int by = g.boss_y + BOSS_H / 2;
        int d2 = (bx - cx) * (bx - cx) + (by - cy) * (by - cy);

        if (d2 <= r2) game_boss_hit(now);
    }

    /*  连锁：其它引信中的炸弹立刻进入倒计时  */
    for (i = 0; i < N_BOMBS; ++i) {
        bomb_t *o = &g.bombs[i];
        int ox, oy, d2;

        if (i == idx || !o->active || o->state != 0) continue;
        if (o->room != (signed char)game_room_id()) continue;
        ox = o->x + BOMB_W / 2;
        oy = o->y + BOMB_H / 2;
        d2 = (ox - cx) * (ox - cx) + (oy - cy) * (oy - cy);
        if (d2 <= r2) o->fuse_until = now;
    }

    /*  本体马上不画了，改由火花表现：爆心取贴图中心（不是碰撞体中心，
     *  否则看着会往下偏 8 px）。  */
    bomb_air_burst(b->x + BOMB_W / 2, b->y + BOMB_H / 2, now);

    b->state = 1;
    b->boom_until = now + BOMB_BOOM_MS;
    b->vx8 = 0;
    b->vy8 = 0;
}

/*----------------------------------------------------------------------------*/
static void bomb_move(bomb_t *b, int idx)
{
    int nx, ny;

    if (b->vx8 != 0) {
        b->rx += (signed char)b->vx8;
        nx = b->x + (b->rx >> 3);
        b->rx -= (signed char)((b->rx >> 3) * 8);
        if (bomb_can_move(nx, b->y, idx)) {
            int px = g.x + PW / 2, py = g.y + PH / 2;
            int dot = (px - (b->x + BOMB_W / 2)) * b->vx8 +
                      (py - (b->y + BOMB_H / 2)) * b->vy8;

            if (!bomb_hits_player(nx, b->y) || dot <= 0) b->x = (short)nx;
            else { b->vx8 = 0; b->rx = 0; }
        } else { b->vx8 = 0; b->rx = 0; }
    }
    if (b->vy8 != 0) {
        b->ry += (signed char)b->vy8;
        ny = b->y + (b->ry >> 3);
        b->ry -= (signed char)((b->ry >> 3) * 8);
        if (bomb_can_move(b->x, ny, idx)) {
            int px = g.x + PW / 2, py = g.y + PH / 2;
            int dot = (px - (b->x + BOMB_W / 2)) * b->vx8 +
                      (py - (b->y + BOMB_H / 2)) * b->vy8;

            if (!bomb_hits_player(b->x, ny) || dot <= 0) b->y = (short)ny;
            else { b->vy8 = 0; b->ry = 0; }
        } else { b->vy8 = 0; b->ry = 0; }
    }

    /*  摩擦：滑一段就停  */
    b->vx8 -= b->vx8 / 8;
    b->vy8 -= b->vy8 / 8;
    if (b->vx8 <= 4 && b->vx8 >= -4) b->vx8 = 0;
    if (b->vy8 <= 4 && b->vy8 >= -4) b->vy8 = 0;
}

void game_bombs_step(u32 now)
{
    int i;

    for (i = 0; i < N_BOMBS; ++i) {
        bomb_t *b = &g.bombs[i];

        if (!b->active) continue;
        if (b->state == 1) {
            if ((int)(now - b->boom_until) >= 0) b->active = 0;
            continue;
        }

        if (b->room != (signed char)game_room_id()) {
            if ((int)(now - b->fuse_until) >= 0) b->active = 0;
            continue;
        }

        if ((int)(now - b->fuse_until) >= 0) {
            bomb_damage_at(b, now, i);
            continue;
        }

        bomb_move(b, i);
    }

    /*  爆炸火花：向外飘，到点消失（跨房间时 room 不匹配，下一帧就不画了）。  */
    for (i = 0; i < N_BOMB_AIRS; ++i) {
        bomb_air_t *p = &g.bomb_airs[i];

        if (!p->active) continue;
        if ((int)(now - p->until) >= 0) { p->active = 0; continue; }

        p->rx += (signed char)p->vx8;
        p->ry += (signed char)p->vy8;
        p->x = (short)(p->x + (p->rx >> 3));
        p->y = (short)(p->y + (p->ry >> 3));
        p->rx -= (signed char)((p->rx >> 3) * 8);
        p->ry -= (signed char)((p->ry >> 3) * 8);
    }
}

/*----------------------------------------------------------------------------*/
static void bomb_draw_one(u32 fb, u32 now, int buf, int i)
{
    bomb_t *b = &g.bombs[i];
    const asset_desc_t *a;

    g.old_bomb[buf][i].active = (unsigned int)(b->active && b->room == (signed char)game_room_id());
    g.old_bomb[buf][i].x = b->x;
    g.old_bomb[buf][i].y = b->y;
    if (!b->active) return;
    if (b->room != (signed char)game_room_id()) return;

    /*  爆炸：本体不再画，画面交给 bomb_air 火花。  */
    if (b->state != 0) return;
    if (!game_visible_world(b->x, b->y, BOMB_W, BOMB_H)) return;

    a = &g_assets[ASSET_background_interact_bomb];

    /*  ★ 引信最后 600 ms：整颗按颜色合成往白里闪。
     *    以前是"隔帧 return 不画" —— 不画会在脏区里露出地砖，双缓冲下看着
     *    像在闪黑块；MODE=4 保底图 α，软边不会被踩成硬边。
     *    100 ms 一档：奇数档白闪，偶数档原色。  */
    if ((int)(b->fuse_until - now) < 600 && (((now / 100u) & 1u) != 0u))
        CI_gpu2d_spriteAlphaTintRect(a->base_word, a->stride_word, fb,
                                     (u32)b->x, (u32)b->y, BOMB_W, BOMB_H,
                                     0x00FFFFFFu, BOMB_FLASH_STRENGTH, 1u, 0u);
    else
        CI_gpu2d_spriteAlphaRect(a->base_word, a->stride_word,
                                 fb, (u32)b->x, (u32)b->y, BOMB_W, BOMB_H, 0u);
}

static void pickup_draw_one(u32 fb, u32 now, int buf, int arg)
{
    const asset_desc_t *a;
    bomb_pickup_t *p = &g.bomb_pickup;

    (void)arg;
    g.old_bomb_pickup[buf].active = (unsigned int)(p->active && p->room == (signed char)game_room_id());
    g.old_bomb_pickup[buf].x = p->x;
    g.old_bomb_pickup[buf].y = p->y;
    if (!p->active) return;
    if (p->room != (signed char)game_room_id()) return;
    if (!game_visible_world(p->x, p->y, BOMB_PICK_W, BOMB_PICK_H)) return;
    a = &g_assets[ASSET_SpecialEffect_bomb];
    CI_gpu2d_spriteAlphaRect(a->base_word, a->stride_word,
        fb, (u32)p->x, (u32)p->y, BOMB_PICK_W, BOMB_PICK_H, 0u);
}

/*----------------------------------------------------------------------------*/
/*  爆炸火花：一张 24×24 的 SpecialEffect_bomb_air  */
static void air_draw_one(u32 fb, u32 now, int buf, int i)
{
    bomb_air_t *p = &g.bomb_airs[i];
    const asset_desc_t *a;

    (void)now;
    g.old_bomb_air[buf][i].active =
        (unsigned int)(p->active && p->room == (signed char)game_room_id());
    g.old_bomb_air[buf][i].x = p->x;
    g.old_bomb_air[buf][i].y = p->y;
    if (!p->active) return;
    if (p->room != (signed char)game_room_id()) return;
    if (!game_visible_world(p->x, p->y, BOMB_PICK_W, BOMB_PICK_H)) return;

    a = &g_assets[ASSET_SpecialEffect_bomb_air];
    CI_gpu2d_spriteAlphaRect(a->base_word, a->stride_word,
        fb, (u32)p->x, (u32)p->y, BOMB_PICK_W, BOMB_PICK_H, 0u);
}

void game_bombs_prepare(dirty_map_t dirty, int buf)
{
    int i;

    for (i = 0; i < N_BOMBS; ++i) {
        if (!g.old_bomb[buf][i].active) continue;
        game_mark_dirty(dirty, g.old_bomb[buf][i].x, g.old_bomb[buf][i].y,
                        BOMB_W, BOMB_H);
    }
    if (g.old_bomb_pickup[buf].active)
        game_mark_dirty(dirty, g.old_bomb_pickup[buf].x, g.old_bomb_pickup[buf].y,
                        BOMB_PICK_W, BOMB_PICK_H);
    for (i = 0; i < N_BOMB_AIRS; ++i) {
        if (!g.old_bomb_air[buf][i].active) continue;
        game_mark_dirty(dirty, g.old_bomb_air[buf][i].x, g.old_bomb_air[buf][i].y,
                        BOMB_PICK_W, BOMB_PICK_H);
    }
}

void game_bombs_submit(void)
{
    int i;

    for (i = 0; i < N_BOMBS; ++i)
        rl_push(ACTOR_Z(g.bombs[i].y + BOMB_H), bomb_draw_one, i);
    rl_push(ACTOR_Z(g.bomb_pickup.y + BOMB_H), pickup_draw_one, 0);

    /*  火花只在"还活着"或"上一帧还画着（要擦）"时入队，不白占渲染队列  */
    for (i = 0; i < N_BOMB_AIRS; ++i)
        if (g.bomb_airs[i].active ||
            g.old_bomb_air[0][i].active || g.old_bomb_air[1][i].active
#if GAME_TRIPLE_BUFFER
            || g.old_bomb_air[2][i].active
#endif
           )
            rl_push(ACTOR_Z(g.bomb_airs[i].y + BOMB_PICK_H), air_draw_one, i);
}
