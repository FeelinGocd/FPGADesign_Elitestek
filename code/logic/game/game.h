//==============================================================================
//  game.h —— 游戏 demo 的公共头（宏 / 类型 / 唯一状态 g / 模块接口）
//------------------------------------------------------------------------------
//  干什么      : 各模块共用的东西都在这里，别的文件不重复定义。
//  上一级是谁  : src/main.c → game_demo.h → game_demo_run()（game/game_main.c）
//  接口契约    :
//    · 状态只有一份：game_state.c 里的 g；别的模块只读写 g，不另立全局
//    · 坐标就是屏幕坐标 —— 本作不做滚屏、没有相机；每间房都用同一套屏幕坐标
//      （换房 = 两块缓冲整屏重铺，不是"移动相机"）
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
#include "render_list.h"    /* 渲染队列：每个元素带自己的 z，统一排序后画 */
#include "assets_desc.h"    /* ASSET_xxx 枚举 / asset_desc_t（表见下面的 g_assets 宏） */

//==================== 屏幕 ====================
#define SCR_W 1280
#define SCR_H 720
#define TILE 32                             /* 地砖边长 */
#define GRID_W (SCR_W / TILE)               /* 脏区格子数：40 */
#define GRID_H ((SCR_H + TILE - 1) / TILE)  /* 23（最后一行不满） */

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

//==================== 玩法参数 ====================
#define N_BULLETS 256
#define N_MOBS     16                       /* 当前房间的小怪（slime） */
#define N_COINS    16                       /* 当前房间的清理奖励金币 */
#define N_WEBS     8                        /* 普通房间的蜘蛛网（减速） */
#define N_STONES   8                        /* 房间里的可交互石头（阻挡，不破坏） */

//==================== 判定框（★ 不是包围盒！）====================
//  ★ 人物碰撞体积 = 【肚子】那一小块：头、脚、尾巴都不参与判定。
//    这样进门时头可以探进墙/门里，不会在门口被头卡住。
//    子弹也只用【中心点】去碰，不用 32×32 的包围盒（那圈透明边会让人"隔空受伤"）。
//    这块同时用于：移动/门洞边界、门洞触发、道具拾取、中弹判定、Boss 瞄准。
#define PB_OFF_X 10                     /* 肚子：相对 40×56 贴图左上角 */
#define PB_OFF_Y 28
#define PB_W 20
#define PB_H 20
#define BB_OFF_X 14                     /* Boss 身体 */
#define BB_OFF_Y 36
#define BB_W 52
#define BB_H 56


//==================== 弹道 ====================
enum { MOTION_LINE, MOTION_SINE, MOTION_ACCEL, MOTION_SPIRAL, MOTION_HOME };
#define ANIM_FRAMES 5u                      /* 序列表长度 */
#define ANIM_HOLD 6u                        /* 每一帧保持几拍 */
#define FIRE_EVERY 10u                      /* 每 10 帧开一枪（约 6 发/秒） */
/*  玩家有惯性：输入决定"目标速度"，实际速度每帧朝目标逼近（加速/刹车）。
 *  单位是 1/8 像素，这样 3 px/帧 这种低速度也能做出加减速的过程。  */
#define PLAYER_MAX_V8    24                 /* 直向最高速 = 3 px/帧（24/8） */
#define PLAYER_DIAG_V8   16                 /* 斜向最高速 = 2 px/帧（16/8） */
#define PLAYER_MUZ_DY    20                 /* 枪口：头和身体的交界（相对精灵顶边） */
#define PLAYER_ACCEL_V8   3                 /* 每帧加速量：约 8 帧到全速 */
#define PLAYER_BRAKE_V8   6                 /* 松手每帧减速量：约 4 帧停住 */
#define PLAYER_BULLET_SPEED 5
#define PLAYER_TRIPLE_SPEED 3               /* 三向弹：慢一点（"SLOWER BUT MORER"） */
/*  三向弹的张角：1 格 = 360/64 = 5.625°
 *    4 格 = 22.5°  ← 默认（接近 20°）
 *    5 格 = 28.1°  ← 接近 30°
 *    8 格 = 45°    ← 原来的值，太散
 *  想要更精确的 20°/30° 只能改这半格粒度：要么 4 格（22.5°），要么 5 格（28.1°）。  */
