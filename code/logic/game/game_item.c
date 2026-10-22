//==============================================================================
//  game_item.c —— 道具：底座 + 悬浮图标 / 拾取判定
//------------------------------------------------------------------------------
//  干什么      :
//    · game_item_try_pickup(): 角色矩形和道具矩形相交就吃掉：换弹种 + 记时间戳
//    · game_item_draw()      : 底座固定、图标上下浮（4 拍循环）
//  上一级是谁  : game/game_player.c（判定）、game/game_main.c（绘制）
//  接口契约    :
//    · 底座和图标是【两张独立精灵】，不用图片合成命令（换颜色只要换图标素材）
//    · 拾取只改 g.bullet_type / g.held_type，不动任何硬件寄存器
//  为什么这么做:
//    · 用时间戳（g.pickup_until）而不是帧计数 —— 帧率会抖，按帧算会闪得忽长忽短
//  参考        : docs/spec/软件_驱动与API.md §2.2.1（颜色合成，可给道具加光）
//==============================================================================
#include "game.h"
#include "CI_gpu2d.h"
#include "bsp.h"

/*==============================================================================
 *  道具属性表（★ 说明文字、图标、送的弹种都是【道具自己的属性】）
 *------------------------------------------------------------------------------
 *  desc 只能全大写 A-Z 和空格 —— 字模只有 26 张大写字母，小写会被跳过。
 *============================================================================*/
/*  效果种类：一个道具可能"换弹种"或"改开火方式"或"加命"  */
enum { FX_NONE, FX_TRIPLE, FX_LIFE };

typedef struct {
    const char *desc;       /* 屏幕上方那行说明（只能大写 A-Z 和空格） */
    int icon;               /* 图标素材下标 */
    int bullet;             /* 吃了换成哪种弹；-1 = 不动弹种 */
    int fx;                 /* FX_xxx */
} item_def_t;

static int item_index(int item)
{
    int index = item % ITEM_COUNT;
    return (index < 0) ? index + ITEM_COUNT : index;
}

static const item_def_t item_defs[ITEM_COUNT] = {
    /* ITEM_CHILI  */ { "YOU FEEL SO HOT",    ASSET_accessory_chili,      BULLET_CHILI,  FX_NONE   },
    /* ITEM_URANUS */ { "IT FEELS LIKE IRON", ASSET_accessory_Uranus,     BULLET_URANUS, FX_NONE   },
    /* ITEM_EYES   */ { "SLOWER BUT MORER",   ASSET_accessory_Inner_eyes, -1,            FX_TRIPLE },
    /* ITEM_1UP    */ { "EXTRA LIFE",         ASSET_accessory_1UP_ExtraLiFE, -1,         FX_LIFE   }
};

const char *game_item_desc(int item)   { return item_defs[item_index(item)].desc; }
int  game_item_icon(int item)          { return item_defs[item_index(item)].icon; }
int  game_item_bullet(int item)        { return item_defs[item_index(item)].bullet; }
int  game_item_fx(int item)            { return item_defs[item_index(item)].fx; }

/*----------------------------------------------------------------------------*/
/*  池子里已经有几件道具（HUD 用）  */
int game_item_count(void)
{
    int i, n = 0;

    for (i = 0; i < ITEM_COUNT; ++i) if (g.item_mask & (1u << i)) ++n;
    return n;
}

/*----------------------------------------------------------------------------*/
/*  道具房：从 ITEM_COUNT 种道具里随机挑一个，放在 (x, y)（房间中心）。
 *  用 LCG 而不是 rand()：不拉 libc 的大表；第一次用毫秒时钟做种子，
 *  同一次上电里每次进房（或新的一轮）拿到的道具不同。  */
void game_item_spawn_random(int x, int y)
{
    static u32 seed;
    u32 r;

    if (seed == 0u) seed = game_ms() | 1u;
    seed = seed * 1664525u + 1013904223u;
    r = seed >> 16;

    g.item.type   = (int)(r % (u32)ITEM_COUNT);
    g.item.x      = x;
    g.item.y      = y;
    g.item.active = 1;

    g.old_item[0].active = 0;       /* 新道具：不要把上一件的旧位置当脏区擦 */
    g.old_item[1].active = 0;
}

