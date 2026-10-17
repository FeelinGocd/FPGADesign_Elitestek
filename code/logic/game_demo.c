/* DEBUG prototype: KEY0 left, KEY1 right, KEY2 down, KEY3 up.
 * Draw only into the back buffer; restore each buffer's own old rectangles.
 * Side walk explicitly ends on standUp. PERF_TEST_MODE can replace the
 * moving model with fixed bullet levels for throughput measurements.
 */
#include "game_demo.h"
#include "CI_gpu2d.h"
#include "user_gpu2d.h"
#include "user_lineBuffer.h"
#include "user_key.h"
#include "IRQ_frame.h"
#include "assets_desc.h"
#include "bsp.h"

#define SW 1280
#define SH 720
#define ROOM_W 960
#define ROOM_H 720
#define GUI_W 160
#define TILE 32
#define GRID_W (ROOM_W/TILE)
#define GRID_H ((ROOM_H+TILE-1)/TILE)
#define PW 56
#define PH 72
#define BW 24
#define BH 24
#define N_BULLETS 300
#define BG 0x182028u
#define FLOOR_BG 0x493326u
#define GUI_LEFT_BG 0x202838u
#define GUI_RIGHT_BG 0x382028u
#define FB_C 0x08000000u
#define FB_D 0x08800000u
#define ANIM_HOLD 6u
#define FIRE_EVERY 4u
#define PLAYER_MOVE_SPEED 3
#define BOSS_PATTERN_EVERY 8u
#define PLAYER_BULLET_SPEED 4
#define ENEMY_BULLET_SPEED 2
#define PICKUP_MS 1000u
#define HURT_MS 1000u
#define ITEM_W 32
#define ITEM_H 32
#define BOSS_W 80
#define BOSS_H 96
#define BOSS_FRAME_MS 250u
#define GUI_STATIC_CMD_COUNT 33
/*
 * Performance diagnosis:
 *   0 = normal moving demo
 *   1 = freeze the model and keep a fixed set of bullets, while still
 *       submitting the same redraw path every frame.
 *
 * This lets us compare CPU/model work against GPU/DDR/CI submission work
 * without changing the RTL.  Change back to 0 after collecting the log.
 */
#define PERF_TEST_MODE 0
#define PERF_LEVELS 5
enum { RIGHT, LEFT, DOWN, UP };
#define KEY_LEFT     0x1u
#define KEY_RIGHT    0x2u
#define KEY_DOWN     0x4u
#define KEY_UP       0x8u
typedef struct { int active,x,y,dx,dy,dir,enemy; } bullet_t;
typedef struct { int active,x,y; } old_t;
typedef struct { int active,x,y; } item_t;
static bullet_t bullets[N_BULLETS];
static old_t old_bullets[2][N_BULLETS], old_player[2], old_boss[2];
static old_t old_item[2];
static item_t item;
static u32 pickup_until;
static u32 hurt_until;
static int camera_x, camera_y;
static const int side_seq[] = {
    ASSET_bigfish_walkRight_00_standUp, ASSET_bigfish_walkRight_01_right,
    ASSET_bigfish_walkRight_00_standUp, ASSET_bigfish_walkRight_01_right,
    ASSET_bigfish_walkRight_00_standUp
};
static const int down_seq[] = {
    ASSET_bigfish_walkDown_00_standUp, ASSET_bigfish_walkDown_01_Right,
    ASSET_bigfish_walkDown_00_standUp, ASSET_bigfish_walkDown_01_Right,
    ASSET_bigfish_walkDown_00_standUp
};
static const u32 down_flip[] = { 0u, 0u, 0u, GPU2D_FLIP_X, 0u };
static const int up_seq[] = {
    ASSET_bigfish_walkUp_00_standUp, ASSET_bigfish_walkUp_01_right,
    ASSET_bigfish_walkUp_00_standUp, ASSET_bigfish_walkUp_01_right,
    ASSET_bigfish_walkUp_00_standUp
};
static const u32 up_flip[] = { 0u, 0u, 0u, GPU2D_FLIP_X, 0u };
#if PERF_TEST_MODE != 0
static const int perf_counts[PERF_LEVELS] = { 16, 32, 64, 96, 128 };
#endif