/*  生命：3 颗心、3 条命；被击中扣 1 颗心，扣光演死亡动画后补满、命数 −1  */
#define HP_MAX              3
#define LIVES_START         1               /* 初始 1 条命；显示规则：1 不显示 */
/*  ★ 调试开关：1 = 无敌期人物闪烁；0 = 不闪（人物永远画）
 *    人物"不见"时先用 0 编译一次，能分辨是闪烁逻辑还是别的原因。  */
#define PLAYER_BLINK_ON_INVULN 0

#define HURT_INVULN_MS      1000u           /* 被打中之后 1 s 无敌 */
#define RESPAWN_INVULN_MS   2000u           /* 补满之后 2 s 保护（人物闪烁） */

#define PLAYER_TRIPLE_SPREAD 4
#define ENEMY_BULLET_SPEED 2
/* 弹幕图案按"表格 + 连发队列"跑：每发之间隔 gap 帧，一轮打完歇 rest 帧 */
#define INTER_VOLLEY_GAP 8                  /* 同一招里，两轮之间的短间隔 */
/*  子弹的"寿命"：不一定要飞到屏幕外才消失  */
/*  敌方弹 = base 弹 + 一层半透明红（颜色合成的 strength 就是这层的浓度）  */
#define ENEMY_TINT_RGB    0x00FF2020u
#define ENEMY_TINT_K      170u              /* 0 = 不上色，255 = 全红 */
#define PLAYER_BULLET_RANGE 420             /* 自机弹射程（像素）：不到半个屏幕 */
#define BOSS_BULLET_LIFE    260             /* 敌方弹最长存活帧数（约 4.3 秒），0 = 不限 */
#define WAVE_CLEAR_MAX 8                    /* 场上敌弹少于这个数才算"上一招清完" */
#define ANGLE_STEPS 64                      /* 一圈分 64 格；a=0 右、16 下、32 左、48 上 */
#define BOSS_FRAME_MS 250u                  /* Boss 待机帧多久换一次 */
#define PICKUP_MS 1000u
#define HURT_MS 1000u
#define PLAYER_RESERVE 32                   /* 子弹池里永远给玩家留 32 槽（否则会静默哑火） */
#define ENEMY_CAP 120                       /* 敌方弹上限：一颗弹 ≈ 3 条命令（自己 + 擦除），是主要开销 */

//==================== Boss 血量与阶段 ====================
#define BOSS_HP_MAX     300                 /* 满血 */
#define BOSS_HIT_DMG    2                   /* 玩家每发伤害（射速降了，单发补一点） */
#define BOSS_GATE_1     200                 /* 打到 200 → 锁血，进第一段弹幕雨 */
#define BOSS_GATE_2     100                 /* 打到 100 → 锁血，进第二段弹幕雨 */
#define BOSS_GATE_END   0                   /* 到 0 = 击破 */
#define RAIN_MS         6000u               /* 锁血阶段持续多久（弹幕雨） */
#define RAIN_WAVE       20u                 /* 弹幕雨：每 20 帧来一波（有间歇，不是一直下） */
#define RAIN_DROP       5                   /* 一波落几颗 */
#define HIT_FLASH_MS    120u                /* 命中闪红多久 */
#define BLINK_ON_MS     55u                 /* 护盾闪白的"亮"持续 */
#define BLINK_OFF_MS    70u                 /* 护盾闪白的"灭"持续 */
#define BLINK_COOL_MS   1000u               /* 闪完两下之后，1 秒内不再闪 */
#define BOSS_RESPAWN_MS 3000u               /* 击破后停多久再开一局（现在由 exit 触发） */
#define BOSS_WAIT_MS    1000u               /* 进 Boss 房先等 1 s 再出招 */

