//==============================================================================
//  game_mob.c —— 当前房间的小怪（slime 2 帧 / bug 4 步）
//------------------------------------------------------------------------------
//  干什么      :
//    · 换房时 reset；第一次进房生成 slime + bug；各自追玩家、接触扣血。
//    · 玩家子弹命中：扣 1 HP；清完后发房间清理奖励（0~5 coin）。
//    · 有怪时禁止传送（game_room_door_open 查 game_mobs_alive），但门贴图照常渲染。
//  上一级是谁  : game/game_main.c（每帧）、game/game_state.c（进房生成 / 清房奖励）
//  接口契约    :
//    · 固定 N_MOBS 槽；只服务【当前房间】，换房时整个池子清空。
//    · slime：2 帧 = 蹲/跳；bug：4 步 = 0, 1, 对称1, 0（第 3 帧用 FLIP_X）。
//==============================================================================
#include "game.h"
#include "CI_gpu2d.h"
#include "bsp.h"

#define MOB_W          32
#define MOB_H          32
#define MOB_HP         3
#define BUG_HP         2
/*  32×32 贴图里真正有像素的范围（两张 slime / 两张 bug 的并集）：
 *    slime x1..31 y4..30；bug x0..30 y2..25
 *  取覆盖全部像素的 32×32 大方块（透明边最多 4 px），不再用 22×18 的小身子。  */
#define MOB_BODY_X     0
#define MOB_BODY_Y     0
#define MOB_BODY_W     32
#define MOB_BODY_H     32

#define MOB_SLIME      0
#define MOB_BUG        1

#define BUG_MOVE_MS    30u          /* bug 每 30 ms 走 1 px */
#define BUG_FRAME_MS   120u         /* bug 每 120 ms 换一帧 */

static u32 s_seed;
static int s_spawned;               /* 本房间已经生成过 slime（用于触发一次清房奖励） */

static u32 mob_rand(void)
{
    if (s_seed == 0u) s_seed = game_ms() | 1u;
    s_seed = s_seed * 1664525u + 1013904223u;
    return s_seed >> 16;
}

static int pt_in(int px, int py, int x, int y, int w, int h)
{
    return px >= x && px < x + w && py >= y && py < y + h;
}

/*  两个矩形是否相交（玩家身体 vs 小怪大方块）  */
static int rect_hit(int ax, int ay, int aw, int ah,
                    int bx, int by, int bw, int bh)
{
    return ax < bx + bw && ax + aw > bx &&
           ay < by + bh && ay + ah > by;
}

/* 怪物和玩家统一使用精灵底部居中的 24×16 脚底占用矩形。 */
static int mob_body_walkable(int x, int y)
{
    int fx, fy, fw, fh;
    game_creature_foot_rect(x, y, MOB_W, MOB_H, &fx, &fy, &fw, &fh);
    return game_room_rect_walkable(fx, fy, fw, fh) &&
           (!game_stone_has_blockers() || !game_stone_overlap(fx,fy,fw,fh));
}

/* 逐像素检查怪物脚底矩形从旧位置到新位置的路径，避免跨墙/斜切墙角。 */
static int mob_path_walkable(int old_x, int old_y, int new_x, int new_y)
{
    int dx = new_x - old_x;
    int dy = new_y - old_y;
    int span = (dx < 0 ? -dx : dx);
    int ay = (dy < 0 ? -dy : dy);
    int steps, i, x, y;

    if (ay > span) span = ay;
    steps = span;
    if (steps < 1) steps = 1;
    for (i = 1; i <= steps; ++i) {
        x = old_x + (dx * i) / steps;
        y = old_y + (dy * i) / steps;
        if (!mob_body_walkable(x, y)) return 0;
    }
    return 1;
}

/* One reverse BFS is shared by all mobs. Byte parent directions plus two
 * bitset frontiers avoid a full index queue. Open ground uses direct steering. */
