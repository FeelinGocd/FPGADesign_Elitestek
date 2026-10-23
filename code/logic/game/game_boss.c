//==============================================================================
//  game_boss.c —— Boss 的宿主逻辑（血量 / 锁血 / 弹幕雨 / 位置 / 绘制）
//------------------------------------------------------------------------------
//  干什么      :
//    · game_boss_step()  每帧：位置(数据决定) → 锁血弹幕雨 / 喂发射器 → game_shot_step()
//    · game_boss_hit()   被打中：掉血 + 闪红；锁血时【不掉血、不闪红，改让护盾亮一下】
//    · game_boss_draw()  三帧待机 + 受击闪红
//    · game_boss_draw_shield() 锁血时的玻璃罩（被打中会变亮）
//  上一级是谁  : game/game_main.c（每帧）、game/game_bullet.c（命中）
//  接口契约    :
//    · 弹幕是【发射器】(game_shot.c) 放的，Boss 只负责更新炮口/瞄准点
//    · 数值全在 game_boss_def.c（血量 / 锁血线 / 弹幕雨 / 炮口 / 招式表 / 运动）
//    · 锁血期间一滴血都不掉，判定框换成玻璃罩
//  参考        : docs/spec/软件_驱动与API.md §8.1 / §8.2
//==============================================================================
#include "game.h"
#include "CI_gpu2d.h"
#include "bsp.h"        /* bsp_printf（关卡切换时打一行日志） */

static const int boss_seq[5] = { 0, 1, 2, 1, 0 };

static u32 rain_seed = 2463534242u;
static int rain_side;               /* 这一波从哪边来：0 上 1 右 2 下 3 左（轮着来） */

/*----------------------------------------------------------------------------*/
/*  血量越低越狂暴：满血 0，2/3 以下 1，1/3 以下 2  */
static int hp_frenzy(void)
{
    int permille = (g.boss_hp * 1000) / g_boss_def.hp;    /* 0..1000 */

    if (permille > 666) return 0;
    if (permille > 333) return 1;
    return 2;
}

/*----------------------------------------------------------------------------*/
static int eff_rain_drop(void)
{
    int v = g_boss_def.rain_drop + (g.stage - 1);
    return v > 9 ? 9 : v;
}

/*----------------------------------------------------------------------------*/
/*  按 motion 摆 Boss 位置（现在数据里是 BOSS_STILL ⇒ 居中不动）  */
static void boss_motion_step(u32 now)
{
    const boss_motion_t *m = &g_boss_def.motion;
    int cx, cy;

    if (game_world_is_boss()) {
        cx = (ARENA_X0 + ARENA_X1) / 2 - g_boss_def.w / 2;
        cy = (ARENA_Y0 + ARENA_Y1) / 2 - g_boss_def.h / 2;
    } else {
        /* 普通房保持原来的"屏幕正中"行为 */
        cx = (SCR_W - g_boss_def.w) / 2;
        cy = (SCR_H - g_boss_def.h) / 2;
    }
    int off;

    if (m->kind == BOSS_STILL) { g.boss_x = cx; g.boss_y = cy; return; }

    off = (g_sin_tab[((now / ((u32)m->period * 16u + 1u)) & 63u)] * m->amp) >> 6;
    if (m->kind == BOSS_SINE_X) { g.boss_x = cx + off; g.boss_y = cy; }
    else                        { g.boss_x = cx;      g.boss_y = cy + off; }
}