//==================== 颜色 ====================
#define FLOOR_BG 0x493326u

//==================== 帧缓冲 ====================
#define FB_C 0x08000000u
#define FB_D 0x08800000u

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
#define HUD_LABEL_CMD_COUNT 67              /* 9 行标签 38 + 9 行数字 21 + COINS 5 字 + 3 位数字 = 67 */
/* HUD 占哪几格（脏区单位）：前 3 行 × 前 6 列 = 192×96 像素。
   这几格每帧都要进脏区，否则上一次画的数字不会被擦掉。 */
#define HUD_CELLS_X 6
#define HUD_CELLS_Y 11                      /* FPS/BULLETS/CMD/STAGE + 心 + DMG/MOVE/BSPD/RANGE/RATE/COINS */
/* Boss 血条（顶部居中）；它占的格子也要标脏，否则擦不干净会留残影 */
#define HUD_HP_W   400
#define HUD_HP_H   14
#define HUD_HP_X   ((SCR_W - HUD_HP_W) / 2)
#define HUD_HP_Y   30                       /* 血条下移，上方给 "BOSS" 留位置 */
#define HUD_HP_LABEL_Y 2                    /* "BOSS" 四个字的 y */
/* 拾取字幕：显示在 Boss 血条下方（字模只有大写 A-Z，文案必须全大写） */
#define HUD_TOAST_MS  2500u                 /* 显示多久 */
#define HUD_TOAST_Y   58                    /* 字模顶边 y（血条 30..44，留 14 px 间距） */
#define GAMEOVER_MS     5000u               /* GAME OVER 停 5 秒后自动重开 */
#define SCORE_HIT       10                  /* 每打中 Boss 一发 */
#define SCORE_KILL      1000                /* 击破一个 Boss */
/*  场地（竞技场）：内圈 = wood，边界 = bedrock（2 格厚 = 64 px）
 *  ★ 玩家/Boss 的活动范围也用这一组，改这里场地和碰撞一起变。  */
#define ARENA_X0   288
#define ARENA_Y0   128
#define ARENA_X1   1024
#define ARENA_Y1   544

#define HUD_LABEL_X 9                       /* 标签左边距 */
#define HUD_VALUE_X 128                     /* 数值左边距：必须让开最长的标签 "BULLETS"(7 字 = 112 px) */

#define VITAL_ROW   4                       /* 面板上"血量"那一行（0 基）：原 ITEMS 的位置 */
#define KEY_DEBUG   (1u << 5)               /* 串口手柄 bit5：按一下 T 切换无敌 */
#define HUD_ROW_H   32                      /* 行高（两行字模） */

//==================== 按键位 ====================
#define KEY_LEFT     0x1u
#define KEY_RIGHT    0x2u
#define KEY_DOWN     0x4u
#define KEY_UP       0x8u
#define KEY_RESTART  0x10u                  /* R 键（只从串口来）：按一下重开一局（边沿触发） */

//==================== 性能诊断（正常演示用 0）====================
//  0 = 正常游戏
//  1 = 冻结玩法，按 16/32/64/96/128/192/256 阶梯扫子弹数
//  2 = 256 发满场，正常恢复背景
//  3 = 256 发满场，【不】恢复背景（只为分离背景恢复的代价，画面会留拖影）
#define PERF_TEST_MODE 0
#define PERF_LEVELS 7

