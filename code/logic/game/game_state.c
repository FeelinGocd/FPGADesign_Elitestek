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

/*----------------------------------------------------------------------------*/
u32 game_ms(void)
{
    return (u32)(clint_getTime(BSP_CLINT) / (BSP_CLINT_HZ / 1000u));
}

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
    bsp_printf("LB ALIGN: %s (cam step %d)\r\n",
               g_cam_quant ? "OLD-BITSTREAM" : "OK", g_cam_quant ? g_cam_quant : 1);
}

/*  老硬件才需要：把相机横向对齐到 g_cam_quant 的整数倍（3*16=48 ⇒ 16B 对齐）  */
static void cam_align(void)
{
    if (g_cam_quant > 0)
        g.cam_x -= g.cam_x % g_cam_quant;
}

/*  换房/复位：直接对准，不做过渡（避免新房间第一帧从旧位置滑进来）  */
void game_camera_snap(void)
{
    if (game_world_is_boss()) {
        int maxx = game_world_w() - SCR_W;
        int maxy = game_world_h() - SCR_H;

        g.cam_x = cam_clamp(g.x + PW / 2 - SCR_W / 2, maxx);
        g.cam_y = cam_clamp(g.y + PH / 2 - SCR_H / 2, maxy);
    } else {
        g.cam_x = 0;
        g.cam_y = 0;
    }
    cam_align();
    /* 不在这里清 cam_prev：同布局换房没有整屏重铺，game_draw 要靠
     * prev != cam 来擦旧 HUD。整屏重铺的路径画完后再调 game_camera_synced()。 */
}

/*  整屏重铺之后调用：告诉 HUD 逻辑"新位置已经画好了，不用再擦旧 HUD"  */
void game_camera_synced(void)
{
    g.cam_prev_x = g.cam_x;
    g.cam_prev_y = g.cam_y;
    g.hud_cam_x[0] = g.hud_cam_x[1] = g.cam_x;
    g.hud_cam_y[0] = g.hud_cam_y[1] = g.cam_y;
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

    if (!game_world_is_boss()) {
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
        bsp_printf("DEBUG: GPU fence timeout\r\n");
        return 0;
    }
    return 1;
}

/*----------------------------------------------------------------------------*/
/*  把显示基址的两个槽都写成刚画好的那块（相位免疫），再等下一个帧边界。  */
int game_present(u32 fb)
{
    u32 before, wait = 0u;

    lineBuffer_setFullBase(game_view_fb(fb), game_view_fb(fb));
#if IRQ_FRAME_USE
    before = IRQ_frame_count();
    while (IRQ_frame_count() == before && wait < 200000u) {
        bsp_uDelay(1u);
        ++wait;
    }
    if (IRQ_frame_count() == before) {
        bsp_printf("DEBUG: display IRQ timeout\r\n");
        return 0;
    }
#else
    (void)before; (void)wait;
    bsp_uDelay(20000u);
#endif
    return 1;
}