#define NAV_ROOT 5u
#define NAV_WALL 7u
static unsigned char s_nav[GRID_W*GRID_H];
static dirty_map_t s_frontier[2];
static int s_nav_valid, s_nav_room;
static u32 s_nav_until;
static const signed char nav_dx[4] = {-1,1,0,0};
static const signed char nav_dy[4] = {0,0,-1,1};
#if GAME_PROFILE && GAME_BOSS_STAGE_TEST
static u32 s_nav_max_ticks;
u32 game_mobs_nav_max_us(void) { return s_nav_max_ticks/(BSP_CLINT_HZ/1000000u); }
#endif

static int abs_i(int x) { return x < 0 ? -x : x; }
static int clamp_step(int x,int step) { return x>step ? step : x < -step ? -step : x; }

static void nav_build(u32 now)
{
#if GAME_PROFILE && GAME_BOSS_STAGE_TEST
    u32 started=clint_getTimeLow(BSP_CLINT);
#endif
    int x,y,i,goal=-1,best=0x7fffffff,layer=0,have=1;
    int gx=(g.x+PW/2)/TILE, gy=(g.y+PH-8)/TILE;
    int x0=ARENA_X0/TILE-2, y0=ARENA_Y0/TILE-2;
    int x1=(ARENA_X1-1)/TILE+2, y1=(ARENA_Y1-1)/TILE+2;
    int blockers=game_stone_has_blockers();
    if(x0<0) x0=0;
    if(y0<0) y0=0;
    if(x1>=GRID_W) x1=GRID_W-1;
    if(y1>=GRID_H) y1=GRID_H-1;
    /* Only the arena and its two-cell door band can be walkable. Avoid
     * expensive geometry/door queries for the unused 45x35 world margins. */
    for(i=0;i<GRID_W*GRID_H;++i) s_nav[i]=NAV_WALL;
    for(y=y0;y<=y1;++y) for(x=x0;x<=x1;++x) {
        int d=abs_i(x-gx)+abs_i(y-gy);
        i=y*GRID_W+x;
        /* At this aligned waypoint the 24x16 feet lie inside exactly one
         * tile: x+4..27, y+8..23. Use the same terrain/stone occupancy rules
         * directly instead of recomputing a rectangle for every map cell. */
        s_nav[i]=game_room_cell_walkable(x,y) &&
            (!blockers || !game_stone_hit_at(x*TILE+16,y*TILE+16)) ? 0 : NAV_WALL;
        if(!s_nav[i] && d<best) { best=d;goal=i; }
    }
    dirty_clear(s_frontier[0]);dirty_clear(s_frontier[1]);
    if(goal>=0) {
        s_nav[goal]=NAV_ROOT;
        dirty_set(s_frontier[0],goal%GRID_W,goal/GRID_W);
        while(have) {
            int byte;have=0;
            for(byte=0;byte<DIRTY_BYTES;++byte) {
                unsigned bits=s_frontier[layer][byte];int bit;
                s_frontier[layer][byte]=0;
                for(bit=0;bits;++bit,bits>>=1) if(bits&1u) {
                    int k,cur=byte*8+bit;
                    x=cur%GRID_W;y=cur/GRID_W;
                    for(k=0;k<4;++k) {
                        int nx=x+nav_dx[k],ny=y+nav_dy[k],n;
                        if(nx<0 || nx>=GRID_W || ny<0 || ny>=GRID_H) continue;
                        n=ny*GRID_W+nx;
                        if(s_nav[n]) continue;
                        s_nav[n]=(unsigned char)((k^1)+1);
                        dirty_set(s_frontier[layer^1],nx,ny);have=1;
                    }
                }
            }
            layer^=1;
        }
    }
    s_nav_room=game_room_id();s_nav_until=now+200u;s_nav_valid=1;
#if GAME_PROFILE && GAME_BOSS_STAGE_TEST
    { u32 elapsed=clint_getTimeLow(BSP_CLINT)-started;
      if(elapsed>s_nav_max_ticks) s_nav_max_ticks=elapsed; }
#endif
}

