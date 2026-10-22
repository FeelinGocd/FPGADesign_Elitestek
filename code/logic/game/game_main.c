//==============================================================================
//  game_main.c —— 主循环：一帧四步
//------------------------------------------------------------------------------
//  干什么      :
//      frame_begin(now)          清脏表、记命令数基线、算 FPS
//      game_update(keys, now)    角色 → Boss → 子弹（只改模型，不画）
//      game_draw(now, buf)       元素各自 push 进渲染队列（带 z）→ 排序后下发
//      frame_end(now, buf)       交帧 + 统计 + 每秒探针（+ 压测推进）
//  上一级是谁  : src/main.c（DEMO_RUN_SEL == 2 时调 game_demo_run()）
//  接口契约    :
//    · 只画后台那块（front ^ 1），永远不碰正在显示的那块
//    · 画完必须 FENCE + 等真帧边界，否则软件跑得比 60 Hz 快就会自己错位
//    · 绘制顺序 = z 序：场景(0) → 实体(1000+底边 y) → 护盾(3000) → HUD(5000+) → GAME OVER(6000)
//  为什么这么做:
//    · 硬件没有 z 缓冲 ⇒ 后发的命令盖前面的（画家算法）
//    · 每个元素自己带 z（render_list.c 统一排序）⇒ 加元素不用改排序逻辑，
//      实体按"底边 y"排就得到俯视视角的前后遮挡（越靠下越近）
//    · 每块缓冲各记一份"上次画在哪"⇒ 双缓冲怎么交替都不会擦错地方
//    · 压测（PERF_TEST_MODE）用运行时 if 而不是 #if：少一层预处理噪声，
//      而且这段代码永远参与编译，不会"只在某个模式才编到、平时有错也发现不了"
//  参考        : docs/spec/软件_驱动与API.md §6.3
//==============================================================================
#include "game.h"
#include "CI_gpu2d.h"
#include "user_gpu2d.h"
#include "user_lineBuffer.h"
#include "bsp.h"

static const u32 g_fb[2] = { FB_C, FB_D };

static unsigned char g_dirty[GRID_H][GRID_W];   /* 一帧的脏区表（格子坐标） */
static int  g_alive;                            /* 本帧在场子弹数 */
static int  g_hud_need;                         /* 本帧要不要重画 HUD 文字 */
static int  g_toast_on;                         /* 本帧要不要画拾取字幕 */
static int  g_toast_fade;                       /* 同上（拾取字幕） */
static int  g_bossbar_fade;                     /* 血条消失后还要擦几帧（它不在面板行里） */


/*----------------------------------------------------------------------------*/
/*  ① 帧首  */
static void frame_begin(u32 now)
{
    int x, y;

    for (y = 0; y < GRID_H; ++y)
        for (x = 0; x < GRID_W; ++x)
            g_dirty[y][x] = 0u;

    g.cmd_before = gpu2d_getCmdDone();

    if (g.fps_epoch == 0u) g.fps_epoch = now;
    ++g.fps_frames;
    if ((u32)(now - g.fps_epoch) >= 1000u) {
        g.fps = (int)((g.fps_frames * 1000u) / (now - g.fps_epoch));
        g.fps_frames = 0u;
        g.fps_epoch = now;
    }
}

/*----------------------------------------------------------------------------*/
/*  ② 更新模型  */
static void game_update(u32 keys, u32 now)
{
    if (game_perf_frozen()) return;     /* 压测 mode 1：冻结玩法，只跑绘制 */

    /*  串口手柄按一下 T：切换"无敌"（调试用）  */
    if (game_take_debug_toggle()) {
        game_debug_invuln = !game_debug_invuln;
        bsp_printf("DEBUG: invincible %s (key T)\r\n",
                   game_debug_invuln ? "ON" : "OFF");
    }

    game_vital_step(now);               /* 死亡动画 / 补满 / 命数 */
    if (game_vital_over()) {            /* 命数耗尽：进 GAME OVER 画面（不再直接重开） */
        game_vital_clear_over();
        game_enter_gameover(now);
        return;
    }

    /*  ★ GAME OVER 期间【不冻结玩法】：世界照常走，只是玩家不画、不挨打。
     *    冻结的话两块缓冲会各停一格，交替显示就成了抽搐。  */
    if (g.phase == PHASE_GAMEOVER && (int)(now - g.gameover_until) >= 0) {
        game_restart();
        return;
    }
    /*  ★ GAME OVER 期间【玩家逻辑停】（不移动/不开火），但世界照常跑：
     *    Boss 继续动、弹幕继续飞。玩家不画，所以不会留下残影。  */
    if (g.phase != PHASE_GAMEOVER && !game_vital_dead(now))
        game_player_step(keys, now);
    if (g.phase != PHASE_GAMEOVER)
        game_room_step();
    game_boss_step(now);
    game_bullets_step(now);
    game_mobs_step(now);                /* slime：动画 / 追玩家 / 清房奖励 */
    game_coins_step(now);               /* 金币拾取：肚子碰到 +1 */
}

