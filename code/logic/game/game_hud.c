//==============================================================================
//  game_hud.c —— 左上角状态面板 + 右上角道具池 + Boss 血条 + 字幕/倒计时
//------------------------------------------------------------------------------
//  干什么      :
//    · 面板每一行 = 【纯色底板(FILL) + 标签(CK字模) + 数字(CK字模)】
//    · 只有"这一行的数变了"才重画那一行；没变就一条命令都不发
//  上一级是谁  : game/game_main.c（每帧 layer_hud）
//  接口契约    :
//    · 面板是【不透明】的：谁走到它后面都被盖住 → 不需要脏区恢复，也不留残影
//    · 数字必须定宽（digits 位），位数一变后面的内容会平移
//  为什么这么做:
//    · ★ 重影的根因：以前文字用 ALPHA 精灵画，同一个位置反复画不同数字时
//      α 混合会把旧数字留在下面（FPS 位置出现 00/59/60 三层叠影）。
//      ⇒ 每行先用 FILL 铺一块【不透明底板】把旧内容整块擦掉，再用 CK 字模直写。
//      底板是一条 FILL 命令，比"恢复 60 格地砖"便宜两个数量级。
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

    if (!game_visible(x, y, (int)a->width, (int)a->height)) return;
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
    int i;

    if (value < 0) value = 0;
    for (i = digits - 1; i >= 0; --i) {
        int div = 1, j;

        for (j = 0; j < i; ++j) div *= 10;
        hud_glyph(fb, x, y, ASSET_num0_16x24 + (value / div) % 10);
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
void game_hud_dirty_row(unsigned char dirty[GRID_H][GRID_W], int row)
{
    game_mark_dirty(dirty, HUD_PLATE_X, row * HUD_ROW_H, HUD_PLATE_W, HUD_ROW_H);
}

/*----------------------------------------------------------------------------*/
/*  开机铺地时画一次：所有标签（数字随后每帧按需补）  */
void game_hud_static(u32 fb)
{
    hud_row(fb, 0, "FPS",     0, 2);
    hud_row(fb, 1, "BULLETS", 0, 3);
    hud_row(fb, 2, "CMD",     0, 3);
    hud_row(fb, 3, "STAGE",   0, 2);
    /*  第 4 行 = 血量（game_vital.c 画）  */
    hud_row(fb, 5, "DMG",     0, 2);
    hud_row(fb, 6, "MOVE",    0, 2);
    hud_row(fb, 7, "BSPD",    0, 2);
    hud_row(fb, 8, "RANGE",   0, 3);
    hud_row(fb, 9, "RATE",    0, 2);
}

/*----------------------------------------------------------------------------*/
/*  每帧：只重画"数值变了"的那些行  */
/*  这一帧各行要显示的值（面板和"帧首标脏"共用一份，别两处各算一遍）  */
static void hud_values(int fps, int cmd_frame, int total_bullets, int v[10])
{
    v[0] = fps;
    v[1] = total_bullets;
    v[2] = cmd_frame;
    v[3] = g.stage;
    v[4] = 0;                                   /* 血量行由 game_vital 负责 */
    v[5] = BOSS_HIT_DMG;
    v[6] = PLAYER_MAX_V8 / 8;
    v[7] = g.fire_triple ? PLAYER_TRIPLE_SPEED : PLAYER_BULLET_SPEED;
    v[8] = PLAYER_BULLET_RANGE;
    v[9] = (int)(60u / FIRE_EVERY);
}

/*  ★ 帧首调用：把"和这块缓冲上次画的不一样"的行标脏（旧文字由脏区恢复擦掉）。
 *    双缓冲各记各的 ⇒ 两块都不会闪、也不会互相留残影。  */
static int s_draw[2][10];                       /* 本帧要重画的行（prepare 置位，frame 消费） */
static int s_last[2][10];                    /* 每块缓冲上一次画的值（-1 = 没画过） */
static int s_init;

/*  ★ 整局重开 / 铺完地砖之后必须调一次：把两块缓冲的"上次值"作废，
 *    否则新画面里这些行永远不重画（重开时 GUI 空白就是这么来的）。  */
void game_hud_invalidate(void)
{
    int b, i;

    for (b = 0; b < 2; ++b)
        for (i = 0; i < 10; ++i) s_last[b][i] = -1;
    for (b = 0; b < 2; ++b)
        for (i = 0; i < 10; ++i) s_draw[b][i] = 1;
    s_init = 0;
}

/*  某一行的格子里有没有"别的精灵留下的脏"（有 ⇒ 这一行的字被擦掉了，要补画）  */
static int row_touched(unsigned char dirty[GRID_H][GRID_W], int row)
{
    int tx, ty, y0 = row * HUD_ROW_H;

    for (ty = y0 / TILE; ty <= (y0 + HUD_ROW_H - 1) / TILE && ty < GRID_H; ++ty)
        for (tx = 0; tx <= 6 && tx < GRID_W; ++tx)
            if (dirty[ty][tx]) return 1;
    return 0;
}

int game_hud_prepare(unsigned char dirty[GRID_H][GRID_W], int buf,
                     int fps, int cmd_frame, int total_bullets)
{
    int any = 0;
    int v[10];
    int i;

    hud_values(fps, cmd_frame, total_bullets, v);

    if (!s_init) {
        for (i = 0; i < 10; ++i) { s_last[0][i] = -1; s_last[1][i] = -1; }
        s_init = 1;
    }

    if (buf < 0 || buf > 1) buf = 0;

    for (i = 0; i < 10; ++i) {
        if (i == 4) continue;

        if (s_last[buf][i] != v[i]) {           /* ① 数值变了：标脏 + 重画这一行 */
            s_last[buf][i] = v[i];
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
void game_hud_frame(u32 fb, int buf, int fps, int cmd_frame, int total_bullets, int full)
{
    static const char *labels[10] = { "FPS", "BULLETS", "CMD", "STAGE", 0,
                                      "DMG", "MOVE", "BSPD", "RANGE", "RATE" };
    static const int digits[10]   = { 2, 3, 3, 2, 0, 2, 2, 2, 3, 2 };
    int v[10];
    int i;

    hud_values(fps, cmd_frame, total_bullets, v);
    if (buf < 0 || buf > 1) buf = 0;

    for (i = 0; i < 10; ++i) {
        if (i == 4) continue;
        if (!full && !s_draw[buf][i]) continue;
        hud_row(fb, i, labels[i], v[i], digits[i]);
        s_draw[buf][i] = 0;
    }
}

/*----------------------------------------------------------------------------*/
/*  GAME OVER：三行大字（居中），main 每帧调一次  */
void game_hud_gameover(u32 fb, int score, int secs)
{
    int x = GAMEOVER_X, y = GAMEOVER_Y;
    int w = GAMEOVER_W, h = GAMEOVER_H;

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
/*  Boss 血条：顶部居中的一条，血量右端往左缩。  */
void game_hud_boss_bar(u32 fb, int hp, int hp_max)
{
    int fill = (hp_max > 0) ? (HUD_HP_W * hp) / hp_max : 0;

    hud_label(fb, HUD_HP_X + (HUD_HP_W - 4 * 16) / 2, HUD_HP_LABEL_Y, "BOSS");

    CI_gpu2d_fillRect(fb, (u32)HUD_HP_X, (u32)HUD_HP_Y, (u32)HUD_HP_W, (u32)HUD_HP_H, 0x00202020u);
    if (fill > 0)
        CI_gpu2d_fillRect(fb, (u32)HUD_HP_X, (u32)HUD_HP_Y, (u32)fill, (u32)HUD_HP_H, 0x00E03030u);
}

/*----------------------------------------------------------------------------*/
void game_hud_toast(u32 fb, const char *text)
{
    int len = 0;
    const char *p = text;

    while (*p++) ++len;
    hud_label(fb, (SCR_W - len * 16) / 2, HUD_TOAST_Y, text);
}

/*----------------------------------------------------------------------------*/
void game_hud_countdown(u32 fb, int n)
{
    hud_label(fb, (SCR_W - 5 * 16) / 2, HUD_CD_Y, "READY");
    if (n > 0 && n < 10)
        hud_number(fb, (SCR_W - 16) / 2, HUD_CD_Y + 34, n, 1);
}

/*----------------------------------------------------------------------------*/
void game_hud_banner(u32 fb, const char *text)
{
    int len = 0;
    const char *p = text;

    while (*p++) ++len;
    hud_label(fb, (SCR_W - len * 16) / 2, 120, text);
}

/*----------------------------------------------------------------------------*/
/*  ★ 面板是不透明底板 ⇒ 不再需要"标脏让地砖盖上来"那套（重影就是从那儿来的）  */
void game_hud_mark_dirty(unsigned char dirty[GRID_H][GRID_W])
{
    (void)dirty;
}
