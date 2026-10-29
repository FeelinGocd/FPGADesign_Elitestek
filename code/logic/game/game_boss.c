//==============================================================================
//  game_boss.c —— Boss 的宿主逻辑（血量 / 锁血 / 弹幕雨 / 位置 / 绘制）
//------------------------------------------------------------------------------
//  干什么      :
//    · game_boss_step()  每帧：位置 → 转阶段护盾 → 召唤 / 弹幕雨 / 主发射器
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

static const unsigned char boss_seq[5] = { 0, 1, 2, 1, 0 };

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
static const boss_phase_def_t *phase_def(void)
{
    int id = g.boss_phase;
    if (id < 0 || id >= BOSS_PHASE_COUNT) id = 0;
    return &g_boss_phases[id];
}

static int phase_for_hp(void)
{
    return g.boss_hp > g_boss_def.gate1 ? BOSS_PHASE_CORE :
           g.boss_hp > g_boss_def.gate2 ? BOSS_PHASE_RAIN : BOSS_PHASE_COMBINED;
}

static int rain_active(void)
{
    const boss_phase_def_t *p = phase_def();
    u32 cycle = (u32)p->rain_on + p->rain_rest;
    return p->rain_every && cycle && g.boss_tick &&
           (g.boss_tick - 1u) % cycle < p->rain_on;
}

int game_boss_shot_limit(void)
{
    int cap = phase_def()->shot_cap;
    if (g.boss_phase == BOSS_PHASE_COMBINED && !rain_active()) cap = ENEMY_CAP;
    return cap < ENEMY_CAP ? cap : ENEMY_CAP;
}

static void phase_pattern_begin(void)
{
    const boss_phase_def_t *p = phase_def();
    game_shot_stop(&g.shot);
    g.shot.move_idx = p->first_move;
    if (p->shot_cap) game_shot_begin(&g.shot, p->first_move);
}

void game_boss_resume(void)
{
    g.boss_phase = phase_for_hp();
    g.boss_tick = 0u;
    rain_side = 0;
    phase_pattern_begin();
    bsp_printf("BOSS PHASE: id=%d hp=%d rain=%d/%d shot_cap=%d\r\n",
               g.boss_phase + 1, g.boss_hp,
               phase_def()->rain_drop, phase_def()->rain_every, phase_def()->shot_cap);
}

static int eff_rain_drop(void)
{
    int n = phase_def()->rain_drop + g.stage - 1;
    int cap = ENEMY_CAP > 500 ? (32 * ENEMY_CAP + 499) / 500 : 32;
    return n > cap ? cap : n;
}

static int overlap(int x,int y,int w,int h,int ax,int ay,int aw,int ah)
{
    return x < ax + aw && x + w > ax && y < ay + ah && y + h > ay;
}

static void summon_wave(void)
{
    int made = 0, i;
    for (i = 0; i < 64 && made < BOSS_SUMMON_COUNT && made < N_MOBS; ++i) {
        int j, blocked = 0, dx, dy;
        int angle = (i*4 + g.boss_phase*3) & 63;
        int radius = 160 + (i/16)*80;
        int x = g.boss_x+BOSS_W/2+g_sin_tab[(angle+16)&63]*radius/64-16;
        int y = g.boss_y+BOSS_H/2+g_sin_tab[angle]*radius/64-16;
        if (x < ARENA_X0+TILE) x = ARENA_X0+TILE;
        if (x > ARENA_X1-2*TILE) x = ARENA_X1-2*TILE;
        if (y < ARENA_Y0+TILE) y = ARENA_Y0+TILE;
        if (y > ARENA_Y1-2*TILE) y = ARENA_Y1-2*TILE;
        dx = x+16-(g.x+PW/2); if (dx < 0) dx = -dx;
        dy = y+16-(g.y+PH/2); if (dy < 0) dy = -dy;
        if (dx+dy < 144 || overlap(x,y,32,32,g.boss_x,g.boss_y,BOSS_W,BOSS_H)) continue;
        for (j = 0; j < N_MOBS; ++j)
            if (g.mobs[j].active && overlap(x,y,32,32,g.mobs[j].x,g.mobs[j].y,32,32)) blocked = 1;
        if (!blocked && game_mobs_spawn_at(x,y,made&1)) ++made;
    }
    bsp_printf("BOSS SUMMON: phase=%d count=%d hp=%d\r\n",g.boss_phase+1,made,g.boss_hp);
}