/*----------------------------------------------------------------------------*/
/*  精灵的脏区有没有压到 HUD 那几格（压到了就得重画 HUD，否则字会被擦掉）  */
static int hud_cells_touched(unsigned char dirty[GRID_H][GRID_W])
{
    int x, y;

    for (y = 0; y < HUD_CELLS_Y; ++y)
        for (x = 0; x < HUD_CELLS_X; ++x)
            if (dirty[y][x]) return 1;
    return 0;
}

/*==============================================================================
 *  渲染层号系统（★ 硬件没有 z 缓冲，层序 = 命令下发顺序，后画的盖前面的）
 *------------------------------------------------------------------------------
 *  每个元素提交时自己带一个 z（见 game.h 的 Z_* / ACTOR_Z）：
 *    · 静态层：场景 0 / 护盾 3000 / HUD 5000+ / GAME OVER 6000
 *    · 实体层：Z_ACTOR + 底边 y ⇒ 越靠下越近，自动盖住上面的东西
 *  绘制 = 全部 rl_push 进队列 → 按 (z, 提交顺序) 排序 → 依次下发命令。
 *  加一种新元素只要在自己的 submit() 里 push 一句，不用再动这里的排序逻辑。
 *============================================================================*/

/*----------------------------------------------------------------------------*/
/*  场景层：把这一帧的脏区恢复成地砖。
 *  ★ 必须最下面：旧精灵就是在这里被擦掉的（先整块擦、再按 z 重画）。  */
static void scene_restore(u32 fb, u32 now, int buf, int arg)
{
    (void)now; (void)buf; (void)arg;
    if (game_perf_keep_bg()) game_restore_dirty(fb, g_dirty);
}

/*----------------------------------------------------------------------------*/
/*  HUD：每一块单独一个 z（game.h 的 Z_HUD_*），从下往上依次登记。
 *  ★ 这里只管"画"，擦除 / 标脏已经在帧首的脏区里做完了。  */
static void hud_toast(u32 fb, u32 now, int buf, int arg)
{
    (void)now; (void)buf; (void)arg;
    game_hud_toast(fb, game_item_desc(g.toast_type));
}

static void hud_bossbar(u32 fb, u32 now, int buf, int arg)
{
    (void)now; (void)buf; (void)arg;
    game_hud_boss_bar(fb, g.boss_hp, g_boss_def.hp);
}

static void exit_draw(u32 fb, u32 now, int buf, int arg)
{
    (void)now; (void)buf; (void)arg;
    game_boss_draw_exit(fb);
}

static void hud_panel(u32 fb, u32 now, int buf, int arg)
{
    (void)now; (void)arg;
    game_hud_frame(fb, buf, g.fps, g.cmd_frame, g_alive, g.coins, game_hud_need_full);
}

static void hud_vital(u32 fb, u32 now, int buf, int arg)
{
    (void)buf; (void)arg;
    game_vital_draw(fb, now, game_hud_need_full);
}

static void hud_pool(u32 fb, u32 now, int buf, int arg)
{
    (void)now; (void)arg;
    game_item_draw_pool(fb, buf, game_hud_need_full);
}

static void hud_minimap(u32 fb, u32 now, int buf, int arg)
{
    (void)now; (void)buf; (void)arg;
    game_minimap_draw(fb);
}

static void hud_gameover(u32 fb, u32 now, int buf, int arg)
{
    int left = (int)(g.gameover_until - now);
    int secs = (left + 999) / 1000;

    (void)buf; (void)arg;
    if (secs < 0) secs = 0;
    game_hud_gameover(fb, g.score, secs);
}

