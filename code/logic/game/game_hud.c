//==============================================================================
//  game_hud.c —— 左上角状态面板 + 右侧道具池 + Boss 血条 + 字幕
//------------------------------------------------------------------------------
//  干什么      :
//    · 面板每一行 = 标签(CK字模) + 数字(CK字模)，透明部分显示场景
//    · 只有"这一行的数变了"才重画那一行；没变就一条命令都不发
//  上一级是谁  : game/game_main.c（每帧 hud_submit()，各块带自己的 Z_HUD_* 层号）
//  接口契约    :
//    · 字模透明处不会覆盖旧字；移动/数值变化时必须先恢复旧字所在的场景
//    · 数字必须定宽（digits 位），位数一变后面的内容会平移
//  为什么这么做:
//    · 擦除由帧首脏区恢复统一完成，标签与数字在实体之后提交。
//    · 相机移动保留完整旧字模擦除；仅不透明的小地图可以只擦矩形差集。
//  参考        : docs/spec/软件_驱动与API.md §5.2
//==============================================================================
#include "game.h"
#include "CI_gpu2d.h"

/*  每行刷新时先恢复这一行的地砖（不填色块），宽度覆盖到 204 px  */
#define HUD_PLATE_X     0
#define HUD_PLATE_W     204

int game_hud_need_full = 0;

/*----------------------------------------------------------------------------*/
/*  字模：CK 直写（不吃 α、不混合）—— 底板上直写不会有残影  */
static void hud_glyph(u32 fb, int x, int y, int asset)
{
    const asset_desc_t *a = &g_assets[asset];

    if (!game_visible_screen(x, y, (int)a->width, (int)a->height)) return;
    CI_gpu2d_spriteFlipCK(a->base_word, a->stride_word, fb,
                          (u32)x, (u32)y, a->width, a->height, 0u);
}

static void hud_label(u32 fb, int x, int y, const char *text)
{
    while (*text) {
        char c = *text++;
        if (c >= 'A' && c <= 'Z') hud_glyph(fb, x, y, ASSET_English_A + (c - 'A'));
        x += 16;
    }
}

static void hud_number(u32 fb, int x, int y, int value, int digits)
{
    /*  ★ 16×8 加进来之后，素材表里 num0_16x24 / num0_16x8 / num1_16x24… 是交错的，
     *    不能再拿 ASSET_num0_16x24 + digit 连续寻址，否则会混进小号数字。  */
    static const unsigned short big_digit[10] = {
        ASSET_num0_16x24, ASSET_num1_16x24, ASSET_num2_16x24, ASSET_num3_16x24,
        ASSET_num4_16x24, ASSET_num5_16x24, ASSET_num6_16x24, ASSET_num7_16x24,
        ASSET_num8_16x24, ASSET_num9_16x24
    };
    int i;

    if (value < 0) value = 0;
    for (i = digits - 1; i >= 0; --i) {
        int div = 1, j;

        for (j = 0; j < i; ++j) div *= 10;
        hud_glyph(fb, x, y, big_digit[(value / div) % 10]);
        x += 16;
    }
}

/*----------------------------------------------------------------------------*/
/*  画一行：标签 + 数字（CK 直写）。
 *  ★ 这里【不许】贴地砖！帧首的脏区恢复已经把旧内容擦掉了；
 *    在绘制层贴地砖会把走到这块区域的人物一起盖掉（"人物走进 GUI 就消失"）。  */
static void hud_row(u32 fb, int row, const char *label, int value, int digits)
{
    int y = row * HUD_ROW_H;

    hud_label(fb, HUD_LABEL_X, y, label);
    hud_number(fb, HUD_VALUE_X, y, value, digits);
}

