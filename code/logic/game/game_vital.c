//==============================================================================
//  game_vital.c —— 生命（心）与命数：扣血 / 死亡动画 / 重生 / 面板第 5 行显示
//------------------------------------------------------------------------------
//  干什么      :
//    · game_vital_hit()  被敌弹打中时调一次：扣一颗心；扣光就进死亡动画
//    · game_vital_step() 每帧推进：死亡动画结束 → 补满 3 颗心、命数 −1、命尽重开
//    · game_vital_draw() 面板第 5 行：心（满的 CK 直写、空的压暗）+ "X" + 额外命数
//  上一级是谁  : game_bullet.c（命中）、game_main.c（每帧）、game_player.c（闪烁）
//  接口契约    :
//    · 只认"被打中一次"，不关心谁打的、扣几分
//    · 只改 hp / lives / 无敌时间，不碰坐标、弹种、道具
//  为什么这么做:
//    · 绘制走"不透明底板 + CK 直写"，只有值变了才整行重画 ⇒ 没有残影、也没命令开销
//    · 生命逻辑和玩家/弹道/道具解耦：以后加护盾、吸血、难度都只改这里
//  参考        : docs/spec/软件_驱动与API.md §8.1
//==============================================================================
#include "game.h"
#include "CI_gpu2d.h"

#define HEART_W        24                    /* SpecialEffect_health 尺寸 */
#define HEART_GAP      4
#define HEART_X        HUD_LABEL_X           /* 面板第 5 行（原 ITEMS 那行） */
#define HEART_Y        (VITAL_ROW * HUD_ROW_H + 4)
#define FX_HEARTS      1                     /* 死亡时飘几个 "-心" */
#define FX_LIFE_MS     800u                  /* 飘多久 */
#define FX_RISE_MS     34u                   /* 每 34 ms 升 1 px（约 2 帧） */

static int s_redraw;                         /* 本帧要不要重画血量行 */
static int s_last_hp2[2] = { -1, -1 }, s_last_extra2[2] = { -1, -1 };

/*  ★ 整局重开：把"上次画的值"作废，下一帧两块缓冲都会重画血量行  */
void game_vital_invalidate(void)
{
    s_last_hp2[0] = s_last_hp2[1] = -1;
    s_last_extra2[0] = s_last_extra2[1] = -1;
    s_redraw = 2;
}


static int  s_hp;
static int  s_lives;
static u32  s_invuln_until;
static u32  s_dead_until;
static int  s_over;

static struct {
    int active;
    int x, y0, y;
    int px, py;
    u32 start;
} s_fx[FX_HEARTS];

/*----------------------------------------------------------------------------*/
void game_vital_reset(void)
{
    int i;

    s_hp = HP_MAX;
    s_lives = LIVES_START;
    s_invuln_until = 0u;
    s_dead_until = 0u;
    s_over = 0;
    for (i = 0; i < FX_HEARTS; ++i) s_fx[i].active = 0;
}

int  game_vital_hp(void)         { return s_hp; }
int  game_vital_lives(void)      { return s_lives; }
int  game_vital_over(void)       { return s_over; }
void game_vital_clear_over(void) { s_over = 0; }

void game_vital_extra_life(void)
{
    if (s_lives < 99) ++s_lives;
}

int game_vital_invuln(u32 now)
{
    return (s_invuln_until != 0u) && ((int)(now - s_invuln_until) < 0);
}

int game_vital_dead(u32 now)
{
    return (s_dead_until != 0u) && ((int)(now - s_dead_until) < 0);
}

/*----------------------------------------------------------------------------*/
int game_vital_hit(u32 now)
{
    int i;

    if (g.phase == PHASE_GAMEOVER) return 0;     /* 结束画面里不再扣血 */
    if (game_debug_invuln)      return 0;       /* 串口 debug0 / T 键：不掉血 */
    if (game_vital_invuln(now)) return 0;
    if (s_dead_until != 0u)     return 0;
    if (s_hp <= 0)              return 0;

    --s_hp;

    if (s_hp > 0) {
        s_invuln_until = now + HURT_INVULN_MS;
        return 1;
    }

    /*  心扣光：飘一个 "-心" 出去，人物闪一下，动画结束补满  */
    for (i = 0; i < FX_HEARTS; ++i) {
        s_fx[i].active = 1;
        s_fx[i].x  = g.x + PW / 2 - HEART_W / 2;
        s_fx[i].y0 = g.y + PH / 2 - HEART_W / 2;
        s_fx[i].y  = s_fx[i].y0;
        s_fx[i].px = s_fx[i].x;
        s_fx[i].py = s_fx[i].y;
        s_fx[i].start = now;
    }

    s_dead_until   = now + FX_LIFE_MS;
    s_invuln_until = now + FX_LIFE_MS + RESPAWN_INVULN_MS;
    return 2;
}

