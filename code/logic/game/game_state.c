//==============================================================================
//  game_state.c —— 状态定义 + 时钟 + 初始化 + 上屏
//------------------------------------------------------------------------------
//  干什么      :
//    · g            : 整个 demo 唯一的一份状态（别的模块只读写它）
//    · game_ms()    : 毫秒时钟（CLINT）
//    · game_init()  : 清两块缓冲、铺好地板、开显示（起始房，暂不生成任何内容）
//    · game_flow_enter_room(): 换房后按房间类型布置内容（道具房随机道具 / Boss 房生成并立即开打）
//    · game_finish_gpu() / game_present(): 一帧画完 → FENCE → 等显示真的翻页
//  上一级是谁  : game/game_main.c
//  接口契约    :
//    · game_init() 返回 0 = 失败，调用方直接退出主循环
//    · 上屏节拍靠【显示帧中断】：FENCE 只表示画完，翻页要等下一个帧边界
//  为什么这么做:
//    · 初始化期间显示是关着的（exclusive_begin），此时 frame_done 可能一直不动，
//      所以 FENCE 等的是"帧完成计数"，不能等显示相位。
//  参考        : docs/spec/软件_驱动与API.md §3（时序纪律）
//==============================================================================
#include "game.h"
#include "CI_gpu2d.h"
#include "user_gpu2d.h"
#include "user_lineBuffer.h"
#include "IRQ_frame.h"
#include "bsp.h"

//==================== 唯一的一份状态 ====================
game_t g;

/*  共享 sin 表：sin(2πi/64) × 64 —— 定点，不用浮点库  */
const signed char g_sin_tab[64] = {
       0,    6,   12,   19,   24,   30,   36,   41,   45,   49,   53,   56,   59,   61,   63,   64,
      64,   64,   63,   61,   59,   56,   53,   49,   45,   41,   36,   30,   24,   19,   12,    6,
       0,   -6,  -12,  -19,  -24,  -30,  -36,  -41,  -45,  -49,  -53,  -56,  -59,  -61,  -63,  -64,
     -64,  -64,  -63,  -61,  -59,  -56,  -53,  -49,  -45,  -41,  -36,  -30,  -24,  -19,  -12,   -6
};

/*==============================================================================
 *  相机：显示窗口 = 1920×1080 帧缓冲里的一个 1280×720 窗口
 *    · 普通房世界就是 1280×720/736 ⇒ cam 固定 0
 *    · Boss 房世界 1440×1120 ⇒ cam_x 0..160, cam_y 0..400（房间 1056×800，比一屏窄，
 *    相机在世界里跟随 ⇒ 房间在画面里左右/上下滑动，黑边随之露出）
 *  场景/实体按【世界坐标】画进 fb；HUD 画进 fb + cam（屏幕上位置不变）。
 *  game_present() 把 fb + cam 写给 lineBuffer 的 C/D 基址，窗口就移动了。
 *============================================================================*/
u32 game_view_fb(u32 fb)
{
    return fb + (u32)g.cam_y * 5760u + (u32)g.cam_x * 3u;
}

static int cam_clamp(int v, int hi)
{
    if (v < 0) return 0;
    if (v > hi) return hi;
    return v;
}

/*  ★ 硬件能力自检（2026-10-23）
 *    老 bitstream 的 APB0_GPU_2D 会【丢弃非 16B 对齐的全屏基址】（PWDATA[3:0]==0 那道门），
 *    相机平移的字节偏移就写不进去 ⇒ 现象是：平移时背景按 16 px 台阶跳、
 *    HUD 文字互相重影、右边 ITEM 看着不刷新。
 *    这里写一个非对齐基址再读回：读不回 = 老硬件 ⇒ 相机改成 16 px 量化。  */
int g_cam_quant = 0;