/*  外部想画一个字符/标签（生命显示、道具池用）：同样走 CK 直写  */
void game_hud_char_at(u32 fb, int x, int y, int asset)   { hud_glyph(fb, x, y, asset); }
void game_hud_label_at(u32 fb, int x, int y, const char *t) { hud_label(fb, x, y, t); }
void game_hud_number_at(u32 fb, int x, int y, int v, int d) { hud_number(fb, x, y, v, d); }
/*  让别的模块标脏"面板某一行"（擦除交给帧首的脏区恢复）  */
void game_hud_dirty_row(dirty_map_t dirty, int row)
{
    game_mark_hud_dirty(dirty, HUD_PLATE_X, row * HUD_ROW_H, HUD_PLATE_W, HUD_ROW_H);
}

/*----------------------------------------------------------------------------*/
/*  铺完地砖时画一次：只画【标签】，不画数字占位。
 *  ★ 以前这里给每行画 "0" 占位，数字又是 CK 精灵（透明像素不写）：
 *    相机一移动、标签重画一次，0 就叠在真实数字的透明缝里 ⇒ 屏幕上
 *    "00 和真实数字重叠"。数字统一由 game_hud_frame() 在更高的 z 层画。  */
/*  ★ 每行的标签/位数只写这一份：静态层和每帧重画都用它。
 *    以前静态层把 12 个标签手抄一遍，改了名字（或加一行）两处就会对不上。
 *    0 = 这一行没有标签（第 4 行是血量，由 game_vital.c 画）。  */
static const char *const s_row_label[HUD_CELLS_Y] = {
    "FPS", "BULLETS", "CMD", "STAGE", 0,
    "DMG", "MOVE", "BSPD", "RANGE", "RATE",
    "COINS", "BOMB"
};
static const unsigned char s_row_digits[HUD_CELLS_Y] = {
    2, 3, 3, 2, 0, 2, 2, 2, 3, 2, 3, 1
};

static const char *row_label(int row)
{
    return row == 5 && g.test_mode ? "TEST" : s_row_label[row];
}

/* 只恢复字模实际覆盖的范围，面板本身没有不透明背景。
 * 生命行整体标脏，以覆盖心形和上一次可能显示的额外命数标签。 */
void game_hud_mark_at(dirty_map_t dirty, int camx, int camy)
{
    int i;
    for (i = 0; i < HUD_CELLS_Y; ++i) {
        int len = 0;
        if (!s_row_label[i]) {
            game_mark_dirty(dirty, camx, camy + i * HUD_ROW_H,
                            HUD_PLATE_W, HUD_ROW_H);
            continue;
        }
        while (s_row_label[i][len]) ++len;
        if (i == 5) len = 4; /* 关闭测试模式时也覆盖上一次的 TEST 标签。 */
        game_mark_dirty(dirty, camx + HUD_LABEL_X, camy + i * HUD_ROW_H,
                        len * 16, 24);
        game_mark_dirty(dirty, camx + HUD_VALUE_X, camy + i * HUD_ROW_H,
                        s_row_digits[i] * 16, 24);
    }
}

void game_hud_static(u32 fb)
{
    int i;

    fb = game_view_fb(fb);
    for (i = 0; i < HUD_CELLS_Y; ++i)
        if (s_row_label[i]) hud_label(fb, HUD_LABEL_X, i * HUD_ROW_H, row_label(i));
}

/*----------------------------------------------------------------------------*/
/*  每帧：只重画"数值变了"的那些行  */
/*  这一帧各行要显示的值（面板和"帧首标脏"共用一份，别两处各算一遍）  */
static void hud_values(int fps, int cmd_frame, int total_bullets, int coins, int v[HUD_CELLS_Y])
{
    v[0] = fps;
    v[1] = total_bullets;
    v[2] = cmd_frame;
    v[3] = g.stage;
    v[4] = 0;                                   /* 血量行由 game_vital 负责 */
    v[5] = game_player_hit_damage(BOSS_HIT_DMG);
    v[6] = g.stats.move_v8 / 8;
    v[7] = g.stats.bullet_speed;
    v[8] = g.stats.range;
    v[9] = 60 / (g.stats.fire_every > 0 ? g.stats.fire_every : (int)FIRE_EVERY);
    v[10] = coins;
    v[11] = g.bomb_count;
}

