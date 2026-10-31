//==============================================================================
//  game_shop.c —— 商店房：heart / 炸弹 / 随机道具（无底座、无浮动）
//------------------------------------------------------------------------------
//  商品按“掉落物”的方式记状态：
//    · 每条商品 = active / room / x / y / kind / type；
//    · 进出商店不重置位置和类型，只按 room 决定是否参与绘制/碰撞；
//    · old_shop[2] 记录两块缓冲上一次画在哪，换房时脏区擦掉旧贴图。
//  规则：
//    · heart 5 币，只回 1 格血，不提升 HP_MAX；
//    · 炸弹 5 币，购买后增加 1 颗可用炸弹；
//    · 随机道具 15 币，类型首次进店时按“拿过权重减半”决定；
//    · 价格用 16×8 数字；商品无底座、无浮动；
//    · 商店内不生成 mob / web / stone（见 game_state.c）。
//==============================================================================
#include "game.h"
#include "CI_gpu2d.h"
#include "bsp.h"

#define SHOP_GOODS      3
#define SHOP_KIND_HEART 0
#define SHOP_KIND_BOMB  1
#define SHOP_KIND_ITEM  2

#define SHOP_HEART_X   560
#define SHOP_HEART_Y   300
#define SHOP_BOMB_X    680
#define SHOP_BOMB_Y    300
#define SHOP_ITEM_X    800
#define SHOP_ITEM_Y    296
#define SHOP_HEART_W   24
#define SHOP_HEART_H   24
#define SHOP_BOMB_W    24
#define SHOP_BOMB_H    24
#define SHOP_ITEM_W    32
#define SHOP_ITEM_H    32
#define SHOP_PRICE_GAP 6

typedef struct {
    unsigned char active;
    signed char   room;
    short         x, y;
    unsigned char kind;         /* SHOP_KIND_HEART / SHOP_KIND_BOMB / SHOP_KIND_ITEM */
    unsigned char type;         /* 道具类型；heart 不用 */
} shop_good_t;

/*  16×8 数字在素材表里和 16×24 交错排列，不能拿 num0 直接加下标。  */
static const unsigned short s_num16x8[10] = {
    ASSET_num0_16x8, ASSET_num1_16x8, ASSET_num2_16x8, ASSET_num3_16x8,
    ASSET_num4_16x8, ASSET_num5_16x8, ASSET_num6_16x8, ASSET_num7_16x8,
    ASSET_num8_16x8, ASSET_num9_16x8
};

static shop_good_t s_goods[SHOP_GOODS];
static old_t       s_old[GAME_FB_COUNT][SHOP_GOODS];
static signed char s_room = -1;

static int shop_hit(int ax, int ay, int aw, int ah,
                    int bx, int by, int bw, int bh)
{
    return ax < bx + bw && ax + aw > bx &&
           ay < by + bh && ay + ah > by;
}

void game_shop_reset(void)
{
    int i, b;

    for (i = 0; i < SHOP_GOODS; ++i) {
        s_goods[i].active = 0;
        s_goods[i].room = -1;
    }
    for (b = 0; b < GAME_FB_COUNT; ++b)
        for (i = 0; i < SHOP_GOODS; ++i) s_old[b][i].active = 0;
    s_room = -1;
}

static void shop_add_good(int kind, int type, int x, int y)
{
    shop_good_t *g = &s_goods[kind];

    g->active = 1;
    g->room   = (signed char)game_room_id();
    g->x      = (short)x;
    g->y      = (short)y;
    g->kind   = (unsigned char)kind;
    g->type   = (unsigned char)type;
}

/*  第一次进这间商店时决定随机道具；之后重进保持“已买/未买”和位置。  */
void game_shop_enter(void)
{
    int id = game_room_id();

    if (s_room == id) return;
    s_room = (signed char)id;

    shop_add_good(SHOP_KIND_HEART, 0, SHOP_HEART_X, SHOP_HEART_Y);
    shop_add_good(SHOP_KIND_BOMB, 0, SHOP_BOMB_X, SHOP_BOMB_Y);
    shop_add_good(SHOP_KIND_ITEM, game_item_random_type(),
                  SHOP_ITEM_X, SHOP_ITEM_Y);
}

static void shop_price(u32 fb, int x, int y, int value, int digits)
{
    int i;

    if (value < 0) value = 0;
    for (i = digits - 1; i >= 0; --i) {
        int div = 1, j;
        const asset_desc_t *a;

        for (j = 0; j < i; ++j) div *= 10;
        a = &g_assets[s_num16x8[(value / div) % 10]];
        if (game_visible_world(x, y, (int)a->width, (int)a->height)) {
            CI_gpu2d_spriteFlipCK(a->base_word, a->stride_word, fb,
                                  (u32)x, (u32)y, a->width, a->height, 0u);
        }
        x += 16;
    }
}

