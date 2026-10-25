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
 *  道具属性表（★ 说明文字、图标、属性增减量都是【道具自己的属性】）
 *------------------------------------------------------------------------------
 *  desc 只能全大写 A-Z 和空格 —— 字模只有 26 张大写字母，小写会被跳过。
 *  属性使用 game.h 的 item_mod_t：0 = 不改，正负 = 加/减。
 *  目前 HUD 还没设计“属性展示”，这里只负责数值结算。
 *============================================================================*/
/*  旧接口 game_item_fx() 的效果种类（保留给外部/字幕判断）  */
enum { FX_NONE, FX_TRIPLE, FX_LIFE };

static int item_index(int item)
{
    int index = item % ITEM_COUNT;
    return (index < 0) ? index + ITEM_COUNT : index;
}

const item_def_t g_item_defs[ITEM_COUNT] = {
    /* ITEM_CHILI  */ {
        "YOU FEEL SO HOT", ASSET_accessory_chili, 2, 0, 0,
        { 0, 0, 0, 0, 0, 0, 0, 0, BULLET_CHILI, 0 }
    },
    /* ITEM_URANUS */ {
        "IT FEELS LIKE IRON", ASSET_accessory_Uranus, 2, 0, 0,
        { 0, 0, 0, 2, -1, 2, 0, 2, BULLET_URANUS, 0 }
    },
    /* ITEM_EYES   */ {
        "SLOWER BUT MORER", ASSET_accessory_Inner_eyes, 2, 0, 0,
        { 0, 0, 0, 0, -5, 0, 0, -1, -1, 2 }
    },
    /* ITEM_1UP    */ {
        "EXTRA LIFE", ASSET_accessory_1UP_ExtraLiFE, 2, 0, 0,
        { 1, 0, 1, 1, 0, 0, 0, 0, -1, 0 }
    },
    /* ITEM_MAGICCARD */ {
        "TURN BACK", ASSET_accessory_MagicCard_, 3, 1, ITEM_FLAG_LIFE_PLUS,
        { 0, 0, 0, 0, -3, -3, -1, 1, -1, 0 }
    }
};

const char *game_item_desc(int item)   { return g_item_defs[item_index(item)].desc; }
int  game_item_icon(int item)          { return g_item_defs[item_index(item)].icon; }
int  game_item_bullet(int item)        { return g_item_defs[item_index(item)].mod.bullet_type; }
int  game_item_fx(int item)
{
    const item_def_t *d = &g_item_defs[item_index(item)];

    if (d->flags & ITEM_FLAG_LIFE_PLUS) return FX_LIFE;
    if (d->mod.triple) return FX_TRIPLE;
    if (d->mod.lives > 0) return FX_LIFE;
    return FX_NONE;
}

/*----------------------------------------------------------------------------*/
/*  池子里已经有几件道具（HUD 用）  */
int game_item_count(void)
{
    return g.inv_n;
}

/*  道具刷新权重：
 *    · 没拿过的权重 2；
 *    · 拿过的权重 1（降一半）；
 *    · 再乘品质权重：普通 100 / 稀有 45 / 史诗 15 / 传说 4；
 *    · pool=0 的道具不参与随机。  */
static int item_weight(int item)
{
    static const unsigned char level_w[4] = { 100u, 45u, 15u, 4u };
    const item_def_t *d;
    int base;

    if (item < 0 || item >= ITEM_COUNT) return 0;
    d = &g_item_defs[item];
    if (!d->pool) return 0;
    base = (g.item_mask & (1u << item)) ? 1 : 2;
    return base * (int)level_w[d->level & 3u];
}

static int item_weighted_pick(u32 r)
{
    u32 total = 0u;
    int i;

    for (i = 0; i < ITEM_COUNT; ++i) total += (u32)item_weight(i);
    if (total == 0u) return 0;

    r %= total;
    for (i = 0; i < ITEM_COUNT; ++i) {
        u32 w = (u32)item_weight(i);

        if (r < w) return i;
        r -= w;
    }
    return ITEM_COUNT - 1;
}

static u32 s_item_seed;
static u32 s_pickup_lock_until;   /* P 生成后短暂禁止拾取，先让图标露个脸 */

static u32 item_rand(void)
{
    if (s_item_seed == 0u) s_item_seed = game_ms() | 1u;
    s_item_seed = s_item_seed * 1664525u + 1013904223u;
    return s_item_seed >> 16;
}

/*  道具房 / 商店共用：按“拿过权重减半”随机挑一个道具类型  */
int game_item_random_type(void)
{
    return item_weighted_pick(item_rand());
}

/*  从 inv[] 重算类型 mask（权重/旧代码快速判断用）。  */
static void item_rebuild_mask(void)
{
    int i;

    g.item_mask = 0u;
    for (i = 0; i < g.inv_n; ++i)
        g.item_mask |= (1u << g.inv[i].type);
}