enum { RIGHT, LEFT, DOWN, UP };
enum { BULLET_A, BULLET_CHILI, BULLET_URANUS };   /* 弹种（贴图/伤害） */
enum { ITEM_CHILI, ITEM_URANUS, ITEM_EYES, ITEM_1UP, ITEM_COUNT };   /* 道具编号（≠ 弹种，属性查表） */
/*  玩法阶段
 *    EXPLORE    起始房 / 空房 / 道具房 / 商店房：没有 Boss，自由走动
 *    FIGHT      Boss 战（进 Boss 房立即开打，没有开场延迟）
 *    GAMEOVER   命尽 */
enum { PHASE_EXPLORE = 0, PHASE_FIGHT, PHASE_GAMEOVER };

typedef struct {
    unsigned char active;
    unsigned char mode;         /* MOTION_xxx */
    unsigned char type;         /* 贴图：BULLET_A / CHILI / URANUS */
    unsigned char enemy;        /* 1 = 敌方弹 */
    unsigned char use8;         /* 1 = 用 vx8/vy8 走位（精细角度），0 = 用整数 vx/vy */
    signed char   rx, ry;       /* 亚像素余量（0..7） */
    short x, y;                 /* 位置（像素） */
    short vx, vy;               /* 速度：LINE/HOME = 每帧位移；SINE = 振幅/下落速度；SPIRAL = 半径/角速度；ACCEL = 初速/加速度 */
    short base_x;               /* SINE 的中线 */
    unsigned short age;         /* 活了几帧（SINE/SPIRAL/ACCEL/HOME 都要） */
    short vx8, vy8;             /* 速度的 1/8 像素精度（只有 use8 时生效） */
    unsigned short dist;        /* 已经飞了多远（1/8 像素，曼哈顿累加） */
    unsigned short range;       /* 射程：飞满这么远就消失（0 = 不管，飞出去为止） */
    unsigned short life;        /* 还能活几帧（0 = 不管，飞出屏幕为止） */
} bullet_t;
typedef struct { short x, y; unsigned char active; } old_t;
typedef struct { short x, y; unsigned char active; unsigned char type; } item_t;
typedef struct {
    unsigned char active;
    unsigned char hp;
    short x, y;
    u32 move_next;              /* 下一次起跳的时间 */
    u32 hop_until;              /* 这一跳腾空到什么时候（帧 1 / 抬升都看它） */
} mob_t;
typedef struct { unsigned char active; short x, y; } coin_t;
typedef struct { unsigned char active; short x, y; } web_t;
typedef struct { unsigned char active; short x, y; } stone_t;
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
} pattern_t;

typedef struct {
    unsigned char pattern;  /* g_boss_patterns[] 的下标 */
    unsigned char times;    /* 这个图案连播几次（0 当 1） */
} move_t;

enum { BOSS_STILL, BOSS_SINE_Y, BOSS_SINE_X };      /* Boss 的运动方式（以后用） */

typedef struct {
    unsigned char kind;     /* BOSS_STILL / BOSS_SINE_Y / BOSS_SINE_X */
    unsigned char amp;      /* 幅度（像素） */
    unsigned char period;   /* 周期（以 16 ms 为一格） */
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
    unsigned int rain_ms;   /* 锁血弹幕雨持续多久 */
    unsigned char rain_wave;/* 每几帧一波雨 */
    unsigned char rain_drop;/* 一波几颗 */
    int w, h;               /* 贴图尺寸 */
    int muzzle_dx, muzzle_dy;   /* 炮口相对左上角 */
    const move_set_t *moves;    /* ★ 用哪套招式（数据） */
    boss_motion_t motion;       /* 运动方式 */
} boss_def_t;

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
    u32 anim, fire_tick, anim_step;
    u32 pickup_until, hurt_until;