static void lb_detect_unaligned(void)
{
    u32 probe = (u32)FB_C + 12u;

    lineBuffer_setFullBase(probe, probe);
    g_cam_quant = (lineBuffer_getFullBaseC() == probe) ? 0 : 16;
    lineBuffer_setFullBase((u32)FB_C, (u32)FB_D);
    GAME_LOG("LB ALIGN: %s (cam step %d)\r\n",
               g_cam_quant ? "OLD-BITSTREAM" : "OK", g_cam_quant ? g_cam_quant : 1);
}

/*  老硬件才需要：把相机横向对齐到 g_cam_quant 的整数倍（3*16=48 ⇒ 16B 对齐）  */
static void cam_align(void)
{
    if (g_cam_quant > 0)
        g.cam_x -= g.cam_x % g_cam_quant;
}

/*  世界比窗口大才需要相机（普通房 1280×720 = 窗口 ⇒ 永远 0,0；
 *  Boss 房 1440×1120 ⇒ 跟随）。不再看"是不是 Boss 房"。  */
static int cam_follows(void)
{
    return game_world_w() > SCR_W || game_world_h() > SCR_H;
}

/*  换房/复位：直接对准，不做过渡（避免新房间第一帧从旧位置滑进来）  */
void game_camera_snap(void)
{
    if (cam_follows()) {
        int maxx = game_world_w() - SCR_W;
        int maxy = game_world_h() - SCR_H;

        g.cam_x = cam_clamp(g.x + PW / 2 - SCR_W / 2, maxx);
        g.cam_y = cam_clamp(g.y + PH / 2 - SCR_H / 2, maxy);
    } else {
        g.cam_x = 0;
        g.cam_y = 0;
    }
    cam_align();
    /* 不在这里清 HUD 的缓冲相机坐标：同布局换房没有整屏重铺，game_draw 要靠
     * prev != cam 来擦旧 HUD。整屏重铺的路径画完后再调 game_camera_synced()。 */
}

/*  整屏重铺之后调用：告诉 HUD 逻辑"新位置已经画好了，不用再擦旧 HUD"  */
void game_camera_synced(void)
{
    int b;
    for (b = 0; b < GAME_FB_COUNT; ++b) {
        g.hud_cam_x[b] = g.cam_x;
        g.hud_cam_y[b] = g.cam_y;
    }
}

/*  每帧：Boss 房跟随（死区 + 每帧限速）
 *    · 玩家中心落在屏幕中心的 CAM_DEAD_W × CAM_DEAD_H 里 ⇒ 相机【完全不动】
 *      （弹幕游戏里小幅走位不该带着整屏抖）；
 *    · 出了死区才把玩家"推回死区边缘"，每帧最多 CAM_MAX_STEP 像素；
 *    · 最后 clamp 到 [0, 世界 - 窗口]，撞边就是硬停。  */
void game_camera_step(void)
{
    int tx, ty, dx, dy;
    int half, left, right, top, bottom;

    if (!cam_follows()) {
        g.cam_x = 0;
        g.cam_y = 0;
        return;
    }

    tx = g.cam_x;
    ty = g.cam_y;

    half  = (SCR_W - CAM_DEAD_W) / 2;
    left  = g.cam_x + half;
    right = left + CAM_DEAD_W;
    if      (g.x + PW / 2 < left)  tx = g.x + PW / 2 - half;
    else if (g.x + PW / 2 > right) tx = g.x + PW / 2 - half - CAM_DEAD_W;

    half   = (SCR_H - CAM_DEAD_H) / 2;
    top    = g.cam_y + half;
    bottom = top + CAM_DEAD_H;
    if      (g.y + PH / 2 < top)    ty = g.y + PH / 2 - half;
    else if (g.y + PH / 2 > bottom) ty = g.y + PH / 2 - half - CAM_DEAD_H;

    tx = cam_clamp(tx, game_world_w() - SCR_W);
    ty = cam_clamp(ty, game_world_h() - SCR_H);

    dx = tx - g.cam_x;
    dy = ty - g.cam_y;
    if (dx >  CAM_MAX_STEP) dx =  CAM_MAX_STEP;
    if (dx < -CAM_MAX_STEP) dx = -CAM_MAX_STEP;
    if (dy >  CAM_MAX_STEP) dy =  CAM_MAX_STEP;
    if (dy < -CAM_MAX_STEP) dy = -CAM_MAX_STEP;

    g.cam_x += dx;
    g.cam_y += dy;
    cam_align();
}

