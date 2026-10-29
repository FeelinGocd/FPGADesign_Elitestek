//==============================================================================
//  game.h —— 游戏 demo 的公共头（宏 / 类型 / 唯一状态 g / 模块接口）
//------------------------------------------------------------------------------
//  干什么      : 各模块共用的东西都在这里，别的文件不重复定义。
//  上一级是谁  : src/main.c → game_demo.h → game_demo_run()（game/game_main.c）
//  接口契约    :
//    · 状态只有一份：game_state.c 里的 g；别的模块只读写 g，不另立全局
//    · 坐标：普通房 = 屏幕坐标（世界 1280×736，cam=0）；Boss 房 = 1440×1120 世界坐标（房间 992×800），
//      显示窗口 1280×720 由 cam_x/cam_y 在 game_present 里移动（见 §6.9）
//      （换房 = 两块缓冲整屏重铺；Boss 房内才是"移动相机"）
//    · 素材一律 g_assets[ASSET_xxx]，不许写裸下标
//  分层        :
//    game_state.c  状态 / 时钟 / 初始化 / 上屏 / 换房内容布置
//    game_room.c   线性房间表 / 门触发 / 换房重铺
//    game_world.c  场景：地砖、脏区、可见性
//    game_hud.c    HUD
//    game_player.c 角色（移动 / 动画 / 开火）
//    game_bullet.c 子弹池
//    game_boss.c   Boss + 弹幕
//    game_item.c   道具（随机生成 / 拾取）
//    game_main.c   主循环（一帧四步）+ HUD 各块的提交
//    render_list.c 渲染队列：每个元素带自己的 z，统一排序后下发
//    game_assets.c 素材表唯一副本
//  参考        : docs/spec/软件_驱动与API.md §6（脏区）/ §8（demo）
//==============================================================================
#pragma once

#include "type.h"
#include "framebuffer_layout.h"
#include "game_limits.h"
#include "render_list.h"    /* 渲染队列：每个元素带自己的 z，统一排序后画 */
#include "assets_desc.h"    /* ASSET_xxx 枚举 / asset_desc_t（表见下面的 g_assets 宏） */
#include "game_layout.h"    /* 固定房间布局（排布）/ 房间类型表 */
#include "game_frame.h"     /* 标准矩形房间框架的生成规则（墙 / 地板 / 门 / 角块） */

//==================== 屏幕 ====================
#define SCR_W 1280
#define SCR_H 720
#define TILE 32                             /* 地砖边长 */
/*  地图最大尺寸：普通房 40×23，Boss 大房间 45×35（1440×1120）。
 *  当前 2×2/Boss 房主体是 31×25 格（992×800），以设计器地图池为准；
 *  左 6 格、右 8 格、上下各 5 格黑边，画布与相机范围不变。
 *  相机在世界里四方向跟随（左右 160 / 上下 400）。
 *  脏区按最大世界建表；普通房只用左上 40×23 格。  */
#define MAP_MAX_COLS 45
#define MAP_MAX_ROWS 35
/*  game_layout.h 的异形地形按同一上限开数组；这里对一下，改一处忘另一处会编译报错。 */
typedef char game_layout_size_check[(LAYOUT_MAX_COLS == MAP_MAX_COLS &&
                                    LAYOUT_MAX_ROWS == MAP_MAX_ROWS) ? 1 : -1];
#define BOSS_MAP_COLS 45                    /* Boss 大房间 45×35 = 1440×1120 */
#define BOSS_MAP_ROWS 35
#define GRID_W MAP_MAX_COLS                 /* 脏区格子数：45 */
#define GRID_H MAP_MAX_ROWS                 /* 35（Boss 世界高 35 格） */
#define DIRTY_BYTES ((GRID_W * GRID_H + 7) / 8)
typedef unsigned char dirty_map_t[DIRTY_BYTES];
static inline void dirty_clear(dirty_map_t dirty)
{
    int i;
    for (i = 0; i < DIRTY_BYTES; ++i) dirty[i] = 0u;
}
static inline int dirty_get(const dirty_map_t dirty, int tx, int ty)
{
    int n = ty * GRID_W + tx;
    return (dirty[n >> 3] >> (n & 7)) & 1u;
}
static inline void dirty_set(dirty_map_t dirty, int tx, int ty)
{
    int n = ty * GRID_W + tx;
    dirty[n >> 3] |= (unsigned char)(1u << (n & 7));
}

//==================== 精灵尺寸 ====================
#define PW 40                               /* 角色（新素材 40×56） */
#define PH 56
#define BW 32                               /* 子弹 */
#define BH 32
#define BOSS_W 80
#define BOSS_H 96
#define ITEM_W 32
#define ITEM_H 32
/*  底座 32×32 在下面，图标 32×32 悬在上面：
 *     图标底边 = y + ITEM_ICON_DY + 32 ± AMP  =  y + 0 ± 6
 *     ⇒ 正好贴在底座顶边上、上下浮动 6 px（看着是"浮在底座上"，不是"飞在天上"）
 *  觉得还是远就调小 ITEM_ICON_DY（-32 = 压进底座 6 px；-50 = 再抬高 12 px） */
#define ITEM_ICON_DY (-33)                  /* 图标相对底座的上浮量（-33 ⇒ 几乎贴在底座上） */
#define ITEM_FLOAT_AMP 5                    /* 图标上下浮动幅度（正弦，不再是硬跳） */
#define ITEM_HELD_DY  (-20)                 /* 被角色举起时：相对角色顶边再往上 20（举在头顶） */

//==================== 炸弹 ====================
#define BOMB_W 32                         /* 放置后的交互贴图 */
#define BOMB_H 32
#define BOMB_HIT_W 16                     /* 实际碰撞体：比贴图小，贴在底部 */
#define BOMB_HIT_H 16
#define BOMB_HIT_OFF_X ((BOMB_W - BOMB_HIT_W) / 2)
#define BOMB_HIT_OFF_Y (BOMB_H - BOMB_HIT_H)
#define BOMB_PICK_W 24                    /* 掉落物贴图 */
#define BOMB_PICK_H 24
#define BOMB_FUSE_MS 3000u
#define BOMB_BOOM_MS 300u
#define BOMB_RADIUS 40
#define BOMB_DMG 2

//==================== 玩法参数 ====================
#define N_BULLETS GAME_BULLET_POOL_CAP      /* includes 32 slots reserved for player shots */
#if N_BULLETS < 32 || N_BULLETS > 1024
#error N_BULLETS must fit the 16-bit free-slot index and the fixed RAM budget
#endif
#ifndef GAME_BOSS_DENSE_RINGS
#define GAME_BOSS_DENSE_RINGS 10
#endif
#define N_MOBS     16                       /* 当前房间的小怪（slime / bug） */
#define N_COINS    56                       /* 全地图持久金币：最多 11 个奖励房 ×5 */
#define MAX_BROKEN 96                       /* 整局"已破坏 interact"清单容量（每条 = 房间 + x + y，3 B）；
                                             * 一张 1×1 布局有 32 块石头，96 条够炸空三间房 */
#define N_BOMBS    12                       /* 跨房间炸弹实体上限 */
/*  ★ 每层开局（人站在初始房里）直接给的炸弹数。
 *    隐藏门全靠炸弹炸开，一颗都没有就只能干瞪眼 —— 这里是保底，不是上限：
 *    路上捡到的会累加，只在"不够"的时候补齐。 0 = 不给。  */
#define START_BOMBS 3
#define N_BOMB_AIRS 20                      /* 爆炸火花池：一次 3~5 个，够两三个炸弹连锁 */
#define BOMB_AIR_MIN  3                     /* 一次爆炸至少撒几个 */
#define BOMB_AIR_MAX  5
#define BOMB_AIR_MS   340u                  /* 火花存活时间 */
#define BOMB_AIR_SPREAD 22                  /* 起点离爆心最远 22 px */
#define BOMB_AIR_V8   11                    /* 向外飘的速度（1/8 px/帧） */
#define BOMB_FLASH_STRENGTH 190u            /* 引信倒数：整颗往白里闪的浓度 */

//==================== 判定框（★ 不是整张贴图的包围盒）====================
//  ★ 人物碰撞体积 = 【脚到眼睛】的小长方形：头/头发不参与判定，脚参与。
//    这样进门时头可以探进墙/门里，不会在门口被头卡住；中弹/接触只看这条身体。
//    子弹仍只用【中心点】去碰这个矩形，不用 40×56 的透明边（否则会"隔空受伤"）。
//    这块同时用于：移动/门洞边界、门洞触发、道具拾取、中弹判定、Boss 瞄准。
#define PB_OFF_X 12                     /* 脚到眼睛：相对 40×56 贴图左上角 */
#define PB_OFF_Y 24
#define PB_W 16
#define PB_H 32
#define PB_FOOT_H 8                     /* 石头/地面只挡脚底，不挡眼睛和躯干 */
#define PB_TOP_H 8                      /* 石头在人物下方时，用最上一排（眼睛）判 */
#define CREATURE_FOOT_W 24              /* 所有生物共用的脚底占用矩形 */
#define CREATURE_FOOT_H 16
#define BB_OFF_X 14                     /* Boss 身体 */
#define BB_OFF_Y 36
#define BB_W 52
#define BB_H 56

static inline void game_creature_foot_rect(int sprite_x, int sprite_y,
                                            int sprite_w, int sprite_h,
                                            int *x, int *y, int *w, int *h)
{
    *w = CREATURE_FOOT_W;
    *h = CREATURE_FOOT_H;
    *x = sprite_x + (sprite_w - CREATURE_FOOT_W) / 2;
    *y = sprite_y + sprite_h - CREATURE_FOOT_H;
}


//==================== 弹道 ====================
enum { MOTION_LINE, MOTION_SINE, MOTION_ACCEL, MOTION_SPIRAL, MOTION_HOME,
       MOTION_PULSE };