/*----------------------------------------------------------------------------*/
/*  弹幕雨：一波 rain_drop 颗，从【四条边轮流】往里砸（不是只从天上掉）  */
static void rain_wave(void)
{
    int k;
    int side = rain_side;
    int vx = 0, vy = 0;

    rain_side = (rain_side + 1) & 3;

    if (side == 0)      vy = 1;         /* 从上面往下 */
    else if (side == 1) vx = 1;         /* 从左边往右 */
    else if (side == 2) vy = -1;        /* 从下面往上 */
    else                vx = -1;        /* 从右边往左 */

    for (k = 0; k < eff_rain_drop(); ++k) {
        int x, y;

        rain_seed ^= rain_seed << 13;
        rain_seed ^= rain_seed >> 17;
        rain_seed ^= rain_seed << 5;

        if (vx == 0) {                  /* 上下两边的：横向随机，贴【当前视口】边出生 */
            x = g.cam_x + (int)(rain_seed % (SCR_W - BW));
            y = g.cam_y + ((vy > 0) ? 0 : (SCR_H - BH));
        } else {                        /* 左右两边的：纵向随机 */
            y = g.cam_y + (int)(rain_seed % (SCR_H - BH));
            x = g.cam_x + ((vx > 0) ? 0 : (SCR_W - BW));
        }
        /*  ★ 房间比视口窄/矮时（正方形大房间）别在黑边里刷弹：夹回场地内。 */
        if (x < ARENA_X0) x = ARENA_X0;
        if (x > ARENA_X1 - BW) x = ARENA_X1 - BW;
        if (y < ARENA_Y0) y = ARENA_Y0;
        if (y > ARENA_Y1 - BH) y = ARENA_Y1 - BH;
        game_bullet_spawn(x, y, vx, vy, MOTION_ACCEL, BULLET_URANUS, 1);
    }
}

/*----------------------------------------------------------------------------*/
/*  当前【朝向方向】里最近的敌人中心；没有返回 0。
 *  方向按“主轴”分：横向差 >= 纵向差 ⇒ 左/右，否则上/下。
 *  这样玩家背对/侧对怪物时不会自动转火，必须自己把朝向转过去。  */
int game_enemy_in_dir(int dir, int *tx, int *ty)
{
    int i, best = -1, bestd = 0x7fffffff;
    int px = g.x + PW / 2;
    int py = g.y + PH / 2;

    /*  Boss 房：Boss 优先（Boss 房不会同时有 slime）  */
    if (g.phase == PHASE_FIGHT && g.boss_hp > 0) {
        int bx = g.boss_x + BB_OFF_X + BB_W / 2;
        int by = g.boss_y + BB_OFF_Y + BB_H / 2;
        int dx = bx - px, dy = by - py;
        int adx = dx < 0 ? -dx : dx;
        int ady = dy < 0 ? -dy : dy;
        int match;

        if (dir == LEFT)       match = (dx < 0 && adx >= ady);
        else if (dir == RIGHT) match = (dx > 0 && adx >= ady);
        else if (dir == UP)    match = (dy < 0 && ady >= adx);
        else                   match = (dy > 0 && ady >= adx);

        if (match) {
            *tx = bx;
            *ty = by;
            return 1;
        }
        return 0;
    }

    for (i = 0; i < N_MOBS; ++i) {
        const mob_t *m = &g.mobs[i];
        int dx, dy, adx, ady, d, match;

        if (!m->active) continue;
        dx = m->x + 16 - px;
        dy = m->y + 16 - py;
        adx = dx < 0 ? -dx : dx;
        ady = dy < 0 ? -dy : dy;

        if (dir == LEFT)       match = (dx < 0 && adx >= ady);
        else if (dir == RIGHT) match = (dx > 0 && adx >= ady);
        else if (dir == UP)    match = (dy < 0 && ady >= adx);
        else                   match = (dy > 0 && ady >= adx);
        if (!match) continue;

        d = adx + ady;
        if (d < bestd) { bestd = d; best = i; }
    }
    if (best < 0) return 0;

    *tx = g.mobs[best].x + 16;
    *ty = g.mobs[best].y + 16;
    return 1;
}