static void nav_target(mob_t *m,int *tx,int *ty,u32 now)
{
    int x=(m->x+16)/TILE,y=(m->y+24)/TILE,dir;
    if(!s_nav_valid || s_nav_room!=game_room_id() || (int)(now-s_nav_until)>=0) nav_build(now);
    if(x<0 || x>=GRID_W || y<0 || y>=GRID_H) return;
    dir=s_nav[y*GRID_W+x];
    if(dir==5) { m->nav_follow=0;return; }
    if(dir<1 || dir>4) return; /* Disconnected target: remain collision safe. */
    *tx=x*TILE;*ty=y*TILE-8;
    if(nav_dx[dir-1]) {
        if(abs_i(m->y-*ty)>2) *tx=m->x;
        else *tx+=nav_dx[dir-1]*TILE;
    } else {
        if(abs_i(m->x-*tx)>2) *ty=m->y;
        else *ty+=nav_dy[dir-1]*TILE;
    }
}

static int mob_try_move(mob_t *m,int tx,int ty,int speed)
{
    int dx=tx-m->x,dy=ty-m->y,nx,ny;
    int step=(abs_i(dx)>speed && abs_i(dy)>speed) ? (speed*181)/256 : speed;
    if(step<1) step=1;
    nx=m->x+clamp_step(dx,step);ny=m->y+clamp_step(dy,step);
    if(nx==m->x && ny==m->y) return 0;
    if(!mob_path_walkable(m->x,m->y,nx,ny)) return 0;
    m->x=(short)nx;m->y=(short)ny;return 1;
}

static void mob_advance(int index,int speed,u32 now)
{
    mob_t *m=&g.mobs[index];
    int tx=g.x+PW/2-16,ty=g.y+PH-8-24,i,rx=0,ry=0;
    if(m->nav_follow) nav_target(m,&tx,&ty,now);
    if(!m->nav_follow) {
        /* Bugs lead a moving target slightly; per-slot flank offsets keep
         * a wave from becoming one stack. Near contact they close directly. */
        if(abs_i(tx-m->x)+abs_i(ty-m->y)>96) {
            if(m->type==MOB_BUG) { tx+=g.vx8*6/8;ty+=g.vy8*6/8; }
            tx+=g_sin_tab[(index*11+16)&63]*24/64;
            ty+=g_sin_tab[(index*11)&63]*24/64;
        }
        for(i=0;i<N_MOBS;++i) if(i!=index && g.mobs[i].active) {
            int dx=m->x-g.mobs[i].x,dy=m->y-g.mobs[i].y;
            if(abs_i(dx)<36 && abs_i(dy)<36) {
                if(!dx && !dy) dx=(index>i)?1:-1;
                rx+=(dx>0?1:dx<0?-1:0)*(36-abs_i(dx));
                ry+=(dy>0?1:dy<0?-1:0)*(36-abs_i(dy));
            }
        }
        tx+=clamp_step(rx,24);ty+=clamp_step(ry,24);
    }
    if((tx==m->x && ty==m->y) || mob_try_move(m,tx,ty,speed)) return;
    m->nav_follow=1;tx=g.x+PW/2-16;ty=g.y+PH-32;
    nav_target(m,&tx,&ty,now);
    if(mob_try_move(m,tx,ty,speed)) return;
    /* Align with the passage before making its next turn. */
    if(!mob_try_move(m,tx,m->y,speed)) (void)mob_try_move(m,m->x,ty,speed);
}

/*  随机挑一个【能站人】的格子当出生点（格左上角对齐，正好 32×32 一格）。
 *  ★ 以前直接在竞技场矩形里撒像素：L 形房的缺口和内墙都在这个矩形里，
 *    怪会刷在黑区/墙里，之后又只夹矩形边走 ⇒ 穿墙追人、隔墙咬人
 *    （2026-10-24 抓到）。现在先查地板，再要求离玩家至少 2 格（不贴脸出生）。  */
