//==============================================================================
//  game_chest.c —— 房间固有箱子：清怪后靠近自动打开并生成持久掉落物
//==============================================================================
#include "game.h"
#include "game_layout_data.h"
#include "CI_gpu2d.h"

#define CHEST_COIN_WEIGHT 8u
#define CHEST_BOMB_WEIGHT 2u
#define CHEST_COIN_MAX    5
#define CHEST_BOMB_MAX    3
#define CHEST_OPEN_GAP    12

static u32 s_seed;
static unsigned char s_test_active;
static signed char s_test_room = -1;
static short s_test_x, s_test_y;
static signed char s_open_room = -1;
static unsigned char s_open_x, s_open_y;

static u32 chest_rand(void)
{
    if (s_seed == 0u) s_seed = game_ms() | 1u;
    s_seed = s_seed * 1664525u + 1013904223u;
    return s_seed >> 16;
}

static int near_chest(int x, int y)
{
    int px = g.x + PB_OFF_X + PB_W / 2;
    int py = g.y + PB_OFF_Y + PB_H / 2;
    int x0 = x * TILE, y0 = y * TILE;
    int dx = px < x0 ? x0 - px : (px >= x0 + TILE ? px - (x0 + TILE - 1) : 0);
    int dy = py < y0 ? y0 - py : (py >= y0 + TILE ? py - (y0 + TILE - 1) : 0);
    return dx * dx + dy * dy <= CHEST_OPEN_GAP * CHEST_OPEN_GAP;
}

static void open_one(int room, int x, int y)
{
    int count = 1 + (int)(chest_rand() % CHEST_COIN_MAX);
    unsigned int roll = chest_rand() % (CHEST_COIN_WEIGHT + CHEST_BOMB_WEIGHT);
    int px = x * TILE + TILE / 2;
    int py = y * TILE + TILE / 2;
    int i;

    game_room_mark_cell_gone(room, x, y);
    game_stone_invalidate();
    game_stone_erase_cell(x * TILE, y * TILE);
    s_open_room = (signed char)room;
    s_open_x = (unsigned char)x;
    s_open_y = (unsigned char)y;
    if (roll < CHEST_COIN_WEIGHT) {
        game_coins_spawn_at(px - 12, py - 12, count);
        GAME_LOG("CHEST: room=%d cell=(%d,%d) coins=%d\r\n", room, x, y, count);
        return;
    }

    count = 1 + (int)(chest_rand() % CHEST_BOMB_MAX);
    for (i = 0; i < count; ++i) {
        int ox = ((i & 1) ? TILE : -TILE) * ((i + 1) / 2);
        int oy = (i & 1) ? TILE : -TILE;
        game_bomb_spawn_pickup(px + ox - BOMB_PICK_W / 2,
                               py + oy - BOMB_PICK_H / 2, room);
    }
    GAME_LOG("CHEST: room=%d cell=(%d,%d) bombs=%d\r\n", room, x, y, count);
}

int game_chest_is_opening(int room, int x, int y, u32 now)
{
    (void)now;
    return room == (int)s_open_room && x == (int)s_open_x && y == (int)s_open_y;
}

void game_chest_spawn_test(void)
{
    s_test_room = (signed char)game_room_id();
    s_test_x = (short)((g.x + PW / 2 - TILE / 2) & ~(TILE - 1));
    s_test_y = (short)((g.y + PH / 2 - TILE / 2) & ~(TILE - 1));
    s_test_active = 1;
}

static void test_box_draw(u32 fb, u32 now, int buf, int arg)
{
    const asset_desc_t *a;
    (void)now; (void)buf; (void)arg;
    if (!s_test_active || s_test_room != (signed char)game_room_id()) return;
    a = &g_assets[ASSET_background_interact_box_close];
    CI_gpu2d_spriteAlphaRect(a->base_word, a->stride_word, fb,
                             (u32)s_test_x, (u32)s_test_y, TILE, TILE, 0u);
}

void game_chest_submit_test(void)
{
    if (s_test_active && s_test_room == (signed char)game_room_id())
        rl_push(ACTOR_Z(s_test_y + TILE), test_box_draw, 0);
}

void game_chest_step(u32 now)
{
    const room_layout_t *layout;
    int room = game_room_id();
    int i;

    (void)now;
    if (s_open_room >= 0 && room != (int)s_open_room)
        s_open_room = -1;
    if (s_test_active && s_test_room != (signed char)room)
        s_test_active = 0;
    if (room < 0 || g.phase != PHASE_EXPLORE || game_mobs_alive()) return;
    layout = game_layout(game_room_layout());
    if (!layout) return;

    for (i = 0; i < (int)layout->n_objs; ++i) {
        const obj_place_t *o = &layout->objs[i];
        int x, y;
        if (o->kind != OBJ_BOX) continue;
        if (game_room_cell_gone(room, o->x, o->y)) continue;
        game_layout_obj_cell(layout, o, game_room_flip(), &x, &y);
        if (near_chest(x, y)) {
            open_one(room, o->x, o->y);
            break;
        }
    }
}