u32 gameover_until;                 /* GAME OVER 结束时刻（PHASE_GAMEOVER 用） */
int score;                          /* 本局得分 */
int gameover_stage;                 /* 结束时的关卡（要显示出来） */
    u32 toast_until;                    /* 拾取字幕显示到什么时候（0 = 没有） */
    int toast_type;                     /* 字幕对应哪种道具 */
    int bullet_type;                    /* 当前弹种（拾取道具后改变） */
    int fire_triple;                    /* 1 = 三向齐射（inner_eyes，一旦拿到就常驻） */
    unsigned item_mask;                 /* ★ 道具池：拿到过哪个道具就置对应 bit（效果累加） */
    int      item_order[ITEM_COUNT];    /* ★ 拾取顺序：item_order[0] 是最先拿的（池子从上往下排） */
    int      item_order_n;              /* 池子里已经排了几个 */
    int held_type;                      /* 身上的道具编号（ITEM_xxx） */
    /* Boss */
    int boss_x, boss_y, boss_pattern;
    u32 boss_tick;
    int stage;                          /* 第几关：击破一次 Boss +1，弹幕跟着变密 */
    int phase;                          /* PHASE_EXPLORE / PHASE_FIGHT / PHASE_GAMEOVER */
    int boss_hp, boss_gate;             /* 当前血量 / 下一个锁血线 */
    int boss_locked;                    /* 1 = 锁血中（无伤 + 弹幕雨） */
    u32 boss_lock_until, boss_flash_until, boss_dead_until;
    u32 boss_wait_until;                /* 进 Boss 房后 1 s 内不出招、也不吃伤害 */
    int exit_active;                    /* Boss 击破后，房间顶部出现的 exit */
    int exit_room, exit_x, exit_y;
    int exit_dirty;                     /* exit 消失后还要擦几帧（两块缓冲） */
    int boss_blink_cnt, boss_blink_on;  /* 护盾被打中：快闪两下（on/off 交替） */
    u32 boss_blink_next, boss_blink_cool;
    shot_t shot;                        /* ★ Boss 挂的弹幕发射器（小怪以后也各挂一个） */
    /* 道具 */
    item_t item;
    /* 子弹池 */
    bullet_t bullets[N_BULLETS];
    /* 每块缓冲各记一份"这个元素上次画在哪"（双缓冲下两块内容不同） */
    old_t old_bullets[2][N_BULLETS];
    old_t old_player[2], old_boss[2], old_item[2];
    /* 小怪 / 清理奖励金币 */
    mob_t mobs[N_MOBS];
    coin_t coin_items[N_COINS];
    old_t old_mob[2][N_MOBS];
    old_t old_coin[2][N_COINS];
    web_t webs[N_WEBS];
    old_t old_web[2][N_WEBS];
    stone_t stones[N_STONES];
    old_t old_stone[2][N_STONES];
    int coins;                          /* 本局金币数（下一层保留，新一局清零） */
    /* 显示与统计 */
    int front;                          /* 正在显示的那块；后台 = front ^ 1 */
    int door_dirty;                     /* 换房后还要重画几帧门（两块缓冲各一次） */
    int cmd_frame;                      /* 上一帧发了多少条命令 */
    int fps;
    u32 fps_epoch, fps_frames;
    u32 cmd_before;                     /* 本帧开始时的命令完成计数 */
    u32 probe_epoch, probe_frames, probe_frame_no;
    int probe_peak, probe_peak_frame, probe_peak_cmd;
} game_t;

extern game_t g;

//==================== 素材表（唯一一份，见 game_assets.c）====================
//  assets_desc.h 里的表是 static 的 ⇒ 每个包含它的 .c 都会复制一份（59 项 ≈ 1.2 KB）。
//  这里统一指到 game_assets.c 里那份，其它模块照旧写 g_assets[i].base_word。
/* 共享的 sin 表：sin(2πi/64)×64，值域 -64..64（弹道摆动 / 道具浮动都用它） */
extern const signed char g_sin_tab[64];

const asset_desc_t *game_asset_table(void);
#define g_assets (game_asset_table())