static int mob_spawn_pos(int *ox, int *oy)
{
    int tx0 = ARENA_X0 / TILE, ty0 = ARENA_Y0 / TILE;
    int tx1 = (ARENA_X1 - 1) / TILE, ty1 = (ARENA_Y1 - 1) / TILE;
    int px = (g.x + PW / 2) / TILE;             /* 玩家脚下那一格 */
    int py = (g.y + PH - 1) / TILE;
    int tries;

    if (tx1 < tx0 || ty1 < ty0) return 0;
    for (tries = 0; tries < 96; ++tries) {
        int tx = tx0 + (int)(mob_rand() % (u32)(tx1 - tx0 + 1));
        int ty = ty0 + (int)(mob_rand() % (u32)(ty1 - ty0 + 1));
        int d;

        if (!game_room_cell_walkable(tx, ty)) continue;
        d = tx - px; if (d < 0) d = -d;
        if (d < 2) continue;
        d = ty - py; if (d < 0) d = -d;
        if (d < 2) continue;
        *ox = tx * TILE;
        *oy = ty * TILE;
        return 1;
    }
    return 0;
}

void game_mobs_dismiss(void)
{
    int i;
    for (i = 0; i < N_MOBS; ++i) g.mobs[i].active = 0;
    s_spawned = 0; /* Boss support waves never consume the room-clear reward. */
}

void game_mobs_sync(int buf)
{
    int i;
    for (i = 0; i < N_MOBS; ++i) g.old_mob[buf][i].active = 0;
}

void game_mobs_reset(void)
{
    int i, b;
    s_nav_valid=0;
#if GAME_PROFILE && GAME_BOSS_STAGE_TEST
    s_nav_max_ticks=0;
#endif

    for (i = 0; i < N_MOBS; ++i) {
        g.mobs[i].active = 0;
        g.mobs[i].hit_until = 0u;
    }
    for (b = 0; b < GAME_FB_COUNT; ++b)
        for (i = 0; i < N_MOBS; ++i) g.old_mob[b][i].active = 0;
    s_spawned = 0;
}

static void mob_init(mob_t *m, int x, int y, int type)
{
    m->active = 1;
    m->type = (unsigned char)((type == MOB_BUG) ? MOB_BUG : MOB_SLIME);
    m->hp = (m->type == MOB_BUG) ? BUG_HP : MOB_HP;
    m->frame = 0;
    m->hop_left=0;m->nav_follow=0;
    m->x = (short)x;
    m->y = (short)y;
    if (m->type == MOB_BUG) {
        m->move_next = game_ms() + 100u + (mob_rand() % 200u);
        m->hop_until = game_ms() + BUG_FRAME_MS;
    } else {
        m->move_next = game_ms() + 250u + (mob_rand() % 350u);
        m->hop_until = 0u;
    }
    m->hit_until = 0u;
}

static void mobs_spawn(int count, int type)
{
    int i, n = 0;
    if (count <= 0) return;
    for (i = 0; i < N_MOBS && n < count; ++i) {
        int x, y;
        if (g.mobs[i].active) continue;
        if (!mob_spawn_pos(&x, &y)) break;
        mob_init(&g.mobs[i], x, y, type);
        ++n;
    }
    s_spawned = 1;
}

void game_mobs_spawn(int count)     { mobs_spawn(count, MOB_SLIME); }
void game_mobs_spawn_bug(int count) { mobs_spawn(count, MOB_BUG); }

