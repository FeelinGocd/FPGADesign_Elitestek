//==============================================================================
//  game_main.c —— 主循环：一帧四步
//------------------------------------------------------------------------------
//  干什么      :
//      frame_begin(now)          清脏表、记命令数基线、算 FPS
//      game_update(keys, now)    角色 → Boss → 子弹（只改模型，不画）
//      game_draw(now, buf)       按层表画：场景 → 角色 → 护盾 → HUD
//      frame_end(now, buf)       交帧 + 统计 + 每秒探针（+ 压测推进）
//  上一级是谁  : src/main.c（DEMO_RUN_SEL == 2 时调 game_demo_run()）
//  接口契约    :
//    · 只画后台那块（front ^ 1），永远不碰正在显示的那块
//    · 画完必须 FENCE + 等真帧边界，否则软件跑得比 60 Hz 快就会自己错位
//    · 绘制顺序 = z 序（层表，从底到顶）：场景 → 角色 → 护盾 → HUD
//  为什么这么做:
//    · 硬件没有 z 缓冲 ⇒ 后发的命令盖前面的
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
static int  g_cd_on;                            /* 本帧要不要画倒计时 */
static int  g_cd_fade;                          /* 结束后还要擦几帧（双缓冲：两块都要擦到） */
static int  g_toast_fade;                       /* 同上（拾取字幕） */
static int  g_win_fade;
static int  g_over_fade;                        /* GAME OVER 字迹擦除计数 */                         /* 同上（WIN 横幅） */


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

    /* 倒计时结束 → Boss 现身 */
    if (g.phase == PHASE_COUNTDOWN && (int)(now - g.countdown_until) >= 0)
        game_start_fight();

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
    game_boss_step(now);
    game_bullets_step(now);
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
/*==============================================================================
 *  层数系统（★ 硬件没有 z 缓冲，层序 = 命令下发顺序，后画的盖前面的）
 *------------------------------------------------------------------------------
 *  每个可画元素挂一个 z（层数）：z 小的先画（在下面），z 大的盖在上面。
 *  渲染时按 z 从小到大【排序】后依次下发命令 —— 改前后关系只改数字。
 *
 *  · 静态元素（场景/护盾/HUD）用固定层数
 *  · 实体（人物/Boss/弹幕/道具）的层数 = 基础层 + 底边 y，
 *    所以"越靠下越在前面"也是按层数排出来的（在实体层内部完成）
 *============================================================================*/
typedef void (*draw_fn)(u32 fb, u32 now, int buf);
/*  层数编号：数字就是优先级，小的在下面  */
enum {
    Z_SCENE   =    0,    /* 地板 / 墙 / 门 */
    Z_ACTOR   =  400,    /* 实体：人物 / Boss / 弹幕 / 道具（内部按 y 深度再排） */
    Z_SHIELD  = 1000,    /* 护盾 */
    Z_HUD     = 2000     /* HUD / 血条 */
};

/*----------------------------------------------------------------------------*/
/*  ★ 角色区：内部的遮挡关系按【深度 = 底边 y】实时决定（越靠下越在前）。
 *    画三趟：先画在自机后面的 → 画自机 → 再画在自机前面的。
 *    这样一颗弹在人物上方就被人物盖住、跑到下方就盖住人物 —— 就像一棵树。  */
static void layer_actors(u32 fb, u32 now, int buf)
{
    const int pd = g.y + PH;            /* 自机的深度（脚底） */
    const int bd = g.boss_y + BOSS_H;   /* Boss 的深度 */

    /* ① 自机后面 */
    if (bd < pd) game_boss_draw(fb, now, buf);
    if (g.item.active && g.item.y + ITEM_H < pd) game_item_draw(fb, now, buf);
    game_bullets_draw(fb, 0, pd);

    /* ② 自机 */
    game_player_draw(fb, now, buf);

    /* ③ 自机前面 */
    game_bullets_draw(fb, pd, 0x7fffffff);
    if (bd >= pd) game_boss_draw(fb, now, buf);
    if (g.item.active && g.item.y + ITEM_H >= pd) game_item_draw(fb, now, buf);
}

static void layer_shield(u32 fb, u32 now, int buf)
{
    if (g.boss_locked) game_boss_draw_shield(fb, now);      /* 罩住 Boss 和弹幕 */
}

static void layer_hud(u32 fb, u32 now, int buf)
{
    /*  拾取字幕先画，血条后画 —— 血条永远在最上面（字幕的脏区会连带擦到
     *  它所在的瓦片行，顺序反了就会出现"字幕把血条盖掉"的画面）。  */
    if (g_toast_on) game_hud_toast(fb, game_item_desc(g.toast_type));
    if (g.phase == PHASE_FIGHT)
        game_hud_boss_bar(fb, g.boss_hp, g_boss_def.hp);     /* 没开战就不显示血条 */
    if (g_cd_on) {
        int left = (int)(g.countdown_until - now);
        int n = (left + 999) / 1000;

        game_hud_countdown(fb, n);
    }
    if (g.phase == PHASE_FIGHT && g.boss_hp <= 0) game_hud_banner(fb, "WIN");
    if (g_hud_need) game_hud_frame(fb, buf, g.fps, g.cmd_frame, g_alive, game_hud_need_full);
    game_vital_draw(fb, now, game_hud_need_full);   /* 心 + X 额外命数 + 死亡飘心 */
    game_item_draw_pool(fb, buf, game_hud_need_full);    /* 右侧道具池 */

    /*  ★ GAME OVER 窗口放在【最后】画：它是"最上面的一层"，
     *    放前面会被飘心 / 道具池 / HUD 之后画的东西压出洞来（看着像抽搐）。  */
    if (g.phase == PHASE_GAMEOVER) {
        int left = (int)(g.gameover_until - now);
        int secs = (left + 999) / 1000;

        if (secs < 0) secs = 0;
        game_hud_gameover(fb, g.score, secs);
    }
}

