/* Fixed RAM schema: all fields are little-endian 32-bit words on the target.
 * work_sum_us = work_sum_us_lo | ((uint64_t)work_sum_us_hi << 32).
 * A stable reader observes the same even sequence before and after a copy.
 * IRQ gaps describe post-submit IRQ samples, not proven physical repeats. */
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
    uint32_t submit_irq_gap_count; /* intervals with IRQ delta > 1 */
    uint32_t submit_irq_delta_max;
    uint32_t camera_moved_frames;
    uint32_t enemy_peak;
    uint32_t drawn_enemy_peak; /* per-frame real tint-enemy draw-counter delta */
    uint32_t queue_drops;
    uint32_t cmd_max;
} game_run_audit_bucket_t;

typedef struct {
    uint32_t magic, version, hz, struct_size, bucket_count, sequence;
    game_run_audit_bucket_t buckets[GAME_RUN_AUDIT_BUCKETS];
} game_run_audit_t;

extern volatile game_run_audit_t g_game_run_audit;
#endif