/*----------------------------------------------------------------------------*/
/*  最近的敌人中心（保留给非射击用途；玩家自动开火改用 game_enemy_in_dir）。  */
int game_nearest_enemy(int *tx, int *ty)
{
    int i, best = -1, bestd = 0x7fffffff;
    int px = g.x + PW / 2;
    int py = g.y + PH / 2;

    /*  Boss 房：Boss 优先（Boss 房不会同时有 slime）  */
    if (g.phase == PHASE_FIGHT && g.boss_hp > 0) {
        *tx = g.boss_x + BB_OFF_X + BB_W / 2;        /* 瞄判定框中心，不是贴图中心 */
        *ty = g.boss_y + BB_OFF_Y + BB_H / 2;
        return 1;
    }

    /*  普通 / 道具房：找最近的 slime  */
    for (i = 0; i < N_MOBS; ++i) {
        const mob_t *m = &g.mobs[i];
        int dx, dy, d;

        if (!m->active) continue;
        dx = m->x + 16 - px;
        dy = m->y + 16 - py;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        d = dx + dy;
        if (d < bestd) { bestd = d; best = i; }
    }
    if (best < 0) return 0;

    *tx = g.mobs[best].x + 16;
    *ty = g.mobs[best].y + 16;
    return 1;
}

/*----------------------------------------------------------------------------*/
/*  护盾闪白：被打中 → 亮/灭快闪两下 → 之后 BLINK_COOL_MS 内不再闪（不然一片白）  */
static void shield_blink_tick(u32 now)
{
    if (g.boss_blink_cnt <= 0) {
        g.boss_blink_on = 0;
        return;
    }
    if ((int)(now - g.boss_blink_next) < 0) return;

    g.boss_blink_on = !g.boss_blink_on;
    --g.boss_blink_cnt;
    g.boss_blink_next = now + (g.boss_blink_on ? BLINK_ON_MS : BLINK_OFF_MS);
    if (g.boss_blink_cnt <= 0) g.boss_blink_on = 0;
}

/*============================================================================*/
void game_boss_step(u32 now)
{
    /*  Boss 击破后房间顶部出现 exit：玩家肚子碰到就进下一关  */
    if (g.exit_active && game_room_id() == g.exit_room && !game_vital_dead(now)) {
        const asset_desc_t *ex = &g_assets[ASSET_background_interact_eixt];
        int hx = g.x + PB_OFF_X;
        int hy = g.y + PB_OFF_Y;

        if (hx < g.exit_x + (int)ex->width && hx + PB_W > g.exit_x &&
            hy < g.exit_y + (int)ex->height && hy + PB_H > g.exit_y) {
            g.exit_active = 0;
            g.exit_dirty = 2;
            ++g.stage;
            bsp_printf("STAGE %d\r\n", g.stage);
            game_room_new_run();
            game_room_request(game_room_start(), -1);
            return;
        }
    }

    shield_blink_tick(now);

    if (g.phase != PHASE_FIGHT) return;         /* 非战斗阶段：Boss 不出弹 */
    if ((int)(now - g.boss_wait_until) < 0) return;  /* 进房先等 1 s 再出招 */

    boss_motion_step(now);                      /* 位置由 g_boss_def.motion 决定 */

    if (g.boss_hp <= 0) return;                 /* 已击破：等玩家走到 exit */

    if (g.boss_locked) {                        /* 锁血：波浪式弹幕雨 */
        if ((int)(now - g.boss_lock_until) < 0) {
            if (++g.boss_tick % (g_boss_def.rain_wave ? g_boss_def.rain_wave : 20u) == 0u)
                rain_wave();
            return;
        }
        g.boss_locked = 0;
        g.boss_tick = 0u;
        game_shot_begin(&g.shot, 0);            /* 锁血结束，从第一招重新开始 */
    }

    /*  发射器：炮口 / 瞄准点 / 急迫度每帧喂给它，然后让它自己按招式表放弹  */
    g.shot.mx = g.boss_x + g_boss_def.muzzle_dx;
    g.shot.my = g.boss_y + g_boss_def.muzzle_dy;
    /*  ★ 瞄【判定框中心】而不是贴图中心：
     *    贴图中心在胸口偏上（会打到"头"），判定框才是真正会受伤的那块。  */
    g.shot.tx = g.x + PB_OFF_X + PB_W / 2;
    g.shot.ty = g.y + PB_OFF_Y + PB_H / 2;
    g.shot.frenzy = hp_frenzy();
    game_shot_step(&g.shot);
}