/*----------------------------------------------------------------------------*/
void game_vital_step(u32 now)
{
    int i;

    for (i = 0; i < FX_HEARTS; ++i) {
        u32 el;

        if (!s_fx[i].active) continue;
        el = (u32)(now - s_fx[i].start);
        s_fx[i].py = s_fx[i].y;
        s_fx[i].y  = s_fx[i].y0 - (int)(el / FX_RISE_MS);
        if (el >= FX_LIFE_MS) s_fx[i].active = 0;
    }

    if (s_dead_until != 0u && (int)(now - s_dead_until) >= 0) {
        s_dead_until = 0u;
        s_hp = HP_MAX;
        if (s_lives > 0) --s_lives;
        if (s_lives <= 0) { s_lives = 0; s_over = 1; }
    }
}

/*----------------------------------------------------------------------------*/
/*  帧首：血量/命数变化 → 把那一行标脏（旧心、旧 X2 由脏区恢复擦掉）  */
void game_vital_prepare(unsigned char dirty[GRID_H][GRID_W], int buf)
{
    int extra = s_lives - 1;

    if (buf > 1) buf = 0;
    if (s_last_hp2[buf] == s_hp && s_last_extra2[buf] == extra) return;

    s_last_hp2[buf] = s_hp;
    s_last_extra2[buf] = extra;
    game_hud_dirty_row(dirty, VITAL_ROW);
    s_redraw = 2;
}

/*  脏区：飘心在动的时候标（它每帧都动）  */
void game_vital_mark_dirty(unsigned char dirty[GRID_H][GRID_W])
{
    int i;

    for (i = 0; i < FX_HEARTS; ++i) {
        if (!s_fx[i].active) continue;
        game_mark_dirty(dirty, s_fx[i].px - 2, s_fx[i].py - 2, 2 * HEART_W + 6, HEART_W + 16);
        game_mark_dirty(dirty, s_fx[i].x  - 2, s_fx[i].y  - 2, 2 * HEART_W + 6, HEART_W + 16);
    }
}

/*----------------------------------------------------------------------------*/
void game_vital_draw(u32 fb, u32 now, int full)
{
    const asset_desc_t *heart = &g_assets[ASSET_SpecialEffect_health];
    const asset_desc_t *minus = &g_assets[ASSET_SpecialEffect_subtract];
    int extra = s_lives - 1;
    int i, x;

    (void)now;

    /*  只有"血量/额外命数变了"或"有精灵压到面板上"才整行重画：
     *  先用底板擦干净，再画心 —— 不会出现旧心 / 旧 X2 的残影  */
    /*  只有"帧首标脏过"或"有精灵压到面板"时才重画；擦除已经在帧首做完了  */
    if (full || s_redraw > 0) {
        int lost, k;

        if (s_redraw > 0) --s_redraw;

        lost = HP_MAX - s_hp;
        k = 210 - lost * 55;                  /* 掉 1→155, 2→100, 3→45 */
        if (k < 40) k = 40;

        for (i = 0; i < HP_MAX; ++i) {
            x = HEART_X + i * (HEART_W + HEART_GAP);
            if (!game_visible(x, HEART_Y, HEART_W, HEART_W)) continue;

            if (i < s_hp)
                CI_gpu2d_spriteFlipCK(heart->base_word, heart->stride_word, fb,
                                      (u32)x, (u32)HEART_Y, HEART_W, HEART_W, 0u);
            else
                CI_gpu2d_combineTintRect(heart->base_word, heart->stride_word,
                    GPU2D_MODE_REPLACE | GPU2D_MODE_SPRITE | GPU2D_MODE_CKEY_EN,
                    0x00000000u, (u32)k, 3u,
                    fb, (u32)x, (u32)HEART_Y, HEART_W, HEART_W);
        }

        if (extra > 0) {
            x = HEART_X + HP_MAX * (HEART_W + HEART_GAP) + 8;
            game_hud_char_at(fb, x, HEART_Y, ASSET_English_X);
            game_hud_number_at(fb, x + 16, HEART_Y, extra, 1);
        }
    }

    /*  死亡飘心：会动，所以每帧按当前位置画（上一帧位置由脏区擦）。
     *  ★ GAME OVER 窗口期间不画：否则它会从窗口里穿出来。  */
    for (i = 0; i < FX_HEARTS; ++i) {
        if (!s_fx[i].active || g.phase == PHASE_GAMEOVER) continue;

        if (game_visible(s_fx[i].x, s_fx[i].y, HEART_W, HEART_W))
            CI_gpu2d_spriteFlipCK(heart->base_word, heart->stride_word, fb,
                                  (u32)s_fx[i].x, (u32)s_fx[i].y, HEART_W, HEART_W, 0u);

        if (game_visible(s_fx[i].x + HEART_W, s_fx[i].y + 6, HEART_W, HEART_W))
            CI_gpu2d_spriteAlphaRect(minus->base_word, minus->stride_word, fb,
                                     (u32)(s_fx[i].x + HEART_W), (u32)(s_fx[i].y + 6),
                                     HEART_W, HEART_W, 0u);
    }
}