#define ANIM_FRAMES 5u                      /* 序列表长度 */
#define ANIM_HOLD 6u                        /* 每一帧保持几拍 */
#define FIRE_EVERY 12u                      /* 每 12 帧开一枪（5 发/秒） */
/*  玩家有惯性：输入决定"目标速度"，实际速度每帧朝目标逼近（加速/刹车）。
 *  单位是 1/8 像素，这样 3 px/帧 这种低速度也能做出加减速的过程。  */
#define PLAYER_MAX_V8    32                 /* 直向最高速 = 4 px/帧（32/8） */
#define PLAYER_DIAG_V8   16                 /* 斜向最高速 = 2 px/帧（16/8） */
#define PLAYER_MUZ_DY    20                 /* 枪口：头和身体的交界（相对精灵顶边） */
#define PLAYER_ACCEL_V8   3                 /* 每帧加速量：约 8 帧到全速 */
#define PLAYER_BRAKE_V8   6                 /* 松手每帧减速量：约 4 帧停住 */
#define PLAYER_BULLET_SPEED 5                 /* 基础玩家弹速；道具在此基础上叠加 */
/*  多弹幕的张角：1 格 = 360/64 = 5.625°
 *    4 格 = 22.5°  ← 默认（接近 20°）
 *    5 格 = 28.1°  ← 接近 30°
 *    8 格 = 45°    ← 原来的值，太散
 *  想要更精确的 20°/30° 只能改这半格粒度：要么 4 格（22.5°），要么 5 格（28.1°）。  */
/*  生命：3 颗心、3 条命；被击中扣 1 颗心，扣光演死亡动画后补满、命数 −1  */
#define HP_MAX              5               /* 基础生命：5 颗心 */
#define LIVES_START         1               /* 初始 1 条命；显示规则：1 不显示 */
/*  ★ 调试开关：1 = 无敌期人物闪烁；0 = 不闪（人物永远画）
 *    人物"不见"时先用 0 编译一次，能分辨是闪烁逻辑还是别的原因。  */
#define PLAYER_BLINK_ON_INVULN 0

#define HURT_INVULN_MS      1000u           /* 被打中之后 1 s 无敌 */
#define RESPAWN_INVULN_MS   2000u           /* 补满之后 2 s 保护（人物闪烁） */
#define RETURN_INVULN_MS    1500u           /* TURN BACK 回到上一房间后的保护 */

#define PLAYER_FAN_STEP 2             /* angular steps between extra shot paths */
#define ENEMY_BULLET_SPEED 2
/* 弹幕图案按"表格 + 连发队列"跑：每发之间隔 gap 帧，一轮打完歇 rest 帧 */
#define INTER_VOLLEY_GAP 8                  /* 同一招里，两轮之间的短间隔 */
/*  子弹的"寿命"：不一定要飞到屏幕外才消失  */
/*  敌方弹 = base 弹 + 一层半透明红（颜色合成的 strength 就是这层的浓度）  */
#define ENEMY_TINT_RGB    0x00FF2020u
#define ENEMY_TINT_K      170u              /* 0 = 不上色，255 = 全红 */
#define PLAYER_BULLET_RANGE 420             /* 旧固定射程（仅压测/兜底保留） */
#define PLAYER_RANGE        8               /* 玩家初始射程：8 点 × 32 px = 256 px */
#define RANGE_PX_PER_UNIT   32
#define RANGE_FRAMES_PER_UNIT 48            /* 追踪时 1 点射程 = 0.8 s × 60 fps */
#define BOSS_BULLET_LIFE    260             /* 敌方弹最长存活帧数（约 4.3 秒），0 = 不限 */
#define WAVE_CLEAR_MAX 8                    /* 场上敌弹少于这个数才算"上一招清完" */
#define ANGLE_STEPS 64                      /* 一圈分 64 格；a=0 右、16 下、32 左、48 上 */
#define BOSS_FRAME_MS 250u                  /* Boss 待机帧多久换一次 */
#define PICKUP_MS 1000u
#define HURT_MS 1000u
#define PLAYER_RESERVE GAME_PLAYER_BULLET_RESERVE /* 子弹池里永远给玩家留 32 槽 */
#define ENEMY_CAP (N_BULLETS - PLAYER_RESERVE)

//==================== Boss 血量与阶段 ====================
#define BOSS_HP_MAX     300                 /* 满血 */
#define BOSS_HIT_DMG    2                   /* Boss 旧默认参数；实际玩家基础伤害见 PLAYER_BASE_DAMAGE */
#define PLAYER_BASE_DAMAGE 3                /* 玩家基础伤害；小怪生命/受击数值另行平衡 */
#define CHILI_TRIGGER_PERCENT 33u            /* 每次玩家发射独立触发火焰弹的概率 */
#define CHILI_FLAME_LIFE_FRAMES 120u         /* 火焰弹存活 2 s（60 Hz） */
#define CHILI_FLAME_BLOCKS 3u                /* 火焰弹最多拦截 3 发敌弹 */
#define CHILI_BURN_MS      2000u
#define CHILI_BURN_TICK_MS 250u
#define URANUS_FREEZE_MS   1000u
#define URANUS_SLOW_MS     1000u
#define URANUS_SLOW_COOLDOWN_MS 5000u
#define URANUS_TINT_RGB    0x004060FFu
#define URANUS_TINT_K      150u
#define GAME_TRIPRISM_MAX              3   /* Each copy adds one prism, capped at three. */
#define GAME_TRIPRISM_SIZE            24
#define GAME_TRIPRISM_ORBIT_RADIUS    56
#define GAME_TRIPRISM_SPLIT_COUNT      4
#define GAME_TRIPRISM_SPLIT_LIMIT     24   /* Leave headroom in the 32 player slots. */
#ifndef GAME_TEST_DAMAGE
#define GAME_TEST_DAMAGE 10                 /* T 测试模式：单次攻击伤害，阶段锁血仍生效 */
#endif
#define BOSS_GATE_1     200                 /* 锁血召唤，清怪后进入第二阶段 */
#define BOSS_GATE_2     100                 /* 锁血召唤，清怪后进入第三阶段 */
#define BOSS_GATE_END   0                   /* 到 0 = 击破 */
#define BOSS_PHASE_SHIFT_MS 1200u           /* 护盾最短时间；小怪未清完则继续保持 */
#define BOSS_SUMMON_COUNT 16                /* 每次转阶段召唤；清完后才解除护盾 */
#ifndef GAME_BOSS_STAGE_TEST
#define GAME_BOSS_STAGE_TEST 0              /* 0 正式；1 自动阶段诊断；2 手动操作、只采集性能 */
#endif
#define HIT_FLASH_MS    120u                /* 命中闪红多久 */
#define BLINK_ON_MS     55u                 /* 护盾闪白的"亮"持续 */
#define BLINK_OFF_MS    70u                 /* 护盾闪白的"灭"持续 */
#define BLINK_COOL_MS   1000u               /* 闪完两下之后，1 秒内不再闪 */
#define BOSS_RESPAWN_MS 3000u               /* 击破后停多久再开一局（现在由 exit 触发） */
#define BOSS_WAIT_MS    1000u               /* 进 Boss 房先等 1 s 再出招 */

//==================== 颜色 ====================
#define FLOOR_BG 0x493326u

//==================== 帧缓冲 ====================
#define FB_C DDR_FB_C
#define FB_D DDR_FB_D
#define FB_BG DDR_FB_BG
#define FB_THIRD DDR_FB_THIRD
#ifndef GAME_TRIPLE_BUFFER
#define GAME_TRIPLE_BUFFER 1
#endif
#define GAME_FB_COUNT (GAME_TRIPLE_BUFFER ? 3 : 2)
#ifndef GAME_STATIC_CULL
#define GAME_STATIC_CULL 1
#endif
#ifndef GAME_UART_TELEMETRY
#define GAME_UART_TELEMETRY 0
#endif
#ifndef GAME_BG_COPY
#define GAME_BG_COPY 1
#endif
#ifndef GAME_BG_VIEW_CULL
#define GAME_BG_VIEW_CULL 1
#endif
#ifndef GAME_BG_EARLY_RESTORE
#define GAME_BG_EARLY_RESTORE 1
#endif
#ifndef GAME_BG_PREUPDATE_RESTORE
#define GAME_BG_PREUPDATE_RESTORE 0
#endif

#ifndef GAME_BULLET_TINT
#define GAME_BULLET_TINT 1
#endif
#ifndef GAME_BULLET_DIRTY_BOUNDS
#define GAME_BULLET_DIRTY_BOUNDS 1
#endif
#ifndef GAME_BG_EXACT_RECT
#define GAME_BG_EXACT_RECT 0
#endif
#ifndef GAME_BULLET_TILE_HISTORY
#define GAME_BULLET_TILE_HISTORY (!GAME_BG_EXACT_RECT)
#endif
#if GAME_BG_EXACT_RECT && GAME_BULLET_TILE_HISTORY
#error Exact rectangle restoration requires coordinate bullet history
#endif
#ifndef GAME_BULLET_MERGE_TILES
#define GAME_BULLET_MERGE_TILES 1
#endif

#ifndef GAME_BG_DIRECT_MAX_CELLS
#define GAME_BG_DIRECT_MAX_CELLS 0
#endif

//==================== 渲染层号（z）====================
/*  画家算法：z 小的先画（在下面），z 大的后画（盖上面）。
 *  · 静态层之间留够间距，实体层的深度范围（0..~816）不会串到护盾层
 *  · 实体层用【底边 y】当深度 ⇒ 越靠下越近、越靠前（俯视视角的透视）
 *  · 加一种元素：在它的 submit() 里 rl_push(自己的 z, 画法, 下标) 即可 */