/*----------------------------------------------------------------------------*/
void game_boss_hit(u32 now)
{
    /*  锁血期间打上去：不掉血、不闪红，只让玻璃罩快闪两下（有 1 秒冷却，
        不然玩家 6 发/秒会把它闪成一片白）。  */
    if (g.boss_locked) {
        if ((int)(now - g.boss_blink_cool) >= 0) {
            g.boss_blink_cnt = 4;               /* 亮-灭-亮-灭 = 两下 */
            g.boss_blink_on = 1;
            g.boss_blink_next = now + BLINK_ON_MS;
            g.boss_blink_cool = now + BLINK_COOL_MS;
        }
        return;
    }
    if (g.phase != PHASE_FIGHT || g.boss_hp <= 0) return;
    if ((int)(now - g.boss_wait_until) < 0) return;   /* 进房 1 s 内无敌 */

    g.boss_flash_until = now + HIT_FLASH_MS;
    g.boss_hp -= BOSS_HIT_DMG;
    g.score += SCORE_HIT;                               /* 打中就给分 */

    if (g.boss_hp <= g.boss_gate && g.boss_gate > 0) {   /* 打到锁血线 */
        g.boss_hp = g.boss_gate;
        g.boss_locked = 1;
        g.boss_lock_until = now + g_boss_def.rain_ms;
        g.boss_tick = 0u;
        game_shot_stop(&g.shot);
        g.boss_gate = (g.boss_gate == g_boss_def.gate1) ? g_boss_def.gate2 : 0;
        return;
    }
    if (g.boss_hp <= 0) {                               /* 击破：生成 exit */
        g.boss_hp = 0;
        g.score += SCORE_KILL;
        game_shot_stop(&g.shot);

        g.exit_active = 1;
        g.exit_room   = game_room_id();
        g.exit_x      = (ARENA_X0 + ARENA_X1) / 2 - 16;
        g.exit_y      = ARENA_Y0 + 64;                  /* 上面那扇门的门下 64 px */
        g.exit_dirty  = 0;
        g.door_dirty  = 2;                              /* 无怪物了：门从"不可传送"变为可传送；重画一次 */
        bsp_printf("EXIT: room %d at (%d,%d)\r\n", g.exit_room, g.exit_x, g.exit_y);

        /*  Boss 击破：掉一个随机饰品（不再发金币）。
         *  ★ game_room_take_reward() 只用来把本房标记成"已清"，返回值不要；
         *    普通房清怪仍然走 game_coins_spawn()。  */
        (void)game_room_take_reward();
        game_item_spawn_random((ARENA_X0 + ARENA_X1) / 2 - ITEM_W / 2,
                               (ARENA_Y0 + ARENA_Y1) / 2 - ITEM_H / 2);
        g.boss_drop_pending = 1;
        bsp_printf("BOSS DROP: accessory #%d\r\n", g.item.type);

        /*  ★ 击破后立刻退出 PHASE_FIGHT：
         *    · 顶部 Boss 血条（含 "BOSS" 标签）只在 FIGHT 时提交，否则
         *      它会一直挂着一个空血条，看起来像"Boss 还在"；
         *    · game_boss_draw() 本来就会跳过 hp<=0，但旧 old_boss 要清掉；
         *    · 门/exit 的逻辑只看 boss_hp 和 exit，不看 phase，所以安全。  */
        g.boss_locked = 0;
        g.boss_blink_cnt = 0;
        g.boss_blink_on = 0;
        g.phase = PHASE_EXPLORE;
    }
}

/*----------------------------------------------------------------------------*/
void game_boss_draw_exit(u32 fb)
{
    const asset_desc_t *ex = &g_assets[ASSET_background_interact_eixt];

    if (!g.exit_active || game_room_id() != g.exit_room) return;
    CI_gpu2d_spriteFlip(ex->base_word, ex->stride_word, fb,
        (u32)g.exit_x, (u32)g.exit_y, ex->width, ex->height, 0u);
}

