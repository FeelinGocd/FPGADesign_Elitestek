//==============================================================================
//  game_web.c —— 房间里的蜘蛛网（站上去减速）
//------------------------------------------------------------------------------
//  干什么      :
//    · 网的位置【只在房间蓝图里】（game_layout.c 的 OBJ_WEB），运行时没有实例池：
//      减速查询 / 落点避让 / 渲染都直接遍历当前蓝图（每间房 1~3 个，成本可忽略）。
//    · 玩家肚子中心落在网矩形内 → 移动速度降到 45%（离开立即恢复）。
//  为什么不再有池
//    · 以前 g.webs[] 存 {room,x,y} + old_web[2][] 记"上次画在哪"，本质是把蓝图里的
//      位置抄了一份；网又不会动、不会消失，这份副本永远是蓝图的影子。
//    · 换房时旧房间的网要擦掉 —— 由 game_room.c 的"换房补标脏"（2 帧）负责，
//      不需要每个对象各记一份旧位置。
//  上一级是谁  : game/game_main.c（每帧 submit）、game_state.c（进房不用生成）
//==============================================================================
#include "game.h"
#include "CI_gpu2d.h"

#define WEB_SIZE 32

/*  当前房间蓝图（没有固定布局 = 0，此时没有网）  */
static const room_layout_t *web_set(void)
{
    return game_layout(game_room_layout());
}


int game_web_slow_at(int x, int y)
{
    const room_layout_t *L = web_set();
    int i;

    if (!L) return 0;
    for (i = 0; i < (int)L->n_objs; ++i) {
        const obj_place_t *o = &L->objs[i];
        int wx, wy;

        if (o->kind != OBJ_WEB) continue;
        wx = (int)o->x * TILE;
        wy = (int)o->y * TILE;
        if (x >= wx && x < wx + WEB_SIZE &&
            y >= wy && y < wy + WEB_SIZE) return 1;
    }
    return 0;
}

/*  矩形是否压住当前房间的网 —— 金币/石头落点避让用  */
int game_web_overlap(int x, int y, int w, int h)
{
    const room_layout_t *L = web_set();
    int i;

    if (!L || w <= 0 || h <= 0) return 0;
    for (i = 0; i < (int)L->n_objs; ++i) {
        const obj_place_t *o = &L->objs[i];
        int wx, wy;

        if (o->kind != OBJ_WEB) continue;
        wx = (int)o->x * TILE;
        wy = (int)o->y * TILE;
        if (x < wx + WEB_SIZE && x + w > wx &&
            y < wy + WEB_SIZE && y + h > wy) return 1;
    }
    return 0;
}

/*  画一张网：arg = 蓝图里的对象下标，坐标从蓝图取  */
static void web_draw_one(u32 fb, u32 now, int buf, int i)
{
    const room_layout_t *L = web_set();
    const asset_desc_t *a = &g_assets[ASSET_background_interact_web];

    (void)now;
    (void)buf;
    if (!L || i < 0 || i >= (int)L->n_objs) return;
    if (L->objs[i].kind != OBJ_WEB) return;

    CI_gpu2d_spriteAlphaRect(a->base_word, a->stride_word, fb,
                             (u32)((int)L->objs[i].x * TILE),
                             (u32)((int)L->objs[i].y * TILE),
                             WEB_SIZE, WEB_SIZE, 0u);
}

void game_web_submit(void)
{
    const room_layout_t *L = web_set();
    int i;

    if (!L) return;
    for (i = 0; i < (int)L->n_objs; ++i) {
        const obj_place_t *o = &L->objs[i];
        int x, y;

        if (o->kind != OBJ_WEB) continue;
        x = (int)o->x * TILE;
        y = (int)o->y * TILE;
        if (!game_visible_world(x, y, WEB_SIZE, WEB_SIZE)) continue;
        rl_push(Z_SCENE + 2, web_draw_one, i);      /* 画在地板上、人物下面 */
    }
}