enum {
    Z_SCENE   =    0,       /* 地板 / 墙 / 门（脏区恢复，永远最下面） */
    Z_ACTOR   = 1000,       /* 实体：角色 / Boss / 弹幕 / 道具（本层内按底边 y 排） */
    Z_SHIELD  = 3000,       /* 锁血玻璃罩：盖住所有实体 */
    Z_HUD     = 5000,       /* HUD：面板 / 血条 / 字幕 ...（子层见下面） */
    Z_OVERLAY = 6000        /* GAME OVER 之类，永远最上面 */
};

/*  实体元素的 z = 层基址 + 底边 y。
 *  深度夹到 [0, 1500]：负坐标（屏外元素）不会掉到场景层下面，越界也不会串到护盾层。 */
#define ACTOR_Z(depth) (Z_ACTOR + \
    ((depth) < 0 ? 0 : ((depth) > 1500 ? 1500 : (depth))))

/*  HUD 子层：数字越大越靠上（顺序和 game_main.c 的 hud_submit 一致） */
#define Z_HUD_TOAST       (Z_HUD + 10)   /* 拾取字幕（最下：血条会盖住它） */
#define Z_HUD_BOSSBAR     (Z_HUD + 20)   /* Boss 血条 */
#define Z_HUD_COUNTDOWN   (Z_HUD + 30)   /* 3-2-1 */
#define Z_HUD_PANEL       (Z_HUD + 50)   /* 左上状态面板 */
#define Z_HUD_VITAL       (Z_HUD + 60)   /* 心 / 额外命数 / 飘心 */
#define Z_HUD_MINIMAP     (Z_HUD + 65)   /* 右上角小地图 */
#define Z_HUD_POOL        (Z_HUD + 70)   /* 右侧道具池 */

//==================== HUD 版式 ====================
#define HUD_LABEL_CMD_COUNT 43              /* 静态 HUD 只画 10 行标签 = 43 个字模（数字由面板每帧补）
                                             * ★ 这是铺地等待的【下界】，只许比实际小 */
/* HUD 占哪几格（脏区单位）：前 3 行 × 前 6 列 = 192×96 像素。
   这几格每帧都要进脏区，否则上一次画的数字不会被擦掉。 */
#define HUD_CELLS_X 6
#define HUD_CELLS_Y 12                      /* FPS/BULLETS/CMD/STAGE + 心 + DMG/MOVE/BSPD/RANGE/RATE/COINS/BOMB */
/* Boss 血条（顶部居中）；它占的格子也要标脏，否则擦不干净会留残影 */
#define HUD_HP_W   400
#define HUD_HP_H   14
#define HUD_HP_X   ((SCR_W - HUD_HP_W) / 2)
#define HUD_HP_Y   30                       /* 血条下移，上方给 "BOSS" 留位置 */
#define HUD_HP_LABEL_W 64                   /* 全称 BOSS：4 个 16 px 字模 */
#define HUD_HP_LABEL_X (HUD_HP_X - HUD_HP_LABEL_W - 16)
#define HUD_HP_LABEL_Y 2                    /* 战斗中持续显示，不随阶段切换消失 */
#define HUD_BOSS_X     HUD_HP_LABEL_X
#define HUD_BOSS_W     (HUD_HP_X + HUD_HP_W - HUD_BOSS_X)
#define HUD_BOSS_H     (HUD_HP_Y + HUD_HP_H)
#define HUD_TOAST_X   ((SCR_W - HUD_TOAST_W) / 2)
/* 拾取字幕：固定在屏幕水平居中、靠近顶部；避开 BOSS 标题、血条与顶部门。
 * 绘制和擦除都使用这一整块固定区域，文案变短时不会留下上一段的尾部。 */
#define HUD_TOAST_MS  2500u                 /* 显示多久 */
#define HUD_TOAST_Y   2
#define HUD_TOAST_W   400
#define HUD_TOAST_H   24
#define GAMEOVER_MS     5000u               /* GAME OVER 停 5 秒后自动重开 */
#define SCORE_HIT       10                  /* 每打中 Boss 一发 */
#define SCORE_KILL      1000                /* 击破一个 Boss */
/*  场地（竞技场）：内圈 = wood，边界 = bedrock（2 格厚 = 64 px）
 *  ★ 玩家/Boss 的活动范围也用这一组，改这里场地和碰撞一起变。
 *  ★ 普通房和 Boss 大房间尺寸不同 ⇒ 这一组是【运行期变量】，
 *    由 game_world_select() 按当前房间设置；宏只是取变量，老代码不用改。  */
extern int g_arena_x0, g_arena_y0, g_arena_x1, g_arena_y1;
#define ARENA_X0   g_arena_x0
#define ARENA_Y0   g_arena_y0
#define ARENA_X1   g_arena_x1
#define ARENA_Y1   g_arena_y1

#define HUD_LABEL_X 9                       /* 标签左边距 */
#define HUD_VALUE_X 128                     /* 数值左边距：必须让开最长的标签 "BULLETS"(7 字 = 112 px) */

#define VITAL_ROW   4                       /* 面板上"血量"那一行（0 基）：原 ITEMS 的位置 */
#define KEY_SPAWN   (1u << 6)               /* 串口手柄 bit6：按一下 Y 在脚下生成道具+底座 */
#define HUD_ROW_H   32                      /* 行高（两行字模） */

//==================== 按键位 ====================
#define KEY_LEFT     0x1u
#define KEY_RIGHT    0x2u
#define KEY_DOWN     0x4u
#define KEY_UP       0x8u
#define KEY_RESTART  0x10u                  /* R 键（只从串口来）：按一下重开一局（边沿触发） */
#define KEY_BOMB     0x20u                  /* E 键：放炸弹（上升沿） */
#define KEY_BOSS_JUMP 0x80u                /* 数字 1：直接跳到 Boss 房（上升沿） */
#ifndef GAME_BOSS_SHORTCUT
#define GAME_BOSS_SHORTCUT 1               /* 独立于无敌/性能诊断，正式构建也可传送 */
#endif

//==================== 性能诊断（正常演示用 0）====================
//  0 = 正常游戏
//  1 = 冻结玩法，按 16/32/48/64/80/96 阶梯扫子弹数
//  2 = PERF_TEST_BULLETS 发运动弹，正常恢复背景
//  3 = PERF_TEST_BULLETS 发运动弹，不恢复背景（仅用于测量，画面会留拖影）
#ifndef PERF_TEST_MODE
#define PERF_TEST_MODE 0
#endif
#ifndef PERF_TEST_BULLETS
#define PERF_TEST_BULLETS N_BULLETS
#endif
#ifndef GAME_BOSS_DEBUG
#define GAME_BOSS_DEBUG 0
#endif

/* Boss 房相机 A/B 测试：保留 Boss 战，但改用 1×1 竞技场。 */
#ifndef GAME_BOSS_1X1_TEST
#define GAME_BOSS_1X1_TEST 0
#endif

/* 仅诊断使用：缩短 Boss 发射器中刻意保留的间隔。 */
#ifndef GAME_BOSS_PACING_TEST
#define GAME_BOSS_PACING_TEST 0
#endif

#ifndef GAME_BOSS_AUTO_DIAG
#define GAME_BOSS_AUTO_DIAG 0
#endif

#ifndef GAME_BOSS_MOTION_TRACE
#define GAME_BOSS_MOTION_TRACE 0
#endif

#ifndef GAME_BOSS_SKILL_DIAG
#define GAME_BOSS_SKILL_DIAG 0
#endif
#ifndef GAME_BOSS_SKILL_CYCLES
#define GAME_BOSS_SKILL_CYCLES 1
#endif
#ifndef GAME_BOSS_CAMERA_PAN
#define GAME_BOSS_CAMERA_PAN 0
#endif
#ifndef GAME_BOSS_BULLET_SWEEP
#define GAME_BOSS_BULLET_SWEEP 0
#endif
#ifndef GAME_CAPACITY_AUDIT
#define GAME_CAPACITY_AUDIT 0
#endif
#ifndef GAME_RUN_AUDIT
#define GAME_RUN_AUDIT 0
#endif
#ifndef GAME_CAPACITY_PHASE
#define GAME_CAPACITY_PHASE 0
#endif
#if GAME_CAPACITY_AUDIT == 2 && (GAME_CAPACITY_PHASE < 0 || GAME_CAPACITY_PHASE >= 3)
#error Invalid real-game capacity phase
#endif
#ifndef GAME_CAPACITY_BOSS
#define GAME_CAPACITY_BOSS 1
#endif
#ifndef GAME_CAPACITY_START
#define GAME_CAPACITY_START 240
#endif
#ifndef GAME_CAPACITY_STOP
#define GAME_CAPACITY_STOP 500
#endif
#ifndef GAME_CAPACITY_STEP
#define GAME_CAPACITY_STEP 40
#endif
#ifndef GAME_CAPACITY_FRAMES
#define GAME_CAPACITY_FRAMES 240
#endif
#if GAME_CAPACITY_AUDIT && !GAME_BOSS_BULLET_SWEEP
#error Capacity audit requires the diagnostic bullet-sweep frame hooks
#endif
#ifndef GAME_BOSS_SWEEP_START
#define GAME_BOSS_SWEEP_START 16
#endif
#ifndef GAME_BOSS_SWEEP_HOLD_LEVEL
#define GAME_BOSS_SWEEP_HOLD_LEVEL N_BULLETS
#endif
#ifndef GAME_BOSS_SWEEP_OPAQUE
#define GAME_BOSS_SWEEP_OPAQUE 0
#endif
#ifndef GAME_BULLET_FAST_CKEY
#define GAME_BULLET_FAST_CKEY 0
#endif
#ifndef GAME_BULLET_ALPHA_CROP
#define GAME_BULLET_ALPHA_CROP 1
#endif
#define PERF_LEVELS 6