/*----------------------------------------------------------------------------*/
/*  玻璃护盾：专门的 TheBoss_shieldUp（80x96 玻璃罩）盖在 Boss 身上。
 *  被打中时用颜色合成 tone=2 加光亮一下 —— 和 Boss 的闪红是两套反馈。  */
void game_boss_draw_shield(u32 fb, u32 now)
{
    const asset_desc_t *sh = &g_assets[ASSET_TheBoss_shieldUp];

    (void)now;

    if (g.boss_blink_on)
        CI_gpu2d_combineTintRect(sh->base_word, sh->stride_word,
            GPU2D_MODE_REPLACE | GPU2D_MODE_SPRITE | GPU2D_MODE_CKEY_EN,
            0x00FFFFFFu, 90u, 2u,               /* 白色 + 加光 + 强度 90 */
            fb, (u32)g.boss_x, (u32)g.boss_y, BOSS_W, BOSS_H);
    else
        CI_gpu2d_spriteAlphaRect(sh->base_word, sh->stride_word, fb,
            (u32)g.boss_x, (u32)g.boss_y, BOSS_W, BOSS_H, 0u);
}

/*----------------------------------------------------------------------------*/
void game_boss_draw(u32 fb, u32 now, int buf)
{
    int frame = boss_seq[(now / BOSS_FRAME_MS) % 5u];
    int ai;

    /*  ★ 非战斗阶段 / 已击破：不画。
     *    这里把 old_boss 置无效是给【下一帧】用的；这一帧的擦除用的是上一帧
     *    留下的旧位置，所以上一次画的 Boss 会被地砖盖掉，不会留在画面上。  */
    if (g.phase != PHASE_FIGHT || g.boss_hp <= 0) {
        g.old_boss[buf].active = 0;
        return;
    }

    if (frame <= 0)      ai = ASSET_TheBoss_00_StandUp;
    else if (frame == 1) ai = ASSET_TheBoss_01_StandUp;
    else                 ai = ASSET_TheBoss_02_StandUp;

    if ((int)(now - g.boss_flash_until) < 0)
        CI_gpu2d_combineTintRect(g_assets[ai].base_word, g_assets[ai].stride_word,
            GPU2D_MODE_REPLACE | GPU2D_MODE_SPRITE | GPU2D_MODE_CKEY_EN,
            0x00FF2828u, 200u, 1u,
            fb, (u32)g.boss_x, (u32)g.boss_y, BOSS_W, BOSS_H);
    else
        CI_gpu2d_spriteAlphaRect(g_assets[ai].base_word, g_assets[ai].stride_word,
            fb, (u32)g.boss_x, (u32)g.boss_y, BOSS_W, BOSS_H, 0u);

    g.old_boss[buf].active = 1;
    g.old_boss[buf].x = g.boss_x;
    g.old_boss[buf].y = g.boss_y;
}

/*----------------------------------------------------------------------------*/
/*  登记进渲染队列：本体按底边 y 排队（和角色/弹幕互相遮挡），护盾单独占 Z_SHIELD  */
static void boss_draw_entry(u32 fb, u32 now, int buf, int arg)
{
    (void)arg;
    game_boss_draw(fb, now, buf);
}

static void shield_draw_entry(u32 fb, u32 now, int buf, int arg)
{
    (void)buf; (void)arg;
    game_boss_draw_shield(fb, now);
}

void game_boss_submit(void)
{
    /*  ★ 本体【每帧都要登记】：哪怕这一帧不画（拾取阶段 / 已击破），
     *    也要让 game_boss_draw() 把 old_boss[buf] 清掉，否则旧位置永远擦不到。 */
    rl_push(ACTOR_Z(g.boss_y + BOSS_H), boss_draw_entry, 0);

    if (g.boss_locked)
        rl_push(Z_SHIELD, shield_draw_entry, 0);
}