/*  这一帧要画哪些 HUD 元素；登记顺序 = z 从下到上  */
static void hud_submit(void)
{
    if (g_toast_on)
        rl_push(Z_HUD_TOAST, hud_toast, 0);
    if (g.phase == PHASE_FIGHT)
        rl_push(Z_HUD_BOSSBAR, hud_bossbar, 0);     /* 没开战就不显示血条 */
    if (g_hud_need)
        rl_push(Z_HUD_PANEL, hud_panel, 0);
    rl_push(Z_HUD_VITAL, hud_vital, 0);             /* 心 + X 命数 + 死亡飘心 */
    rl_push(Z_HUD_MINIMAP, hud_minimap, 0);         /* 右上角小地图（道具池已下移 64） */
    rl_push(Z_HUD_POOL, hud_pool, 0);               /* 右侧道具池 */

    /*  ★ GAME OVER 窗口放最上面（Z_OVERLAY）：不能再被飘心 / 道具池压出洞  */
    if (g.phase == PHASE_GAMEOVER)
        rl_push(Z_OVERLAY, hud_gameover, 0);
}

/*----------------------------------------------------------------------------*/
/*  ③ 绘制  */
static void game_draw(u32 now, int buf)
{
    u32 fb = g_fb[buf];
    int i;

    /*  ★ 换房后的两帧：把 4 扇门的两格标脏（两块缓冲各重画一次）。
     *    地图没变时 game_room_commit 不再整屏重铺，门开/关的变化就靠这里更新。  */
    if (g.door_dirty > 0) {
        game_mark_doors_dirty(g_dirty);
        --g.door_dirty;
    }

    /*  exit：触发/换关后擦两帧；不在 Boss 房时也要把旧 exit 擦掉  */
    if (g.exit_dirty > 0) {
        game_mark_dirty(g_dirty, g.exit_x, g.exit_y, 32, 32);
        --g.exit_dirty;
    } else if (g.exit_active && game_room_id() != g.exit_room) {
        game_mark_dirty(g_dirty, g.exit_x, g.exit_y, 32, 32);
    }

    /* 收集脏区：把"这块缓冲里上次画在哪"都并进格子 */
    if (g.old_boss[buf].active)
        game_mark_dirty(g_dirty, g.old_boss[buf].x, g.old_boss[buf].y, BOSS_W, BOSS_H);
    if (g.old_player[buf].active)
        /* 往上多标 ITEM_HELD_DY 那一截：拾取时道具是举在头顶的 */
        game_mark_dirty(g_dirty, g.old_player[buf].x,
                        g.old_player[buf].y + ITEM_HELD_DY, PW, PH - ITEM_HELD_DY);
    if (g.old_item[buf].active)
        game_mark_dirty(g_dirty, g.old_item[buf].x,
                        g.old_item[buf].y + ITEM_ICON_DY - ITEM_FLOAT_AMP - 2,
                        ITEM_W, ITEM_H - ITEM_ICON_DY + ITEM_FLOAT_AMP + 2);
    for (i = 0; i < N_BULLETS; ++i)
        if (g.old_bullets[buf][i].active)
            game_mark_dirty(g_dirty, g.old_bullets[buf][i].x, g.old_bullets[buf][i].y, BW, BH);
    for (i = 0; i < N_MOBS; ++i)
        if (g.old_mob[buf][i].active)
            game_mark_dirty(g_dirty, g.old_mob[buf][i].x, g.old_mob[buf][i].y - 8, 32, 40);
    for (i = 0; i < N_COINS; ++i)
        if (g.old_coin[buf][i].active)
            game_mark_dirty(g_dirty, g.old_coin[buf][i].x, g.old_coin[buf][i].y, 24, 24);
    for (i = 0; i < N_WEBS; ++i)
        if (g.old_web[buf][i].active)
            game_mark_dirty(g_dirty, g.old_web[buf][i].x, g.old_web[buf][i].y, 32, 32);
    for (i = 0; i < N_STONES; ++i)
        if (g.old_stone[buf][i].active)
            game_mark_dirty(g_dirty, g.old_stone[buf][i].x, g.old_stone[buf][i].y, 32, 32);

    /* HUD 的文字：只有"数字变了"或"有精灵压在它那块"时才重画并标脏。
       平时一条命令都不花 —— 之前每帧都标 32 个格子，把帧率压下去了（移动发卡）。 */
    g_alive = game_bullets_alive();

    /* Keep HUD overlap separate from HUD value changes. Overlap needs a full
     * panel redraw; a changed value only needs its own row. */
    {
        int hud_overlap = hud_cells_touched(g_dirty);
        int hud_changed = game_hud_prepare(g_dirty, buf,
                                           g.fps, g.cmd_frame, g_alive, g.coins);

        game_vital_prepare(g_dirty, buf);
        game_item_prepare(g_dirty, buf);

        game_hud_need_full = 0;   /* 遮挡不再整面板重画：prepare 已按行处理 */
        g_hud_need = hud_overlap || hud_changed;
        if (g_hud_need) {
            game_hud_mark_dirty(g_dirty);
            /*  面板重画会连第 4 行（心）一起盖掉；这里强制补画心/命数  */
            game_vital_invalidate();
        }
    }

    /*  ★ 临时文字（拾取字幕 / WIN）的擦除规则：
     *    显示期间每帧标脏；【结束后还要再标 3 帧】。
     *    为什么？双缓冲下每帧只画一块，一帧只擦得掉一块 ——
     *    只擦一帧的话，另一块缓冲里的字会留下来，两块交替显示就成了"疯狂闪"。  */

    /* 拾取字幕 */
    {
        const char *t = game_item_desc(g.toast_type);
        int len = 0, w, x0;

        while (t[len]) ++len;
        w  = len * 16 + 16;
        x0 = (SCR_W - len * 16) / 2 - 8;

        g_toast_on = (g.toast_until != 0u && (int)(now - g.toast_until) < 0);
        if (g.toast_until != 0u && !g_toast_on) {
            g.toast_until = 0u;
            g_toast_fade = 3;                   /* 之后 3 帧继续擦 */
        }
        if (g_toast_on) {
            game_mark_dirty(g_dirty, x0, HUD_TOAST_Y - 8, w, 32);
            /* 字幕的脏区会按【整瓦片】擦，连带血条所在那行 ⇒ 一起标脏，血条随后重画 */
            game_mark_dirty(g_dirty, HUD_HP_X, HUD_HP_Y, HUD_HP_W, HUD_HP_H);
        } else if (g_toast_fade > 0) {
            game_mark_dirty(g_dirty, x0, HUD_TOAST_Y - 8, w, 32);
            --g_toast_fade;
        }
    }

    /*  Boss 血条：离开 Boss 房 / 击破后要擦掉 ——
     *   它是画在面板外面（y = 0..44）的，不在面板行的脏区里；
     *   和临时文字一样，消失后再擦 3 帧（双缓冲两块都要擦到）。  */
    {
        int bar_on = (g.phase == PHASE_FIGHT);

        if (bar_on) {
            g_bossbar_fade = 3;
        } else if (g_bossbar_fade > 0) {
            game_mark_dirty(g_dirty, HUD_HP_X, 0, HUD_HP_W,
                            HUD_HP_Y + HUD_HP_H);
            --g_bossbar_fade;
        }
    }

    /*  左下角生命显示：和 HUD 一样是"浮在场景上的东西"，每帧标脏让它重画  */
    /*  ★ 帧首：把"这块缓冲里已经过期"的 HUD 行 / 血量行 / 道具池标脏，
     *    让下面的脏区恢复统一擦掉 —— 擦除只在帧首做，绘制层不再贴地砖，
     *    否则会把走到面板上的人物一起盖掉。  */
    game_vital_mark_dirty(g_dirty);

    game_bullets_sync(buf);             /* 脏区用：记下这一帧每颗弹画在哪 */

    /*  ★ 先提交、再统一排序绘制：z 小的在下、z 大的在上（同 z 保持提交顺序）。
     *    实体的 z 在各自的 submit() 里 = Z_ACTOR + 底边 y ⇒ 自然的透视遮挡。  */
    rl_begin();
    rl_push(Z_SCENE, scene_restore, 0);   /* 脏区恢复（擦旧精灵）必须最先 */
    if (g.exit_active && game_room_id() == g.exit_room)
        rl_push(Z_SCENE + 5, exit_draw, 0);  /* exit 在地板上、人物下面 */
    game_player_submit();                 /* 同深度时玩家在实体里最靠后 ⇒ 先提交 */
    game_bullets_submit();
    game_boss_submit();                   /* 本体 + 锁血护盾（Z_SHIELD） */
    game_mobs_submit();
    game_coins_submit();
    game_web_submit();
    game_stones_submit();
    game_item_submit();
    hud_submit();                         /* toast → 血条 → ... → 道具池 → GAME OVER */

    rl_draw_all(fb, now, buf);
}