/*----------------------------------------------------------------------------*/
void game_item_try_pickup(u32 now)
{
    int hx = g.x + PB_OFF_X;
    int hy = g.y + PB_OFF_Y;

    if (!g.item.active) return;
    /*  用【肚子】去碰道具，头/脚不算  */
    if (hx < g.item.x + ITEM_W && hx + PB_W > g.item.x &&
        hy < g.item.y + ITEM_H && hy + PB_H > g.item.y) {
        int bt = game_item_bullet(g.item.type);
        int fx = game_item_fx(g.item.type);

        g.item.active = 0;
        g.held_type = g.item.type;

        /*  ★ 道具池：拿过的永久记下来，效果【累加】，不会因为再拿一个就丢掉。
         *    · 换弹种类（辣椒/天王星）：弹种以后拿的为准（"后面的盖前面的"）
         *    · 三向（内眼）：一旦拿到就常驻（fire_triple 由池子算，不是开关）
         *    · 加命（1UP）：每拿一次 +1                                    */
        g.item_mask |= (1u << g.item.type);

        /*  按【获取先后】进队列：同一个道具只排一次，新的追加到末尾  */
        {
            int k, dup = 0;

            for (k = 0; k < g.item_order_n; ++k)
                if (g.item_order[k] == g.item.type) dup = 1;

            if (!dup && g.item_order_n < ITEM_COUNT)
                g.item_order[g.item_order_n++] = g.item.type;
        }
        if (bt >= 0) g.bullet_type = bt;        /* -1 = 这个道具不换弹种 */
        g.fire_triple = (g.item_mask & (1u << ITEM_EYES)) ? 1 : 0;
        if (fx == FX_LIFE) game_vital_extra_life();
        g.pickup_until = now + PICKUP_MS;
        g.toast_type = g.item.type;             /* 屏幕上方弹一行说明 */
        g.toast_until = now + HUD_TOAST_MS;
        bsp_printf("DEBUG: accessory picked; bullet type=%d\r\n", g.bullet_type);

        /*  ★ 道具房：拿过就算用过，这一轮再进来不会重新生成  */
        game_room_mark_used(game_room_id());
    }
}

/*----------------------------------------------------------------------------*/
/*  右侧道具池：捡到哪件就在【固定格子】里画它偏暗的图标（不显示数量）
 *    · 格子按道具类型固定，不随拾取顺序跳动
 *    · 用 tone=3 乘系数压暗，看起来像"已经收进背包"的状态  */
#define POOL_ICON      32
#define POOL_GAP       8
#define POOL_X         (SCR_W - POOL_ICON - 10)
#define POOL_LABEL_Y   140                    /* "ITEM" 标签的字顶（小地图下面，再下移 64） */
#define POOL_LABEL_W   (4 * 16)               /* 标签 4 个字 = 64 px */
/*  ★ 标签右端和图标列右端对齐（往左延伸 64 px），否则超出屏宽会被整块丢弃
 *    —— 之前 x 起点太靠右，"EM" 两个字母因为超出 1280 直接被裁掉了。  */
#define POOL_LABEL_X   (POOL_X + POOL_ICON - POOL_LABEL_W + 4)
#define POOL_Y0        (POOL_LABEL_Y + 28)    /* 图标从标签下面开始 */

void game_item_pool_rect(int *x, int *y, int *w, int *h)
{
    *x = POOL_LABEL_X - 4;
    *y = POOL_LABEL_Y - 4;
    *w = (POOL_X + POOL_ICON + 4) - *x;
    *h = POOL_Y0 - POOL_LABEL_Y + ITEM_COUNT * (POOL_ICON + POOL_GAP) + 10;
}

static int s_pool_redraw[2];
static unsigned s_pool_last[2] = { 0xFFFFFFFFu, 0xFFFFFFFFu };

void game_item_reset_cache(void)
{
    s_pool_last[0] = s_pool_last[1] = 0xFFFFFFFFu;
    s_pool_redraw[0] = s_pool_redraw[1] = 1;
}