static void item_add_slot(int type)
{
    if (g.inv_n >= INV_MAX) return;
    g.inv[g.inv_n].type  = (unsigned char)type;
    g.inv[g.inv_n].level = g_item_defs[type].level;
    g.inv[g.inv_n].flags = g_item_defs[type].flags;
    ++g.inv_n;
    ++g.inv_rev;
    item_rebuild_mask();
}

/*  道具消失后，从剩余背包【整体重算】属性，而不是只减一项。
 *  这样 bullet_type / triple 这类“覆盖型/布尔型”字段也能正确恢复；
 *  hp / lives 是拾取时的一次性收益，不参与重算（也不可逆）。  */
static void item_recompute_stats(void)
{
    int i;

    g.stats.move_v8      = PLAYER_MAX_V8;
    g.stats.fire_every   = (int)FIRE_EVERY;
    g.stats.bullet_speed = PLAYER_BULLET_SPEED;
    g.stats.range        = PLAYER_RANGE;
    g.stats.damage       = 1;
    g.bullet_type        = BULLET_A;
    g.fire_triple        = 0;

    for (i = 0; i < g.inv_n; ++i) {
        const item_mod_t *m = &g_item_defs[g.inv[i].type].mod;

        g.stats.move_v8 += m->move_v8;
        if (g.stats.move_v8 < 8)  g.stats.move_v8 = 8;
        if (g.stats.move_v8 > 48) g.stats.move_v8 = 48;

        g.stats.fire_every += m->fire_every;
        if (g.stats.fire_every < 1)  g.stats.fire_every = 1;
        if (g.stats.fire_every > 60) g.stats.fire_every = 60;

        g.stats.bullet_speed += m->bullet_speed;
        if (g.stats.bullet_speed < 1)  g.stats.bullet_speed = 1;
        if (g.stats.bullet_speed > 12) g.stats.bullet_speed = 12;

        g.stats.range += m->range;
        if (g.stats.range < 1)  g.stats.range = 1;
        if (g.stats.range > 99) g.stats.range = 99;

        g.stats.damage += m->damage;
        if (g.stats.damage < 1) g.stats.damage = 1;
        if (g.stats.damage > 9) g.stats.damage = 9;

        if (m->bullet_type >= 0) g.bullet_type = m->bullet_type;
        if (m->triple) g.fire_triple = 1;
    }
}

/*  道具有效期结束/被消耗：移除实例，并按剩余背包重算属性。  */
static void item_remove_slot(int idx)
{
    int i;

    if (idx < 0 || idx >= g.inv_n) return;
    for (i = idx; i + 1 < g.inv_n; ++i) g.inv[i] = g.inv[i + 1];
    --g.inv_n;
    ++g.inv_rev;
    item_rebuild_mask();
    item_recompute_stats();
    game_item_invalidate();
}

/*  死亡判定：按拾取顺序找第一个 LIFE_PLUS。找到就消费并回退。  */
int game_item_try_death_save(u32 now)
{
    int i, death_room, target, entry;

    for (i = 0; i < g.inv_n; ++i) {
        if (g.inv[i].flags & ITEM_FLAG_LIFE_PLUS) break;
    }
    if (i >= g.inv_n) return 0;

    death_room = game_room_id();
    target = g.last_room;
    entry  = g.last_door;
    if (target < 0 || target >= ROOM_COUNT || target == death_room) {
        target = game_room_start();
        entry  = -1;
    }
    if (target < 0) return 0;

    /*  死亡房间下次进入重新刷怪；Boss 已死不重置。  */
    if (!(game_room_type() == ROOM_BOSS && g.boss_hp <= 0))
        game_room_mark_respawn(death_room);

    g.held_type = g.inv[i].type;
    g.toast_type = g.held_type;
    g.toast_until = now + HUD_TOAST_MS;
    /*  受伤闪红 1 s + 无敌闪烁 1.5 s 会盖住举手姿势；
     *  把 pickup 拉长到无敌结束以后，保证玩家能看到“举起道具”。  */
    g.pickup_until = now + RETURN_INVULN_MS + PICKUP_MS;
    item_remove_slot(i);

    game_room_request_full(target, entry);
    return 1;
}

/*  拿到一个道具的效果结算（拾取 / 商店购买共用）。
 *  只改背包与效果，不碰道具实体、不弹 toast。  */
void game_item_apply(int type)
{
    const item_def_t *d = &g_item_defs[type];
    const item_mod_t *m = &d->mod;
    int i;

    g.held_type = type;

    /*  ★ 道具池：每个实例按拾取顺序进数组；效果【累加】。  */
    item_add_slot(type);

    /*  ---- 属性增减（HUD 展示还没设计，这里先结算数值） ---- */
    for (i = 0; i < m->hp; ++i) game_vital_heal_one();
    for (i = 0; i < m->lives; ++i) game_vital_extra_life();
    /*  m->hp_max 先保留字段：等血条 UI 支持“上限变化”后再接。  */

    /*  ★ 属性不在这里逐项加：背包已经多了一件，直接从背包【整体重算】。
     *    以前这里和 item_recompute_stats() 各抄一份"加 + 钳位"，两处钳位值
     *    改一处忘一处就会漂（例如加滿道具后再死一次，属性会和捡的时候不一样）。  */
    item_recompute_stats();
}

