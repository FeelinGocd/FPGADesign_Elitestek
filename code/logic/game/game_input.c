//==============================================================================
//  game_input.c —— 输入来源：板载按键 + 串口"虚拟手柄"
//------------------------------------------------------------------------------
//  干什么      :
//    · game_keys_read() 返回移动/功能掩码，game_aim_read() 返回瞄准方向掩码
//    · 优先用串口：收到 PC 工具发来的帧就用它
//    · 超过 PAD_TIMEOUT 没收到就自动回落到板载四个按键（拔了线也能玩）
//  上一级是谁  : game/game_main.c（每帧读一次）
//  接口契约    :
//    · 新协议为三字节 {0xA6, move, aim}：move 的低 4 位控制 WASD 移动，
//      bit4..7 保留 R/E/Y/1 功能；aim 的低 4 位控制小键盘射击方向；
//    · 兼容旧两字节 {0xA5, mask}：方向同时用于移动和瞄准；
//    · 只在"有新帧"时刷新，掩码在 PAD_TIMEOUT 内一直有效 ⇒ PC 工具只需在变化时发
//  为什么这么做:
//    · 双摇杆式操作需要区分移动和瞄准；串口是现成的双向通道，不用改硬件。
//  参考        : tools/GamePad/game_pad.py（PC 端）、docs/spec/软件_驱动与API.md §8.3
//==============================================================================
#include "game.h"
#include "bsp.h"        /* BSP_UART_TERMINAL / uart_readOccupancy / uart_read */
#include "user_key.h"

#define PAD_MAGIC    0xA5u      /* 旧协议：后随 1 个移动/瞄准共用掩码 */
#define PAD_MAGIC_V2 0xA6u      /* 新协议：后随 move、aim 两个掩码 */
#define PAD_TIMEOUT  500u       /* 这么久没收到就走板载按键 */

/*  ---- 串口【文本命令】----
 *  和"虚拟手柄"的二进制帧共存：帧头 0xA5/0xA6 不是可打印字符，所以
 *  可打印 ASCII 一律当命令收集，遇到回车执行。
 *      Y      → 在玩家脚下生成一个道具
 *      E      → 在玩家脚下放置炸弹
 *      T      → 切换测试模式（无敌 / GAME_TEST_DAMAGE 伤害）
 */
#define CMD_MAX 16
static char s_cmd[CMD_MAX];
static int  s_cmd_n;

/*  P 键生成道具请求 / B 放置炸弹请求  */
static int s_spawn_req;
static int s_bomb_req;
#if GAME_BOSS_SHORTCUT
static int s_boss_req;
#endif
#if GAME_BOSS_DEBUG && GAME_PROFILE
static int s_stats_req;
#endif

static void cmd_exec(void)
{
    int single = (s_cmd_n == 1);
    int command = (unsigned char)s_cmd[0] | 0x20;
    s_cmd[s_cmd_n] = 0;
    s_cmd_n = 0;

    if (single && command == 'y') {
        s_spawn_req = 1;
        bsp_printf("DEBUG: spawn queued\r\n");
    } else if (single && command == 'e') {
        s_bomb_req = 1;
        bsp_printf("DEBUG: bomb queued\r\n");
    } else if (single && command == 's') {
        bsp_printf("LOAD fps=%d enemy=%d total=%d cmds=%d drops=%d\r\n",
                   g.fps, game_enemy_bullets_alive(), game_bullets_alive(),
                   g.cmd_frame, (int)rl_dropped());
    } else if (single && command == 't') {
        g.test_mode = !g.test_mode;
        bsp_printf("TEST MODE: %s invincible=%d damage=%d hp=%d lives=%d boss_hp=%d boss_phase=%d mobs=%d\r\n",
                   g.test_mode ? "ON" : "OFF", g.test_mode,
                   game_player_hit_damage(BOSS_HIT_DMG), game_vital_hp(),
                   game_vital_lives(), g.boss_hp, g.boss_phase+1,game_mobs_alive());
#if GAME_BOSS_SHORTCUT
    } else if (single && command == '1') {
        s_boss_req = 1;
#endif
#if GAME_BOSS_DEBUG && GAME_PROFILE
    } else if (s_cmd[0] == 's' && s_cmd[1] == 't' &&
               s_cmd[2] == 'a' && s_cmd[3] == 't' && s_cmd[4] == 0) {
        s_stats_req = 1;
#endif
    } else if (s_cmd[0] != 0) {
        bsp_printf("DEBUG: unknown cmd '%s'\r\n", s_cmd);
    }
}

static int s_pad_wait;          /* 新/旧协议后续还需接收的字节数 */
static int s_pad_v2;            /* 当前帧是否为新协议 */
static u32 s_pad_move;          /* 新协议中已收到的移动/功能掩码 */
static u32 s_keys;              /* 最近一次收到的移动/功能掩码 */
static u32 s_aim_keys = KEY_DOWN; /* 最近一次收到的瞄准方向 */
static u32 s_until;             /* 这个掩码的有效期（毫秒时间戳） */
static int s_restart_req;       /* R 键锁存：一次按下的"按下+松开"可能在同一帧里被吃掉 */
static u32 s_prev_mask;         /* 上一帧的掩码：用来做 P/B 的上升沿 */