/*----------------------------------------------------------------------------*/
/*  ④ 帧尾：交帧 → 统计 → 探针  */
static int frame_end(u32 now, int buf)
{
    if (!game_finish_gpu()) return 0;
    if (!game_present(g_fb[buf])) return 0;

    g.cmd_frame = (int)(gpu2d_getCmdDone() - g.cmd_before);

    /* 每秒打一行：峰值子弹数 / 峰值那帧的命令数 / 帧率 */
    ++g.probe_frames;
    ++g.probe_frame_no;
    if (g.probe_epoch == 0u) g.probe_epoch = now;
    if (g_alive > g.probe_peak) {
        g.probe_peak = g_alive;
        g.probe_peak_frame = (int)g.probe_frame_no;
        g.probe_peak_cmd = g.cmd_frame;
    }
    if ((u32)(now - g.probe_epoch) >= 1000u) {
        u32 dt = now - g.probe_epoch;
        /*  ★ 只能用 %d / %x / %X / %s / %c：这个 BSP 的 bsp_printf 不认 %u，
         *    遇到 %u 会一路扫到字符串末尾，把后面的 "\r\n" 一起吃掉（日志就不换行了）。  */
        bsp_printf("PROBE 1s: peak bullets=%d at frame=%d, fps=%d, frames=%d, cmd/frame=%d, px=%d py=%d hp=%d life=%d, z-drop=%d\r\n",
                   g.probe_peak, g.probe_peak_frame,
                   (int)((g.probe_frames * 1000u) / (dt ? dt : 1u)),
                   (int)g.probe_frames, g.probe_peak_cmd,
                   g.x, g.y, game_vital_hp(), game_vital_lives(),
                   (int)rl_dropped());
        g.probe_epoch = now;
        g.probe_frames = 0u;
        g.probe_peak = 0;
        g.probe_peak_frame = (int)g.probe_frame_no;
        g.probe_peak_cmd = 0;
    }

    return game_perf_tick(now);
}