/*----------------------------------------------------------------------------*/
/*  FENCE 完成是命令侧事件；初始化期间显示关着，frame_done 可能根本不动，
 *  所以这里等"帧完成计数"，并带超时。  */
int game_finish_gpu(void)
{
    u32 before = gpu2d_getFrameDone();

    CI_gpu2d_fence();
    if (gpu2d_waitFrame(before, 3000000u) != 0) {
        GAME_LOG("DEBUG: GPU fence timeout\r\n");
        return 0;
    }
    return 1;
}

/*----------------------------------------------------------------------------*/
/*  把显示基址的两个槽都写成刚画好的那块（相位免疫），再等下一个帧边界。  */
int game_present(u32 fb)
{
    lineBuffer_setFullBase(game_view_fb(fb), game_view_fb(fb));
    return game_wait_display();
}

int game_wait_display_after(u32 before)
{
#if IRQ_FRAME_USE
    u32 wait = 0u;
    while (IRQ_frame_count() == before && wait < 200000u) {
        bsp_uDelay(1u);
        ++wait;
    }
    if (IRQ_frame_count() == before) {
        GAME_LOG("DEBUG: display IRQ timeout\r\n");
        return 0;
    }
#else
    (void)before;
    bsp_uDelay(20000u);
#endif
    return 1;
}

int game_wait_display(void)
{
#if IRQ_FRAME_USE
    return game_wait_display_after(IRQ_frame_count());
#else
    return game_wait_display_after(0u);
#endif
}

