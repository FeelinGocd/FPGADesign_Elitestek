//==============================================================================
//  game_run_audit.h —— 普通 Boss 战的只读运行审计数据
//------------------------------------------------------------------------------
//  干什么      : 定义供调试器或上位机读取的固定 RAM 数据结构。
//  上一级是谁  : game_run_audit.c
//  接口契约    :
//    · 目标机上所有字段均为小端 32 位字；
//    · 64 位工作耗时由 lo/hi 两个字段拼成，读取需检查 sequence 未变化；
//    · 本模块只观察，不修改游戏状态、相机、伤害或弹幕归属；
//    · IRQ 间隔来自提交后的采样，不等同于物理漏帧证明。
//==============================================================================
#ifndef GAME_RUN_AUDIT_H
#define GAME_RUN_AUDIT_H
#include <stdint.h>

#define GAME_RUN_AUDIT_MAGIC 0x4752554eu /* GRUN */
#define GAME_RUN_AUDIT_VERSION 1u
#define GAME_RUN_AUDIT_BUCKETS 5u
#define GAME_RUN_AUDIT_WARMUP 30u

enum {
    GAME_RUN_CORE, GAME_RUN_INTERMISSION1, GAME_RUN_RAIN,
    GAME_RUN_INTERMISSION2, GAME_RUN_COMBINED
};

typedef struct {
    uint32_t frames;
    uint32_t work_sum_us_lo, work_sum_us_hi;
    uint32_t work_max_us;
    uint32_t submit_irq_gap_count; /* IRQ 增量大于 1 的次数 */
    uint32_t submit_irq_delta_max;
    uint32_t camera_moved_frames;
    uint32_t enemy_peak;
    uint32_t drawn_enemy_peak; /* 单帧实际绘制敌弹数的峰值 */
    uint32_t queue_drops;
    uint32_t cmd_max;
} game_run_audit_bucket_t;

typedef struct {
    uint32_t magic, version, hz, struct_size, bucket_count, sequence;
    game_run_audit_bucket_t buckets[GAME_RUN_AUDIT_BUCKETS];
} game_run_audit_t;

extern volatile game_run_audit_t g_game_run_audit;
#endif