//==================== 输入（game_input.c）====================
extern int game_debug_invuln;               /* 串口 debug0 打开的无敌 */
u32  game_keys_read(void);
int  game_take_debug_toggle(void);          /* 取一次"按了 T"（取完清） */          /* 串口虚拟手柄优先，没串口就用板载按键 */
int  game_take_restart(void);       /* 这一帧有没有"按过 R"（取完清零） */

//==================== 状态 / 时钟 / 上屏（game_state.c）====================
u32  game_ms(void);
void game_reset_state(void);            /* 只复位数据，不重画 */
int  game_restart(void);                /* 复位 + 把两块缓冲重铺一遍（R 键用） */
void game_enter_gameover(u32 now);      /* 命尽：进 GAME OVER 画面 */
void game_flow_enter_room(void);        /* 换房之后：按房间类型布置内容（道具 / Boss） */
void game_boss_spawn(void);             /* 第一次进 Boss 房：满血到场中，立即开打 */
int  game_init(void);
int  game_finish_gpu(void);
int  game_present(u32 fb);

//==================== 场景（game_world.c）====================
/*  —— 房间系统（game_room.c）——
 *  房间不再硬编码：在 6×6 网格上随机生成一片连通的房间。
 *  每个房间渲染门时直接看【周围 4 格】：
 *    · 邻居格没有房间       → 墙（门锁死，不画）
 *    · 邻居是普通房         → normal 门
 *    · 邻居是特殊房         → 邻居的特殊门
 *    · 自己是特殊房         → 本房间所有门都用本房间的特殊门
 *  特殊房 = 非普通房（ITEM / SHOP / BOSS）；普通房 = START / EMPTY。
 *  生成规则：一片连通房间；道具房 1 间、Boss 房 1 间；
 *    特殊房之间不直接相连、空间 8 邻域不相邻；
 *    **特殊房必须是叶子：只接 1 间普通房**；
 *    **Boss 房不能直接连起点房**；
 *    普通房可以和特殊房相邻/连通，也可以同时接多间特殊房。
 *    **Boss 还活着时，Boss 房所有门锁死，不能离开。**
 *  game_room_validate() 启动自检。  */
#define MAP_COLS    40
#define MAP_ROWS    23
#define ROOM_GRID_W 6
#define ROOM_GRID_H 6
#define ROOM_COUNT  (ROOM_GRID_W * ROOM_GRID_H)
enum { ROOM_NONE = -1, ROOM_START, ROOM_EMPTY, ROOM_ITEM, ROOM_SHOP, ROOM_BOSS };

int  game_room_id(void);
int  game_room_start(void);          /* 本轮起点房间的格子编号 */
int  game_room_type(void);           /* 当前房间的类型：ROOM_START / EMPTY / ITEM / SHOP / BOSS */
void game_room_pos(int *gx, int *gy);/* 当前房间在 6×6 网格里的坐标 */
int  game_room_type_at(int gx, int gy); /* 任意格子的房间类型；无房间 = ROOM_NONE */
int  game_room_door_asset(int dir);  /* 看周围 4 格的类型决定这扇门用哪张贴图 */
int  game_room_door_exists(int dir); /* 邻居格有没有房间（门本身存在，不看锁） */
int  game_room_door_open(int dir);   /* 邻居格有房间且当前房间没有怪 */
int  game_room_validate(void);       /* 启动自检：起点/道具/Boss 唯一 / 特殊房不直连 / 空间不相邻 / 连通 */
void game_room_reset(void);
void game_room_step(void);
void game_room_request(int target, int entry_dir);  /* 剧情跳转；entry_dir < 0 = 落在房间中心 */
void game_room_mark_used(int room);  /* 标记"这间的道具已经拿过了" */
int  game_room_is_used(int room);
void game_room_mark_spawned(int room);  /* 特殊房内容（道具/Boss）只生成一次 */
int  game_room_is_spawned(int room);
int  game_room_take_reward(void);    /* 房间清理：返回 0..5 个金币，只给一次 */
int  game_room_is_cleared(int room);
void game_room_new_run(void);        /* 新一轮：清掉所有"已用过/已生成"标记 */
void game_room_door_bounds(int px, int py, int *x0, int *y0, int *x1, int *y1);
int  game_room_pending(void);
int  game_room_commit(void);