void game_shop_step(u32 now)
{
    int px, py, i;

    if (game_room_type() != ROOM_SHOP) return;

    px = g.x + PB_OFF_X;
    py = g.y + PB_OFF_Y;

    for (i = 0; i < SHOP_GOODS; ++i) {
        shop_good_t *good = &s_goods[i];
        int w, h, price;

        if (!good->active || good->room != (signed char)game_room_id()) continue;

        w = (good->kind == SHOP_KIND_ITEM) ? SHOP_ITEM_W :
            (good->kind == SHOP_KIND_BOMB) ? SHOP_BOMB_W : SHOP_HEART_W;
        h = (good->kind == SHOP_KIND_ITEM) ? SHOP_ITEM_H :
            (good->kind == SHOP_KIND_BOMB) ? SHOP_BOMB_H : SHOP_HEART_H;
        price = (good->kind == SHOP_KIND_ITEM) ? 15 : 5;

        if (!shop_hit(px, py, PB_W, PB_H, good->x, good->y, w, h)) continue;
        if (g.coins < price) continue;

        if (good->kind == SHOP_KIND_HEART) {
            /*  满血不扣钱、不买走  */
            if (!game_vital_heal_one()) continue;
            g.coins -= price;
            g.held_type = HELD_HEART;           /* get_acc 姿势头顶举 heart，不是上一个道具 */
            g.pickup_until = now + PICKUP_MS;   /* 和捡道具一样播 get_acc 动画 */
            good->active = 0;
            GAME_LOG("SHOP: buy heart (+1 hp, -5)\r\n");
        } else if (good->kind == SHOP_KIND_BOMB) {
            g.coins -= price;
            ++g.bomb_count;
            g.held_type = HELD_BOMB;
            g.pickup_until = now + PICKUP_MS;
            good->active = 0;
            GAME_LOG("SHOP: buy bomb (+1, -5), count=%d\r\n", g.bomb_count);
        } else {
            g.coins -= price;
            game_item_apply(good->type);    /* 属性和普通掉落物完全一致 */
            g.pickup_until = now + PICKUP_MS;
            g.toast_type = good->type;
            g.toast_until = now + HUD_TOAST_MS;
            good->active = 0;
            GAME_LOG("SHOP: buy item #%d (-15)\r\n", good->type);
        }
    }
}

/*  画出“这块缓冲上次画过的商品”需要擦的矩形；双缓冲各记一份。  */
void game_shop_mark_old(dirty_map_t dirty, int buf)
{
    int i;

    for (i = 0; i < SHOP_GOODS; ++i) {
        const old_t *o = &s_old[buf][i];
        int w = (i == SHOP_KIND_ITEM) ? SHOP_ITEM_W :
                (i == SHOP_KIND_BOMB) ? SHOP_BOMB_W : SHOP_HEART_W;
        int h = (i == SHOP_KIND_ITEM) ? SHOP_ITEM_H :
                (i == SHOP_KIND_BOMB) ? SHOP_BOMB_H : SHOP_HEART_H;

        if (!o->active) continue;
        game_mark_dirty(dirty, o->x - 4, o->y - 4,
                        w + 8, h + 8 + SHOP_PRICE_GAP + 8);
    }
}

/*  每帧把 old_shop[buf] 对齐到当前房间；和 bullets/coins 同一个节奏。  */
void game_shop_sync(int buf)
{
    int i;
    signed char room = (signed char)game_room_id();

    for (i = 0; i < SHOP_GOODS; ++i) {
        const shop_good_t *g = &s_goods[i];

        s_old[buf][i].active = g->active && (g->room == room);
        s_old[buf][i].x = g->x;
        s_old[buf][i].y = g->y;
    }
}

/*----------------------------------------------------------------------------*/
static void shop_draw_one(u32 fb, u32 now, int buf, int arg)
{
    int i;
    signed char room = (signed char)game_room_id();

    (void)now;
    (void)buf;
    (void)arg;

    if (game_room_type() != ROOM_SHOP) return;

    for (i = 0; i < SHOP_GOODS; ++i) {
        const shop_good_t *good = &s_goods[i];
        const asset_desc_t *a;

        if (!good->active || good->room != room) continue;

        if (good->kind == SHOP_KIND_HEART) {
            a = &g_assets[ASSET_SpecialEffect_heart];
            if (!game_visible_world(good->x, good->y, SHOP_HEART_W, SHOP_HEART_H)) continue;
            CI_gpu2d_spriteAlphaRect(a->base_word, a->stride_word, fb,
                                     (u32)good->x, (u32)good->y,
                                     SHOP_HEART_W, SHOP_HEART_H, 0u);
            shop_price(fb, good->x + 4,
                       good->y + SHOP_HEART_H + SHOP_PRICE_GAP, 5, 1);
        } else if (good->kind == SHOP_KIND_BOMB) {
            a = &g_assets[ASSET_SpecialEffect_bomb];
            if (!game_visible_world(good->x, good->y, SHOP_BOMB_W, SHOP_BOMB_H)) continue;
            CI_gpu2d_spriteAlphaRect(a->base_word, a->stride_word, fb,
                                     (u32)good->x, (u32)good->y,
                                     SHOP_BOMB_W, SHOP_BOMB_H, 0u);
            shop_price(fb, good->x + 4,
                       good->y + SHOP_BOMB_H + SHOP_PRICE_GAP, 5, 1);
        } else {
            a = &g_assets[game_item_icon(good->type)];
            if (!game_visible_world(good->x, good->y, SHOP_ITEM_W, SHOP_ITEM_H)) continue;
            CI_gpu2d_spriteAlphaRect(a->base_word, a->stride_word, fb,
                                     (u32)good->x, (u32)good->y,
                                     SHOP_ITEM_W, SHOP_ITEM_H, 0u);
            shop_price(fb, good->x,
                       good->y + SHOP_ITEM_H + SHOP_PRICE_GAP, 15, 2);
        }
    }
}

void game_shop_submit(void)
{
    if (game_room_type() != ROOM_SHOP) return;
    rl_push(Z_SCENE + 5, shop_draw_one, 0);
}
