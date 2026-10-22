//==============================================================================
//  game_player.c —— 角色：移动 / 动画选帧 / 自动开火 / 受伤与拾取表现
//------------------------------------------------------------------------------
//  干什么      :
//    · game_player_step(): 读按键改坐标（一次只走一个方向）、夹到屏内、
//                          判断是否踩到道具、推进动画节拍、到点自动开火
//    · game_player_draw(): 选帧（受伤 / 拾取 / 四方向）→ 画精灵，
//                          拾取时再叠一张道具图标在角色身上
//    · game_player_fire(): 在空闲槽里放一发玩家子弹
//  上一级是谁  : game/game_main.c
//  接口契约    :
//    · 坐标就是屏幕坐标（不做滚屏）
//    · 【只存朝右一套素材】：朝左用 FLIP_X；朝下/朝上的个别帧也靠 FLIP_X 复用
//    · 受伤 / 拾取帧强制不翻面（这两张素材只有朝右的）
//  为什么这么做:
//    · 硬件没有源裁剪 ⇒ 精灵必须整块在屏内，坐标在这里就夹好
//    · 硬件只有镜像、没有人物旋转 ⇒ 四方向 = 序列表 + 镜像位（零素材开销）
//  参考        : docs/spec/软件_驱动与API.md §2.3（旋转与四方向）
//==============================================================================
#include "game.h"
#include "CI_gpu2d.h"

/* 每方向一张序列表 + 每一拍要不要翻面，共用同一套"朝右"素材。 */
/*  侧向跑现在是三帧（素材里多给了 walkRight_02_left，专门画了迈另一边腿的姿势）  */
static const int side_seq[ANIM_FRAMES] = {
    ASSET_bigfish_walkRight_00_standUp, ASSET_bigfish_walkRight_01_right,
    ASSET_bigfish_walkRight_02_left,    ASSET_bigfish_walkRight_01_right,
    ASSET_bigfish_walkRight_00_standUp
};
static const int down_seq[ANIM_FRAMES] = {
    ASSET_bigfish_walkDown_00_standUp, ASSET_bigfish_walkDown_01_Right,
    ASSET_bigfish_walkDown_00_standUp, ASSET_bigfish_walkDown_01_Right,
    ASSET_bigfish_walkDown_00_standUp
};
static const int up_seq[ANIM_FRAMES] = {
    ASSET_bigfish_walkUp_00_standUp, ASSET_bigfish_walkUp_01_right,
    ASSET_bigfish_walkUp_00_standUp, ASSET_bigfish_walkUp_01_right,
    ASSET_bigfish_walkUp_00_standUp
};
/* 第 4 拍翻一下，看着像脚换边 */
static const u32 down_flip[ANIM_FRAMES] = { 0u, 0u, 0u, GPU2D_FLIP_X, 0u };
static const u32 up_flip[ANIM_FRAMES]   = { 0u, 0u, 0u, GPU2D_FLIP_X, 0u };

/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
/*  速度朝目标逼近（加速 / 刹车都用它）  */
static int approach(int cur, int target, int step)
{
    if (cur < target) { cur += step; if (cur > target) cur = target; }
    else if (cur > target) { cur -= step; if (cur < target) cur = target; }
    return cur;
}