/*  整局重开：池子必须重画（否则重开后 ITEM 那一列永远是空的）  */
void game_item_invalidate(void)
{
    game_item_reset_cache();
}

/*  帧首：池子内容变了 → 标脏这一列（旧图标由脏区恢复擦掉）  */
void game_item_prepare(unsigned char dirty[GRID_H][GRID_W], int buf)
{
    int x, y, w, h;

    if (buf < 0 || buf > 1) buf = 0;
    if (s_pool_last[buf] == g.item_mask) return;
    s_pool_last[buf] = g.item_mask;

    game_item_pool_rect(&x, &y, &w, &h);
    game_mark_dirty(dirty, x, y, w, h);
    s_pool_redraw[buf] = 1;
}

void game_item_draw_pool(u32 fb, int buf, int full)
{
    int i;

    if (buf < 0 || buf > 1) buf = 0;
    if (!full && !s_pool_redraw[buf]) return;
    s_pool_redraw[buf] = 0;

    game_hud_label_at(fb, POOL_LABEL_X, POOL_LABEL_Y, "ITEM");

    /*  ★ 按【拾取先后】从上往下排，第 1 个拿到的在最上面；
     *    图标列右端和标签右端对齐。  */
    for (i = 0; i < g.item_order_n; ++i) {
        int x = POOL_X;
        int y = POOL_Y0 + i * (POOL_ICON + POOL_GAP);
        const asset_desc_t *a;

        if (!game_visible(x, y, POOL_ICON, POOL_ICON)) continue;

        a = &g_assets[game_item_icon(g.item_order[i])];
        CI_gpu2d_combineTintRect(a->base_word, a->stride_word,
            GPU2D_MODE_REPLACE | GPU2D_MODE_SPRITE | GPU2D_MODE_CKEY_EN,
            0x00000000u, 130u, 3u,
            fb, (u32)x, (u32)y, POOL_ICON, POOL_ICON);
    }
}

/*----------------------------------------------------------------------------*/
void game_item_draw(u32 fb, u32 now, int buf)
{
    int alive;

    g.old_item[buf].active = g.item.active;
    g.old_item[buf].x = g.item.x;
    g.old_item[buf].y = g.item.y;
    if (!g.item.active) return;

    /* 整块（含图标上浮余量）都在屏内才画 —— 硬件没有源裁剪。 */
    alive = game_visible(g.item.x, g.item.y + ITEM_ICON_DY - ITEM_FLOAT_AMP - 2,
                         ITEM_W, ITEM_H - ITEM_ICON_DY + ITEM_FLOAT_AMP + 2);
    if (!alive) return;

    {
        const asset_desc_t *base = &g_assets[ASSET_accessory_base];
        const asset_desc_t *icon = &g_assets[game_item_icon(g.item.type)];
        /* 正弦浮动：±ITEM_FLOAT_AMP，约 1 秒一个来回（原来是 4 档硬跳，看着发僵） */
        int float_y = -((g_sin_tab[(now >> 4) & 63] * ITEM_FLOAT_AMP) >> 6);

        CI_gpu2d_spriteAlphaRect(base->base_word, base->stride_word,
            fb, (u32)g.item.x, (u32)g.item.y, base->width, base->height, 0u);
        CI_gpu2d_spriteAlphaRect(icon->base_word, icon->stride_word,
            fb, (u32)g.item.x, (u32)(g.item.y + ITEM_ICON_DY + float_y),
            icon->width, icon->height, 0u);
    }
}

/*----------------------------------------------------------------------------*/
/*  登记进渲染队列：z = 底座底边 y —— 道具和角色/Boss/弹幕按同一条深度尺子排。
 *  ★ 每帧都要登记：被捡走后 game_item_draw() 还要把 old_item 清掉。 */
static void item_draw_entry(u32 fb, u32 now, int buf, int arg)
{
    (void)arg;
    game_item_draw(fb, now, buf);
}

void game_item_submit(void)
{
    rl_push(ACTOR_Z(g.item.y + ITEM_H), item_draw_entry, 0);
}