/*  ★ 帧首调用：把"和这块缓冲上次画的不一样"的行标脏（旧文字由脏区恢复擦掉）。
 *    双缓冲各记各的 ⇒ 两块都不会闪、也不会互相留残影。  */
static unsigned char s_draw[GAME_FB_COUNT][HUD_CELLS_Y]; /* per-buffer row flags */
static int s_last[GAME_FB_COUNT][HUD_CELLS_Y];              /* 每块缓冲上一次画的值（-1 = 没画过） */
static int s_init;

/*  ★ 整局重开 / 铺完地砖之后必须调一次：把两块缓冲的"上次值"作废，
 *    否则新画面里这些行永远不重画（重开时 GUI 空白就是这么来的）。  */
void game_hud_invalidate(void)
{
    int b, i;

    for (b = 0; b < GAME_FB_COUNT; ++b)
        for (i = 0; i < HUD_CELLS_Y; ++i) s_last[b][i] = -1;
    for (b = 0; b < GAME_FB_COUNT; ++b)
        for (i = 0; i < HUD_CELLS_Y; ++i) s_draw[b][i] = 1;
    s_init = 0;
}

/*  某一行的格子里有没有"别的精灵留下的脏"（有 ⇒ 这一行的字被擦掉了，要补画）  */
static int row_touched(dirty_map_t dirty, int row)
{
    int tx, ty;
    int y0 = g.cam_y + row * HUD_ROW_H;
    int x0 = g.cam_x;
    int x1 = g.cam_x + HUD_PLATE_W - 1;

    for (ty = y0 / TILE; ty <= (y0 + HUD_ROW_H - 1) / TILE && ty < GRID_H; ++ty)
        for (tx = x0 / TILE; tx <= x1 / TILE && tx < GRID_W; ++tx)
            if (dirty_get(dirty, tx, ty)) return 1;
    return 0;
}

/*  ★ 第二次检查（在 game_vital_prepare / game_vital_mark_dirty 之后调用）：
 *    脏区恢复是按 32×32 瓦片擦的，cam_y 不是 32 的倍数时，一行的脏区会跨
 *    两行瓦片 ⇒ 相邻的 HUD 行也会被擦掉。这里把所有"所占瓦片被擦过"的行
 *    补进 s_draw（只置重画位，不重复标脏），这样只补画真正被擦到的行。  */
int game_hud_prepare_overlap(dirty_map_t dirty, int buf)
{
    int i, any = 0;

    if (buf < 0 || buf >= GAME_FB_COUNT) buf = 0;
    for (i = 0; i < HUD_CELLS_Y; ++i) {
        if (!row_touched(dirty, i)) continue;
        any = 1;
        /* 第 4 行由 hud_vital 在本帧补画；不作废另一块缓冲的生命值。 */
        if (i != 4) s_draw[buf][i] = 1;
    }
    return any;
}

int game_hud_prepare(dirty_map_t dirty, int buf,
                     int fps, int cmd_frame, int total_bullets, int coins)
{
    int any = 0;
    int v[HUD_CELLS_Y];
    int i;

    hud_values(fps, cmd_frame, total_bullets, coins, v);

    if (!s_init) {
        for (i = 0; i < HUD_CELLS_Y; ++i) { int b; for (b = 0; b < GAME_FB_COUNT; ++b) s_last[b][i] = -1; }
        s_init = 1;
    }

    if (buf < 0 || buf >= GAME_FB_COUNT) buf = 0;

    for (i = 0; i < HUD_CELLS_Y; ++i) {
        if (i == 4) continue;

        /* 缓存中同时包含模式和伤害值，即使当前配置与普通模式相同。 */
        int key = v[i] + (i == 5 && g.test_mode ? 1000 : 0);
        if (s_last[buf][i] != key) {           /* ① 数值变了：标脏 + 重画这一行 */
            s_last[buf][i] = key;
            s_draw[buf][i] = 1;
            any = 1;
            game_hud_dirty_row(dirty, i);
        } else if (row_touched(dirty, i)) {     /* ② 有精灵压到这一行：只补画这一行 */
            s_draw[buf][i] = 1;                 /*    地砖已由那个精灵的脏区恢复 */
            any = 1;
        }
    }

    return any;
}