/*----------------------------------------------------------------------------*/
/*  道具房：从 ITEM_COUNT 种道具里加权挑一个，放在 (x, y)（房间中心）。
 *  用 LCG 而不是 rand()：不拉 libc 的大表；第一次用毫秒时钟做种子，
 *  同一次上电里每次进房（或新的一轮）拿到的道具不同。
 *  ★ 拿过的道具权重减半（2 → 1），但不降到 0 —— 仍有机会再刷出来。  */
void game_item_spawn_random(int x, int y)
{
    g.item.type   = game_item_random_type();
    g.item.x      = x;
    g.item.y      = y;
    g.item.active = 1;
    s_pickup_lock_until = 0u;

    g.old_item[0].active = 0;       /* 新道具：不要把上一件的旧位置当脏区擦 */
    g.old_item[1].active = 0;
}

/*  P 键：在玩家脚下生成一个道具；400 ms 内不拾取，避免刚生成就消失。  */
void game_item_spawn_at_player(void)
{
    int x = g.x + PW / 2 - ITEM_W / 2;
    int y = g.y + PH / 2 - ITEM_H / 2;

    game_item_spawn_random(x, y);
    s_pickup_lock_until = game_ms() + 400u;
}

/*----------------------------------------------------------------------------*/
void game_item_try_pickup(u32 now)
{
    int hx = g.x + PB_OFF_X;
    int hy = g.y + PB_OFF_Y;

    if (!g.item.active) return;
    if ((int)(now - s_pickup_lock_until) < 0) return;
    /*  用【脚到眼睛】的身体矩形去碰道具，头不算  */
    if (hx < g.item.x + ITEM_W && hx + PB_W > g.item.x &&
        hy < g.item.y + ITEM_H && hy + PB_H > g.item.y) {
        g.item.active = 0;
        g.boss_drop_pending = 0;        /* Boss 掉的饰品已经捡走，重进 Boss 房不再恢复 */
        game_item_apply(g.item.type);   /* 背包 / 弹种 / 增加弹幕 / 加命统一结算 */
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
#define POOL_MAX       13                     /* 720p 右侧最多列 13 个（再多要改布局） */
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
    *h = POOL_Y0 - POOL_LABEL_Y + POOL_MAX * (POOL_ICON + POOL_GAP) + 10;
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

    /*  ★ 右侧道具池同样会被"按瓦片"的脏区恢复擦到：弹幕/角色/石头走到
     *    相机窗口右缘时，它们旧位置的脏瓦片会盖住池子。池子原本只在
     *    item_mask 变化或相机移动时重画 ⇒ 那些帧里图标会"闪掉"。
     *    这里先查一次池子所占瓦片，脏了就强制重画（不重复标脏：
     *    场景恢复已经在帧首做过了）。  */
    game_item_pool_rect(&x, &y, &w, &h);
    x += g.cam_x;
    y += g.cam_y;
    if (game_dirty_touches(x / TILE, y / TILE,
                           (x + w - 1) / TILE + 1,
                           (y + h - 1) / TILE + 1))
        s_pool_redraw[buf] = 1;

    if (s_pool_last[buf] == g.inv_rev) return;
    s_pool_last[buf] = g.inv_rev;

    game_item_pool_rect(&x, &y, &w, &h);
    game_mark_hud_dirty(dirty, x, y, w, h);
    s_pool_redraw[buf] = 1;
}

void game_item_draw_pool(u32 fb, int buf, int full)
{
    int i;

    if (buf < 0 || buf > 1) buf = 0;
    if (!full && !s_pool_redraw[buf]) return;
    s_pool_redraw[buf] = 0;

    fb = game_view_fb(fb);
    game_hud_label_at(fb, POOL_LABEL_X, POOL_LABEL_Y, "ITEM");

    /*  ★ 按【拾取先后】从上往下排，第 1 个拿到的在最上面；
     *    图标列右端和标签右端对齐。  */
    for (i = 0; i < g.inv_n && i < POOL_MAX; ++i) {
        int x = POOL_X;
        int y = POOL_Y0 + i * (POOL_ICON + POOL_GAP);
        const asset_desc_t *a;

        if (!game_visible_screen(x, y, POOL_ICON, POOL_ICON)) continue;

        a = &g_assets[game_item_icon(g.inv[i].type)];
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
    alive = game_visible_world(g.item.x, g.item.y + ITEM_ICON_DY - ITEM_FLOAT_AMP - 2,
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