/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
/*  纯数据复位（不碰帧缓冲）：初始化和"R 键重开"共用  */
void game_reset_state(void)
{
    int i;

    /* 玩家在初始房正中间（房间系统负责摆位置） */
    game_room_reset();
    g.dir = DOWN;
    g.moving = 0; g.was_moving = 0; g.last_dir = DOWN;
    g.anim = 0u; g.fire_tick = 0u; g.anim_step = 0u;
    g.vx8 = 0; g.vy8 = 0; g.rx = 0; g.ry = 0;
    g.fire_triple = 0;
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
    g.stats.damage       = 1;
    g.pickup_until = 0u;
    g.hurt_until = 0u;
    g.toast_until = 0u;
    g.toast_type = ITEM_CHILI;

    g.boss_x = (ARENA_X0 + ARENA_X1) / 2 - BOSS_W / 2;
    g.boss_y = (ARENA_Y0 + ARENA_Y1) / 2 - BOSS_H / 2;
    g.boss_pattern = 0;
    g.boss_tick = 0u;
    g.stage = 1;                            /* 重开也从第 1 关开始 */
    g.score = 0;                            /* 本局得分清零 */
    g.coins = 0;                            /* 本局金币清零（下一层保留） */
    g.boss_hp = g_boss_def.hp;
    g.boss_gate = g_boss_def.gate1;
    g.boss_locked = 0;
    g.boss_lock_until = 0u;
    g.boss_flash_until = 0u;
    g.boss_dead_until = 0u;
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
    game_shot_stop(&g.shot);

    /*  开局在起始房：没有道具、没有 Boss。
     *  道具由"进道具房"时随机生成（见 game_flow_enter_room）。  */
    g.item.active = 0;
    g.boss_drop_pending = 0;                /* Boss 饰品掉落状态也清零 */
    g.item.type = ITEM_CHILI;
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
    for (i = 0; i < N_BULLETS; ++i) {
        g.old_bullets[0][i].active = 0;
        g.old_bullets[1][i].active = 0;
    }
    g.old_item[0].active = 0;
    g.old_item[1].active = 0;
    g.old_boss[0].active = 0;
    g.old_boss[1].active = 0;
    g.old_player[0].active = 0;
    g.old_player[1].active = 0;
    g.front = 0;
}

/*----------------------------------------------------------------------------*/
/*  进入"拾取阶段"：场上放一个道具（屏幕正中），Boss 不出现  */
/*----------------------------------------------------------------------------*/
/*  命尽 → GAME OVER 画面：冻结玩法、清弹幕、定时回第 1 关  */
void game_enter_gameover(u32 now)
{
    g.phase          = PHASE_GAMEOVER;
    g.gameover_until = now + GAMEOVER_MS;
    g.gameover_stage = g.stage;

    game_bullets_clear();

    /*  GAME OVER 期间玩法冻结；5 秒后由 game_update 调 game_restart() 重开  */
    game_shot_stop(&g.shot);            /* Boss 停火 */
    bsp_printf("GAME OVER: stage %d (auto restart in %d ms)\r\n",
               g.stage, (int)GAMEOVER_MS);
}

/*----------------------------------------------------------------------------*/
/*  换房之后：按房间类型布置内容。
 *  由 game_room_commit() 在"新房间已经显示出来"之后调用 ——
 *  Boss 房的 Boss 就是在这里生成的。  */
