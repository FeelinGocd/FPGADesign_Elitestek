PROJ_NAME=GamingPlayUnit
STANDALONE = ..

# ---------------------------------------------------------------------------
# Efinity C 宸ョ▼ makefile 妯℃澘
#
#   鏈枃浠跺湪 code/app/<宸ョ▼鍚?/ 涓嬶紝鏂板缓宸ョ▼鏃舵嫹鍒板伐绋嬬洰褰曪紝
#   瑕嗙洊鍚戝鐢熸垚鐨勯偅浠?makefile銆?#
#   宸ョ▼鏄嚜鍖呭惈鐨勶細
#       src/          搴旂敤浠ｇ爜锛堜粠 code/app/<宸ョ▼鍚?/ 鎷凤級
#       peripheral/   澶栬椹卞姩锛堜粠 code/peripheral/ 鎷凤紝淇濇寔鍚屾瀯锛?#   璇﹁ code/README_濡備綍瀵煎叆C鏂囦欢鍒板伐绋?md
# ---------------------------------------------------------------------------


SRCS =  $(wildcard src/*.c) \
	$(wildcard src/*.cpp) \
	$(wildcard src/*.S) \
	$(wildcard peripheral/*.c) \
	$(wildcard peripheral/*/*.c) \
	$(wildcard peripheral/*/*/*.c) \
	$(wildcard game/*.c) \
	${STANDALONE}/common/start.S \
	${STANDALONE}/common/trap.S


# 澶存枃浠舵悳绱㈣矾寰勶紙鍙奖鍝嶇紪璇戯紝涓嶅奖鍝嶉摼鎺ワ級
#   姣忓姞涓€涓璁剧洰褰曪紝灏卞姞涓€琛?#   娉細SRCS 鐢ㄧ殑鏄?wildcard锛宲eripheral/*/*/*.c 浼氳嚜鍔ㄦ敹杩涙潵锛屼笉鐢ㄦ墜鍔ㄥ姞锛?#       浣?INC 鏄樉寮忓垪琛紝鏂扮洰褰曞繀椤绘墜鍔ㄨˉ涓€琛?INC += -Iperipheral/APB0/key
INC += -Iperipheral/APB0/key
INC += -Iperipheral/APB0/checker
INC += -Iperipheral/APB0/lineBuffer
INC += -Iperipheral/APB0/gpu2d
INC += -Iperipheral/CInstruction
# game/ : game demo split by function (game.h / world / hud / player / bullet / boss / item / main)
INC += -Isrc
INC += -Igame


include ${STANDALONE}/common/bsp.mk
include ${STANDALONE}/common/riscv64-unknown-elf.mk

# 本工程按发布模式编译：BSP 的 soc.mk 把 DEBUG 钉成 yes（-Og），
# 游戏 + 持久化对象后 RAM 余量小；这里再追加 -Os（GCC 以最后一个 -O 为准）。
# ★ 必须在 include standalone.mk 之前加：那个文件里的 define 会立刻展开
#   $(CFLAGS) 生成每条编译规则，晚加就进不去。
# 需要 -O0/-Og 单步调试时：make RELEASE=no ...
RELEASE ?= yes
ifeq ($(RELEASE),yes)
CFLAGS += -Os
# Share RISC-V register save/restore sequences; disable for size/performance A/B.
SAVE_RESTORE ?= yes
ifeq ($(SAVE_RESTORE),yes)
CFLAGS += -msave-restore
endif
endif

# ????????????? IDE ????????????? PROFILE ???
# ????????make GAME_PROFILE=0 ...
GAME_PROFILE ?= 0
CFLAGS += -DGAME_PROFILE=$(GAME_PROFILE)
GAME_TEST_DAMAGE ?= 10
CFLAGS += -DGAME_TEST_DAMAGE=$(GAME_TEST_DAMAGE)
GAME_UART_TELEMETRY ?= $(GAME_PROFILE)
CFLAGS += -DGAME_UART_TELEMETRY=$(GAME_UART_TELEMETRY)
GAME_FIXED_UPDATE ?= 1
CFLAGS += -DGAME_FIXED_UPDATE=$(GAME_FIXED_UPDATE)
DEMO_RUN_SEL ?= 2
CFLAGS += -DDEMO_RUN_SEL=$(DEMO_RUN_SEL)
GAME_TRIPLE_BUFFER ?= 1
CFLAGS += -DGAME_TRIPLE_BUFFER=$(GAME_TRIPLE_BUFFER)
GAME_STATIC_CULL ?= 1
CFLAGS += -DGAME_STATIC_CULL=$(GAME_STATIC_CULL)
GAME_WORLD_CACHE ?= 0
CFLAGS += -DGAME_WORLD_CACHE=$(GAME_WORLD_CACHE)
GAME_BG_VIEW_CULL ?= 1
CFLAGS += -DGAME_BG_VIEW_CULL=$(GAME_BG_VIEW_CULL)
GAME_BG_EARLY_RESTORE ?= 1
CFLAGS += -DGAME_BG_EARLY_RESTORE=$(GAME_BG_EARLY_RESTORE)
GAME_TEST_START_LAYOUT ?= -1
CFLAGS += -DGAME_TEST_START_LAYOUT=$(GAME_TEST_START_LAYOUT)
GAME_BOSS_STAGE_TEST ?= 0
CFLAGS += -DGAME_BOSS_STAGE_TEST=$(GAME_BOSS_STAGE_TEST)
GAME_BOSS_DEBUG ?= 0
CFLAGS += -DGAME_BOSS_DEBUG=$(GAME_BOSS_DEBUG)
GAME_BOSS_SHORTCUT ?= 1
CFLAGS += -DGAME_BOSS_SHORTCUT=$(GAME_BOSS_SHORTCUT)
GAME_BOSS_1X1_TEST ?= 0
CFLAGS += -DGAME_BOSS_1X1_TEST=$(GAME_BOSS_1X1_TEST)
GAME_BOSS_PACING_TEST ?= 0
CFLAGS += -DGAME_BOSS_PACING_TEST=$(GAME_BOSS_PACING_TEST)
# 1: Boss camera load; 2: ordinary-room render sweep and quiet timing
GAME_BOSS_AUTO_DIAG ?= 0
CFLAGS += -DGAME_BOSS_AUTO_DIAG=$(GAME_BOSS_AUTO_DIAG)
GAME_BOSS_MOTION_TRACE ?= 0
CFLAGS += -DGAME_BOSS_MOTION_TRACE=$(GAME_BOSS_MOTION_TRACE)
# 0: off; 1: dense window + cycle; 2: cycle only (no mid-cycle UART stall)
GAME_BOSS_SKILL_DIAG ?= 0
CFLAGS += -DGAME_BOSS_SKILL_DIAG=$(GAME_BOSS_SKILL_DIAG)
GAME_BOSS_BULLET_SWEEP ?= 0
CFLAGS += -DGAME_BOSS_BULLET_SWEEP=$(GAME_BOSS_BULLET_SWEEP)
GAME_BOSS_SWEEP_START ?= 16
CFLAGS += -DGAME_BOSS_SWEEP_START=$(GAME_BOSS_SWEEP_START)
GAME_BOSS_SWEEP_HOLD_LEVEL ?= $(GAME_ENEMY_BULLET_CAP)
CFLAGS += -DGAME_BOSS_SWEEP_HOLD_LEVEL=$(GAME_BOSS_SWEEP_HOLD_LEVEL)
GAME_BOSS_SWEEP_OPAQUE ?= 0
CFLAGS += -DGAME_BOSS_SWEEP_OPAQUE=$(GAME_BOSS_SWEEP_OPAQUE)
GAME_ENEMY_BULLET_CAP ?= 500
CFLAGS += -DGAME_ENEMY_BULLET_CAP=$(GAME_ENEMY_BULLET_CAP)
GAME_BOSS_DENSE_RINGS ?= 10
CFLAGS += -DGAME_BOSS_DENSE_RINGS=$(GAME_BOSS_DENSE_RINGS)
PERF_TEST_MODE ?= 0
CFLAGS += -DPERF_TEST_MODE=$(PERF_TEST_MODE)
PERF_TEST_BULLETS ?= N_BULLETS
CFLAGS += -DPERF_TEST_BULLETS=$(PERF_TEST_BULLETS)

# Enemy bullet tint A/B switch. 1 keeps the red tint; 0 uses alpha-only.
GAME_BULLET_TINT ?= 1
CFLAGS += -DGAME_BULLET_TINT=$(GAME_BULLET_TINT)
GAME_BULLET_FAST_CKEY ?= 0
CFLAGS += -DGAME_BULLET_FAST_CKEY=$(GAME_BULLET_FAST_CKEY)
GAME_BULLET_ALPHA_CROP ?= 1
CFLAGS += -DGAME_BULLET_ALPHA_CROP=$(GAME_BULLET_ALPHA_CROP)
GAME_BULLET_DIRTY_BOUNDS ?= 1
CFLAGS += -DGAME_BULLET_DIRTY_BOUNDS=$(GAME_BULLET_DIRTY_BOUNDS)
# Prefer clean DDR spans; avoids terrain queries in the per-frame hot path.
GAME_BG_DIRECT_MAX_CELLS ?= 0
CFLAGS += -DGAME_BG_DIRECT_MAX_CELLS=$(GAME_BG_DIRECT_MAX_CELLS)
DIRTY_LIST_MAX ?= 128
CFLAGS += -DDIRTY_LIST_MAX=$(DIRTY_LIST_MAX)
GAME_BG_MERGE_MAX_CLEAN_CELLS ?= 2
CFLAGS += -DGAME_BG_MERGE_MAX_CLEAN_CELLS=$(GAME_BG_MERGE_MAX_CLEAN_CELLS)
GAME_BG_EXACT_RECT ?= 0
CFLAGS += -DGAME_BG_EXACT_RECT=$(GAME_BG_EXACT_RECT)
GAME_BULLET_TILE_HISTORY ?= $(if $(filter 1,$(GAME_BG_EXACT_RECT)),0,1)
CFLAGS += -DGAME_BULLET_TILE_HISTORY=$(GAME_BULLET_TILE_HISTORY)

# LTO：跨文件内联/去重，进一步压代码；默认开（已验证链接通过）。
# 需要排查链接/单步问题时：make LTO=no ...
LTO ?= yes
ifeq ($(LTO),yes)
# ★ 必须单分片：CI_gpu2d.c 的 ".word" 内联汇编依赖 riscv.h 顶部的 `.set regnum_*`，
#   这些 .set 只在本 TU 的汇编输出里有效。LTO 一旦分成 2 个 LTRANS 任务，
#   汇编会被搬到别的分片里，汇编阶段就报 invalid operands (*UND* and *ABS*)。
CFLAGS  += -flto -flto-partition=one
LDFLAGS += -flto -flto-partition=one
endif

# Track included headers: changing generated ASSET_* values must rebuild every
# translation unit that compiled them, not just the descriptor table itself.
CFLAGS += -MMD -MP

include ${STANDALONE}/common/standalone.mk

-include $(OBJS:.o=.d)

# Existing IDE objects predate .d generation.  The generated asset header is
# also an explicit dependency so the first asset repack cannot reuse them.
$(OBJS): peripheral/APB0/gpu2d/assets_desc.h

LDSCRIPT =  ${BSP_PATH}/linker/default.ld