static u32 milliseconds(void)
{
    return (u32)(clint_getTime(BSP_CLINT)/(BSP_CLINT_HZ/1000u));
}

/* Static HUD labels use the 16x24 English glyphs from PRAM.  The room starts
   at GUI_W, so these labels are never touched by the world redraw path. */
static void draw_gui_label(u32 fb, int x, int y, const char *text)
{
    while(*text) {
        char c=*text++;
        if(c>='A' && c<='Z') {
            int ai=ASSET_English_A + (c-'A');
            CI_gpu2d_spriteAlphaRect(g_assets[ai].base_word,
                g_assets[ai].stride_word,fb,(u32)x,(u32)y,16u,24u,0u);
        }
        x+=16;
    }
}
static void draw_gui_number(u32 fb, int x, int y, int value, int digits)
{
    int i;
    if(value<0) value=0;
    for(i=digits-1;i>=0;--i) {
        int div=1,j;
        int digit;
        for(j=0;j<i;++j) div*=10;
        digit=(value/div)%10;
        {
            int ai=ASSET_num0_16x24+digit;
            CI_gpu2d_spriteAlphaRect(g_assets[ai].base_word,
                g_assets[ai].stride_word,fb,(u32)x,(u32)y,16u,24u,0u);
        }
        x+=16;
    }
}
static void draw_gui_dynamic(u32 fb, int fps, int cmd_frame,
                             int total_bullets)
{
    /* Clear only value slots; labels and the room remain untouched. */
    CI_gpu2d_fillRect(fb,64u,88u,96u,24u,GUI_LEFT_BG);
    CI_gpu2d_fillRect(fb,8u,160u,144u,24u,GUI_LEFT_BG);
    CI_gpu2d_fillRect(fb,8u,208u,144u,24u,GUI_LEFT_BG);
    draw_gui_number(fb,64,88,fps,2);
    draw_gui_number(fb,8,160,total_bullets,3);
    draw_gui_number(fb,8,208,cmd_frame,3);
}

static void draw_gui_static(u32 fb)
{
    const int rx=GUI_W+ROOM_W+8;
    /* Left panel: system/performance. */
    draw_gui_label(fb,8,24,"SYSTEM");
    draw_gui_label(fb,8,88,"FPS");
    draw_gui_label(fb,8,136,"BULLETS");
    draw_gui_label(fb,8,184,"CMD");
    /* Right panel: game state. */
    draw_gui_label(fb,rx,24,"GAME");
    draw_gui_label(fb,rx,160,"PLAYER");
    draw_gui_label(fb,rx,232,"BOSS");
}
static void draw_floor_world(u32 fb)
{
    const asset_desc_t *wood=&g_assets[ASSET_minecraft_wood];
    int wx,wy;

    CI_gpu2d_fillRect(fb,0u,0u,SW,SH,GUI_LEFT_BG);
    CI_gpu2d_fillRect(fb,GUI_W,0u,ROOM_W,ROOM_H,FLOOR_BG);
    CI_gpu2d_fillRect(fb,GUI_W+ROOM_W,0u,GUI_W,SH,GUI_RIGHT_BG);
    draw_gui_static(fb);
    /* Tile the complete game viewport.  The final 16-pixel row is a clipped
       tile so no untextured strip is left at the bottom of the room. */
    for(wy=0;wy<ROOM_H;wy+=TILE) {
        int th=(wy+TILE<=ROOM_H) ? TILE : (ROOM_H-wy);
        for(wx=0;wx+TILE<=ROOM_W;wx+=TILE) {
            CI_gpu2d_spriteAlphaRect(wood->base_word,wood->stride_word,
                fb,(u32)(GUI_W+wx),(u32)wy,TILE,th,0u);
        }
    }
}
static u32 floor_cmd_count(void)
{
    return 3u + GUI_STATIC_CMD_COUNT + (ROOM_W/TILE) * ((ROOM_H+TILE-1)/TILE);
}
static void restore_world_rect(u32 fb,int x,int y,int w,int h)
{
    const asset_desc_t *wood=&g_assets[ASSET_minecraft_wood];
    int x0=(x/TILE)*TILE, y0=(y/TILE)*TILE;
    int x1=((x+w-1)/TILE)*TILE, y1=((y+h-1)/TILE)*TILE;
    int tx,ty;

    CI_gpu2d_fillRect(fb,(u32)(GUI_W+x),(u32)y,(u32)w,(u32)h,FLOOR_BG);
    for(ty=y0;ty<=y1 && ty<ROOM_H;ty+=TILE) {
        int th=(ty+TILE<=ROOM_H) ? TILE : (ROOM_H-ty);
        for(tx=x0;tx<=x1 && tx+TILE<=ROOM_W;tx+=TILE) {
            CI_gpu2d_spriteAlphaRect(wood->base_word,wood->stride_word,
                fb,(u32)(GUI_W+tx),(u32)ty,TILE,th,0u);
        }
    }
}
static void mark_dirty_cells(unsigned char dirty[GRID_H][GRID_W],
                              int x, int y, int w, int h)
{
    int tx0=x/TILE, ty0=y/TILE;
    int tx1=(x+w-1)/TILE, ty1=(y+h-1)/TILE;
    int tx,ty;
    if(tx0<0) tx0=0;
    if(ty0<0) ty0=0;
    if(tx1>=GRID_W) tx1=GRID_W-1;
    if(ty1>=GRID_H) ty1=GRID_H-1;
    for(ty=ty0;ty<=ty1;++ty)
        for(tx=tx0;tx<=tx1;++tx)
            dirty[ty][tx]=1u;
}
static void restore_dirty_cells(u32 fb,
                                unsigned char dirty[GRID_H][GRID_W])
{
    const asset_desc_t *wood=&g_assets[ASSET_minecraft_wood];
    int tx,ty;
    for(ty=0;ty<GRID_H;++ty) for(tx=0;tx<GRID_W;++tx)
        if(dirty[ty][tx]) {
            int th=(ty*TILE+TILE<=ROOM_H) ? TILE : (ROOM_H-ty*TILE);
            CI_gpu2d_fillRect(fb,(u32)(GUI_W+tx*TILE),(u32)(ty*TILE),
                              TILE,(u32)th,FLOOR_BG);
            CI_gpu2d_spriteAlphaRect(wood->base_word,wood->stride_word,fb,
                (u32)(GUI_W+tx*TILE),(u32)(ty*TILE),TILE,(u32)th,0u);
        }
}

