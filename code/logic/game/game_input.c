//==============================================================================
//  game_input.c —— 输入来源：板载按键 + 串口"虚拟手柄"
//------------------------------------------------------------------------------
//  干什么      :
//    · game_keys_read() 返回本帧的按键掩码（bit0 左 / bit1 右 / bit2 下 / bit3 上）
//    · 优先用串口：收到 PC 工具发来的两字节帧 {0xA5, mask} 就用它
//    · 超过 PAD_TIMEOUT 没收到就自动回落到板载四个按键（拔了线也能玩）
//  上一级是谁  : game/game_main.c（每帧读一次）
//  接口契约    :
//    · 串口帧固定两字节：0xA5 是帧头，mask 的低 4 位才是按键
//      （带帧头是为了让板子打印/其它文本不会被我误当成按键）
//    · 只在"有新帧"时刷新，掩码在 PAD_TIMEOUT 内一直有效 ⇒ PC 工具只需在变化时发
//  为什么这么做:
//    · 板载只有 4 个按键，玩弹幕太反人类；串口是现成的双向通道，不用改硬件。
//  参考        : tools/GamePad/game_pad.py（PC 端）、docs/spec/软件_驱动与API.md §8.3
//==============================================================================
#include "game.h"
#include "bsp.h"        /* BSP_UART_TERMINAL / uart_readOccupancy / uart_read */
#include "user_key.h"

#define PAD_MAGIC    0xA5u      /* PC → 板 的帧头 */
#define PAD_TIMEOUT  500u       /* 这么久没收到就走板载按键 */

/*  ---- 串口【文本命令】----
 *  和"虚拟手柄"的二进制帧共存：帧头 0xA5 不是可打印字符，所以
 *  可打印 ASCII 一律当命令收集，遇到回车执行。
 *      P      → 在玩家脚下生成一个道具（等同按 P）
 */
#define CMD_MAX 16
static char s_cmd[CMD_MAX];
static int  s_cmd_n;

/*  P 键生成道具请求 / B 放置炸弹请求  */
static int s_spawn_req;
static int s_bomb_req;
#if GAME_BOSS_DEBUG
static int s_boss_req;
#if GAME_PROFILE
static int s_stats_req;
#endif
#endif

static void cmd_exec(void)
{
    int single = (s_cmd_n == 1);
    int command = (unsigned char)s_cmd[0] | 0x20;
    s_cmd[s_cmd_n] = 0;
    s_cmd_n = 0;

    if (single && command == 'p') {
        s_spawn_req = 1;
        bsp_printf("DEBUG: spawn queued\r\n");
    } else if (single && command == 'b') {
        s_bomb_req = 1;
        bsp_printf("DEBUG: bomb queued\r\n");
#if GAME_BOSS_DEBUG && GAME_PROFILE
    } else if (s_cmd[0] == 's' && s_cmd[1] == 't' &&
               s_cmd[2] == 'a' && s_cmd[3] == 't' && s_cmd[4] == 0) {
        s_stats_req = 1;
#endif
    } else if (s_cmd[0] != 0) {
        bsp_printf("DEBUG: unknown cmd '%s' (P/B)\r\n", s_cmd);
    }
}

static int s_want_mask;         /* 已经看到帧头，等 mask */
static u32 s_keys;              /* 最近一次收到的掩码 */
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

#if GAME_BOSS_DEBUG
int game_take_boss_jump(void)
{
    int r = s_boss_req;
    s_boss_req = 0;
    return r;
}
#if GAME_PROFILE
int game_take_debug_stats(void)
{
    int r = s_stats_req;
    s_stats_req = 0;
    return r;
}
#endif
#endif

/*----------------------------------------------------------------------------*/
u32 game_keys_read(void)
{
    int c;

    /* 把串口里积压的字节一次吃完（不阻塞：只在有数据时才读） */
    while (uart_readOccupancy(BSP_UART_TERMINAL) > 0) {
        c = (int)(uart_read(BSP_UART_TERMINAL) & 0xFF);

        /*  ★ 0xA5 后面紧跟的一定是 mask；bit5(B)/bit6(P) 会到 0x20/0x40，
         *    不能再按"可打印字符"区分。  */
        if (s_want_mask) {
            s_want_mask = 0;
            s_keys = (u32)c & 0xFFu;        /* bit0..3 方向 / bit4 R / bit5 B / bit6 P / bit7 T */
            /* ★ 锁存：快速点一下时，"按下"和"松开"两帧可能落在同一帧里，
               照搬 s_keys 会把 R 覆盖掉 ⇒ 单独记一个请求位，等游戏来取。 */
            if (s_keys & KEY_RESTART) s_restart_req = 1;

            /*  ★ P 用【上升沿】：一直按住只生成一次，松开再按才生成下一次  */
            if ((s_keys & KEY_SPAWN) && !(s_prev_mask & KEY_SPAWN))
                s_spawn_req = 1;
            if ((s_keys & KEY_BOMB) && !(s_prev_mask & KEY_BOMB))
                s_bomb_req = 1;
#if GAME_BOSS_DEBUG
            if ((s_keys & KEY_BOSS_JUMP) && !(s_prev_mask & KEY_BOSS_JUMP))
                s_boss_req = 1;
#endif
            s_prev_mask = s_keys;

            s_until = game_ms() + PAD_TIMEOUT;
            continue;
        }

        if (c == (int)PAD_MAGIC && !s_want_mask) {
            s_want_mask = 1;
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
    if ((int)(game_ms() - s_until) < 0) return s_keys;
    return key_getData() & 15u;             /* 板载按键没有 R（只能从串口来） */
}

/*----------------------------------------------------------------------------*/
/*  取一次"重开请求"（取完就清）—— 按一下 R 就重开，不用按住  */
int game_take_restart(void)
{
    int r = s_restart_req;

    s_restart_req = 0;
    return r;
}