enum { RIGHT, LEFT, DOWN, UP };
enum { BULLET_A, BULLET_CHILI, BULLET_URANUS };   /* 弹种（贴图/伤害） */
enum { ITEM_CHILI, ITEM_URANUS, ITEM_EYES, ITEM_1UP, ITEM_MAGICCARD,
       ITEM_TRIPRISM, ITEM_COUNT };   /* 道具编号（≠ 弹种，属性查表） */
#define HELD_HEART (-1)              /* 商店买 heart 时头顶举的图标类型（不是道具池道具） */
#define ITEM_FLAG_LIFE_PLUS 0x01u    /* 死亡时可用：回到上一房间并消耗（TURN BACK） */

/*  道具对玩家属性的“增减量”。0 = 不改；display/HUD 还没设计，这里只做数值结算。
 *  hp/hp_max/lives：正数加、负数减（hp_max 先保留字段，等血条 UI 支持上限再启用）。
 *  move_v8/fire_every/bullet_speed/damage：有符号增量；bullet_speed 对普通/多弹幕都生效。  */
typedef struct {
    /* 属性增量定义为 -128..127；参与运算时提升为 int。 */
    signed char hp, hp_max, lives;
    signed char move_v8, fire_every, bullet_speed;
    signed char range;      /* 普通弹 32 px/点；追踪 0.8 s/点 */
    signed char damage;
    signed char bullet_type; /* -1 = 不改弹种 */
    signed char triple;        /* 额外弹道数；重复获得时累加 */
} item_mod_t;

typedef struct {
    const char *desc;
    unsigned short icon;        /* generated ASSET_* index */
    unsigned char level;        /* 0 普通 / 1 稀有 / 2 史诗 / 3 传说（固定） */
    unsigned char pool;         /* 1 = 进道具池；0 = 只定义/不随机 */
    unsigned char flags;        /* ITEM_FLAG_* */
    item_mod_t mod;
} item_def_t;

#define INV_MAX 16
typedef struct {
    unsigned char type;
} item_slot_t;

/*  玩家可被道具加减的属性；HP/命数仍由 game_vital 管，避免两份真相。  */
typedef struct {
    int move_v8;            /* 直向最高速（1/8 px/帧） */
    int fire_every;         /* 每几帧开一枪 */
    int bullet_speed;       /* 玩家所有弹幕的速度（普通/多弹幕共用） */
    int range;              /* 射程：普通 32 px/点；追踪 0.8 s/点 */
    int damage;
} player_stats_t;

extern const item_def_t g_item_defs[ITEM_COUNT];
/*  玩法阶段
 *    EXPLORE    起始房 / 空房 / 道具房 / 商店房：没有 Boss，自由走动
 *    FIGHT      Boss 战（进 Boss 房立即开打，没有开场延迟）
 *    GAMEOVER   命尽 */
enum { PHASE_EXPLORE = 0, PHASE_FIGHT, PHASE_GAMEOVER };

typedef struct {
    unsigned char active : 1;
    unsigned char mode   : 3;   /* MOTION_xxx（当前 0..5） */
    unsigned char type   : 2;   /* 贴图：BULLET_A / CHILI / URANUS */
    unsigned char enemy  : 1;   /* 1 = 敌方弹 */
    unsigned char use8   : 1;   /* 1 = 用 vx8/vy8 走位（精细角度），0 = 用整数 vx/vy */
    unsigned char home_kind : 2; /* 0 none, 1 mob, 2 Boss */
    signed char home_idx : 5;    /* -16..15; N_MOBS=16 */
    signed char   rx, ry;       /* 亚像素余量（0..7） */
    short x, y;                 /* 位置（像素） */
    short vx, vy;               /* 速度：LINE/HOME = 每帧位移；SINE = 振幅/下落速度；SPIRAL = 半径/角速度；ACCEL = 初速/加速度 */
    short base_x;               /* SINE 的中线 */
    unsigned short age;         /* 活了几帧（SINE/SPIRAL/ACCEL/HOME 都要） */
    short vx8, vy8;             /* 速度的 1/8 像素精度（只有 use8 时生效） */
    unsigned short dist;        /* 已经飞了多远（1/8 像素，曼哈顿累加） */
    unsigned short range;       /* 射程：飞满这么远就消失（0 = 不管，飞出去为止） */
    unsigned short life;        /* 还能活几帧（0 = 不管，飞出屏幕为止） */
    unsigned char chili_blocks : 5; /* 火焰弹剩余拦截次数；其它弹为 0 */
    unsigned char split_color   : 3; /* 0 = not split; 1..4 = red/yellow/green/blue */
} bullet_t;
/*  双缓冲"上次画在哪"：4 字节一槽（原来 short+short+char 对齐成 6 字节）。
 *  x/y 范围 -8192..8191，包含世界坐标和屏外精灵；kind 保存旧弹的贴图/旋转。
 *  仍为 4 字节，不再另开每缓冲一份 kind 数组。 */
typedef struct {
    unsigned int active : 1;
    unsigned int kind   : 3;
    int          x      : 14;
    int          y      : 14;
} old_t;
typedef char bullet_state_size_check[(sizeof(bullet_t) == 28 && sizeof(old_t) == 4 && N_MOBS <= 16) ? 1 : -1];
typedef struct { short x, y; unsigned char active; unsigned char type; } item_t;

typedef struct {
    unsigned char active;       /* 0 = 空槽 */
    unsigned char state;        /* 0 = 引信；1 = 爆炸动画 */
    signed char   room;
    signed char   rx, ry;       /* 推动的亚像素余量 */
    short x, y;
    short vx8, vy8;             /* 滑动速度（1/8 px/帧） */
    u32 fuse_until;
    u32 boom_until;
} bomb_t;

typedef struct {
    unsigned char active;
    signed char   room;
    short x, y;
} bomb_pickup_t;

/*  爆炸火花：炸弹炸开时在爆心周围随机撒 3~5 个，向外飘一小段就消失。
 *  纯表现层 —— 不参与碰撞，也不结算伤害。  */
typedef struct {
    unsigned char active;
    signed char   room;
    signed char   rx, ry;       /* 飘动的亚像素余量 */
    short x, y;
    short vx8, vy8;             /* 向外飘的速度（1/8 px/帧） */
    u32 until;
} bomb_air_t;

typedef struct {
    unsigned char active;
    unsigned char hp;
    unsigned char type;         /* 0 = slime / 1 = bug */
    unsigned char frame;        /* bug 动画 0..3：0,1,对称1,0 */
    unsigned char hop_left;     /* slime 当前跳跃剩余移动步数 */
    unsigned char nav_follow;   /* 受阻后沿共享路径场绕行 */
    unsigned char elemental_hits;
    short x, y;
    u32 move_next;              /* 下一次起跳 / 下一次移动的时间 */
    u32 hop_until;              /* slime：这一跳腾空到什么时候；bug：下一帧的时间 */
    u32 hit_until;              /* 被子弹打中后的闪红显示到什么时候（0 = 不闪） */
    u32 burn_until, burn_next;
    u32 freeze_until;
} mob_t;
typedef struct { unsigned char active; signed char room; short x, y; } coin_t;
/*  石头 / 箱子 / 板条箱不再有实体池：
 *  位置和种类都在房间布局（room_layout_t）里，运行时只记"哪个房间的第几个对象碎了"
 *  （game_room.c 的 s_broken[]）。石头可被炸弹炸碎；箱子/板条箱只挡人挡弹。  */
/*==============================================================================
 *  Boss 的数据结构（★ 招式是【数据】，不是代码）
 *------------------------------------------------------------------------------
 *  pattern_t : 图案的参数（形状 / 节奏 / 转法）—— 引擎照着它放弹
 *  move_t    : 一个招式 = 用哪个图案 + 连播几次
 *  boss_def_t: 整个 Boss = 血量 / 锁血线 / 炮口 / 招式表 / 运动方式
 *  引擎（game_boss.c）只做两件事：按表放弹、按运动参数摆位置。
 *  加招式 / 换图案 / 让 Boss 动起来，全都是改 game_boss_def.c 里的数据。
 *============================================================================*/
typedef struct {
    unsigned char count;    /* 一轮几发 */
    unsigned char gap;      /* 每发隔几帧 */
    unsigned char rest;     /* 整招打完歇几帧 */
    unsigned char step;     /* 每发角度往前走几格（64 格一圈） */
    unsigned char base;     /* 起始角度：0 右 / 16 下 / 32 左 / 48 上 */
    unsigned char speed;    /* 像素每帧 */
    unsigned char twin;     /* 1 = 同时朝反向(180°)再发一发 */
    unsigned char speed2;   /* twin 那发的速度（0 = 同速） */
    unsigned char spin;     /* 同一招里每轮再转几格 */
    unsigned char repeat;   /* 这一招连续发几轮（>1 = 会动的招式） */
    unsigned char aim;      /* 1 = 起始角对准目标（自机狙），0 = 用 base */
    unsigned char motion;   /* 0 = line shot; MOTION_PULSE = full radial ring */
    unsigned char layers;   /* 0/1 = one shot; >1 = separated speed/parallel lanes */
} pattern_t;

typedef struct {
    unsigned char pattern;  /* g_boss_patterns[] 的下标 */
    unsigned char times;    /* 这个图案连播几次（0 当 1） */
    unsigned char motion;   /* Index in the Boss movement pool */
} move_t;

enum {
    BOSS_STILL, BOSS_SINE_Y, BOSS_SINE_X, BOSS_LEFT, BOSS_RIGHT,
    BOSS_UP, BOSS_DOWN, BOSS_ORBIT
};

typedef struct {
    unsigned char kind;     /* 定点、横/纵向往返、左/右目标点 */
    unsigned char amp;      /* 幅度（像素） */
    unsigned char period;   /* 周期（以 16 ms 为一格） */
    unsigned char speed;    /* 每个逻辑步的最大位移，避免切换轨迹时瞬移 */
} boss_motion_t;

/* 一套招式表（图案库 + 播放列表）。Boss / 小怪 / 炮台都可以各挂一套，共享图案。 */
typedef struct {
    const pattern_t *patterns;
    int n_patterns;
    const move_t *moves;
    int n_moves;
} move_set_t;