static int visible(int x,int y,int w,int h)
{
    int sx=x, sy=y;
    /* The current CI API has no source clipping.  Keep the whole sprite
       inside the viewport so a negative destination never reaches RTL. */
    return sx>=0 && sy>=0 && sx+w<=SW && sy+h<=SH;
}
static void draw_item(u32 fb, int x, int y)
{
    const asset_desc_t *base=&g_assets[ASSET_accessory_base];
    const asset_desc_t *icon=&g_assets[ASSET_accessory_chili];
    CI_gpu2d_spriteAlphaRect(base->base_word,base->stride_word,
        fb,(u32)x,(u32)y,ITEM_W,ITEM_H,0u);
    CI_gpu2d_spriteAlphaRect(icon->base_word,icon->stride_word,
        fb,(u32)x,(u32)y,ITEM_W,ITEM_H,0u);
}
static void draw_boss(u32 fb,int x,int y,int frame)
{
    int ai;
    if(frame<=0) ai=ASSET_TheBoss_00_StandUp;
    else if(frame==1) ai=ASSET_TheBoss_01_StandUp;
    else ai=ASSET_TheBoss_02_StandUp;
    CI_gpu2d_spriteAlphaRect(g_assets[ai].base_word,g_assets[ai].stride_word,
        fb,(u32)(GUI_W+x),(u32)y,BOSS_W,BOSS_H,0u);
}
static int finish_gpu(void)
{
    /* FENCE completion is a command-side event.  During initialization the
       display is disabled, so frame_done may not advance at all. */
    u32 before=gpu2d_getFrameDone();
    CI_gpu2d_fence();
    /* FENCE raises frame_done; it is deliberately excluded from cmd_done. */
    if(gpu2d_waitFrame(before,3000000u)!=0) {
        bsp_printf("DEBUG: GPU fence timeout\r\n");
        return 0;
    }
    return 1;
}
static int present(u32 fb,int cam_x,int cam_y)
{
    u32 before,wait=0;
    (void)cam_x;
    (void)cam_y;
    lineBuffer_setFullBase(fb,fb);
#if IRQ_FRAME_USE
    before=IRQ_frame_count();
    while(IRQ_frame_count()==before && wait<200000u) {
        bsp_uDelay(1u); ++wait;
    }
    if(IRQ_frame_count()==before) {
        bsp_printf("DEBUG: display IRQ timeout\r\n"); return 0;
    }
#else
    (void)before; (void)wait;
    bsp_uDelay(20000u);
#endif
    return 1;
}
static void fire(int x,int y,int dir)
{
    int i;
    for(i=0;i<N_BULLETS;++i) if(!bullets[i].active) {
        bullet_t *b=&bullets[i];
        b->x=x+(PW-BW)/2; b->y=y+(PH-BH)/2;
        b->dx=0; b->dy=0; b->dir=dir;
        if(dir==RIGHT) { b->x=x+PW; b->dx=PLAYER_BULLET_SPEED; }
        else if(dir==LEFT) { b->x=x-BW; b->dx=-PLAYER_BULLET_SPEED; }
        else if(dir==DOWN) { b->y=y+PH; b->dy=PLAYER_BULLET_SPEED; }
        else { b->y=y-BH; b->dy=-PLAYER_BULLET_SPEED; }
        b->enemy=0;
        b->active=1;
        return;
    }
}
static void fire_enemy(int x,int y,int dir)
{
    int i;
    for(i=0;i<N_BULLETS;++i) if(!bullets[i].active) {
        bullet_t *b=&bullets[i];
        b->x=x; b->y=y; b->dx=0; b->dy=0; b->dir=dir; b->enemy=1;
        if(dir==RIGHT) b->dx=ENEMY_BULLET_SPEED;
        else if(dir==LEFT) b->dx=-ENEMY_BULLET_SPEED;
        else if(dir==DOWN) b->dy=ENEMY_BULLET_SPEED;
        else b->dy=-ENEMY_BULLET_SPEED;
        b->active=1;
        return;
    }
}
static void boss_fire_pattern(int pattern, int boss_x, int boss_y)
{
    const int cx=boss_x+BOSS_W/2-BW/2;
    const int y=boss_y+BOSS_H;
    if(pattern==0) {
        /* Four-lane downward spread. */
        fire_enemy(cx-24,y,DOWN);
        fire_enemy(cx-8,y,DOWN);
        fire_enemy(cx+8,y,DOWN);
        fire_enemy(cx+24,y,DOWN);
    } else if(pattern==1) {
        /* Seven-lane downward spread. */
        fire_enemy(cx-48,y,DOWN);
        fire_enemy(cx-32,y,DOWN);
        fire_enemy(cx-16,y,DOWN);
        fire_enemy(cx,y,DOWN);
        fire_enemy(cx+16,y,DOWN);
        fire_enemy(cx+32,y,DOWN);
        fire_enemy(cx+48,y,DOWN);
    } else if(pattern==2) {
        /* Ten-shot horizontal cross sweep. */
        fire_enemy(cx,y+4,LEFT);
        fire_enemy(cx,y+16,LEFT);
        fire_enemy(cx,y+28,LEFT);
        fire_enemy(cx,y+40,LEFT);
        fire_enemy(cx,y+52,LEFT);
        fire_enemy(cx,y+4,RIGHT);
        fire_enemy(cx,y+16,RIGHT);
        fire_enemy(cx,y+28,RIGHT);
        fire_enemy(cx,y+40,RIGHT);
        fire_enemy(cx,y+52,RIGHT);
    } else {
        /* Thirteen-lane suppression pattern for the density test. */
        fire_enemy(cx-96,y,DOWN);
        fire_enemy(cx-80,y,DOWN);
        fire_enemy(cx-64,y,DOWN);
        fire_enemy(cx-48,y,DOWN);
        fire_enemy(cx-32,y,DOWN);
        fire_enemy(cx-16,y,DOWN);
        fire_enemy(cx,y,DOWN);
        fire_enemy(cx+16,y,DOWN);
        fire_enemy(cx+32,y,DOWN);
        fire_enemy(cx+48,y,DOWN);
        fire_enemy(cx+64,y,DOWN);
        fire_enemy(cx+80,y,DOWN);
        fire_enemy(cx+96,y,DOWN);
    }
}
static void move_bullets(u32 now,int px,int py)
{
    int i;
    for(i=0;i<N_BULLETS;++i) if(bullets[i].active) {
        bullet_t *b=&bullets[i];
        b->x+=b->dx; b->y+=b->dy;
        if(b->enemy && b->x < px+PW && b->x+BW > px &&
           b->y < py+PH && b->y+BH > py+PH-24) {
            hurt_until=now+HURT_MS;
#if PERF_TEST_MODE == 2
            b->x=16+(i%24)*40;
            b->y=40+(i/24)*80;
#else
            b->active=0;
            continue;
#endif
        }
        if(b->x<0 || b->x+BW>ROOM_W || b->y<0 || b->y+BH>ROOM_H) {
#if PERF_TEST_MODE == 2
            /* Keep the dynamic benchmark at a constant 128 active sprites;
               gameplay mode still removes bullets at the room boundary. */
            b->x=16+(i%24)*40;
            b->y=40+(i/24)*80;
            b->dx=(i%3)-1;
            b->dy=((i/3)%3)-1;
            if(b->dx==0 && b->dy==0) b->dy=1;
#else
            b->active=0; continue;
#endif
        }
        (void)now;
    }
}
static void draw_bullet(u32 fb,const bullet_t *b)
{
    /* Player shots stay type A permanently.  Enemy shots use type B;
       picking up an item never changes an already defined faction. */
    int ai=b->enemy ? ASSET_bullet_type_B_chiliFrie_DONTROT
                    : ASSET_bullet_type_A;
    u32 flip=0u;
    if(ai==ASSET_bullet_type_B_chiliFrie_DONTROT) {
        /* The B asset is authored in its final direction.  Only mirror it
           for left-facing shots; do not apply the ROT90 path. */
        if(b->dir==LEFT) flip=GPU2D_FLIP_X;
    } else if(b->dir==LEFT) flip=GPU2D_FLIP_X;
    /* ROT90 is a clockwise image rotation in screen coordinates: the
       right-facing source token becomes a downward-facing token.  MY then
       reverses the output rows for the upward-facing variant. */
    else if(b->dir==DOWN) flip=GPU2D_ROT90;
    else if(b->dir==UP) flip=GPU2D_ROT90|GPU2D_FLIP_Y;
    if(visible(b->x,b->y,BW,BH))
        CI_gpu2d_spriteAlphaRect(g_assets[ai].base_word,g_assets[ai].stride_word,
            fb,(u32)(GUI_W+b->x),(u32)b->y,BW,BH,flip);
}
#if PERF_TEST_MODE != 0
static void perf_fill_bullets(int count)
{
    int i;
    for(i=0;i<N_BULLETS;++i) {
        bullets[i].active=(i<count);
        bullets[i].x=16+(i%24)*40;
        bullets[i].y=40+(i/24)*80;
        bullets[i].dx=0;
        bullets[i].dy=0;
        bullets[i].dir=(i&1) ? RIGHT : LEFT;
        bullets[i].enemy=(i&1) ? 1 : 0;
    }
}
#endif