int game_mobs_spawn_at(int x, int y, int type)
{
    int i;
    if (x < ARENA_X0 || y < ARENA_Y0) return 0;
    if (x + MOB_W > ARENA_X1 || y + MOB_H > ARENA_Y1) return 0;
    if (!mob_body_walkable(x, y)) return 0;
    for (i = 0; i < N_MOBS; ++i) {
        if (g.mobs[i].active) continue;
        mob_init(&g.mobs[i], x, y, type);
        s_spawned = 1;
        return 1;
    }
    return 0;
}

int game_mobs_alive(void)
{
    int i, n = 0;

    for (i = 0; i < N_MOBS; ++i)
        if (g.mobs[i].active) ++n;
    return n;
}

/*  玩家子弹中心点 (cx,cy) 是否打中某只 slime；命中返回 1  */
int game_mobs_try_hit_ex(int cx, int cy, int bullet_type)
{
    int i;

    for (i = 0; i < N_MOBS; ++i) {
        mob_t *m = &g.mobs[i];

        if (!m->active) continue;
        if (!pt_in(cx, cy, m->x + MOB_BODY_X, m->y + MOB_BODY_Y, MOB_BODY_W, MOB_BODY_H))
            continue;
        if (bullet_type == BULLET_CHILI) {
            m->burn_until = game_ms() + CHILI_BURN_MS;
            m->burn_next = game_ms() + CHILI_BURN_TICK_MS;
        }
        if (bullet_type == BULLET_URANUS && ++m->elemental_hits >= 3u) {
            m->freeze_until = game_ms() + URANUS_FREEZE_MS;
            m->elemental_hits = 0u;
        }
        if (m->hp <= game_player_hit_damage(1)) {
            m->active = 0;
            g.score += 10;
        } else {
            m->hp = (unsigned char)(m->hp - game_player_hit_damage(1));
            /*  命中闪红：和玩家/Boss 一样按【时间】算，不按帧数。  */
            m->hit_until = game_ms() + HIT_FLASH_MS;
        }
        return 1;
    }
    return 0;
}

int game_mobs_try_hit(int cx, int cy)
{
    return game_mobs_try_hit_ex(cx, cy, BULLET_A);
}

void game_mobs_step(u32 now)
{
    int i;
#if GAME_PROFILE && GAME_BOSS_STAGE_TEST == 1
    /* Diagnostic stress: measure a full field even in the open Boss arena. */
    if(g.boss_locked && game_mobs_alive() && !s_nav_valid) nav_build(now);
#endif

    for (i = 0; i < N_MOBS; ++i) {
        mob_t *m = &g.mobs[i];
        if (!m->active) continue;
        if (m->burn_until && (int)(now - m->burn_until) >= 0)
            m->burn_until = m->burn_next = 0u;
        if (m->burn_until && (int)(now - m->burn_next) >= 0) {
            m->burn_next += CHILI_BURN_TICK_MS;
            if (m->hp <= game_player_hit_damage(1)) {
                m->active = 0;
                g.score += 10;
                continue;
            }
            m->hp = (unsigned char)(m->hp - game_player_hit_damage(1));
        }
        if (m->freeze_until && (int)(now - m->freeze_until) < 0)
            goto mob_contact;
        if (m->type == MOB_BUG) {
            if ((int)(now-m->move_next)>=0) {
                mob_advance(i,2,now);
                m->move_next=now+BUG_MOVE_MS;
            }
            if ((int)(now-m->hop_until)>=0) {
                m->frame=(unsigned char)((m->frame+1u)&3u);
                m->hop_until=now+BUG_FRAME_MS;
            }
        } else {
            if (!m->hop_left && (int)(now-m->move_next)>=0) {
                m->hop_left=6;m->hop_until=now+180u;
                m->move_next=now+360u+(mob_rand()%200u);
            }
            if(m->hop_left) { mob_advance(i,3,now);--m->hop_left; }
        }

        /*  接触伤害：玩家【脚到眼睛】矩形 vs 小怪【大方块】  */
mob_contact:
        if (!game_vital_dead(now) &&
            rect_hit(g.x + PB_OFF_X, g.y + PB_OFF_Y, PB_W, PB_H,
                     m->x + MOB_BODY_X, m->y + MOB_BODY_Y,
                     MOB_BODY_W, MOB_BODY_H)) {
            if (game_vital_hit(now) != 0) g.hurt_until = now + HURT_MS;
        }
    }

    /*  最后一只死掉的当帧：发清理奖励 + 恢复门的传送  */
    if (s_spawned && game_mobs_alive() == 0 && game_world_is_boss()) s_spawned = 0;
    if (s_spawned && game_mobs_alive() == 0) {
        int n = game_room_take_reward();

        s_spawned = 0;
        game_coins_spawn(n);
        g.door_dirty = GAME_FB_COUNT;
        bsp_printf("ROOM CLEAR: %d coins\r\n", n);
    }
}