typedef struct {
    int hp;                 /* 满血 */
    int gate1, gate2;       /* 两条锁血线 */
    unsigned int phase_shift_ms;
    int w, h;               /* 贴图尺寸 */
    int muzzle_dx, muzzle_dy;   /* 炮口相对左上角 */
    const move_set_t *moves;    /* ★ 用哪套招式（数据） */
    const boss_motion_t *motions;
    unsigned char n_motions;
} boss_def_t;

enum { BOSS_PHASE_CORE, BOSS_PHASE_RAIN, BOSS_PHASE_COMBINED, BOSS_PHASE_COUNT };
typedef struct {
    unsigned short rain_on, rain_rest, shot_cap; /* 60 Hz ticks / slots */
    unsigned char rain_every, rain_drop, first_move;
} boss_phase_def_t;

/*==============================================================================
 *  弹幕发射器：谁都能挂一个（Boss / 小怪 / 炮台）
 *------------------------------------------------------------------------------
 *  · 只管"按招式表放弹"，不关心宿主是谁
 *  · 宿主每帧更新 mx/my（炮口）和 tx/ty（瞄准点），再调 game_shot_step()
 *============================================================================*/
typedef struct {
    int active;
    int enemy;                  /* 1 = 敌方弹（决定寿命/颜色） */
    int type;                   /* 弹种（贴图） */
    int mx, my;                 /* 炮口世界坐标 */
    int tx, ty;                 /* 瞄准点（aim=1 的图案用） */
    int frenzy;                 /* 0..2：宿主自己算的急迫程度（残血加速） */
    const move_set_t *set;      /* 用哪套招式 */
    int move_idx, move_times;   /* 当前招式 / 还要连播几次 */
    int burst_left, burst_wait, burst_idx, burst_base;
    int rep_left, move_base;
    u32 tick;
} shot_t;


//==================== 唯一的一份状态 ====================
typedef struct {
    /* 角色 */
    int x, y, dir;
    int moving, was_moving, last_dir;
    int vx8, vy8;                       /* 当前速度（1/8 像素每帧） */
    int rx, ry;                         /* 亚像素余量（累计到 8 才走 1 像素） */
    u32 anim, fire_tick, anim_step;     /* fire_tick = 距下一发的剩余冷却帧 */
    u32 pickup_until, hurt_until;
u32 gameover_until;                 /* GAME OVER 结束时刻（PHASE_GAMEOVER 用） */
int score;                          /* 本局得分 */
    u32 toast_until;                    /* 拾取字幕显示到什么时候（0 = 没有） */
    int toast_type;                     /* 字幕对应哪种道具 */
    int bullet_type;                    /* 当前弹种（拾取道具后改变） */
    int fire_extra_paths;               /* accumulated Inner Eyes extra projectiles */
    int test_mode;                      /* T 开关：玩家无敌，攻击使用 GAME_TEST_DAMAGE */
    player_stats_t stats;               /* 道具可加减的玩家属性 */
    unsigned item_mask;                 /* 从 inv[] 派生：拿到过哪个类型（效果/权重用） */
    item_slot_t inv[INV_MAX];           /* 道具实例，按拾取顺序；允许重复 */
    int      inv_n;                     /* 当前道具数量 */
    unsigned inv_rev;                   /* 道具池内容版本（池子重画用） */
    int      last_room;                 /* 上一个房间；死亡卡回退用 */
    int      last_door;                 /* 上一个房间里通往死亡房间的门方向 */
    int held_type;                      /* 身上的道具编号（ITEM_xxx） */
    /* Boss */
    int boss_x, boss_y;
    u32 boss_tick;
    int stage;                          /* 第几关：击破一次 Boss +1，弹幕跟着变密 */
    int phase;                          /* PHASE_EXPLORE / PHASE_FIGHT / PHASE_GAMEOVER */
    int boss_hp, boss_gate;             /* 当前血量 / 下一个锁血线 */
    int boss_phase;                     /* BOSS_PHASE_*，与关卡 stage 分开 */
    int boss_locked;                    /* 1 = 转阶段护盾，无伤且暂停攻击 */
    u32 boss_lock_until, boss_flash_until;
    u32 boss_wait_until;                /* 进 Boss 房后 1 s 内不出招、也不吃伤害 */
    int exit_active;                    /* Boss 击破后，房间顶部出现的 exit */
    int exit_room, exit_x, exit_y;
    int exit_dirty;                     /* exit 消失后还要擦几帧（两块缓冲） */
    int boss_blink_cnt, boss_blink_on;  /* 护盾被打中：快闪两下（on/off 交替） */
    u32 boss_blink_next, boss_blink_cool;
    u32 boss_slow_until, boss_slow_next;
    shot_t shot;                        /* ★ Boss 挂的弹幕发射器（小怪以后也各挂一个） */
    /* 道具 */
    item_t item;
    int boss_drop_pending;
    /* 炸弹 */
    int bomb_count;                     /* 可放置数量 */
    bomb_t bombs[N_BOMBS];
    old_t old_bomb[GAME_FB_COUNT][N_BOMBS];
    bomb_pickup_t bomb_pickup;
    old_t old_bomb_pickup[GAME_FB_COUNT];              /* Boss 掉的饰品还没捡（重进 Boss 房要恢复） */
    bomb_air_t bomb_airs[N_BOMB_AIRS];      /* 爆炸火花（表现层） */
    old_t old_bomb_air[GAME_FB_COUNT][N_BOMB_AIRS];
    /* 子弹池 */
    bullet_t bullets[N_BULLETS];
    /* 精确恢复需要每个旧矩形；按格恢复只需所有旧矩形的并集，
     * 与弹槽复用和相机移动无关。 */
#if !GAME_BULLET_TILE_HISTORY
    old_t old_bullets[GAME_FB_COUNT][N_BULLETS];
#else
    dirty_map_t old_bullet_tiles[GAME_FB_COUNT];
#endif
    old_t old_player[GAME_FB_COUNT], old_boss[GAME_FB_COUNT], old_item[GAME_FB_COUNT];
    old_t old_triprism[GAME_FB_COUNT][GAME_TRIPRISM_MAX];
    /* 小怪 / 清理奖励金币 */
    mob_t mobs[N_MOBS];
    coin_t coin_items[N_COINS];
    old_t old_mob[GAME_FB_COUNT][N_MOBS];
    old_t old_coin[GAME_FB_COUNT][N_COINS];
    int coins;                          /* 本局金币数（下一层保留，新一局清零） */
    /* 显示与统计 */
    int front;                          /* 已确认显示的缓冲；三缓冲另跟踪待显示块 */
    int cam_x, cam_y;                   /* 当前显示窗口左上角（Boss 世界坐标；普通房 0） */
    int hud_cam_x[GAME_FB_COUNT], hud_cam_y[GAME_FB_COUNT];     /* 每块缓冲上次画 HUD 时的相机位置 */
    int door_dirty;                     /* 换房后还要重画几帧门（两块缓冲各一次） */
    int cmd_frame;                      /* 上一帧发了多少条命令 */
    int fps;
    u32 fps_epoch, fps_frames;
    u32 cmd_before;                     /* 本帧开始时的命令完成计数 */
    u32 probe_epoch, probe_frames, probe_frame_no;
    int probe_peak, probe_peak_frame, probe_peak_cmd;
} game_t;

extern game_t g;

static inline int game_player_bullet_paths(void)
{
    return 1 + g.fire_extra_paths;
}

static inline int game_player_hit_damage(int normal_damage)
{
    if (g.test_mode) return GAME_TEST_DAMAGE;
    return g.stats.damage > 0 ? g.stats.damage : normal_damage;
}

//==================== 素材表（唯一一份，见 game_assets.c）====================
//  assets_desc.h 里的表是 static 的 ⇒ 每个包含它的 .c 都会复制一份（59 项 ≈ 1.2 KB）。
//  这里统一指到 game_assets.c 里那份，其它模块照旧写 g_assets[i].base_word。
/* 共享的 sin 表：sin(2πi/64)×64，值域 -64..64（弹道摆动 / 道具浮动都用它） */
extern const signed char g_sin_tab[64];

static inline int game_triprism_count(void)
{
    int i, count = 0;

    for (i = 0; i < g.inv_n; ++i)
        if (g.inv[i].type == ITEM_TRIPRISM && ++count >= GAME_TRIPRISM_MAX)
            break;
    return count;
}

/* 碰撞、绘制和脏区历史共用同一套确定性环绕位置。 */
static inline void game_triprism_position(int index, int count, u32 now,
                                          int *x, int *y)
{
    int phase;
    int cx = g.x + PW / 2;
    int cy = g.y + PH / 2;

    if (count < 1 || index < 0 || index >= count || index >= GAME_TRIPRISM_MAX) {
        *x = *y = 0;
        return;
    }
    phase = (int)((now >> 6) & 63u) + index * 64 / count;
    *x = cx - GAME_TRIPRISM_SIZE / 2 +
         ((g_sin_tab[(phase + 16) & 63] * GAME_TRIPRISM_ORBIT_RADIUS) >> 6);
    *y = cy - GAME_TRIPRISM_SIZE / 2 +
         ((g_sin_tab[phase & 63] * GAME_TRIPRISM_ORBIT_RADIUS) >> 6);
}

static inline int game_triprism_hit_test(int cx, int cy, u32 now)
{
    int count = game_triprism_count();
    int i;

    if (g.phase == PHASE_GAMEOVER) return 0;
    for (i = 0; i < count; ++i) {
        int x, y;
        game_triprism_position(i, count, now, &x, &y);
        if (cx >= x && cx < x + GAME_TRIPRISM_SIZE &&
            cy >= y && cy < y + GAME_TRIPRISM_SIZE)
            return 1;
    }
    return 0;
}