const char          (*game_world_map(void))[MAP_COLS + 1];
const unsigned char (*game_world_rot(void))[MAP_COLS];
void game_world_select(int room);

int  game_tile_asset(int tx, int ty);
void game_blit_tile(u32 fb, int tx, int ty, int fill_bg);
void game_draw_floor(u32 fb);
u32  game_floor_cmd_count(void);
void game_mark_dirty(unsigned char dirty[GRID_H][GRID_W], int x, int y, int w, int h);
void game_mark_doors_dirty(unsigned char dirty[GRID_H][GRID_W]);   /* 4 扇门的两格都标脏 */
void game_restore_dirty(u32 fb, unsigned char dirty[GRID_H][GRID_W]);
void game_restore_rect(u32 fb, int x, int y, int w, int h);
int  game_visible(int x, int y, int w, int h);

//==================== HUD（game_hud.c）====================
void game_hud_static(u32 fb);
extern int game_hud_need_full;              /* 这一帧要不要整块重画（有精灵压到面板上） */
int  game_hud_prepare(unsigned char dirty[GRID_H][GRID_W], int buf,
                      int fps, int cmd_frame, int total_bullets, int coins);
void game_hud_dirty_row(unsigned char dirty[GRID_H][GRID_W], int row);
void game_vital_prepare(unsigned char dirty[GRID_H][GRID_W], int buf);
void game_hud_invalidate(void);
void game_vital_invalidate(void);
void game_item_invalidate(void);
void game_item_reset_cache(void);
void game_item_prepare(unsigned char dirty[GRID_H][GRID_W], int buf);
void game_hud_frame(u32 fb, int buf, int fps, int cmd_frame, int total_bullets, int coins, int full);
void game_hud_boss_bar(u32 fb, int hp, int hp_max);
void game_hud_toast(u32 fb, const char *text);
/*  右上角小地图（game_hud.c）：换房时 refresh、下一层时 reset+refresh，每帧 draw  */
void game_minimap_reset(void);
void game_minimap_refresh(void);
void game_minimap_draw(u32 fb);
/* 道具属性（都查 game_item.c 里的道具表） */
const char *game_item_desc(int item);       /* 屏幕上方那行说明（全大写 A-Z） */
int  game_item_fx(int item);                /* 效果种类：0 只换弹 / 1 三向 / 2 加命 */
int  game_item_count(void);                 /* 道具池里已有几件 */
void game_item_draw_pool(u32 fb, int buf, int full);           /* 右侧道具池：偏暗的固定图标 */
void game_item_pool_rect(int *x, int *y, int *w, int *h);
int  game_item_pool_need(void);
/*  生命 / 命数（game_vital.c）  */
void game_vital_reset(void);
void game_vital_refill(void);               /* 新一关：血量补满，命数保留 */
int  game_vital_hit(u32 now);               /* 1=扣爱心 2=扣光进死亡 0=无敌中无效 */
void game_vital_step(u32 now);
int  game_vital_hp(void);
int  game_vital_lives(void);
int  game_vital_invuln(u32 now);
int  game_vital_dead(u32 now);              /* death animation timer is active */
void game_vital_extra_life(void);           /* 1UP! 加一条命 */
int  game_vital_over(void);                 /* 1 = 命数耗尽，该整局重开了 */
void game_vital_clear_over(void);
void game_vital_mark_dirty(unsigned char dirty[GRID_H][GRID_W]);
void game_vital_draw(u32 fb, u32 now, int full);
/*  HUD 任意位置的数字（生命显示要用）  */
void game_hud_number_at(u32 fb, int x, int y, int value, int digits);
void game_hud_char_at(u32 fb, int x, int y, int asset);
void game_hud_panel_row(u32 fb, int row);
void game_hud_label_at(u32 fb, int x, int y, const char *text);
int  game_item_icon(int item);              /* 图标素材下标 */
int  game_item_bullet(int item);            /* 吃了之后换成哪种弹 */
void game_hud_mark_dirty(unsigned char dirty[GRID_H][GRID_W]);   /* HUD 占的格子标脏 */
void game_hud_gameover(u32 fb, int score, int secs);
/*  GAME OVER 三行字占的矩形（脏区用）  */
#define GAMEOVER_X  480
#define GAMEOVER_Y  260
#define GAMEOVER_W  320
#define GAMEOVER_H  150