/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
/*  纯数据复位（不碰帧缓冲）：初始化和"R 键重开"共用  */
void game_reset_state(void)
{
    int i;

    game_static_draw_reset();
    game_enemy_bullet_limit_reset();

    /* 玩家在初始房正中间（房间系统负责摆位置） */
    game_room_reset();
    game_bombs_reset();
    g.bomb_count = START_BOMBS;             /* 初始房保底给几颗（隐藏门靠它） */
    game_bomb_spawn_pickup((ARENA_X0 + ARENA_X1) / 2 + 24 - BOMB_PICK_W / 2,
                           (ARENA_Y0 + ARENA_Y1) / 2 - BOMB_PICK_H / 2,
                           game_room_start());
    g.dir = DOWN;
    g.moving = 0; g.was_moving = 0; g.last_dir = DOWN;
    g.anim = 0u; g.fire_tick = 0u; g.anim_step = 0u;
    g.vx8 = 0; g.vy8 = 0; g.rx = 0; g.ry = 0;
    g.fire_extra_paths = 0;
    g.test_mode = 0;
    g.item_mask = 0u;                       /* 道具池清空 */
    g.inv_n = 0;                            /* 实例数组清空 */
    ++g.inv_rev;                            /* 池子内容版本变化 */
    g.last_room = -1;                       /* 还没有“上一间房” */
    g.last_door = -1;
    game_vital_reset();                     /* 3 颗心、3 条命 */

    /*  ★ 重开时地砖会整屏重铺 ⇒ 上一帧的字全被盖掉了。
     *    这里把 UI 的"上次画的值"作废，下一帧两块缓冲都会老老实实重画。
     *    （不这样做的话：重开后 GUI 一片空白，要等数值变了才慢慢出现。）  */
    game_hud_invalidate();
    game_vital_invalidate();
    game_item_invalidate();                            /* 初始 3 条命（1UP! 再加一） */
    g.bullet_type = BULLET_A;
    g.held_type = ITEM_CHILI;
    g.stats.move_v8      = PLAYER_MAX_V8;
    g.stats.fire_every   = (int)FIRE_EVERY;
    g.stats.bullet_speed = PLAYER_BULLET_SPEED;
    g.stats.range        = PLAYER_RANGE;
    g.stats.damage       = PLAYER_BASE_DAMAGE;
    g.pickup_until = 0u;
    g.hurt_until = 0u;
    g.toast_until = 0u;
    g.toast_type = ITEM_CHILI;

    g.boss_x = (ARENA_X0 + ARENA_X1) / 2 - BOSS_W / 2;
    g.boss_y = (ARENA_Y0 + ARENA_Y1) / 2 - BOSS_H / 2;
    g.boss_tick = 0u;
    g.boss_phase = BOSS_PHASE_CORE;
    g.stage = 1;                            /* 重开也从第 1 关开始 */
    g.score = 0;                            /* 本局得分清零 */
    g.coins = 0;                            /* 本局金币清零（下一层保留） */
    g.boss_hp = g_boss_def.hp;
    g.boss_gate = g_boss_def.gate1;
    g.boss_locked = 0;
    g.boss_lock_until = 0u;
    g.boss_flash_until = 0u;
    g.boss_wait_until = 0u;
    g.exit_active = 0;
    g.exit_dirty = 0;
    g.exit_room = 0;
    g.exit_x = 0;
    g.exit_y = 0;
    g.boss_blink_cnt = 0;
    g.boss_blink_on = 0;
    g.boss_blink_next = 0u;
    g.boss_blink_cool = 0u;
    g.boss_slow_until = 0u;
    g.boss_slow_next = 0u;
    game_shot_stop(&g.shot);

    /*  开局在起始房：没有道具、没有 Boss。
     *  道具由"进道具房"时随机生成（见 game_flow_enter_room）。  */
    g.item.active = 0;
    g.boss_drop_pending = 0;                /* Boss 饰品掉落状态也清零 */
    g.item.type = ITEM_CHILI;
    g.hidden_item_type = ITEM_CHILI;
    g.item.x = (SCR_W - ITEM_W) / 2;
    g.item.y = (SCR_H - ITEM_H) / 2;
    g.phase = PHASE_EXPLORE;

    g.front = 0;
    g.door_dirty = 0;
    g.cmd_frame = 0;
    g.fps = 0;
    g.fps_epoch = 0u; g.fps_frames = 0u; g.cmd_before = 0u;
    g.probe_epoch = 0u; g.probe_frames = 0u; g.probe_frame_no = 0u;
    g.probe_peak = 0; g.probe_peak_frame = 0; g.probe_peak_cmd = 0;

    game_bullets_clear();                   /* 同时复位 alive 计数 / 空闲槽栈 */
    for (i = 0; i < GAME_FB_COUNT; ++i) {
        game_bullets_clear_history(i);
        g.old_item[i].active = 0;
        g.old_boss[i].active = 0;
        g.old_player[i].active = 0;
        for (int k = 0; k < GAME_TRIPRISM_MAX; ++k)
            g.old_triprism[i][k].active = 0;
    }
}

/*----------------------------------------------------------------------------*/
/*  进入"拾取阶段"：场上放一个道具（屏幕正中），Boss 不出现  */
/*----------------------------------------------------------------------------*/
/*  命尽 → GAME OVER 画面：冻结玩法、清弹幕、定时回第 1 关  */
void game_enter_gameover(u32 now)
{
    g.phase          = PHASE_GAMEOVER;
    g.gameover_until = now + GAMEOVER_MS;

    game_bullets_clear();

    /*  GAME OVER 期间玩法冻结；5 秒后由 game_update 调 game_restart() 重开  */
    game_shot_stop(&g.shot);            /* Boss 停火 */
    GAME_LOG("GAME OVER: stage %d (auto restart in %d ms)\r\n",
               g.stage, (int)GAMEOVER_MS);
}