const asset_desc_t *game_asset_table(void);
/*  热路径用全局指针，避免每次 g_assets[i] 都走一次函数调用。
 *  指针在 game_assets.c 里初始化，指向唯一那份 static 表。  */
extern const asset_desc_t *const g_assets_ptr;
#define g_assets (g_assets_ptr)

//==================== 输入（game_input.c）====================

u32  game_keys_read(void);                   /* WASD 移动及功能键掩码 */
u32  game_aim_read(void);                    /* 小键盘瞄准方向掩码 */
int  game_input_is_serial(void);             /* 1 = 串口手柄；0 = 板载按键回退 */
int  game_take_spawn_item(void);             /* 取一次"按了 Y"（取完清） */
int  game_take_bomb(void);                   /* 取一次"按了 E"（取完清） */
int  game_take_restart(void);       /* 这一帧有没有"按过 R"（取完清零） */
#if GAME_BOSS_SHORTCUT
int  game_take_boss_jump(void);
#endif
#if GAME_BOSS_DEBUG && GAME_PROFILE
int  game_take_debug_stats(void);
void game_debug_frame_report(void);
#endif

//==================== 状态 / 时钟 / 上屏（game_state.c）====================
u32  game_ms(void);
void game_reset_state(void);            /* 只复位数据，不重画 */
int  game_restart(void);                /* 复位 + 把两块缓冲重铺一遍（R 键用） */
void game_enter_gameover(u32 now);      /* 命尽：进 GAME OVER 画面 */
void game_flow_enter_room(void);        /* 换房之后：按房间类型布置内容（道具 / Boss） */
void game_boss_spawn(void);             /* 第一次进 Boss 房：满血到场中，立即开打 */
int  game_init(void);                   /* 初始化状态、GPU 和帧缓存；0 = 成功 */
int  game_finish_gpu(void);             /* 结束当前帧的 GPU 命令并等待完成；0 = 成功 */
int  game_present(u32 fb);              /* 把指定后台缓冲排到下一个显示边界；0 = 成功 */
int  game_wait_display(void);           /* 等待一个显示边界，不改变当前显示内容 */
int  game_wait_display_after(u32 mark); /* 等待 mark 之后的显示边界，正确处理已到达事件 */

//==================== 相机（game_state.c / game_main.c）====================
/*  HUD 画在 fb + cam 处 ⇒ 屏幕上位置固定；场景/实体画在 fb 的世界坐标处。
 *  game_present() 用 game_view_fb(后台缓冲) 下发显示基址，窗口就移动了。  */
u32  game_view_fb(u32 fb);                  /* fb + cam_y*5760 + cam_x*3 */
void game_camera_snap(void);                /* 换房/复位：相机直接对准玩家（无过渡） */
void game_camera_synced(void);              /* 整屏重铺后：新位置已经画好，不用擦旧 HUD */
void game_camera_step(void);                /* 每帧：Boss 房平滑跟随，普通房固定 0 */
/*  ★ 硬件能力：老 bitstream 的 APB 会丢弃非 16B 对齐的全屏基址 ⇒ 相机只能 16px 台阶。
 *    game_init() 里写一个非对齐基址再读回来判断；0 = 新硬件(任意字节偏移)，16 = 老硬件。  */
extern int g_cam_quant;
/*  跟随时玩家中心在屏幕中心的这块矩形里 ⇒ 相机不动（死区）。
 *  弹幕游戏里"玩家小幅走位画面不抖"比"永远居中"重要得多。  */
#define CAM_DEAD_W 160
#define CAM_DEAD_H 120
#define CAM_MAX_STEP 8                      /* 每帧最多挪几像素（玩家 3 px/帧 ⇒ 1:1 跟随） */

//==================== 场景（game_world.c）====================
/*  —— 房间系统（game_room.c）——
 *  房间不再硬编码：在 7×7 网格上随机生成一片连通的房间。
 *  每个房间渲染门时直接看【周围 4 格】：
 *    · 邻居格没有房间       → 墙（门不存在，不画）
 *    · 邻居是普通房         → normal 门
 *    · 邻居是特殊房         → 邻居的特殊门
 *    · 自己是特殊房         → 本房间所有门都用本房间的特殊门
 *  特殊房 = 非普通房（ITEM / SHOP / BOSS）；普通房 = START / EMPTY。
 *  生成规则：一片连通房间；道具房 1 间、Boss 房 1 间；
 *    特殊房之间不直接相连、空间 8 邻域不相邻；
 *    **特殊房必须是叶子：只接 1 间普通房**；
 *    **Boss 房不能直接连起点房**；
 *    普通房可以和特殊房相邻/连通，也可以同时接多间特殊房。
 *    **Boss 还活着时，Boss 房的门不能传送（门贴图照常渲染）。**
 *  game_room_validate() 启动自检。  */
#define MAP_COLS    40
#define MAP_ROWS    23
#define ROOM_GRID_W 7
#define ROOM_GRID_H 7
#define ROOM_COUNT  (ROOM_GRID_W * ROOM_GRID_H)
enum { ROOM_NONE = -1, ROOM_START, ROOM_EMPTY, ROOM_ITEM, ROOM_SHOP, ROOM_BOSS,
       ROOM_HIDDEN,          /* 隐藏房：1×1、基岩地板、四面隐藏门，炸开才进得去 */
       ROOM_HIDDEN_SUPER };  /* 预留：本版不生成 */

int  game_room_id(void);
int  game_room_start(void);          /* 本轮起点房间的格子编号 */
int  game_room_type(void);           /* 当前房间的类型：ROOM_START / EMPTY / ITEM / SHOP / BOSS */
int  game_room_layout(void);         /* 当前房间用的固定布局下标；-1 = 没有（走随机兜底） */
int  game_room_cell_gone(int room, int x, int y);      /* 这间房的 (x,y) 格上那个 interact 是否已被破坏 */
void game_room_clear_broken(void);                    /* 新一局 / 换一层：清空"已破坏"清单 */
void game_room_mark_cell_gone(int room, int x, int y); /* 标记破坏（重进房间 / 重建布局后不再出现） */
int  game_room_prev_layout(void);    /* 换房前的蓝图下标（-1 = 无） */
int  game_room_prev_flip(void);      /* 旧布局覆盖区使用旧房间朝向 */
int  game_room_prop_dirty(void);     /* 换房后还要为"旧蓝图的对象"标脏几帧（帧数递减） */
void game_room_pos(int *gx, int *gy);/* 当前房间在 7×7 网格里的坐标 */
int  game_room_type_at(int gx, int gy); /* 任意格子的房间类型；无房间 = ROOM_NONE */
int  game_room_owner_at(int gx, int gy);/* 这一格属于哪间房（锚点格号）；空格 = -1 */
void game_room_span(int *w, int *h);    /* 当前房间占几格（1×1 / 2×2） */
int  game_room_flip(void);              /* 异形房镜像：bit0 左右翻、bit1 上下翻（0 = 原图） */
int  game_room_cell_walkable(int tx, int ty);  /* 这一格能不能站人（地板 / 门洞） */
int  game_room_rect_walkable(int x, int y, int w, int h); /* 矩形覆盖的所有格都能走 */
/*  ★ 门按【格子位置】查，不再按方向：一间房可以有多扇同方向的门（L 形房、2×2 房）。
 *    每扇门通到哪间房，由它"朝房间内那一格"所在的格子、再往外走一格决定。 */
int  game_room_door_target(int tx, int ty);      /* 通到的房间 id；-1 = 外面没房间（画成墙） */
int  game_room_door_asset_at(int tx, int ty);    /* 这扇门用哪张贴图（非门格返回 normal） */
int  game_room_door_exists_at(int tx, int ty);   /* 外面有没有房间 */
int  game_room_door_open_at(int tx, int ty);     /* 能不能传送：有邻居且无怪 / 非 Boss 战 */
int  game_room_door_visible(int tx, int ty);    /* 这扇门【画不画】：隐藏连接没炸开就是 0（当墙） */
int  game_room_hidden_is_open(int a, int b);    /* 这两间房之间的隐藏连接炸开了没（a/b 顺序无关） */
int  game_room_hidden_open(int a, int b);       /* 记为已打开；返回 1 = 这次新打开 */
void game_room_hidden_reset(void);              /* 换层 / 新一局：全部封回去 */
int  game_room_blast_hidden(int cx, int cy, int r);  /* 炸弹：命中未开的隐藏门就打开，返回打开条数 */
int  game_room_validate(void);       /* 启动自检：起点/道具/商店/Boss 唯一 / 特殊房不直连 / 空间不相邻 / 连通 */
void game_room_reset(void);             /* 新一局：生成房间图并把玩家放回起点 */
void game_room_step(void);              /* 检查门触发；真正换房由主循环随后提交 */

/*  一帧的模型更新（玩家 / 生命 / Boss / 子弹 / 怪 / 炸弹 / 金币 / 商店）：只改 g，不画 */
void game_update_step(u32 keys, u32 aim_keys, u32 now); /* 按固定顺序推进 60 Hz 模型状态 */
void game_room_request(int target, int entry_dir);  /* 剧情跳转；entry_dir < 0 = 落在房间中心 */
void game_room_request_full(int target, int entry_dir); /* 同上，但强制整屏重铺（清旧怪贴图） */
#if GAME_BOSS_SHORTCUT
void game_room_debug_boss(void);
#endif
void game_room_mark_used(int room);  /* 标记"这间的道具已经拿过了" */
int  game_room_is_used(int room);
void game_room_mark_spawned(int room);  /* 特殊房内容（道具/Boss）只生成一次 */
int  game_room_is_spawned(int room);
void game_room_clear_spawned(int room); /* 死亡卡回退：死亡房间下次进入重新刷怪 */
void game_room_mark_respawn(int room);  /* 标记死亡房间下次进入刷新怪物（Boss 已死时不标） */
int  game_room_take_respawn(int room);  /* 取并清除刷新标记 */
int  game_room_take_reward(void);    /* 房间清理：返回 0..5 个金币，只给一次 */
int  game_room_is_cleared(int room);
void game_room_new_run(void);        /* 新一轮：清掉所有"已用过/已生成"标记 */
void game_room_door_bounds(int px, int py, int *x0, int *y0, int *x1, int *y1);
int  game_room_find_type(int type);    /* 按房间类型找格子编号（-1 = 没有） */
int  game_room_pending(void);              /* 是否有等待提交的换房请求 */
int  game_room_rebuild_pending(void);      /* 分批整屏重建是否尚未完成 */
int  game_room_finish_rebuild(void);       /* 推进一步分批重建；0 = 本帧让出 */
int  game_room_commit(void);               /* 提交已排队换房；0 = 需要后续分批完成 */
int  game_room_can_overlap_transition(void); /* 新地形已缓存时可与当前画面过渡重叠 */

