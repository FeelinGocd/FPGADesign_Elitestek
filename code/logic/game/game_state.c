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

    lineBuffer_setFullBase(fb, fb);
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
    g.item_order_n = 0;                     /* 拾取顺序也清空 */
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
    g.stats.triple_speed = PLAYER_TRIPLE_SPEED;
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
        if (!game_room_is_spawned(id) && !game_room_is_cleared(id)) {
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
            g.item.active = 1;
            bsp_printf("ROOM: item (kept #%d)\r\n", g.item.type);
        }
        break;

    case ROOM_BOSS:
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
    lineBuffer_setFullBase(FB_C, FB_D);
    if (gpu2d_exclusive_end(cmd0 + 2u * game_floor_cmd_count()) != 0) {
        bsp_printf("RESTART ERROR: timeout\r\n");
        return 0;
    }
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

    /* 等一个帧边界，让显示侧先锁存"全屏"几何，再开始画。 */
    bsp_printf("INIT 3/7: wait display boundary\r\n");
    bsp_uDelay(20000u);

    bsp_printf("INIT 4/7: draw floor into both buffers\r\n");
    for (bi = 0; bi < 2; ++bi) {
        bsp_printf("INIT 5/7: buffer %d\r\n", bi);
        game_draw_floor(fb[bi]);
    }

    bsp_printf("INIT 6/7: wait GPU\r\n");
    lineBuffer_setFullBase(FB_C, FB_D);
    if (gpu2d_exclusive_end(init_cmd0 + 2u * game_floor_cmd_count()) != 0) {
        bsp_printf("INIT ERROR: initial floor timeout\r\n");
        return 0;
    }

    bsp_printf("INIT 7/7: %dx%d ready, display re-enabled\r\n", SCR_W, SCR_H);
    if (!game_present(FB_C)) return 0;
    return 1;
}