/*----------------------------------------------------------------------------*/
/*  当前招式选运动池目标，再逐帧靠近；切招式时不会瞬移。  */
static void boss_motion_step(u32 now)
{
    unsigned int motion_idx = 0;
    const boss_motion_t *m;
    int cx, cy, tx, ty, off = 0, side_off = 0, max_step;
#if GAME_BOSS_MOTION_TRACE
    static int last_move = -1;
    int old_x = g.boss_x, old_y = g.boss_y;
#endif

    if (g.shot.set == g_boss_def.moves && g.shot.move_idx >= 0 &&
        g.shot.move_idx < g.shot.set->n_moves)
        motion_idx = g.shot.set->moves[g.shot.move_idx].motion;
    if (motion_idx >= g_boss_def.n_motions) motion_idx = 0;
    m = &g_boss_def.motions[motion_idx];

    if (game_world_is_boss()) {
        cx = (ARENA_X0 + ARENA_X1) / 2 - g_boss_def.w / 2;
        cy = (ARENA_Y0 + ARENA_Y1) / 2 - g_boss_def.h / 2;
    } else {
        /* 普通房保持原来的"屏幕正中"行为 */
        cx = (SCR_W - g_boss_def.w) / 2;
        cy = (SCR_H - g_boss_def.h) / 2;
    }
    tx = cx;
    ty = cy;
    if (m->kind == BOSS_SINE_X || m->kind == BOSS_SINE_Y ||
        m->kind == BOSS_ORBIT) {
        u32 step_ms = (u32)(m->period ? m->period : 1u) * 16u;
        u32 phase = (now - g.boss_wait_until) % (step_ms * 64u);
        u32 idx = phase / step_ms;
        u32 frac = phase % step_ms;
        int a = g_sin_tab[idx];
        int b = g_sin_tab[(idx + 1u) & 63u];

        off = ((a * (int)(step_ms - frac) + b * (int)frac) * m->amp) /
              ((int)step_ms * 64);
        if (m->kind == BOSS_ORBIT) {
            a = g_sin_tab[(idx + 16u) & 63u];
            b = g_sin_tab[(idx + 17u) & 63u];
            side_off = ((a * (int)(step_ms - frac) + b * (int)frac) *
                        m->amp * 3) / ((int)step_ms * 256);
        }
    }
    if (m->kind == BOSS_SINE_X) tx += off;
    else if (m->kind == BOSS_SINE_Y) ty += off;
    else if (m->kind == BOSS_ORBIT) { tx += off; ty += side_off; }
    else if (m->kind == BOSS_LEFT) tx -= m->amp;
    else if (m->kind == BOSS_RIGHT) tx += m->amp;
    else if (m->kind == BOSS_UP) ty -= m->amp;
    else if (m->kind == BOSS_DOWN) ty += m->amp;

    if (game_world_is_boss()) {
        if (tx < ARENA_X0) tx = ARENA_X0;
        if (tx > ARENA_X1 - g_boss_def.w) tx = ARENA_X1 - g_boss_def.w;
        if (ty < ARENA_Y0) ty = ARENA_Y0;
        if (ty > ARENA_Y1 - g_boss_def.h) ty = ARENA_Y1 - g_boss_def.h;
    }
    max_step = m->speed ? m->speed : 1;
    if (g.boss_slow_until && (int)(now - g.boss_slow_until) < 0)
        max_step = (max_step + 1) / 2;
    if (tx > g.boss_x + max_step) tx = g.boss_x + max_step;
    if (tx < g.boss_x - max_step) tx = g.boss_x - max_step;
    if (ty > g.boss_y + max_step) ty = g.boss_y + max_step;
    if (ty < g.boss_y - max_step) ty = g.boss_y - max_step;
    g.boss_x = tx;
    g.boss_y = ty;
#if GAME_BOSS_MOTION_TRACE
    if (last_move != g.shot.move_idx) {
        bsp_printf("BOSS MOVE: skill=%d motion=%d x=%d y=%d\r\n",
                   g.shot.move_idx, (int)motion_idx, g.boss_x, g.boss_y);
        last_move = g.shot.move_idx;
    }
    if (g.boss_x - old_x > max_step || old_x - g.boss_x > max_step ||
        g.boss_y - old_y > max_step || old_y - g.boss_y > max_step)
        bsp_printf("BOSS MOVE ERROR: dx=%d dy=%d max=%d\r\n",
                   g.boss_x - old_x, g.boss_y - old_y, max_step);
#endif
}