//==================== 角色（game_player.c）====================
void game_player_step(u32 keys, u32 now);
void game_player_draw(u32 fb, u32 now, int buf);
void game_player_submit(void);          /* 登记到渲染队列：z = 底边 y */
void game_player_fire(void);            /* 自动瞄准最近的敌人 */

//==================== 子弹（game_bullet.c）====================
int  game_bullet_spawn(int x, int y, int vx, int vy, int mode, int type, int enemy);
/*  亚像素版：速度单位是 1/8 像素/帧，用来做"精确瞄准"（整数速度角度会被量化得很糙）  */
int  game_bullet_spawn8(int x, int y, int vx8, int vy8, int mode, int type, int enemy);
void game_bullet_fire_enemy(int x, int y, int dir);
void game_bullets_step(u32 now);
void game_bullets_sync(int buf);                    /* 记录"这块缓冲这次画在哪"（每帧一次） */
void game_bullets_submit(void);                     /* 登记到渲染队列：每颗弹 z = 底边 y */
int  game_bullets_alive(void);
int  game_enemy_bullets_alive(void);
void game_bullets_clear(void);
void game_bullets_perf_fill(int count);

//==================== 小怪 / 金币（game_mob.c / game_coin.c）====================
void game_mobs_reset(void);                         /* 换房时清空当前房间的小怪 */
void game_mobs_spawn(int count);                    /* 生成 count 只 slime */
void game_mobs_step(u32 now);                        /* 2 帧动画 / 追玩家 / 接触扣血 / 清房 */
void game_mobs_submit(void);
int  game_mobs_alive(void);
int  game_mobs_try_hit(int cx, int cy);              /* 玩家子弹命中一只 slime；1 = 命中 */
void game_coins_reset(void);
void game_coins_spawn(int count);                    /* 清理奖励：0..5 个 coin */
void game_coins_step(u32 now);                       /* 拾取：肚子碰到 coin，金币 +1 */
void game_coins_submit(void);

//==================== 蜘蛛网（game_web.c）====================
void game_web_reset(void);                           /* 换房时清空当前房间的网 */
void game_web_spawn(int count);                      /* 普通房随机生成 count 张网 */
int  game_web_slow_at(int x, int y);                 /* 这个点是否在网里（减速） */
void game_web_submit(void);                          /* 画在地板上、人物下面 */

//==================== 可交互石头（game_stone.c）====================
void game_stones_reset(void);                        /* 换房时清空当前房间的石头 */
void game_stones_spawn(int count);                   /* 随机生成 count 块，保证门与门连通 */
int  game_stone_block(int px, int py);               /* 玩家左上角，按肚子判是否被石头挡住 */
int  game_stone_hit_at(int x, int y);                /* 弹幕中心是否撞到石头（撞到就消失） */
void game_stones_submit(void);

//==================== Boss（game_boss.c）====================
int  game_nearest_enemy(int *tx, int *ty);   /* 最近的敌人中心；没有返回 0 */
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
void game_item_try_pickup(u32 now);
void game_item_draw(u32 fb, u32 now, int buf);
void game_item_submit(void);            /* 登记到渲染队列：z = 底边 y */
