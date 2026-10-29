//==============================================================================
//  framebuffer_layout.h —— 游戏与 GPU/CPU 对比 Demo 共用的 DDR 布局契约
//------------------------------------------------------------------------------
//  干什么      : 集中定义帧缓存、分屏半幅和背景缓存的 DDR 地址。
//  上一级是谁  : game/game.h、src/main.c、src/compare_demo.c
//  接口契约    :
//    · 720p 可见视口仍沿用 1080p 的物理行跨距（1920×3 = 5760 字节）；
//    · 所有实地址必须落在 GPU 可访问的低 256 MiB（28 位地址）范围内；
//    · 各槽位不得重叠，下面的预处理器断言在编译期检查布局。
//  修改注意    :
//    · 地址同时影响 C 绘图命令、显示基址和 RTL 验证参数，不能只改一处；
//    · 新增缓冲须先核对槽位大小和地址边界。
//==============================================================================
#pragma once

#define DDR_FB_C       0x08000000u
#define DDR_FB_D       0x08800000u
#define DDR_FB_A_LEFT  0x09000000u
#define DDR_FB_A_RIGHT 0x09400000u
#define DDR_FB_B_LEFT  0x09800000u
#define DDR_FB_B_RIGHT 0x09C00000u
#define DDR_FB_BG      0x0A000000u
#define DDR_FB_THIRD   0x0A800000u
/* 七张独立的干净背景图：原始 BG 加六个缓存槽。
 * 这些槽位不与对比 Demo 的半幅缓冲重叠。 */
#define DDR_BG_CACHE_FIRST 0x0B000000u
#define DDR_BG_CACHE_COUNT 7u

#define DDR_FULL_SLOT_BYTES 0x00800000u
#define DDR_HALF_SLOT_BYTES 0x00400000u
#define DDR_FULL_FRAME_BYTES (5760u * 1440u)
#define DDR_HALF_FRAME_BYTES (2880u * 1440u)

#if DDR_FULL_FRAME_BYTES > DDR_FULL_SLOT_BYTES || \
    DDR_HALF_FRAME_BYTES > DDR_HALF_SLOT_BYTES || \
    DDR_FB_C + DDR_FULL_SLOT_BYTES > DDR_FB_D || \
    DDR_FB_D + DDR_FULL_SLOT_BYTES > DDR_FB_A_LEFT || \
    DDR_FB_A_LEFT + DDR_HALF_SLOT_BYTES > DDR_FB_A_RIGHT || \
    DDR_FB_A_RIGHT + DDR_HALF_SLOT_BYTES > DDR_FB_B_LEFT || \
    DDR_FB_B_LEFT + DDR_HALF_SLOT_BYTES > DDR_FB_B_RIGHT || \
    DDR_FB_B_RIGHT + DDR_HALF_SLOT_BYTES > DDR_FB_BG || \
    DDR_FB_BG + DDR_FULL_SLOT_BYTES > DDR_FB_THIRD || \
    DDR_FB_THIRD + DDR_FULL_SLOT_BYTES > DDR_BG_CACHE_FIRST || \
    DDR_BG_CACHE_FIRST + (DDR_BG_CACHE_COUNT - 1u) * DDR_FULL_SLOT_BYTES > 0x10000000u
#error "DDR framebuffer slots overlap or exceed the GPU 28-bit address window"
#endif