/*----------------------------------------------------------------------------*/
/*  固定布局落地：石头/箱子/网/怪按 game_layout.c 里设计好的格子摆。
 *  want_mobs = 0 时只摆静物（这间房已经刷过怪了）。
 *  ★ 同格重复摆由 spawn_at 自己挡掉（幂等），所以重进房间/死亡回退都安全。  */
static void spawn_layout_objects(const room_layout_t *L, int want_mobs,
                                 int *n_mob, int *n_web, int *n_stone)
{
    int i;

    for (i = 0; i < (int)L->n_objs; ++i) {
        const obj_place_t *o = &L->objs[i];
        int x, y;
        game_layout_obj_cell(L, o, game_room_flip(), &x, &y);
        x *= TILE;
        y *= TILE;

        /*  蓝图里这个对象已经打碎了 ⇒ 不再生成  */
        if (game_room_cell_gone((int)game_room_id(), (int)o->x, (int)o->y)) continue;


        switch (o->kind) {
        case OBJ_SLIME:
        case OBJ_BUG:
        case OBJ_AIMER:
            if (want_mobs && game_mobs_spawn_at(x, y, o->kind)) ++*n_mob;
            break;
        case OBJ_WEB:
            /*  网没有实例：位置就在蓝图里，渲染/减速都直接查表，这里不用生成  */
            ++*n_web;
            break;
        case OBJ_STONE:
        case OBJ_BOX:
        case OBJ_CRATE:
            /*  ★ 石头/箱子没有实体：位置就在布局里，画和碰撞都直接查布局。
             *    这里只统计个数，给上面那行日志用。  */
            ++*n_stone;
            break;
        default:
            break;
        }
    }
}

/*----------------------------------------------------------------------------*/
/*  落点兜底：设计器允许把石头/箱子摆到任意格子（map1 就把板条箱摆在房间正中）。
 *  落点一旦压在障碍上，玩家会卡在里面出不来（石头是硬的，那个方向一直被挡）。
 *  这里以落点为中心一圈一圈往外找，取最近一个能放下身体的格子，最多 4 圈（128 px）。
 *  ★ 必须在【摆完对象之后】调用，否则石头还没生成，查了也是白查。  */
static int pick_free_spot(int x, int y)
{
    return !game_stone_overlap(x + PB_OFF_X, y + PB_OFF_Y, PB_W, PB_H);
}

static void player_place_safe(void)
{
    int x0 = g.x, y0 = g.y;
    int r, dx, dy;

    if (pick_free_spot(x0, y0)) return;

    for (r = 1; r <= 4; ++r) {
        for (dy = -r; dy <= r; ++dy) {
            for (dx = -r; dx <= r; ++dx) {
                int x, y;

                /*  只看第 r 这一圈：更里面的圈上一轮已经查过  */
                if (dx > -r && dx < r && dy > -r && dy < r) continue;

                x = x0 + dx * TILE;
                y = y0 + dy * TILE;
                if (x < ARENA_X0 - PB_OFF_X || x > ARENA_X1 - PB_OFF_X - PB_W) continue;
                if (y < ARENA_Y0 - PB_OFF_Y || y > ARENA_Y1 - PB_OFF_Y - PB_H) continue;
                if (!pick_free_spot(x, y)) continue;

                g.x = x;
                g.y = y;
                GAME_LOG("ROOM: spawn blocked, moved to (%d,%d)\r\n", x, y);
                return;
            }
        }
    }
}

/*----------------------------------------------------------------------------*/
/*  换房之后：按【房间类型 + 固定布局】布置内容。
 *  由 game_room_commit() 在"新房间已经显示出来"之后调用 ——
 *  Boss 房的 Boss 就是在这里生成的。
 *  优先级：布局里写了的对象 > 类型兜底的随机刷（没有固定布局的房间保持老行为）。  */