/*  一张地图 = 框架参数（或异形表）+ 世界像素尺寸。
 *  ★ 墙 / 地板 / 门 / 角块都是【生成】的（game_frame.c），不再整张表存：
 *    两张框架 = 几十字节，加新房间只要加一行参数。
 *  map/rot 非 0 时才是"异形房"（手画的不规则地形），生成规则不生效。 */
typedef struct {
    const char          (*map)[MAP_MAX_COLS + 1];   /* 0 = 按 frame 生成 */
    const unsigned char (*rot)[MAP_MAX_COLS];       /* 0 = 按 frame 生成 */
    frame_geom_t         frame;                     /* 尺寸 / 竞技场 / 门 / 木纹 / 角块 */
    short                world_w, world_h;          /* 像素：相机可移动范围 */
} world_map_t;

int  game_world_map_count(void);        /* 地图池里有几张图 */
const world_map_t *game_world_map_at(int idx);   /* 越界返回 0 */
int  game_world_map_index(void);
int  game_world_map_key_for_layout(int layout);        /* 当前用的是池子里第几张 */

/*  逐格取地形 / 旋转：异形表或生成，调用方不用关心（越界返回 '.' / 0） */
char          game_world_tile_at(int tx, int ty);
unsigned char game_world_rot_at(int tx, int ty);
int           game_world_dir_at(int tx, int ty);   /* 这一格的"朝向"（门用它，已按镜像翻过） */
void game_world_select(int room);
int  game_world_door_count(void);
void game_world_door_at(int index, int *tx, int *ty);
void game_world_hidden_cell(int tx, int ty, int *hx, int *hy);
int  game_world_cols(void);             /* 当前地图多少格宽（普通 40 / Boss 45） */
int  game_world_rows(void);             /* 当前地图多少格高（普通 23 / Boss 35） */
int  game_world_w(void);                /* 当前世界像素宽（普通 1280 / Boss 1440） */
int  game_world_h(void);                /* 当前世界像素高（普通 736  / Boss 1120） */
int  game_world_is_boss(void);          /* 1 = 当前是 Boss 大房间（玩法语义，不是渲染语义） */
int  game_world_terrain(void);          /* 1 = 异形房（逐格地形，需要逐格碰撞） */

int  game_tile_asset(int tx, int ty);
void game_blit_tile(u32 fb, int tx, int ty, int fill_bg);
void game_draw_floor(u32 fb);
void game_floor_view_rows(int *ty0, int *ty1);
u32  game_draw_floor_rows(u32 fb, int ty0, int ty1);
u32  game_draw_floor_finish(u32 fb);
void game_floor_reset(void);
void game_floor_prepare_begin(u32 fb);
int  game_floor_needs_prepare(u32 fb);
u32  game_floor_reused_cells(void);
void game_floor_prepare_frame(u32 fb);
void game_floor_damage(int buf, dirty_map_t dirty);
void game_capture_floor_damage(void); /* 换房前先记录旧叠加层占用的区域。 */
u32  game_floor_cmd_count(void);
void game_mark_dirty(dirty_map_t dirty, int x, int y, int w, int h);
#if !GAME_BG_EXACT_RECT
void game_dirty_merge_tiles(dirty_map_t dirty, const dirty_map_t saved);
#endif
void game_mark_hud_dirty(dirty_map_t dirty, int x, int y, int w, int h);
void game_dirty_begin(void);                                      /* 帧首：清空脏格队列 */
int  game_dirty_touches(int x0, int y0, int x1, int y1);          /* 脏格队列是否碰到矩形 */
void game_mark_doors_dirty(dirty_map_t dirty);   /* 4 扇门的两格都标脏 */
void game_restore_dirty(u32 fb, dirty_map_t dirty);
void game_restore_dirty_view(u32 fb, dirty_map_t dirty); /* 只恢复当前视口；调用前先标相机新暴露区 */
int  game_restore_preupdate(u32 fb, dirty_map_t dirty);
int  game_restore_preupdate_active(u32 fb);
void game_restore_preupdate_cancel(void);
u32  game_restore_dirty_cells(void);
u32  game_restore_dirty_cmds(void);
u32  game_restore_direct_frames(void);
u32  game_restore_copy_frames(void);
void game_background_invalidate(void);
u32  game_background_build(u32 fb);
u32  game_background_refresh_doors(void); /* 复用有效地形，只刷新随房间变化的门贴图。 */
u32  game_background_warmup(void);
u32  game_background_fb(void);
int  game_background_ready(void);
int  game_background_cached_for_layout(int layout, int flip);
u32  game_background_cmd_count(void);
void game_restore_rect(u32 fb, int x, int y, int w, int h);
/*  ★ 可见性分两套坐标，别混用：
 *     world  : 场景/实体/子弹/怪 —— 世界坐标。必须整块落在【世界缓冲】内
 *              （硬件没有源裁剪，写出去会毁相邻内存），并且与【相机窗口】相交
 *              （不相交就不下发命令，省带宽）。普通房 cam=0，等价于老的屏幕裁剪。
 *     screen : HUD（屏幕坐标，画在 game_view_fb(fb) 上）。
 *  混用的后果：HUD 用 world 判定永远为真（不致命），实体用 screen 判定会把
 *  相机窗口外的怪全裁掉（Boss 房转身就看不见）。  */
int  game_visible_world(int x, int y, int w, int h);
int  game_visible_screen(int x, int y, int w, int h);

//==================== HUD（game_hud.c）====================
void game_hud_static(u32 fb);
void game_hud_mark_at(dirty_map_t dirty, int camx, int camy);
extern int game_hud_need_full;              /* 这一帧要不要整块重画（有精灵压到面板上） */
int  game_hud_prepare(dirty_map_t dirty, int buf,
                      int fps, int cmd_frame, int total_bullets, int coins);
int  game_hud_prepare_overlap(dirty_map_t dirty, int buf);
void game_hud_dirty_row(dirty_map_t dirty, int row);
void game_vital_prepare(dirty_map_t dirty, int buf);
void game_hud_invalidate(void);
void game_vital_invalidate(void);
void game_item_invalidate(void);
void game_item_reset_cache(void);
void game_item_prepare(dirty_map_t dirty, int buf);
void game_hud_frame(u32 fb, int buf, int fps, int cmd_frame, int total_bullets, int coins, int full);
void game_hud_boss_bar(u32 fb, int hp, int hp_max);
void game_hud_toast(u32 fb, const char *text);
/*  右上角小地图（game_hud.c）：换房时 refresh、下一层时 reset+refresh，每帧 draw  */
void game_minimap_reset(void);
void game_minimap_refresh(void);
void game_minimap_draw(u32 fb);
void game_minimap_rect(int *x, int *y, int *w, int *h);
/* 道具属性（都查 game_item.c 里的道具表） */
const char *game_item_desc(int item);       /* 屏幕上方那行说明（全大写 A-Z） */
int  game_item_fx(int item);                /* 效果种类：0 只换弹 / 1 增加弹幕 / 2 加命 */
int  game_item_count(void);                 /* 道具池里已有几件 */
int  game_item_try_death_save(u32 now);     /* 死亡判定：找到 LIFE_PLUS 就回退并返回 1 */
void game_item_draw_pool(u32 fb, int buf, int full);           /* 右侧道具池：偏暗的固定图标 */
void game_item_pool_rect(int *x, int *y, int *w, int *h);
int  game_item_pool_need(void);
/*  生命 / 命数（game_vital.c）  */
void game_vital_reset(void);
void game_vital_refill(void);               /* 新一关：血量补满，命数保留 */
int  game_vital_hit(u32 now);               /* 1=扣爱心 2=扣光进死亡 3=死亡卡回退 0=无敌中无效 */
void game_vital_step(u32 now);
int  game_vital_hp(void);
int  game_vital_lives(void);
int  game_vital_invuln(u32 now);
int  game_vital_dead(u32 now);              /* 1 = 死亡动画计时器仍在运行 */
void game_vital_extra_life(void);           /* 1UP! 加一条命 */
int  game_vital_heal_one(void);             /* 商店 heart：回 1 格血，不提升上限 */
int  game_vital_over(void);                 /* 1 = 命数耗尽，该整局重开了 */
void game_vital_clear_over(void);
void game_vital_mark_dirty(dirty_map_t dirty);
void game_vital_draw(u32 fb, u32 now, int full);
/*  HUD 任意位置的数字（生命显示要用）  */
void game_hud_number_at(u32 fb, int x, int y, int value, int digits);
void game_hud_char_at(u32 fb, int x, int y, int asset);
void game_hud_panel_row(u32 fb, int row);
void game_hud_label_at(u32 fb, int x, int y, const char *text);
int  game_item_icon(int item);              /* 图标素材下标 */
int  game_item_bullet(int item);            /* 吃了之后换成哪种弹 */
int  game_item_has(int item);
int  game_item_random_type(void);           /* 按“拿过减半”权重随机挑一个道具 */
void game_item_apply(int item);             /* 结算一个道具的效果（拾取/商店共用） */
void game_hud_mark_dirty(dirty_map_t dirty);   /* HUD 占的格子标脏 */
void game_hud_gameover(u32 fb, int score, int secs);
/*  GAME OVER 三行字占的矩形（脏区用）  */
#define GAMEOVER_X  480
#define GAMEOVER_Y  260
#define GAMEOVER_W  320
#define GAMEOVER_H  150