/*----------------------------------------------------------------------------*/
/*  弹幕雨：一波 rain_drop 颗，从【四条边轮流】往里砸（不是只从天上掉）  */
static void rain_wave(void)
{
    int k, side, emit = eff_rain_drop();
    int budget = ENEMY_CAP - game_enemy_bullets_alive();
    int free_slots = N_BULLETS - game_bullets_alive();
    int x0 = g.cam_x > ARENA_X0 ? g.cam_x : ARENA_X0;
    int y0 = g.cam_y > ARENA_Y0 ? g.cam_y : ARENA_Y0;
    int x1 = g.cam_x + SCR_W - BW, y1 = g.cam_y + SCR_H - BH;
    int lo, span, gap, vertical;
    if (x1 > ARENA_X1 - BW) x1 = ARENA_X1 - BW;
    if (y1 > ARENA_Y1 - BH) y1 = ARENA_Y1 - BH;
    if (budget > free_slots) budget = free_slots;
#if ENEMY_CAP <= 500
    if (emit > budget) emit = budget;
#else
    /* 保留完整的空间采样，只截断实际分配数量。
     * 若把 emit 缩成 1，最后一个候选可能总落在安全通道，
     * 导致大容量试跑池长期无法填满。 */
    if (budget <= 0) return;
#endif
    if (emit <= 0 || x1 < x0 || y1 < y0) return;
    side = rain_side; rain_side = (rain_side + 1) & 3;
    vertical = !(side & 1);
    lo = vertical ? x0 : y0;
    span = (vertical ? x1-x0 : y1-y0) + 1;
    /* 保留一条缓慢扫动的 96 px 安全通道，通道不跟随玩家。 */
    gap = lo + span/2 + g_sin_tab[((g.boss_tick/phase_def()->rain_every)/4u*2u)&63u]*span/256;
    for (k = 0; k < emit; ++k) {
#if ENEMY_CAP > 500
        if (budget <= 0) break;
#endif
        int pos = lo + (2*k+1)*span/(2*emit);
        int d = pos-gap;
        if (d > -48 && d < 48) continue;
        game_bullet_spawn(vertical ? pos : (side == 1 ? x1 : x0),
                          vertical ? (side == 0 ? y0 : y1) : pos,
                          vertical ? 0 : (side == 1 ? -1 : 1),
                          vertical ? (side == 0 ? 1 : -1) : 0,
                          MOTION_ACCEL,BULLET_URANUS,1);
#if ENEMY_CAP > 500
        --budget;
#endif
    }
}

static int dir_match(int dir, int dx, int dy)
{
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;

    if (dir == LEFT)  return dx < 0 && adx >= ady;
    if (dir == RIGHT) return dx > 0 && adx >= ady;
    if (dir == UP)    return dy < 0 && ady >= adx;
    return dy > 0 && ady >= adx;
}

/*  Boss 中心（没开战 / 已击破返回 0）；召唤物由 nearest_mob 单独选择。 */
static int boss_center(int *tx, int *ty)
{
    if (g.phase != PHASE_FIGHT || g.boss_hp <= 0 || g.boss_locked) return 0;
    *tx = g.boss_x + BB_OFF_X + BB_W / 2;        /* 瞄判定框中心，不是贴图中心 */
    *ty = g.boss_y + BB_OFF_Y + BB_H / 2;
    return 1;
}

/*  最近的活着的 slime 中心（dir < 0 = 不限方向）。返回下标，-1 = 没有。  */
static int nearest_mob(int dir, int *tx, int *ty)
{
    int i, best = -1, bestd = 0x7fffffff;
    int px = g.x + PW / 2;
    int py = g.y + PH / 2;

    for (i = 0; i < N_MOBS; ++i) {
        const mob_t *m = &g.mobs[i];
        int dx, dy, d;

        if (!m->active) continue;
        dx = m->x + 16 - px;
        dy = m->y + 16 - py;
        if (dir >= 0 && !dir_match(dir, dx, dy)) continue;
        d = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
        if (d < bestd) { bestd = d; best = i; }
    }
    if (best < 0) return -1;
    *tx = g.mobs[best].x + 16;
    *ty = g.mobs[best].y + 16;
    return best;
}