/*----------------------------------------------------------------------------*/
/*  取一次"按了 P"：取完就清 —— 主循环里在脚下生成一个道具  */
int game_take_spawn_item(void)
{
    int r = s_spawn_req;

    s_spawn_req = 0;
    return r;
}

int game_take_bomb(void)
{
    int r = s_bomb_req;

    s_bomb_req = 0;
    return r;
}

#if GAME_BOSS_SHORTCUT
int game_take_boss_jump(void)
{
    int r = s_boss_req;
    s_boss_req = 0;
    return r;
}
#endif
#if GAME_BOSS_DEBUG && GAME_PROFILE
int game_take_debug_stats(void)
{
    int r = s_stats_req;
    s_stats_req = 0;
    return r;
}
#endif

/*----------------------------------------------------------------------------*/
static void pad_accept(u32 move, u32 aim)
{
    s_keys = move & 0xFFu;
    s_aim_keys = aim & 15u;

    /* ★ 锁存：快速点一下时，"按下"和"松开"两帧可能落在同一帧里，
       照搬 s_keys 会把 R 覆盖掉 ⇒ 单独记一个请求位，等游戏来取。 */
    if ((s_keys & KEY_RESTART) && !(s_prev_mask & KEY_RESTART))
        s_restart_req = 1;

    /*  ★ P 用【上升沿】：一直按住只生成一次，松开再按才生成下一次  */
    if ((s_keys & KEY_SPAWN) && !(s_prev_mask & KEY_SPAWN))
        s_spawn_req = 1;
    if ((s_keys & KEY_BOMB) && !(s_prev_mask & KEY_BOMB))
        s_bomb_req = 1;
#if GAME_BOSS_SHORTCUT
    if ((s_keys & KEY_BOSS_JUMP) && !(s_prev_mask & KEY_BOSS_JUMP))
        s_boss_req = 1;
#endif
    s_prev_mask = s_keys;
    s_until = game_ms() + PAD_TIMEOUT;
}

static int pad_live(void)
{
    return (int)(game_ms() - s_until) < 0;
}

/* 主循环据此选择射击策略：串口手柄按瞄准键射击，板载按键沿用自动索敌规则。 */
int game_input_is_serial(void)
{
    return pad_live();
}

/*----------------------------------------------------------------------------*/
u32 game_keys_read(void)
{
    int c;

    /* 把串口里积压的字节一次吃完（不阻塞：只在有数据时才读） */
    while (uart_readOccupancy(BSP_UART_TERMINAL) > 0) {
        c = (int)(uart_read(BSP_UART_TERMINAL) & 0xFF);

        /* 掩码字节可能是任意值，必须在读取帧头后才能按二进制数据处理。 */
        if (s_pad_wait > 0) {
            if (s_pad_wait == 2) {
                s_pad_move = (u32)c & 0xFFu;
                s_pad_wait = 1;
            } else {
                if (s_pad_v2) pad_accept(s_pad_move, (u32)c);
                else          pad_accept((u32)c, (u32)c);
                s_pad_wait = 0;
            }
            continue;
        }

        if (c == (int)PAD_MAGIC_V2) {
            s_pad_v2 = 1;
            s_pad_wait = 2;
        } else if (c == (int)PAD_MAGIC) {
            s_pad_v2 = 0;
            s_pad_wait = 1;
        } else if (c == '\r' || c == '\n') {
            uart_write(BSP_UART_TERMINAL, '\r');
            uart_write(BSP_UART_TERMINAL, '\n');
            cmd_exec();                     /* 回车 = 执行一条文本命令 */
        } else if (c >= 0x20 && c < 0x7F) {
            /*  回显：让操作者立刻看出"板子到底收到没收到" */
            uart_write(BSP_UART_TERMINAL, (char)c);
            if (s_cmd_n < CMD_MAX - 1) s_cmd[s_cmd_n++] = (char)c;
        }
    }

    /* 串口还在"保鲜期"内就用串口，否则回落到板载按键 */
    if (pad_live()) return s_keys;
    return key_getData() & 15u;             /* 板载按键没有 R（只能从串口来） */
}

/*----------------------------------------------------------------------------*/
/* 取当前瞄准方向。新旧协议都会更新它；无串口帧时板载按键同时充当移动和瞄准。 */
u32 game_aim_read(void)
{
    if (pad_live()) return s_aim_keys;
    return key_getData() & 15u;
}

/*----------------------------------------------------------------------------*/
/*  取一次"重开请求"（取完就清）—— 按一下 R 就重开，不用按住  */
int game_take_restart(void)
{
    int r = s_restart_req;

    s_restart_req = 0;
    return r;
}