static void layer_scene(u32 fb, u32 now, int buf)
{
    (void)now;
    (void)buf;
    if (game_perf_keep_bg()) game_restore_dirty(fb, g_dirty);
}
/*  层表：每条 = 【层数 z】+【这一层怎么画】。表里顺序随便写，
 *  渲染时统一按 z 从小到大排。  */
typedef struct {
    int     z;
    draw_fn fn;
} layer_t;
static const layer_t g_layers[] = {
    { Z_SCENE,  layer_scene  },   /* 0     地板 / 墙 / 门 */
    { Z_ACTOR,  layer_actors },   /* 400   人物 / Boss / 弹幕 / 道具 */
    { Z_SHIELD, layer_shield },   /* 1000  护盾 */
    { Z_HUD,    layer_hud    },   /* 2000  HUD */
};
#define LAYER_COUNT ((int)(sizeof(g_layers) / sizeof(g_layers[0])))

/*----------------------------------------------------------------------------*/
/*  ③ 绘制  */
static void game_draw(u32 now, int buf)
{
    u32 fb = g_fb[buf];
    int i;

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

    /* HUD 的文字：只有"数字变了"或"有精灵压在它那块"时才重画并标脏。
       平时一条命令都不花 —— 之前每帧都标 32 个格子，把帧率压下去了（移动发卡）。 */
    g_alive = game_bullets_alive();

    /* Keep HUD overlap separate from HUD value changes. Overlap needs a full
     * panel redraw; a changed value only needs its own row. */
    {
        int hud_overlap = hud_cells_touched(g_dirty);
        int hud_changed = game_hud_prepare(g_dirty, buf,
                                           g.fps, g.cmd_frame, g_alive);

        game_vital_prepare(g_dirty, buf);
        game_item_prepare(g_dirty, buf);

        game_hud_need_full = 0;   /* 遮挡不再整面板重画：prepare 已按行处理 */
        g_hud_need = hud_overlap || hud_changed;
        if (g_hud_need) game_hud_mark_dirty(g_dirty);
    }

    /*  ★ 临时文字（拾取字幕 / 倒计时 / WIN）的擦除规则：
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

    /* 倒计时 */
    g_cd_on = (g.phase == PHASE_COUNTDOWN);
    if (g_cd_on) {
        g_cd_fade = 3;
        game_mark_dirty(g_dirty, HUD_CD_X, HUD_CD_Y, HUD_CD_W, HUD_CD_H);
    } else if (g_cd_fade > 0) {
        game_mark_dirty(g_dirty, HUD_CD_X, HUD_CD_Y, HUD_CD_W, HUD_CD_H);
        --g_cd_fade;
    }

    /* WIN 横幅（击破后显示，同样要擦） */
    if (g.phase == PHASE_FIGHT && g.boss_hp <= 0) {
        g_win_fade = 3;
        game_mark_dirty(g_dirty, 608, 112, 64, 40);
    } else if (g_win_fade > 0) {
        game_mark_dirty(g_dirty, 608, 112, 64, 40);
        --g_win_fade;
    }

    /*  左下角生命显示：和 HUD 一样是"浮在场景上的东西"，每帧标脏让它重画  */
    /*  ★ 帧首：把"这块缓冲里已经过期"的 HUD 行 / 血量行 / 道具池标脏，
     *    让下面的脏区恢复统一擦掉 —— 擦除只在帧首做，绘制层不再贴地砖，
     *    否则会把走到面板上的人物一起盖掉。  */
    game_vital_mark_dirty(g_dirty);

    game_bullets_sync(buf);             /* 脏区用：记下这一帧每颗弹画在哪 */

    /*  ★ 按层数排序后画：z 小的在下、z 大的在上。
    层表只有几条，每帧选择排序一次即可（写乱顺序也能自愈）。  */
    {
        int order[LAYER_COUNT];
        int i, j;

        for (i = 0; i < LAYER_COUNT; ++i) order[i] = i;
        for (i = 0; i + 1 < LAYER_COUNT; ++i)
            for (j = i + 1; j < LAYER_COUNT; ++j)
                if (g_layers[order[j]].z < g_layers[order[i]].z) {
                    int t = order[i]; order[i] = order[j]; order[j] = t;
                }

        for (i = 0; i < LAYER_COUNT; ++i)
            g_layers[order[i]].fn(fb, now, buf);
    }
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
        bsp_printf("PROBE 1s: peak bullets=%d at frame=%d, fps=%d, frames=%d, cmd/frame=%d, px=%d py=%d hp=%d life=%d\r\n",
                   g.probe_peak, g.probe_peak_frame,
                   (int)((g.probe_frames * 1000u) / (dt ? dt : 1u)),
                   (int)g.probe_frames, g.probe_peak_cmd,
                   g.x, g.y, game_vital_hp(), game_vital_lives());
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