/*  绘制：把标脏过的行画出来（没标脏的不用管）  */
void game_hud_frame(u32 fb, int buf, int fps, int cmd_frame, int total_bullets, int coins, int full)
{
    int v[HUD_CELLS_Y];
    int i;

    fb = game_view_fb(fb);
    hud_values(fps, cmd_frame, total_bullets, coins, v);
    if (buf < 0 || buf >= GAME_FB_COUNT) buf = 0;

    for (i = 0; i < HUD_CELLS_Y; ++i) {
        if (i == 4) continue;
        if (!full && !s_draw[buf][i]) continue;
        hud_row(fb, i, row_label(i), v[i], s_row_digits[i]);
        s_draw[buf][i] = 0;
    }
}

/*----------------------------------------------------------------------------*/
/*  GAME OVER：三行大字（居中），main 每帧调一次  */
void game_hud_gameover(u32 fb, int score, int secs)
{
    int x = GAMEOVER_X, y = GAMEOVER_Y;
    int w = GAMEOVER_W, h = GAMEOVER_H;

    fb = game_view_fb(fb);
    /*  窗口 = 不透明窗底 + 四条边  */
    CI_gpu2d_fillRect(fb, (u32)x, (u32)y, (u32)w, (u32)h, 0x00101820u);
    CI_gpu2d_fillRect(fb, (u32)x, (u32)y,               (u32)w, 4u, 0x00C04040u);
    CI_gpu2d_fillRect(fb, (u32)x, (u32)(y + h - 4),     (u32)w, 4u, 0x00C04040u);
    CI_gpu2d_fillRect(fb, (u32)x, (u32)y,               4u, (u32)h, 0x00C04040u);
    CI_gpu2d_fillRect(fb, (u32)(x + w - 4), (u32)y,     4u, (u32)h, 0x00C04040u);

    /*  三行：GAME OVER / SCORE nnnnn / RESTART IN n  */
    hud_label(fb, x + (w - 9 * 16) / 2, y + 22, "GAME OVER");

    hud_label(fb,  x + 48, y + 70, "SCORE");
    hud_number(fb, x + 48 + 5 * 16 + 8, y + 70, score, 5);

    hud_label(fb,  x + 32, y + 112, "RESTART");
    hud_label(fb,  x + 32 + 7 * 16 + 8, y + 112, "IN");
    hud_number(fb, x + 32 + 9 * 16 + 16, y + 112, secs, 1);
}

/*----------------------------------------------------------------------------*/
/*  Boss 名称与血条：名称固定在血条上方，进入锁血换阶段时仍持续绘制。 */
void game_hud_boss_bar(u32 fb, int hp, int hp_max)
{
    int fill = (hp_max > 0) ? (HUD_HP_W * hp) / hp_max : 0;

    fb = game_view_fb(fb);
    hud_label(fb, HUD_HP_LABEL_X, HUD_HP_LABEL_Y, "BOSS");
    CI_gpu2d_fillRect(fb, (u32)HUD_HP_X, (u32)HUD_HP_Y, (u32)HUD_HP_W, (u32)HUD_HP_H, 0x00202020u);
    if (fill > 0)
        CI_gpu2d_fillRect(fb, (u32)HUD_HP_X, (u32)HUD_HP_Y, (u32)fill, (u32)HUD_HP_H, 0x00E03030u);
}

/*----------------------------------------------------------------------------*/
void game_hud_toast(u32 fb, const char *text)
{
    int len = 0;
    const char *p = text;

    fb = game_view_fb(fb);
    while (*p++) ++len;
    hud_label(fb, HUD_TOAST_X + (HUD_TOAST_W - len * 16) / 2,
              HUD_TOAST_Y, text);
}

/*----------------------------------------------------------------------------*/
/*  ★ 面板是不透明底板 ⇒ 不再需要"标脏让地砖盖上来"那套（重影就是从那儿来的）  */
void game_hud_mark_dirty(dirty_map_t dirty)
{
    (void)dirty;
}