/*----------------------------------------------------------------------------*/
void game_demo_run(void)
{
    bsp_printf("\r\nGAME: KEY0=left KEY1=right KEY2=down KEY3=up, auto fire\r\n");
    bsp_printf("DEBUG CMDS: debug0 = invincible ON, debug1 = OFF (type + Enter)\r\n");
    if (PERF_TEST_MODE == 1)
        bsp_printf("PERF TEST: fixed model, bullet levels 16/32/64/96/128/192/256\r\n");
    else if (PERF_TEST_MODE == 2)
        bsp_printf("PERF TEST: 256 moving bullets, background restore ON\r\n");
    else if (PERF_TEST_MODE == 3)
        bsp_printf("PERF TEST: 256 moving bullets, background restore OFF\r\n");

    if (!game_init()) return;

    for (;;) {
        u32 now  = game_ms();
        u32 keys = game_keys_read();
        int back = g.front ^ 1;

        /* R 键按一下就重开（输入层锁存过，快按也不会丢） */
        if (game_take_restart()) {
            if (!game_restart()) return;
            continue;                       /* 这一帧已重开，别再用旧状态画 */
        }

        /*  走进门底下那 32 格 ⇒ 换房间：独占把两块缓冲都重铺一遍  */
        if (game_room_pending()) {
            if (!game_room_commit()) return;
            continue;
        }

        frame_begin(now);
        game_update(keys, now);
        game_room_step();   /* 检测是否走进门 → 置 s_pending */
        game_draw(now, back);
        if (!frame_end(now, back)) return;

        g.front = back;                 /* 这块画完并交帧了，下一帧画另一块 */
    }
}