void game_demo_run(void)
{
    int bi,i,disp=0,x=(ROOM_W-PW)/2,y=(ROOM_H-PH)/2,dir=DOWN,moving=0;
    int boss_x=(ROOM_W-BOSS_W)/2,boss_y=64,boss_frame;
    int was_moving=0,last_dir=DOWN,ai;
    static const int boss_seq[5]={0,1,2,1,0};
    u32 anim=0,fire_tick=0,enemy_tick=0,now,init_cmd0;
    int boss_pattern=0;
    u32 fps_epoch=0u, fps_frames=0u;
    int fps_value=0, cmd_value=0;
    u32 cmd_frame_before=0u;
    u32 probe_epoch=0u, probe_frames=0u, probe_frame_no=0u;
    int probe_peak=0, probe_peak_frame=0, probe_peak_cmd=0;
#if PERF_TEST_MODE != 0
    u32 perf_frames=0, perf_t0=0, perf_cmd0=0;
#if PERF_TEST_MODE == 1
    int perf_level=0;
#endif
#endif
    const u32 fb[2]={FB_C,FB_D};
    {
        u32 seed=milliseconds();
        item.active=1;
        item.x=128 + (int)(((seed*1103515245u + 12345u) >> 8) % ((ROOM_W-256)/32))*32;
        item.y=128 + (int)(((seed*1664525u + 1013904223u) >> 8) % 12u)*32;
        camera_x=0;
        camera_y=0;
        pickup_until=0u;
        hurt_until=0u;
    }
    bsp_printf("\r\nDEBUG: KEY0=left KEY1=right KEY2=down KEY3=up; auto fire\r\n");
#if PERF_TEST_MODE == 1
    bsp_printf("PERF TEST: fixed GUI/room, bullet levels 16/32/64/96/128\r\n");
#elif PERF_TEST_MODE == 2
    bsp_printf("PERF TEST: dynamic model, 192 moving bullets\r\n");
#endif
    for(i=0;i<N_BULLETS;++i) {
        bullets[i].active=0;
        old_bullets[0][i].active=old_bullets[1][i].active=0;
    }
    old_item[0].active=old_item[1].active=0;
    old_boss[0].active=old_boss[1].active=0;
    bsp_printf("DEBUG: accessory at (%d,%d), bullet=A\r\n",item.x,item.y);
#if PERF_TEST_MODE == 1
    /* Keep a stable command count so movement/collision does not affect the
       comparison.  The first level is active immediately after init. */
    perf_fill_bullets(perf_counts[0]);
#elif PERF_TEST_MODE == 2
    perf_fill_bullets(N_BULLETS);
    for(i=0;i<N_BULLETS;++i) {
        bullets[i].dx=(i%3)-1;
        bullets[i].dy=((i/3)%3)-1;
        if(bullets[i].dx==0 && bullets[i].dy==0) bullets[i].dy=1;
    }
#endif
    old_player[0].active=old_player[1].active=0;
    bsp_printf("PERF INIT 1: disable display\r\n");
    init_cmd0=gpu2d_getCmdDone();
    gpu2d_exclusive_begin();
    bsp_printf("PERF INIT 2: set full-screen mode\r\n");
    lineBuffer_setSplit(0);
    /* Allow the display boundary to latch full-screen geometry before drawing. */
    bsp_printf("PERF INIT 3: wait display boundary\r\n");
    bsp_uDelay(20000u);
    bsp_printf("PERF INIT 4: clear both framebuffers\r\n");
    for(bi=0;bi<2;++bi) {
        bsp_printf("PERF INIT 5.%d: submit clear/glass\r\n", bi);
        draw_floor_world(fb[bi]);
    }
    bsp_printf("PERF INIT 6: wait GPU command completion\r\n");
    lineBuffer_setFullBase(FB_C,FB_D);
    if(gpu2d_exclusive_end(init_cmd0 +
                           2u * floor_cmd_count()) != 0) {
        bsp_printf("PERF INIT ERROR: initial DDR fill timeout\r\n");
        return;
    }
    bsp_printf("DEBUG: fixed room=%dx%d, GUI left/right=%d, wood tiles fill room\r\n",
               ROOM_W,ROOM_H,GUI_W);
    bsp_printf("PERF INIT 7: GPU complete, display re-enabled\r\n");
    if(!present(FB_C,camera_x,camera_y)) return;
#if PERF_TEST_MODE != 0
    perf_t0=milliseconds();
    perf_cmd0=gpu2d_getCmdDone();
#endif
    for(;;) {
        u32 keys=key_getData()&15u;
        int nx=x,ny=y;
        int frame_total_bullets=0;
        unsigned char dirty_cells[GRID_H][GRID_W];
        int dirty_y,dirty_x;
        now=milliseconds();
        for(dirty_y=0;dirty_y<GRID_H;++dirty_y)
            for(dirty_x=0;dirty_x<GRID_W;++dirty_x)
                dirty_cells[dirty_y][dirty_x]=0u;
        cmd_frame_before=gpu2d_getCmdDone();
        if(fps_epoch==0u) fps_epoch=now;
        ++fps_frames;
        if((u32)(now-fps_epoch)>=1000u) {
            fps_value=(int)((fps_frames*1000u)/(now-fps_epoch));
            fps_frames=0u;
            fps_epoch=now;
        }
        moving=0;
#if PERF_TEST_MODE == 1
        /* Deliberately skip movement, animation, firing and collision model.
           The draw/erase/glass/CI/present path below remains unchanged. */
        (void)keys;
        (void)now;
#else
        /* One cardinal direction per frame, matching automatic firing.
           The board convention is KEY0/1/2/3 = left/right/down/up. */
        if(keys&KEY_LEFT)       { nx-=PLAYER_MOVE_SPEED; dir=LEFT; }
        else if(keys&KEY_RIGHT) { nx+=PLAYER_MOVE_SPEED; dir=RIGHT; }
        else if(keys&KEY_DOWN)  { ny+=PLAYER_MOVE_SPEED; dir=DOWN; }
        else if(keys&KEY_UP)    { ny-=PLAYER_MOVE_SPEED; dir=UP; }
        if(nx<0) nx=0;
        if(nx>ROOM_W-PW) nx=ROOM_W-PW;
        if(ny<0) ny=0;
        if(ny>ROOM_H-PH) ny=ROOM_H-PH;
        camera_x=0;
        camera_y=0;
        moving=(nx!=x || ny!=y);
        x=nx; y=ny;
        if(item.active && x < item.x+ITEM_W && x+PW > item.x &&
           y < item.y+ITEM_H && y+PH > item.y) {
            item.active=0;
            pickup_until=now+PICKUP_MS;
            bsp_printf("DEBUG: accessory picked; player bullet remains type A\r\n");
        }
        if(!moving || !was_moving || dir!=last_dir) anim=0;
        else anim=(anim+1u)%(5u*ANIM_HOLD);
        if(++fire_tick>=FIRE_EVERY) { fire_tick=0; fire(x,y,dir); }
        if(++enemy_tick>=BOSS_PATTERN_EVERY) {
            enemy_tick=0;
            boss_fire_pattern(boss_pattern,boss_x,boss_y);
            boss_pattern=(boss_pattern+1)&3;
        }
        move_bullets(now,x,y);
#endif
        was_moving=moving; last_dir=dir;
        if(dir==DOWN)
            ai=down_seq[(moving?anim:0u)/ANIM_HOLD];
        else if(dir==UP)
            ai=up_seq[(moving?anim:0u)/ANIM_HOLD];
        else
            ai=side_seq[(moving?anim:0u)/ANIM_HOLD];
        bi=1-disp;
        boss_frame=boss_seq[(now/BOSS_FRAME_MS)%5u];
        /* Merge all old sprite footprints into 32x32 wood cells.  A cell is
           restored once even when several bullets occupied it. */
        if(old_boss[bi].active)
            mark_dirty_cells(dirty_cells,old_boss[bi].x,old_boss[bi].y,BOSS_W,BOSS_H);
        if(old_player[bi].active)
            mark_dirty_cells(dirty_cells,old_player[bi].x,old_player[bi].y,PW,PH);
        if(old_item[bi].active)
            mark_dirty_cells(dirty_cells,old_item[bi].x,old_item[bi].y,ITEM_W,ITEM_H);
        for(i=0;i<N_BULLETS;++i) if(old_bullets[bi][i].active)
            mark_dirty_cells(dirty_cells,old_bullets[bi][i].x,old_bullets[bi][i].y,BW,BH);
        restore_dirty_cells(fb[bi],dirty_cells);
        {
            int player_bullets=0, enemy_bullets=0;
            for(i=0;i<N_BULLETS;++i) if(bullets[i].active) {
                if(bullets[i].enemy) ++enemy_bullets;
                else ++player_bullets;
            }
            frame_total_bullets=player_bullets+enemy_bullets;
            draw_gui_dynamic(fb[bi],fps_value,cmd_value,
                             frame_total_bullets);
        }
        {
            int picking = (pickup_until!=0u && (u32)(pickup_until-now)<PICKUP_MS);
            int hurt = (hurt_until!=0u && (u32)(hurt_until-now)<HURT_MS);
            u32 player_flip = (dir==LEFT) ? GPU2D_FLIP_X : 0u;
            if (hurt) {
                ai=ASSET_bigfish_Get_hurt;
                player_flip=0u;
            } else if (picking) {
                ai=ASSET_bigfish_Get_accessory;
                player_flip=0u;
            } else if (dir==DOWN) player_flip = down_flip[(moving?anim:0u)/ANIM_HOLD];
            else if (dir==UP) player_flip = up_flip[(moving?anim:0u)/ANIM_HOLD];
            if(visible(x,y,PW,PH))
                CI_gpu2d_spriteFlipCK(g_assets[ai].base_word,g_assets[ai].stride_word,
                    fb[bi],(u32)(GUI_W+x),(u32)y,PW,PH,player_flip);
            if (picking && !hurt)
                CI_gpu2d_spriteAlphaRect(g_assets[ASSET_accessory_chili].base_word,
                    g_assets[ASSET_accessory_chili].stride_word,fb[bi],
                    (u32)(GUI_W+x+12),(u32)y,ITEM_W,ITEM_H,0u);
        }
        draw_boss(fb[bi],boss_x,boss_y,boss_frame);
        old_boss[bi].active=1; old_boss[bi].x=boss_x; old_boss[bi].y=boss_y;
        old_player[bi].active=1; old_player[bi].x=x; old_player[bi].y=y;
        old_item[bi].active=item.active;
        old_item[bi].x=item.x; old_item[bi].y=item.y;
        if(item.active && visible(item.x,item.y,ITEM_W,ITEM_H))
            draw_item(fb[bi],GUI_W+item.x,item.y);
        for(i=0;i<N_BULLETS;++i) {
            old_bullets[bi][i].active=bullets[i].active;
            old_bullets[bi][i].x=bullets[i].x; old_bullets[bi][i].y=bullets[i].y;
            if(bullets[i].active) draw_bullet(fb[bi],&bullets[i]);
        }
        if(!finish_gpu() || !present(fb[bi],camera_x,camera_y)) return;
        cmd_value=(int)(gpu2d_getCmdDone()-cmd_frame_before);
        ++probe_frames;
        ++probe_frame_no;
        if(probe_epoch==0u) probe_epoch=now;
        if(frame_total_bullets>probe_peak) {
            probe_peak=frame_total_bullets;
            probe_peak_frame=(int)probe_frame_no;
            probe_peak_cmd=cmd_value;
        }
        if((u32)(now-probe_epoch)>=1000u) {
            u32 probe_ms=now-probe_epoch;
            int probe_fps=(int)((probe_frames*1000u)/(probe_ms?probe_ms:1u));
            bsp_printf("PROBE 1s: peak bullets=%d at frame=%d, fps=%d, frames=%d, cmd/frame=%d\r\n",
                       probe_peak,probe_peak_frame,probe_fps,(int)probe_frames,probe_peak_cmd);
            probe_epoch=now;
            probe_frames=0u;
            probe_peak=0;
            probe_peak_frame=(int)probe_frame_no;
            probe_peak_cmd=0;
        }
        disp=bi;
#if PERF_TEST_MODE != 0
        if(++perf_frames>=120u) {
            u32 dt=milliseconds()-perf_t0;
            u32 dc=gpu2d_getCmdDone()-perf_cmd0;
            if(dt==0u) dt=1u;
            /* This BSP printf supports the integer formats used elsewhere in
               the project; do not use %u here (it is parsed as float on this
               firmware build). */
#if PERF_TEST_MODE == 1
            bsp_printf("PERF bullets=%d: %d frames, %d ms, %d fps, %d cmd/frame\r\n",
                       perf_counts[perf_level],(int)perf_frames,(int)dt,
                       (int)((perf_frames*1000u)/dt),
                       (int)(dc/perf_frames));
            ++perf_level;
            if(perf_level>=PERF_LEVELS) {
                bsp_printf("PERF DONE: all bullet levels completed\r\n");
                return;
            }
            perf_fill_bullets(perf_counts[perf_level]);
            perf_frames=0u;
            perf_t0=milliseconds();
            perf_cmd0=gpu2d_getCmdDone();
#else
            bsp_printf("PERF dynamic: %d frames, %d ms, %d fps, %d cmd/frame\r\n",
                       (int)perf_frames,(int)dt,
                       (int)((perf_frames*1000u)/dt),
                       (int)(dc/perf_frames));
            perf_frames=0u;
            perf_t0=milliseconds();
            perf_cmd0=gpu2d_getCmdDone();
#endif
        }
#endif
    }
}