/*----------------------------------------------------------------------------*/
static void mob_draw_one(u32 fb, u32 now, int buf, int i)
{
    mob_t *m = &g.mobs[i];
    const asset_desc_t *a;
    int jumping;
    u32 hop = 0;

    g.old_mob[buf][i].active = m->active;
    g.old_mob[buf][i].x = m->x;
    g.old_mob[buf][i].y = m->y;
    if (!m->active) return;

    /*  slime：帧 1 只在腾空时显示；抛物线抬升最多 6 px。
     *  bug：4 步 0, 1, 对称1, 0；第 3 步用 FLIP_X 画 frame 1 的镜像。  */
    jumping = (int)(m->hop_until - now) > 0;
    if (jumping) {
        u32 t = 180u - (m->hop_until - now);      /* 0..180：这一跳已经过了多久 */
        hop = (t * (180u - t) * 24u) / (180u * 180u);
        if (hop > 6u) hop = 6u;
    }

    {
        int dx = (int)m->x;
        int dy = (int)m->y;
        u32 flip = 0u;

        if (m->type == MOB_BUG) {
            unsigned char f = (unsigned char)(m->frame & 3u);

            if (f == 1u) {
                a = &g_assets[ASSET_mobs_bug_01_Right];
            } else if (f == 2u) {
                a = &g_assets[ASSET_mobs_bug_01_Right];
                flip = GPU2D_FLIP_X;
            } else {
                a = &g_assets[ASSET_mobs_bug_00_standUp];
            }
        } else {
            a = &g_assets[jumping ? ASSET_mobs_slime_01 : ASSET_mobs_slime_00];
            dy = (int)m->y - (int)hop;
        }

        /*  命中闪红：用 MODE=4（tint + alpha），保留小怪贴图的软边；
         *    duration 由 game_mobs_try_hit() 按 HIT_FLASH_MS 写入。  */
        if (m->freeze_until && (int)(now - m->freeze_until) < 0)
            CI_gpu2d_spriteAlphaTintRect(a->base_word, a->stride_word, fb,
                                         (u32)dx, (u32)dy, MOB_W, MOB_H,
                                         URANUS_TINT_RGB, URANUS_TINT_K, 1u, flip);
        else if ((int)(now - m->hit_until) < 0)
            CI_gpu2d_spriteAlphaTintRect(a->base_word, a->stride_word, fb,
                                         (u32)dx, (u32)dy, MOB_W, MOB_H,
                                         0x00FF2828u, 180u, 1u, flip);
        else
            CI_gpu2d_spriteAlphaRect(a->base_word, a->stride_word, fb,
                                     (u32)dx, (u32)dy, MOB_W, MOB_H, flip);
    }
}

void game_mobs_submit(void)
{
    int i;

    for (i = 0; i < N_MOBS; ++i) {
        const mob_t *m = &g.mobs[i];

        if (!m->active) continue;
        if (!game_visible_world(m->x, m->y - 8, MOB_W, MOB_H + 8)) continue;
        rl_push(ACTOR_Z(m->y + MOB_H), mob_draw_one, i);
    }
}
