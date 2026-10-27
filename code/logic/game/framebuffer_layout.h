#pragma once

/* Shared DDR address contract for the game and GPU/CPU comparison demo.
 * The 720p viewport still uses the original 1080p physical row strides. */
#define DDR_FB_C       0x08000000u
#define DDR_FB_D       0x08800000u
#define DDR_FB_A_LEFT  0x09000000u
#define DDR_FB_A_RIGHT 0x09400000u
#define DDR_FB_B_LEFT  0x09800000u
#define DDR_FB_B_RIGHT 0x09C00000u
#define DDR_FB_BG      0x0A000000u
#define DDR_FB_THIRD   0x0A800000u

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
    DDR_FB_THIRD + DDR_FULL_SLOT_BYTES > 0x10000000u
#error "DDR framebuffer slots overlap or exceed the GPU 28-bit address window"
#endif