void game_flow_enter_room(void)
{
    int type = game_room_type();
    int id = game_room_id();
    int respawn = game_room_take_respawn(id);
    const room_kind_t   *k = game_kind(type);
    const room_layout_t *L = game_layout(game_room_layout());
    int n_mob = 0, n_web = 0, n_stone = 0;
    int want_mobs;

    /*  换房：小怪按房间重建；金币 / 网 / 石头都带 room 持久化，不在这里清  */
    game_mobs_reset();

    /*  默认：这间房没有道具；Boss 房保留 Boss 当前状态（Boss 只生成一次）。
     *  ★ Boss 击破掉的饰品如果还没捡，重进 Boss 房要恢复，不能在这里清掉。  */
    g.item.active = (type == ROOM_BOSS && g.boss_drop_pending) ? 1 : 0;
    if (type != ROOM_BOSS) {
        g.boss_locked = 0;
        g.boss_blink_cnt = 0;
        g.boss_blink_on = 0;
    }
    game_shot_stop(&g.shot);
    g.phase = (k->flags & RK_FIGHT) ? PHASE_FIGHT : PHASE_EXPLORE;

    /*  小怪只在"这间房还没刷过"时生成：布局里有就按布局摆，没有就按类型兜底  */
    want_mobs = !(k->flags & RK_NO_MOB) &&
                (respawn || (!game_room_is_spawned(id) && !game_room_is_cleared(id)));

    if (L) spawn_layout_objects(L, want_mobs, &n_mob, &n_web, &n_stone);

    switch (type) {
    case ROOM_ITEM:
        /*  单槽道具：进道具房就以这间的道具为准；上一间 Boss 没捡的饰品作废  */
        g.boss_drop_pending = 0;
        if (game_room_is_used(id)) {
            /*  已经拿过：什么都不放  */
        } else if (!game_room_is_spawned(id)) {
            /*  ★ 道具房：这一轮第一次进来才随机挑一个，放在【房间中心】 */
            game_item_spawn_random((ARENA_X0 + ARENA_X1) / 2 - ITEM_W / 2,
                                   (ARENA_Y0 + ARENA_Y1) / 2 - ITEM_H / 2);
            game_room_mark_spawned(id);
        } else {
            g.item.active = 1;              /*  之前生成过、还没拿：原样恢复，不重掷  */
        }
        break;

    case ROOM_HIDDEN: {
        int cx = (ARENA_X0 + ARENA_X1) / 2;
        int cy = (ARENA_Y0 + ARENA_Y1) / 2;

        g.boss_drop_pending = 0;
        if (game_room_is_used(id)) break;
        if (!game_room_is_spawned(id)) {
            g.hidden_item_type = (unsigned char)game_item_spawn_high(
                cx - ITEM_W / 2, cy - ITEM_H / 2);
            game_bomb_spawn_pickup(cx - 2 * TILE - BOMB_PICK_W / 2,
                                   cy + TILE - BOMB_PICK_H / 2, id);
            game_bomb_spawn_pickup(cx + 2 * TILE - BOMB_PICK_W / 2,
                                   cy + TILE - BOMB_PICK_H / 2, id);
            game_room_mark_spawned(id);
        } else {
            g.item.type = g.hidden_item_type;
            g.item.x = (short)(cx - ITEM_W / 2);
            g.item.y = (short)(cy - ITEM_H / 2);
            g.item.active = 1;
        }
        break;
    }

    case ROOM_BOSS:
        /*  死亡卡回退：Boss 还活着才重置；Boss 已死时保持房间原样。  */
        if (respawn && g.boss_hp > 0) game_room_clear_spawned(id);
        if (!game_room_is_spawned(id)) {
            game_boss_spawn();              /* 满血到场中，先等 1 s 再出招 */
            game_room_mark_spawned(id);
        } else if (g.boss_hp > 0) {
            /*  只生成一次：重进不重置血量/位置，接着打（但先等 1 s）  */
            g.phase = PHASE_FIGHT;
            g.boss_wait_until = game_ms() + BOSS_WAIT_MS;
            game_shot_init(&g.shot, g_boss_def.moves, 1, BULLET_URANUS);
            game_boss_resume();
        } else {
            g.phase = PHASE_EXPLORE;        /* 已击破：exit 在上面等玩家 */
        }
        break;

    case ROOM_SHOP:
        game_shop_enter();                  /* 首次进商店：随机挑一个在售商品 */
        break;

    default:                                /* ROOM_START / ROOM_EMPTY */
        break;
    }

    /*  ★ 布局是权威的：房间里的东西只来自布局，不再随机加料。
     *    石头/网连实体都没有（画和碰撞都查布局），所以这里只处理小怪。  */
    if (want_mobs) {
        if (!L && n_mob == 0) {
            game_mobs_spawn(2);
            game_mobs_spawn_bug(1);
        }
        game_room_mark_spawned(id);
    }

    GAME_LOG("ROOM: %s layout=%d mob=%d web=%d stone=%d\r\n",
               k->name, game_room_layout(), n_mob, n_web, n_stone);

    /*  对象都摆完了：最后确认玩家没站在石头/箱子里（map1 的中心板条箱就靠这条兜底）  */
    player_place_safe();
}