/*----------------------------------------------------------------------------*/
void game_player_step(u32 keys, u32 now)
{
    int nx = g.x, ny = g.y;
    int mx = 0, my = 0;
    int tvx, tvy;

    /* 两个轴分别看：同时按两个方向就是斜着走（以前用 else if，一次只走一个轴） */
    if (keys & KEY_LEFT)  --mx;
    if (keys & KEY_RIGHT) ++mx;
    if (keys & KEY_UP)    --my;
    if (keys & KEY_DOWN)  ++my;

    /*  目标速度：按下就是满速，松开就是 0。
     *  实际速度每帧朝目标逼近 —— 这就是"加速 / 减速"，子弹会继承它（动量）。  */
    if (mx && my) { tvx = mx * PLAYER_DIAG_V8; tvy = my * PLAYER_DIAG_V8; }
    else          { tvx = mx * PLAYER_MAX_V8;  tvy = my * PLAYER_MAX_V8;  }

    /*  站在蜘蛛网上：目标速度降到 45%（离开就恢复）  */
    if (game_web_slow_at(g.x + PB_OFF_X + PB_W / 2,
                         g.y + PB_OFF_Y + PB_H / 2)) {
        tvx = tvx * 45 / 100;
        tvy = tvy * 45 / 100;
    }

    g.vx8 = approach(g.vx8, tvx, tvx ? PLAYER_ACCEL_V8 : PLAYER_BRAKE_V8);
    g.vy8 = approach(g.vy8, tvy, tvy ? PLAYER_ACCEL_V8 : PLAYER_BRAKE_V8);

    /* 亚像素余量：速度是 1/8 像素，攒够 1 像素才真的挪一格 */
    g.rx += g.vx8;
    g.ry += g.vy8;
    nx += g.rx >> 3;
    ny += g.ry >> 3;
    g.rx -= (g.rx >> 3) * 8;
    g.ry -= (g.ry >> 3) * 8;

    /* 朝向：人物只有 4 个方向，有水平分量就按"侧身跑"处理（斜走看着最自然） */
    if (mx < 0)      g.dir = LEFT;
    else if (mx > 0) g.dir = RIGHT;
    else if (my < 0) g.dir = UP;
    else if (my > 0) g.dir = DOWN;

    /*  ★ 撞墙 = 【肚子】撞到场地边界（基岩），不是贴图包围盒。
     *    头/脚/尾巴可以探进墙里；正对着门时门洞那一段再往外放一点，让人走进门。  */
    {
        int bx0 = ARENA_X0 - PB_OFF_X, by0 = ARENA_Y0 - PB_OFF_Y;
        int bx1 = ARENA_X1 - PB_OFF_X - PB_W, by1 = ARENA_Y1 - PB_OFF_Y - PB_H;

        game_room_door_bounds(nx, ny, &bx0, &by0, &bx1, &by1);

        if (nx < bx0) { nx = bx0; g.vx8 = 0; g.rx = 0; }
        if (nx > bx1) { nx = bx1; g.vx8 = 0; g.rx = 0; }
        if (ny < by0) { ny = by0; g.vy8 = 0; g.ry = 0; }
        if (ny > by1) { ny = by1; g.vy8 = 0; g.ry = 0; }
    }

    /*  ★ 可交互石头：按【肚子】挡；分轴处理，可以贴着石头滑动  */
    if (game_stone_block(nx, ny)) {
        if (!game_stone_block(nx, g.y)) {
            ny = g.y;
        } else if (!game_stone_block(g.x, ny)) {
            nx = g.x;
        } else {
            nx = g.x;
            ny = g.y;
        }
        g.vx8 = 0; g.vy8 = 0; g.rx = 0; g.ry = 0;
    }

    g.moving = (nx != g.x || ny != g.y || g.vx8 != 0 || g.vy8 != 0);
    g.x = nx;
    g.y = ny;

    game_item_try_pickup(now);

    /* 动画只在"一直在走、方向没变"时推进；停下或换向立刻回到站立帧。 */
    if (!g.moving || !g.was_moving || g.dir != g.last_dir) g.anim = 0u;
    else g.anim = (g.anim + 1u) % (ANIM_FRAMES * ANIM_HOLD);
    g.anim_step = g.moving ? (g.anim / ANIM_HOLD) : 0u;

    if (++g.fire_tick >= FIRE_EVERY) {
        g.fire_tick = 0u;
        game_player_fire();
    }

    /* ★ 这一行必须在【用完旧值之后】更新：下一帧的"上一帧有没有在走 / 上一帧朝哪"就是它们。
       重构时漏掉这两个赋值 ⇒ anim 每帧被清零 ⇒ 人物永远站着（踩过）。 */
    g.was_moving = g.moving;
    g.last_dir = g.dir;
}

/*----------------------------------------------------------------------------*/
/*  发一发：从枪口沿 angle(1/64 圈) 出去，速度 = speed + 自机动量            */
static void player_shot(int angle, int speed)
{
    int a  = angle & (ANGLE_STEPS - 1);
    int px = g.x + PW / 2 - BW / 2;
    int py = g.y + PLAYER_MUZ_DY - BH / 2;
    int vx8 = (g_sin_tab[(a + ANGLE_STEPS / 4) & 63] * speed * 8) >> 6;
    int vy8 = (g_sin_tab[a] * speed * 8) >> 6;

    /*  ★ 动量：直接加自机速度（它本来就是 1/8 像素单位）。
     *    跑动中开枪，弹道会朝走的方向偏 —— 这就是"动能"手感。  */
    game_bullet_spawn8(px, py, vx8 + g.vx8, vy8 + g.vy8,
                       MOTION_LINE, g.bullet_type, 0);
}

/*----------------------------------------------------------------------------*/
/*  自动瞄准：枪口朝"最近的敌人"，只打上下左右。
 *  · 背对敌人不开火；场上没敌人不开火
 *  · g.fire_triple（inner_eyes）时一次发三发：正前 + 左右各 45°，速度更慢  */