/*  当前【朝向方向】里最近的敌人中心；没有返回 0。
 *  ★ 玩家背对/侧对怪物时不会自动转火，必须自己把朝向转过去。  */
int game_enemy_in_dir(int dir, int *tx, int *ty)
{
    if (boss_center(tx, ty)) {
        int dx = *tx - (g.x + PW / 2);
        int dy = *ty - (g.y + PH / 2);

        if (dir_match(dir, dx, dy)) return 1;
    }
    return nearest_mob(dir, tx, ty) >= 0;
}

/*  最近的敌人中心（板载按键模式用它判断当前朝向是否有敌人）。  */
int game_nearest_enemy(int *tx, int *ty)
{
    if (boss_center(tx, ty)) return 1;
    return nearest_mob(-1, tx, ty) >= 0;
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
            g.exit_dirty = GAME_FB_COUNT;
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


    if (g.boss_hp <= 0) return;                 /* 已击破：等玩家走到 exit */

    if (g.boss_locked) {
        if ((int)(now - g.boss_lock_until) < 0 || game_mobs_alive()) return;
        g.boss_locked = 0;
        g.boss_tick = 0u;
        phase_pattern_begin();
        bsp_printf("BOSS RESUME: phase=%d hp=%d mobs=0\r\n",g.boss_phase+1,g.boss_hp);
    }
    boss_motion_step(now);
#if GAME_BOSS_STAGE_TEST == 1
#if !GAME_BOSS_DEBUG
#error GAME_BOSS_STAGE_TEST requires GAME_BOSS_DEBUG
#endif
    if (g.boss_tick >= 720u) {
        g.boss_hp = (g.boss_gate > 0 ? g.boss_gate : 0) + BOSS_HIT_DMG;
        game_boss_hit(now);
        return;
    }
#endif
    ++g.boss_tick;
    {
        const boss_phase_def_t *p = phase_def();
        if (rain_active() && (g.boss_tick-1u)%p->rain_every == 0u) rain_wave();
        if (!p->shot_cap) return;
    }

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
    if (game_item_has(ITEM_URANUS) && (int)(now - g.boss_slow_next) >= 0) {
        g.boss_slow_until = now + URANUS_SLOW_MS;
        g.boss_slow_next = now + URANUS_SLOW_COOLDOWN_MS;
    }
    g.boss_hp -= game_player_hit_damage(BOSS_HIT_DMG);
    g.score += SCORE_HIT;                               /* 打中就给分 */

    if (g.boss_hp <= g.boss_gate && g.boss_gate > 0) {   /* 打到锁血线 */
        g.boss_hp = g.boss_gate;
        g.boss_locked = 1;
        g.boss_lock_until = now + g_boss_def.phase_shift_ms;
        g.boss_tick = 0u;
        game_shot_stop(&g.shot);
        game_enemy_bullets_clear();
        game_mobs_dismiss();
        g.boss_phase = phase_for_hp();
        g.shot.move_idx = phase_def()->first_move;
        rain_side = 0;
        summon_wave();
        bsp_printf("BOSS PHASE: id=%d hp=%d shift_ms=%d\r\n",
                   g.boss_phase+1,g.boss_hp,(int)g_boss_def.phase_shift_ms);
        g.boss_gate = (g.boss_gate == g_boss_def.gate1) ? g_boss_def.gate2 : 0;
        return;
    }
    if (g.boss_hp <= 0) {                               /* 击破：生成 exit */
        g.boss_hp = 0;
        game_enemy_bullets_clear();
        game_mobs_dismiss();
        g.score += SCORE_KILL;
        game_shot_stop(&g.shot);

        g.exit_active = 1;
        g.exit_room   = game_room_id();
        g.exit_x      = (ARENA_X0 + ARENA_X1) / 2 - 16;
        g.exit_y      = ARENA_Y0 + 64;                  /* 上面那扇门的门下 64 px */
        g.exit_dirty  = 0;
        g.door_dirty  = GAME_FB_COUNT;
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
    else if (g.boss_slow_until && (int)(now - g.boss_slow_until) < 0)
        CI_gpu2d_spriteAlphaTintRect(g_assets[ai].base_word, g_assets[ai].stride_word,
                                     fb, (u32)g.boss_x, (u32)g.boss_y,
                                     BOSS_W, BOSS_H, URANUS_TINT_RGB,
                                     URANUS_TINT_K, 1u, 0u);
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