/*----------------------------------------------------------------------------*/
/*  Boss 第一次生成：满血 / 场地正中，先等 1 s 再出招  */
void game_boss_spawn(void)
{
    g.boss_x = (ARENA_X0 + ARENA_X1) / 2 - BOSS_W / 2;
    g.boss_y = (ARENA_Y0 + ARENA_Y1) / 2 - BOSS_H / 2;
    g.boss_lock_until = 0u;
    g.boss_flash_until = 0u;
    g.boss_blink_cnt = 0;
    g.boss_blink_on = 0;
    g.boss_blink_next = 0u;
    g.boss_blink_cool = 0u;
    g.boss_slow_until = 0u;
    g.boss_slow_next = 0u;
    g.boss_wait_until = game_ms() + BOSS_WAIT_MS;
    g.exit_active = 0;
    g.exit_dirty = 0;

    for (int b = 0; b < GAME_FB_COUNT; ++b) g.old_boss[b].active = 0;

    game_start_fight();                     /* phase = FIGHT + 招式表初始化 */
}

/*----------------------------------------------------------------------------*/
/*  Boss 开打：切战斗阶段 + 初始化招式表（第一次生成 / 重进续战都用这套）  */
void game_start_fight(void)
{
    g.phase = PHASE_FIGHT;

    game_shot_init(&g.shot, g_boss_def.moves, 1, BULLET_URANUS);

    g.boss_hp = g_boss_def.hp;
    g.boss_gate = g_boss_def.gate1;
#if GAME_CAPACITY_AUDIT == 2
    /* 直接进入选定的真实攻击阶段，不跨越锁血线，
     * 也不在测量前触发召唤或清除过场波次。 */
#if GAME_CAPACITY_PHASE == 1
    g.boss_hp = g_boss_def.gate1;
    g.boss_gate = g_boss_def.gate2;
#elif GAME_CAPACITY_PHASE == 2
    g.boss_hp = g_boss_def.gate2;
    g.boss_gate = 0;
#endif
#endif
    g.boss_locked = 0;
    g.boss_tick = 0u;
    game_boss_resume();
    GAME_LOG("FIGHT: stage %d\r\n", g.stage);
}