//==================== 角色（game_player.c）====================
void game_player_step(u32 keys, u32 aim_keys, u32 now); /* WASD 移动，小键盘控制朝向 */
void game_player_draw(u32 fb, u32 now, int buf);
void game_player_submit(void);          /* 登记到渲染队列：z = 底边 y */
void game_player_fire(void);            /* 按当前朝向发射一组玩家弹 */

//==================== 子弹（game_bullet.c）====================
int  game_bullet_spawn(int x, int y, int vx, int vy, int mode, int type, int enemy);
/*  亚像素版：速度单位是 1/8 像素/帧，用来做"精确瞄准"（整数速度角度会被量化得很糙）  */
int  game_bullet_spawn8(int x, int y, int vx8, int vy8, int mode, int type, int enemy);
void game_bullet_fire_enemy(int x, int y, int dir);
void game_bullets_step(u32 now);
void game_bullets_sync(int buf);                    /* 开始本缓冲历史；坐标路径同时快照 */
void game_bullets_record_history(int buf, int i);    /* 实际绘制后记录本弹覆盖的格子 */
void game_bullets_clear_history(int buf);           /* 清掉指定缓冲的旧弹覆盖范围 */
void game_bullets_mark_old(dirty_map_t dirty, int buf);
void game_bullets_submit(void);                     /* 登记到渲染队列：每颗弹 z = 底边 y */
int  game_bullets_alive(void);
void game_bullet_draw_stats(u32 *alpha, u32 *tint_enemy, u32 *tint_player);
int  game_enemy_bullets_alive(void);
#if GAME_BOSS_SKILL_DIAG
u32  game_enemy_spawn_rejects(void);
int  game_enemy_bullets_visible(void);
void game_dense_diag_start(void);
#endif
void game_enemy_bullets_clear(void);
void game_bullets_clear(void);
void game_bullets_perf_fill(int count);
#if GAME_BOSS_BULLET_SWEEP
void game_bullets_stress_fill(int count);
void game_bullets_stress_step(void);
int game_bullets_stress_visible(void);
void game_boss_stress_arm(void);
int game_boss_stress_step(void);
void game_boss_stress_frame(u32 now);
#endif
#if GAME_RUN_AUDIT
void game_run_audit_begin(void);
void game_run_audit_end(void);
void game_run_audit_frame(void);
#endif
#if GAME_BOSS_BULLET_SWEEP
#if GAME_CAPACITY_AUDIT
void game_capacity_work_begin(void);
void game_capacity_work_end(void);
void game_capacity_mark(unsigned phase);
void game_capacity_camera(u32 now);
#endif
#endif

//==================== 小怪 / 金币（game_mob.c / game_coin.c）====================
void game_mobs_dismiss(void); /* 保留旧覆盖区，直到两块缓冲都完成恢复。 */
void game_mobs_sync(int buf); /* 开始记录该缓冲本帧实际绘制的小怪位置 */
void game_mobs_reset(void);                         /* 换房时清空当前房间的小怪 */
void game_mobs_spawn(int count);                    /* 生成 count 只 slime */
void game_mobs_spawn_bug(int count);                /* 生成 count 只 bug（0,1,对称1,0 动画） */
int  game_mobs_spawn_at(int x, int y, int type);    /* 固定布局用：在 (x,y) 像素处放一只；1 = 成功 */
void game_mobs_step(u32 now);                        /* 动画 / 追玩家 / 接触扣血 / 清房 */
#if GAME_PROFILE && GAME_BOSS_STAGE_TEST
u32 game_mobs_nav_max_us(void);
#endif
void game_mobs_submit(void);
int  game_mobs_alive(void);
int  game_mobs_try_hit(int cx, int cy);              /* 玩家子弹命中一只 slime；1 = 命中 */
int  game_mobs_try_hit_ex(int cx, int cy, int bullet_type);
void game_coins_reset(void);
void game_coins_spawn(int count);                    /* 清理奖励：0..5 个 coin（避开石头） */
void game_coins_step(u32 now);                       /* 拾取：只捡【当前房间】的 coin */
void game_coins_sync(int buf);                       /* 每帧记下 old_coin（同 bullets，按房间过滤） */
void game_coins_submit(void);
int  game_coin_overlap(int x, int y, int w, int h);  /* 石头落点是否压住当前房间的金币 */

//==================== 蜘蛛网（game_web.c）====================
int  game_web_slow_at(int x, int y);                 /* 当前房间的网是否踩中（减速） */
void game_web_submit(int buf);                          /* 画在地板上、人物下面 */
int  game_web_overlap(int x, int y, int w, int h);   /* 矩形是否压住当前房间的网 */

//==================== 可交互石头（game_stone.c）====================
void game_stones_reset(void);
void game_static_draw_reset(void);
int  game_static_draw_force(void);                        /* 新一局 / 新一关：碰撞格失效（布局本身不变） */
int  game_stones_blast(int cx, int cy, int r);       /* 炸弹冲击波：炸碎半径内的石头；返回块数 */
int  game_stone_block(int px, int py);               /* 兼容查询：当前碰撞盒是否压住阻挡物 */
int  game_stone_block_move(int old_px, int old_py, int px, int py); /* 按移动方向穿越边界判定 */
int  game_stone_hit_at(int x, int y);                /* 弹幕中心是否撞到石头（撞到就消失） */
int  game_stone_has_blockers(void);                  /* 当前房间是否有挡弹格，供弹幕热路径快速跳过 */
int  game_stone_overlap(int x, int y, int w, int h); /* 矩形是否和石头重叠（金币落点避让） */
int  game_stones_erase_at(int i, int *x, int *y);    /* 第 i 个"要补擦"的格子；1 = 有效（i 从 0 递增） */
void game_stones_erase_tick(void);                   /* 帧末：待擦队列 ttl-- */
void game_stones_submit(int buf);

//==================== Boss（game_boss.c）====================
int  game_nearest_enemy(int *tx, int *ty);   /* 最近的敌人中心；没有返回 0 */
int  game_enemy_in_dir(int dir, int *tx, int *ty); /* 当前朝向方向里最近的敌人 */
extern const pattern_t  g_boss_patterns[];
extern const int        g_boss_pattern_n;
extern const move_t     g_boss_moves[];
extern const int        g_boss_move_n;
extern const move_set_t g_boss_move_set;    /* Boss 用的一套招式 */
extern const move_set_t g_minion_move_set;  /* 小怪用的一套（预留） */

void game_shot_init(shot_t *s, const move_set_t *set, int enemy, int type);
void game_shot_begin(shot_t *s, int move_idx);
void game_shot_step(shot_t *s);
void game_shot_stop(shot_t *s);
extern const boss_def_t g_boss_def;
extern const boss_phase_def_t g_boss_phases[BOSS_PHASE_COUNT];
void game_boss_resume(void);
int game_boss_shot_limit(void);

void game_boss_step(u32 now);
void game_boss_draw_shield(u32 fb, u32 now);   /* 护盾单独画在元素之上 */
void game_boss_hit(u32 now);            /* 玩家子弹打到 Boss 了 */
void game_boss_draw(u32 fb, u32 now, int buf);
void game_boss_draw_exit(u32 fb);       /* Boss 击破后房间顶部的 exit */
void game_boss_submit(void);            /* 登记 Boss 本体 + 锁血护盾 */

//==================== 压测脚手架（game_perf.c，正常游戏时不参与逻辑）====================
void game_perf_init(void);
int  game_perf_frozen(void);        /* mode 1：冻结玩法 */
int  game_perf_pinned(void);        /* mode 2/3：子弹恒定在飞数 */
int  game_perf_keep_bg(void);       /* mode 3 = 0：不恢复背景 */
int  game_perf_tick(u32 now);       /* 每 120 帧打点；返回 0 = 跑完了 */

//==================== 道具（game_item.c）====================
void game_start_fight(void);            /* Boss 开打：FIGHT + 招式表初始化 */
void game_item_spawn_random(int x, int y);  /* 在 (x,y) 随机放一个道具（道具房用） */
void game_item_spawn_at_player(void);      /* Y 键：在玩家脚下生成一个道具+底座 */
void game_item_try_pickup(u32 now);
void game_item_draw(u32 fb, u32 now, int buf);
void game_item_submit(void);            /* 登记到渲染队列：z = 底边 y */
void game_triprism_submit(void);        /* 环绕棱镜；每个棱镜按自身底边深度登记 */

/* ---- 炸弹 ---- */
void game_bombs_reset(void);
void game_bomb_spawn_pickup(int x, int y, int room);
void game_bomb_pickup_try(u32 now);
int  game_bomb_place(u32 now);
int  game_bomb_push(int *nx, int *ny, int oldx, int oldy, u32 now);
void game_bombs_step(u32 now);
void game_bombs_prepare(dirty_map_t dirty, int buf);
void game_bombs_submit(void);

//==================== 商店（game_shop.c）====================
void game_shop_reset(void);                                 /* 新一局 / 新一关 */
void game_shop_enter(void);                                 /* 进商店：首次决定随机道具 */
void game_shop_step(u32 now);                               /* 身体碰商品：扣币 / 回血 / 给道具 */
void game_shop_mark_old(dirty_map_t dirty, int buf); /* 擦这块缓冲上次画的商品 */
void game_shop_sync(int buf);                               /* 每帧把 old_shop 对齐到当前房间 */
void game_shop_submit(void);                                /* 画 heart / 道具 / 16×8 价格 */