void game_flow_enter_room(void)
{
    int type = game_room_type();
    int id = game_room_id();
    int respawn = game_room_take_respawn(id);

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

    switch (type) {
    case ROOM_EMPTY:
        g.phase = PHASE_EXPLORE;
        if (respawn || (!game_room_is_spawned(id) && !game_room_is_cleared(id))) {
            game_mobs_spawn(2);             /* 普通房：2 只 slime */
            game_mobs_spawn_bug(1);         /*        + 1 只 bug */
            game_room_mark_spawned(id);
        }
        game_web_spawn(2 + (int)(game_ms() % 2u));   /* 普通房 2~3 张网 */
        bsp_printf("ROOM: empty\r\n");
        break;

    case ROOM_ITEM:
        g.phase = PHASE_EXPLORE;
        /*  单槽道具：进道具房就以这间的道具为准；上一间 Boss 没捡的饰品作废  */
        g.boss_drop_pending = 0;
        if (game_room_is_used(id)) {
            if (respawn) {
                game_mobs_spawn(2);
                game_mobs_spawn_bug(1);
                game_room_mark_spawned(id);
            }
            bsp_printf("ROOM: item (already taken)\r\n");
        } else if (!game_room_is_spawned(id)) {
            /*  ★ 道具房：这一轮第一次进来才随机挑一个，放在【房间中心】 */
            game_item_spawn_random((ARENA_X0 + ARENA_X1) / 2 - ITEM_W / 2,
                                   (ARENA_Y0 + ARENA_Y1) / 2 - ITEM_H / 2);
            game_mobs_spawn(2);             /* 道具房：2 只 slime */
            game_mobs_spawn_bug(1);         /*        + 1 只 bug */
            game_room_mark_spawned(id);
            bsp_printf("ROOM: item (random #%d)\r\n", g.item.type);
        } else {
            /*  之前生成过、还没拿：原样恢复，不重掷  */
            if (respawn) {
                game_mobs_spawn(2);
                game_mobs_spawn_bug(1);
                game_room_mark_spawned(id);
            }
            g.item.active = 1;
            bsp_printf("ROOM: item (kept #%d)\r\n", g.item.type);
        }
        break;

    case ROOM_BOSS:
        /*  死亡卡回退：Boss 还活着才重置；Boss 已死时保持房间原样。  */
        if (respawn && g.boss_hp > 0) game_room_clear_spawned(id);
        if (!game_room_is_spawned(game_room_id())) {
            game_boss_spawn();              /* 满血到场中，先等 1 s 再出招 */
            game_room_mark_spawned(game_room_id());
            bsp_printf("ROOM: boss\r\n");
        } else {
            /*  只生成一次：重进不重置血量/位置，接着打（但先等 1 s）  */
            if (g.boss_hp > 0) {
                g.phase = PHASE_FIGHT;
                g.boss_wait_until = game_ms() + BOSS_WAIT_MS;
                game_shot_init(&g.shot, g_boss_def.moves, 1, BULLET_URANUS);
                game_shot_begin(&g.shot, 0);
            } else {
                g.phase = PHASE_EXPLORE;    /* 已击破：exit 在上面等玩家 */
            }
            bsp_printf("ROOM: boss (resume, hp=%d)\r\n", g.boss_hp);
        }
        break;

    case ROOM_SHOP:
        g.phase = PHASE_EXPLORE;
        game_shop_enter();                  /* 首次进商店：随机挑一个在售道具 */
        bsp_printf("ROOM: shop (heart=5, item=15)\r\n");
        break;

    default:                            /* ROOM_START */
        g.phase = PHASE_EXPLORE;
        game_web_spawn(2 + (int)(game_ms() % 2u));   /* 起点房也是普通房 */
        bsp_printf("ROOM: start\r\n");
        break;
    }

    /*  商店是特殊房：不刷 mob / web / stone；其它房间增加石头，
     *  Boss 房再稍微多一点点（普通 5~7，Boss 7~8）。  */
    if (type != ROOM_SHOP) {
        int n = (type == ROOM_BOSS) ? (7 + (int)(game_ms() % 2u))
                                    : (5 + (int)(game_ms() % 3u));
        game_stones_spawn(n);
    }
}

/*----------------------------------------------------------------------------*/
/*  Boss 第一次生成：满血 / 场地正中，先等 1 s 再出招  */
void game_boss_spawn(void)
{
    g.boss_x = (ARENA_X0 + ARENA_X1) / 2 - BOSS_W / 2;
    g.boss_y = (ARENA_Y0 + ARENA_Y1) / 2 - BOSS_H / 2;
    g.boss_hp = g_boss_def.hp;
    g.boss_gate = g_boss_def.gate1;
    g.boss_locked = 0;
    g.boss_lock_until = 0u;
    g.boss_flash_until = 0u;
    g.boss_dead_until = 0u;
    g.boss_blink_cnt = 0;
    g.boss_blink_on = 0;
    g.boss_blink_next = 0u;
    g.boss_blink_cool = 0u;
    g.boss_tick = 0u;
    g.boss_wait_until = game_ms() + BOSS_WAIT_MS;
    g.exit_active = 0;
    g.exit_dirty = 0;
    game_shot_stop(&g.shot);

    g.old_boss[0].active = 0;               /* 不要在别的房间擦一个"上一场的 Boss" */
    g.old_boss[1].active = 0;

    game_start_fight();                     /* phase = FIGHT + 招式表初始化 */
}