/*----------------------------------------------------------------------------*/
/*  重开一局：数据复位 + 两块缓冲重新铺地板（旧精灵自然被覆盖掉）  */
int game_restart(void)
{
    static const u32 fb[GAME_FB_COUNT] = { FB_C, FB_D
#if GAME_TRIPLE_BUFFER
        , FB_THIRD
#endif
    };
    u32 cmd0, background_cmds = 0u;
    int bi;

    GAME_LOG("RESTART: hold R released, new game\r\n");

    game_reset_state();

    cmd0 = gpu2d_getCmdDone();
    gpu2d_exclusive_begin();
    /* Reset regenerated the connection graph. Refresh the shared terrain's
     * doors before any later dirty restoration can copy its previous skin
     * or a wall from the old game over the freshly drawn doors. */
#if GAME_BG_COPY
    background_cmds = game_background_refresh_doors();
#endif
    for (bi = 0; bi < GAME_FB_COUNT; ++bi) game_draw_floor(fb[bi]);
    lineBuffer_setFullBase(game_view_fb(FB_C), game_view_fb(FB_D));
    if (gpu2d_exclusive_end(cmd0 + background_cmds + (u32)GAME_FB_COUNT * game_floor_cmd_count()) != 0) {
        GAME_LOG("RESTART ERROR: timeout\r\n");
        return 0;
    }
    game_camera_synced();
    return game_present(FB_C);
}

/*----------------------------------------------------------------------------*/
int game_init(void)
{
    static const u32 fb[GAME_FB_COUNT] = { FB_C, FB_D
#if GAME_TRIPLE_BUFFER
        , FB_THIRD
#endif
    };
    u32 init_cmd0;
    int bi;

    game_reset_state();

    /*  硬规则自检：起点/道具/Boss 唯一、特殊房不直连/空间不相邻、连通  */
    if (game_room_validate() != 0) {
        GAME_LOG("ROOM ERROR: room generation rule broken "
                   "(count / adjacent / connectivity)\r\n");
        return 0;
    }

    GAME_LOG("DEBUG: accessory at (%d,%d), bullet=A\r\n", g.item.x, g.item.y);

    game_perf_init();

    GAME_LOG("INIT 1/7: disable display\r\n");
    init_cmd0 = gpu2d_getCmdDone();
    gpu2d_exclusive_begin();

    GAME_LOG("INIT 2/7: full-screen mode\r\n");
    lineBuffer_setSplit(0);

    /*  ★ 硬件能力自检：老 bitstream 的 APB 会丢弃非 16B 对齐的全屏基址。
     *    必须在画第一帧之前测；新 bitstream 返回 0（相机可 1 px 步进），
     *    老 bitstream 返回 16（cam_x 量化到 16 px，避免 HUD 搬家错位）。
     *    以前这个函数写了却没人调用，g_cam_quant 永远是 0。  */
    if (!g_cam_quant) lb_detect_unaligned();

    /* 等一个帧边界，让显示侧先锁存"全屏"几何，再开始画。 */
    GAME_LOG("INIT 3/7: wait display boundary\r\n");
    bsp_uDelay(20000u);

    GAME_LOG("INIT 4/7: draw floor into %d buffers\r\n", GAME_FB_COUNT);
    for (bi = 0; bi < GAME_FB_COUNT; ++bi) {
        GAME_LOG("INIT 5/7: buffer %d\r\n", bi);
        game_draw_floor(fb[bi]);
    }

    GAME_LOG("INIT 6/7: wait GPU\r\n");
    lineBuffer_setFullBase(game_view_fb(FB_C), game_view_fb(FB_D));
    if (gpu2d_exclusive_end(init_cmd0 + (u32)GAME_FB_COUNT * game_floor_cmd_count()) != 0) {
        GAME_LOG("INIT ERROR: initial floor timeout\r\n");
        return 0;
    }
#if GAME_BG_COPY
    {
        u32 bg0 = gpu2d_getCmdDone();
        u32 bg_cmds = game_background_warmup();
        u32 bg_target = bg0 + bg_cmds;
        if (gpu2d_waitCmdDone(bg_target, 10000000u) != 0) {
            GAME_LOG("INIT ERROR: background build timeout base=%u cmds=%u target=%u done=%u\r\n",
                       bg0, bg_cmds, bg_target, gpu2d_getCmdDone());
            return 0;
        }
    }
#endif

    GAME_LOG("INIT 7/7: %dx%d ready, display re-enabled\r\n", SCR_W, SCR_H);
    game_camera_synced();
    if (!game_present(FB_C)) return 0;
    return 1;
}
