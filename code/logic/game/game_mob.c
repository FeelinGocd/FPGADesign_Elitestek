//==============================================================================
//  game_mob.c —— 当前房间的小怪（slime，2 帧动画）
//------------------------------------------------------------------------------
//  干什么      :
//    · 换房时 reset；第一次进房 spawn 2 只；2 帧循环、慢慢朝玩家跳。
//    · 玩家子弹命中：扣 1 HP，3 发打死；清完后发房间清理奖励（0~5 coin）。
//    · 有怪时房间门锁死（game_room_door_open 查 game_mobs_alive）。
//  上一级是谁  : game/game_main.c（每帧）、game/game_state.c（进房生成 / 清房奖励）
//  接口契约    :
//    · 固定 N_MOBS 槽；只服务【当前房间】，换房时整个池子清空。
//    · 2 帧动画直接从 ms 时钟取，不占额外字段。
//==============================================================================
#include "game.h"
#include "CI_gpu2d.h"
#include "bsp.h"

#define MOB_W          32
#define MOB_H          32
#define MOB_HP         3
#define MOB_BODY_X     5
#define MOB_BODY_Y     12
#define MOB_BODY_W     22
#define MOB_BODY_H     18

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

void game_mobs_reset(void)
{
    int i, b;

    for (i = 0; i < N_MOBS; ++i) g.mobs[i].active = 0;
    for (b = 0; b < 2; ++b)
        for (i = 0; i < N_MOBS; ++i) g.old_mob[b][i].active = 0;
    s_spawned = 0;
}

void game_mobs_spawn(int count)
{
    int i, n = 0;

    if (count <= 0) return;

    for (i = 0; i < N_MOBS && n < count; ++i) {
        mob_t *m = &g.mobs[i];

        if (m->active) continue;
        m->active = 1;
        m->hp = MOB_HP;
        m->x = (short)(ARENA_X0 + 64 + (int)(mob_rand() % (u32)(ARENA_X1 - ARENA_X0 - 128 - MOB_W)));
        m->y = (short)(ARENA_Y0 + 64 + (int)(mob_rand() % (u32)(ARENA_Y1 - ARENA_Y0 - 128 - MOB_H)));
        m->move_next = game_ms() + 250u + (mob_rand() % 350u);
        m->hop_until = 0u;
        ++n;
    }
    s_spawned = 1;
}

int game_mobs_alive(void)
{
    int i, n = 0;

    for (i = 0; i < N_MOBS; ++i)
        if (g.mobs[i].active) ++n;
    return n;
}

/*  玩家子弹中心点 (cx,cy) 是否打中某只 slime；命中返回 1  */
int game_mobs_try_hit(int cx, int cy)
{
    int i;

    for (i = 0; i < N_MOBS; ++i) {
        mob_t *m = &g.mobs[i];

        if (!m->active) continue;
        if (!pt_in(cx, cy, m->x + MOB_BODY_X, m->y + MOB_BODY_Y, MOB_BODY_W, MOB_BODY_H))
            continue;
        if (--m->hp == 0) {
            m->active = 0;
            g.score += 10;
        }
        return 1;
    }
    return 0;
}

void game_mobs_step(u32 now)
{
    int i;

    for (i = 0; i < N_MOBS; ++i) {
        mob_t *m = &g.mobs[i];
        int px, py, mx, my, dx, dy;

        if (!m->active) continue;

        /*  一跳：起跳瞬间真正位移 12 px，然后 180 ms 腾空（帧 1 + 抬升 6 px），
         *    再落回帧 0 蹲 520~840 ms。2 帧因此是"蹲 → 跳"而不是原地蠕动。  */
        if ((int)(now - m->move_next) >= 0) {
            px = g.x + PW / 2;
            py = g.y + PH / 2;
            mx = m->x + MOB_W / 2;
            my = m->y + MOB_H / 2;
            dx = px - mx;
            dy = py - my;

            if (dx < 0) m->x -= 12;
            else if (dx > 0) m->x += 12;
            if (dy < 0) m->y -= 12;
            else if (dy > 0) m->y += 12;

            if (m->x < ARENA_X0) m->x = ARENA_X0;
            if (m->x > ARENA_X1 - MOB_W) m->x = ARENA_X1 - MOB_W;
            if (m->y < ARENA_Y0) m->y = ARENA_Y0;
            if (m->y > ARENA_Y1 - MOB_H) m->y = ARENA_Y1 - MOB_H;

            m->hop_until = now + 180u;
            m->move_next = now + 520u + (mob_rand() % 320u);
        }

        /*  接触伤害：只看 slime 的身体 vs 玩家肚子  */
        if (!game_vital_dead(now) &&
            pt_in(g.x + PB_OFF_X + PB_W / 2, g.y + PB_OFF_Y + PB_H / 2,
                  m->x + MOB_BODY_X, m->y + MOB_BODY_Y, MOB_BODY_W, MOB_BODY_H)) {
            if (game_vital_hit(now) != 0) g.hurt_until = now + HURT_MS;
        }
    }

    /*  最后一只死掉的当帧：发清理奖励 + 开门  */
    if (s_spawned && game_mobs_alive() == 0) {
        int n = game_room_take_reward();

        s_spawned = 0;
        game_coins_spawn(n);
        g.door_dirty = 2;
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

    /*  帧 1 只在腾空时显示；抛物线抬升最多 6 px。  */
    jumping = (int)(m->hop_until - now) > 0;
    if (jumping) {
        u32 t = 180u - (m->hop_until - now);      /* 0..180：这一跳已经过了多久 */
        hop = (t * (180u - t) * 24u) / (180u * 180u);
        if (hop > 6u) hop = 6u;
    }

    a = &g_assets[jumping ? ASSET_mobs_slime_01 : ASSET_mobs_slime_00];
    CI_gpu2d_spriteAlphaRect(a->base_word, a->stride_word, fb,
                             (u32)m->x, (u32)((int)m->y - (int)hop), MOB_W, MOB_H, 0u);
}

void game_mobs_submit(void)
{
    int i;

    for (i = 0; i < N_MOBS; ++i) {
        const mob_t *m = &g.mobs[i];

        if (!m->active) continue;
        if (!game_visible(m->x, m->y - 8, MOB_W, MOB_H + 8)) continue;
        rl_push(ACTOR_Z(m->y + MOB_H), mob_draw_one, i);
    }
}