void game_player_fire(void)
{
    int sx = g.x + PW / 2;                   /* 枪口 = 人物横向中心 */
    int sy = g.y + PLAYER_MUZ_DY;            /*      + 头和身体的交界 */
    int tx, ty, dx, dy, adx, ady, a;
    int fx = 0, fy = 0;                      /* 当前朝向的单位向量 */

    if (g.dir == LEFT)       fx = -1;
    else if (g.dir == RIGHT) fx =  1;
    else if (g.dir == UP)    fy = -1;
    else                     fy =  1;

    if (!game_nearest_enemy(&tx, &ty)) return;   /* 场上没敌人：不放空枪 */

    dx = tx - sx; dy = ty - sy;
    adx = dx < 0 ? -dx : dx;
    ady = dy < 0 ? -dy : dy;

    /*  只打【上下左右】四个方向：横向差得多就横着打，否则竖着打。  */
    if (adx >= ady) a = (dx < 0) ? 32 : 0;
    else            a = (dy < 0) ? 48 : 16;

    /*  背对敌人不开火：把瞄准角换成单位向量，和朝向点积（< 0 就是背后）  */
    if (g_sin_tab[(a + ANGLE_STEPS / 4) & 63] * fx + g_sin_tab[a] * fy < 0) return;

    if (g.fire_triple) {
        /* 三向：正前 + 左右各偏 PLAYER_TRIPLE_SPREAD 格（默认 4 格 = 22.5°），速度 3 */
        player_shot(a - PLAYER_TRIPLE_SPREAD, PLAYER_TRIPLE_SPEED);
        player_shot(a,                        PLAYER_TRIPLE_SPEED);
        player_shot(a + PLAYER_TRIPLE_SPREAD, PLAYER_TRIPLE_SPEED);
    } else {
        player_shot(a, PLAYER_BULLET_SPEED);
    }
}

/*----------------------------------------------------------------------------*/
void game_player_draw(u32 fb, u32 now, int buf)
{
    int step = (int)g.anim_step;
    int hurt    = (g.hurt_until   != 0u && (u32)(g.hurt_until   - now) < HURT_MS);
    int picking = (g.pickup_until != 0u && (u32)(g.pickup_until - now) < PICKUP_MS);
    int ai;
    u32 flip;

    if (g.dir == DOWN)    { ai = down_seq[step]; flip = down_flip[step]; }
    else if (g.dir == UP) { ai = up_seq[step];   flip = up_flip[step]; }
    else {
        ai = side_seq[step];
        flip = (g.dir == LEFT) ? GPU2D_FLIP_X : 0u;
    }

    /* 受伤 / 拾取：换帧并强制不翻面（这两张素材只有朝右的）。 */
    if (hurt)         { ai = ASSET_bigfish_Get_hurt;      flip = 0u; }
    else if (picking) { ai = ASSET_bigfish_Get_accessory; flip = 0u; }

    /*  受伤闪红：用颜色合成 tone=1 插值染色。
     *  ★ 只闪不常亮：按时间交替（120 ms 红 / 120 ms 正常），否则一整秒全红太糊。  */
    /*  重生保护期：整块不画 = 闪烁（脏区照样擦得到，不会留残影）  */
    /*  GAME OVER：玩家不画（但记下位置，好让脏区把上一帧擦掉）  */
    if (g.phase == PHASE_GAMEOVER || game_vital_dead(now)) {
        /* ??????????????????????????? */
        g.old_player[buf].active = 1;
        g.old_player[buf].x = g.x;
        g.old_player[buf].y = g.y;
        return;
    }

#if PLAYER_BLINK_ON_INVULN
    if (game_vital_invuln(now) && ((now / 90u) & 1u)) {
        g.old_player[buf].active = 1;
        g.old_player[buf].x = g.x;
        g.old_player[buf].y = g.y;
        return;
    }
#endif

    if (game_visible(g.x, g.y, PW, PH)) {
        if (hurt && ((now / 120u) & 1u)) {
            CI_gpu2d_combineTintRect(g_assets[ai].base_word, g_assets[ai].stride_word,
                GPU2D_MODE_REPLACE | GPU2D_MODE_SPRITE | GPU2D_MODE_CKEY_EN,
                0x00FF2828u, 180u, 1u,
                fb, (u32)g.x, (u32)g.y, PW, PH);
        } else {
            CI_gpu2d_spriteFlipCK(g_assets[ai].base_word, g_assets[ai].stride_word,
                fb, (u32)g.x, (u32)g.y, PW, PH, flip);
        }
    }

    /*  拾取后把道具【举在头顶】（原来是压在脸上，看着像长在头上）。
     *  水平居中、竖直往上 ITEM_HELD_DY；负坐标会被当成巨大无符号数 ⇒ 先在屏内才画。  */
    if (picking && !hurt) {
        int icon = game_item_icon(g.held_type);
        int ix = g.x + (PW - ITEM_W) / 2;
        int iy = g.y + ITEM_HELD_DY;

        if (game_visible(ix, iy, ITEM_W, ITEM_H))
            CI_gpu2d_spriteAlphaRect(g_assets[icon].base_word, g_assets[icon].stride_word,
                fb, (u32)ix, (u32)iy, ITEM_W, ITEM_H, 0u);
    }

    g.old_player[buf].active = 1;
    g.old_player[buf].x = g.x;
    g.old_player[buf].y = g.y;
}

/*----------------------------------------------------------------------------*/
/*  登记进渲染队列：z = 脚底 y（越靠下越前）。
 *  ★ 每帧都要登记：GAME OVER / 死亡时不画，但 game_player_draw() 里要把
 *    old_player 记下来，下一帧才有位置可擦。 */
static void player_draw_entry(u32 fb, u32 now, int buf, int arg)
{
    (void)arg;
    game_player_draw(fb, now, buf);
}

void game_player_submit(void)
{
    rl_push(ACTOR_Z(g.y + PH), player_draw_entry, 0);
}