/*----------------------------------------------------------------------------*/
/*  Boss 开打：切战斗阶段 + 初始化招式表（第一次生成 / 重进续战都用这套）  */
void game_start_fight(void)
{
    g.phase = PHASE_FIGHT;

    game_shot_init(&g.shot, g_boss_def.moves, 1, BULLET_URANUS);
    game_shot_begin(&g.shot, 0);

    g.boss_hp = g_boss_def.hp;
    g.boss_gate = g_boss_def.gate1;
    g.boss_locked = 0;
    g.boss_tick = 0u;
    g.boss_dead_until = 0u;
    bsp_printf("FIGHT: stage %d\r\n", g.stage);
}

/*----------------------------------------------------------------------------*/
/*  重开一局：数据复位 + 两块缓冲重新铺地板（旧精灵自然被覆盖掉）  */
int game_restart(void)
{
    static const u32 fb[2] = { FB_C, FB_D };
    u32 cmd0;
    int bi;

    bsp_printf("RESTART: hold R released, new game\r\n");

    game_reset_state();

    cmd0 = gpu2d_getCmdDone();
    gpu2d_exclusive_begin();
    for (bi = 0; bi < 2; ++bi) game_draw_floor(fb[bi]);
    lineBuffer_setFullBase(game_view_fb(FB_C), game_view_fb(FB_D));
    if (gpu2d_exclusive_end(cmd0 + 2u * game_floor_cmd_count()) != 0) {
        bsp_printf("RESTART ERROR: timeout\r\n");
        return 0;
    }
    game_camera_synced();
    return game_present(FB_C);
}

/*----------------------------------------------------------------------------*/
int game_init(void)
{
    static const u32 fb[2] = { FB_C, FB_D };
    u32 init_cmd0;
    int bi;

    game_reset_state();

    /*  硬规则自检：起点/道具/Boss 唯一、特殊房不直连/空间不相邻、连通  */
    if (game_room_validate() != 0) {
        bsp_printf("ROOM ERROR: room generation rule broken "
                   "(count / adjacent / connectivity)\r\n");
        return 0;
    }

    bsp_printf("DEBUG: accessory at (%d,%d), bullet=A\r\n", g.item.x, g.item.y);

    game_perf_init();

    bsp_printf("INIT 1/7: disable display\r\n");
    init_cmd0 = gpu2d_getCmdDone();
    gpu2d_exclusive_begin();

    bsp_printf("INIT 2/7: full-screen mode\r\n");
    lineBuffer_setSplit(0);

    /*  ★ 硬件能力自检：老 bitstream 的 APB 会丢弃非 16B 对齐的全屏基址。
     *    必须在画第一帧之前测；新 bitstream 返回 0（相机可 1 px 步进），
     *    老 bitstream 返回 16（cam_x 量化到 16 px，避免 HUD 搬家错位）。
     *    以前这个函数写了却没人调用，g_cam_quant 永远是 0。  */
    if (!g_cam_quant) lb_detect_unaligned();

    /* 等一个帧边界，让显示侧先锁存"全屏"几何，再开始画。 */
    bsp_printf("INIT 3/7: wait display boundary\r\n");
    bsp_uDelay(20000u);

    bsp_printf("INIT 4/7: draw floor into both buffers\r\n");
    for (bi = 0; bi < 2; ++bi) {
        bsp_printf("INIT 5/7: buffer %d\r\n", bi);
        game_draw_floor(fb[bi]);
    }

    bsp_printf("INIT 6/7: wait GPU\r\n");
    lineBuffer_setFullBase(game_view_fb(FB_C), game_view_fb(FB_D));
    if (gpu2d_exclusive_end(init_cmd0 + 2u * game_floor_cmd_count()) != 0) {
        bsp_printf("INIT ERROR: initial floor timeout\r\n");
        return 0;
    }

    bsp_printf("INIT 7/7: %dx%d ready, display re-enabled\r\n", SCR_W, SCR_H);
    game_camera_synced();
    if (!game_present(FB_C)) return 0;
    return 1;
}
