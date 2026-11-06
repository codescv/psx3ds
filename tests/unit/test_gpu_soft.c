/*
 * Unit tests for Slice C PSX Software Rasterizer (gpu_soft.c):
 * - Top-left fill rule & zero seam cracks / zero double-blending on ABR1 quads & shared-edge tris
 * - Max primitive span culling (|dx| > 1023 || |dy| > 511)
 * - Scissor clipping (E3..E4) & signed 11-bit drawing offset (E5)
 * - 4bpp CLUT, 8bpp CLUT, and 15bpp direct textures on polygons & sprites (1x1, 8x8, 16x16, var)
 * - Transparent 0x0000 skip vs 0x8000 STP black drawn, overbright modulation, per-texel STP ABR 0..3
 * - Texture Window (E2), Sprite X/Y flip (E1.12/13), Sony 4x4 dithering, CLUT$ cache (clut-cache.exe)
 * - Monochrome & Gouraud lines and E6 check_mask / set_mask
 */
#include "../../src/gpu/gpu_internal.h"
#include "psx3ds/arena.h"
#include "psx3ds/gpu.h"
#include "psx3ds/system.h"

#include <stdio.h>
#include <string.h>

static PsxSystem s_sys;

#define CHECK_TRUE(cond, msg)                                                       \
    do {                                                                            \
        if (!(cond)) {                                                              \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, (msg));       \
            return 1;                                                               \
        }                                                                           \
    } while (0)

#define CHECK_EQ_U32(actual, expected, msg)                                         \
    do {                                                                            \
        u32 _a = (u32)(actual);                                                     \
        u32 _e = (u32)(expected);                                                   \
        if (_a != _e) {                                                             \
            fprintf(stderr, "FAIL [%s:%d]: %s (got 0x%08X, expected 0x%08X)\n",     \
                    __FILE__, __LINE__, (msg), (unsigned)_a, (unsigned)_e);         \
            return 1;                                                               \
        }                                                                           \
    } while (0)

static void setup_clip(PsxSystem *sys, u32 x0, u32 y0, u32 x1, u32 y1) {
    gpu_gp0_write(sys, 0xE3000000u | (x0 & 0x3FFu) | ((y0 & 0x1FFu) << 10));
    gpu_gp0_write(sys, 0xE4000000u | (x1 & 0x3FFu) | ((y1 & 0x1FFu) << 10));
}

static int test_quad_seam_and_top_left_rule(PsxSystem *sys) {
    gpu_reset(sys);
    u16 *vram = sys->gpu.vram;
    setup_clip(sys, 0u, 0u, 255u, 255u);

    /* Fill background (0..63, 0..63) with (8, 8, 8) = 0x2108 via GP0(02h) */
    gpu_gp0_write(sys, 0x02404040u); /* R=0x40>>3=8, G=8, B=8 -> 0x2108 */
    gpu_gp0_write(sys, 0u | (0u << 16));
    gpu_gp0_write(sys, 64u | (64u << 16));
    CHECK_EQ_U32(vram[15 * PSX_VRAM_W + 15], 0x2108u, "Background seeded to 0x2108");

    /* Set E1 ABR = 1 (1.0B + 1.0F additive) */
    gpu_gp0_write(sys, 0xE1000020u); /* bits 5..6 = 1 (ABR1) */

    /* Draw semi-transparent untextured quad GP0(2Ah) covering [10..30) x [10..30)
       with foreground BGR = 0x404040 -> (8,8,8).
       If ANY pixel on the diagonal seam (v1-v2) is drawn twice, it becomes (24,24,24)=0x6318;
       if skipped (crack), it stays (8,8,8)=0x2108. Every pixel in [10..29]x[10..29] MUST be (16,16,16)=0x4210! */
    gpu_gp0_write(sys, 0x2A404040u);
    gpu_gp0_write(sys, 10u | (10u << 16)); /* v0 top-left */
    gpu_gp0_write(sys, 30u | (10u << 16)); /* v1 top-right */
    gpu_gp0_write(sys, 10u | (30u << 16)); /* v2 bottom-left */
    gpu_gp0_write(sys, 30u | (30u << 16)); /* v3 bottom-right */

    for (u32 y = 8u; y < 32u; y++) {
        for (u32 x = 8u; x < 32u; x++) {
            u16 got = vram[y * PSX_VRAM_W + x];
            bool inside = (x >= 10u && x < 30u && y >= 10u && y < 30u);
            u16 exp = inside ? 0x4210u : 0x2108u;
            if (got != exp) {
                fprintf(stderr, "FAIL seam/top-left at (%u,%u): got 0x%04X expected 0x%04X\n",
                        (unsigned)x, (unsigned)y, (unsigned)got, (unsigned)exp);
                return 1;
            }
        }
    }

    /* Also test a non-axis-aligned / skewed shared-edge triangle pair (CW + CCW windings) */
    gpu_gp0_write(sys, 0x02404040u);
    gpu_gp0_write(sys, 0u | (0u << 16));
    gpu_gp0_write(sys, 64u | (64u << 16));

    /* Quad with slanted interior diagonal: (12,12)-(28,15)-(14,27)-(29,29) in ABR1 */
    gpu_gp0_write(sys, 0x2A404040u);
    gpu_gp0_write(sys, 12u | (12u << 16));
    gpu_gp0_write(sys, 28u | (15u << 16));
    gpu_gp0_write(sys, 14u | (27u << 16));
    gpu_gp0_write(sys, 29u | (29u << 16));

    for (u32 y = 10u; y < 32u; y++) {
        for (u32 x = 10u; x < 32u; x++) {
            u16 got = vram[y * PSX_VRAM_W + x];
            CHECK_TRUE(got == 0x2108u || got == 0x4210u,
                       "Slanted ABR1 quad has ZERO double-blended seam pixels (no 0x6318)");
        }
    }
    /* Check midpoint right on the slanted diagonal (21, 21) WAS drawn once */
    CHECK_EQ_U32(vram[21 * PSX_VRAM_W + 21], 0x4210u, "Slanted quad diagonal midpoint drawn without crack");

    return 0;
}

static int test_max_span_culling_and_clip_offset(PsxSystem *sys) {
    gpu_reset(sys);
    u16 *vram = sys->gpu.vram;
    setup_clip(sys, 0u, 0u, 1023u, 511u);

    /* |dx| == 1023 -> MUST draw; |dx| == 1024 (via vertex -1..+1023 using E5) -> MUST cull! */
    gpu_gp0_write(sys, 0x200000FFu); /* Red */
    gpu_gp0_write(sys, 0u    | (0u << 16));
    gpu_gp0_write(sys, 1023u | (0u << 16));
    gpu_gp0_write(sys, 0u    | (4u << 16));
    CHECK_EQ_U32(vram[0 * PSX_VRAM_W + 1], 0x001Fu, "|dx| == 1023 is NOT culled");

    gpu_vram_clear(vram);
    gpu_gp0_write(sys, 0x200000FFu);
    gpu_gp0_write(sys, ((u32)(-1) & 0xFFFFu) | (0u << 16)); /* x = -1 */
    gpu_gp0_write(sys, 1023u                 | (0u << 16)); /* x = 1023 -> dx = 1024 > 1023! */
    gpu_gp0_write(sys, 0u                    | (4u << 16));
    CHECK_EQ_U32(vram[0 * PSX_VRAM_W + 1], 0x0000u, "|dx| == 1024 IS culled per PSX hardware");

    /* |dy| == 511 -> MUST draw; |dy| == 512 -> MUST cull! */
    gpu_gp0_write(sys, 0x2000FF00u); /* Green */
    gpu_gp0_write(sys, 0u | (0u   << 16));
    gpu_gp0_write(sys, 4u | (0u   << 16));
    gpu_gp0_write(sys, 0u | (511u << 16));
    CHECK_EQ_U32(vram[1 * PSX_VRAM_W + 0], (31u << 5), "|dy| == 511 is NOT culled");

    gpu_vram_clear(vram);
    gpu_gp0_write(sys, 0x2000FF00u);
    gpu_gp0_write(sys, 0u | (((u32)(-1) & 0xFFFFu) << 16)); /* y = -1 */
    gpu_gp0_write(sys, 4u | (0u << 16));
    gpu_gp0_write(sys, 0u | (511u << 16));                  /* y = 511 -> dy = 512 > 511! */
    CHECK_EQ_U32(vram[1 * PSX_VRAM_W + 0], 0x0000u, "|dy| == 512 IS culled per PSX hardware");

    /* Scissor clip [20..25] x [30..35] + E5 offset (+15, +25) */
    gpu_vram_clear(vram);
    setup_clip(sys, 20u, 30u, 25u, 35u);
    gpu_gp0_write(sys, 0xE5000000u | 15u | (25u << 11));

    /* Sprite GP0(60h) at local (0, 0) size 20x20 -> offset to [15..35) x [25..45), clipped to [20..25]x[30..35] */
    gpu_gp0_write(sys, 0x60FF0000u); /* Blue (31<<10) = 0x7C00 */
    gpu_gp0_write(sys, 0u  | (0u << 16));
    gpu_gp0_write(sys, 20u | (20u << 16));

    CHECK_EQ_U32(vram[30 * PSX_VRAM_W + 19], 0x0000u, "Left of clip_x0 clipped");
    CHECK_EQ_U32(vram[29 * PSX_VRAM_W + 20], 0x0000u, "Above clip_y0 clipped");
    CHECK_EQ_U32(vram[30 * PSX_VRAM_W + 20], 0x7C00u, "Inclusive clip_x0,clip_y0 drawn");
    CHECK_EQ_U32(vram[35 * PSX_VRAM_W + 25], 0x7C00u, "Inclusive clip_x1,clip_y1 drawn");
    CHECK_EQ_U32(vram[35 * PSX_VRAM_W + 26], 0x0000u, "Right of clip_x1 clipped");
    CHECK_EQ_U32(vram[36 * PSX_VRAM_W + 25], 0x0000u, "Below clip_y1 clipped");

    return 0;
}

static int test_textures_clut_modulation_stp_texwin_flip(PsxSystem *sys) {
    gpu_reset(sys);
    u16 *vram = sys->gpu.vram;
    setup_clip(sys, 0u, 0u, 255u, 255u);

    /* 1. 4bpp CLUT texture at texpage (256, 0) [tx=4, ty=0], CLUT at (0, 240):
          nibble 0 -> 0x0000 (transparent black: MUST skip!)
          nibble 1 -> 0x8000 (STP black: MUST draw!)
          nibble 2 -> 0x0010 (R=16 opaque: modulated by R=255 -> (16*255)>>7 = 31!)
          nibble 3 -> 0x8010 (R=16 STP=1: blends in PF_SEMI, whereas nibble 2 stays opaque in PF_SEMI!) */
    u32 clut_y = 240u;
    vram[clut_y * PSX_VRAM_W + 0] = 0x0000u;
    vram[clut_y * PSX_VRAM_W + 1] = 0x8000u;
    vram[clut_y * PSX_VRAM_W + 2] = 0x0010u;
    vram[clut_y * PSX_VRAM_W + 3] = 0x8010u;
    u16 clut_attr = (u16)((0u >> 4) | (clut_y << 6));

    /* Pack 4 4bpp texels (0, 1, 2, 3) at u=0..3, v=0 in texpage (256, 0) */
    vram[0 * PSX_VRAM_W + 256] = 0x3210u;

    /* Seed destination row y=10, x=10..13 with background R=8 (0x0008) */
    for (u32 x = 10u; x < 14u; x++) {
        vram[10 * PSX_VRAM_W + x] = 0x0008u;
    }

    /* Set E1: tx=4 (256), ty=0, abr=1 (B+F), tex_depth=0 (4bpp) */
    gpu_gp0_write(sys, 0xE1000000u | 4u | (1u << 5) | (0u << 7));

    /* Draw semi-transparent modulated variable sprite GP0(66h) at (10,10) size 4x1
       with overbright Red=255, Green=128, Blue=128 (0x8080FF) */
    gpu_gp0_write(sys, 0x668080FFu);
    gpu_gp0_write(sys, 10u | (10u << 16));
    gpu_gp0_write(sys, 0u  | (0u << 8) | ((u32)clut_attr << 16));
    gpu_gp0_write(sys, 4u  | (1u << 16));

    /* Pixel 0 (nibble 0 -> 0x0000): skipped -> stays background 0x0008 */
    CHECK_EQ_U32(vram[10 * PSX_VRAM_W + 10], 0x0008u, "Texel 0x0000 is transparent (skipped)");
    /* Pixel 1 (nibble 1 -> 0x8000 STP black): modulated (0,0,0), STP=1 -> ABR1(bg=8, fg=0) = 8 | 0x8000! */
    CHECK_EQ_U32(vram[10 * PSX_VRAM_W + 11], 0x8008u, "Texel 0x8000 STP black IS drawn & preserves STP bit");
    /* Pixel 2 (nibble 2 -> 0x0010, STP=0): modulated R=(16*255)>>7=31, STP=0 -> NOT blended despite PF_SEMI! */
    CHECK_EQ_U32(vram[10 * PSX_VRAM_W + 12], 0x001Fu, "Overbright modulation clamps to 31 & STP=0 skips blend");
    /* Pixel 3 (nibble 3 -> 0x8010, STP=1): modulated R=31, blended ABR1 with bg=8 -> 31 | 0x8000 = 0x801F! */
    /* Wait: let's also test non-saturated ABR2 (B - F) on STP=1 vs STP=0! */
    vram[11 * PSX_VRAM_W + 12] = 0x0018u; /* bg R=24 */
    vram[11 * PSX_VRAM_W + 13] = 0x0018u; /* bg R=24 */
    gpu_gp0_write(sys, 0xE1000000u | 4u | (2u << 5) | (0u << 7)); /* ABR=2 (B - F) */
    gpu_gp0_write(sys, 0x66808080u); /* neutral 1.0x modulation (128 -> R=16) */
    gpu_gp0_write(sys, 10u | (11u << 16));
    gpu_gp0_write(sys, 0u  | (0u << 8) | ((u32)clut_attr << 16));
    gpu_gp0_write(sys, 4u  | (1u << 16));
    CHECK_EQ_U32(vram[11 * PSX_VRAM_W + 12], 0x0010u, "PF_SEMI + STP=0 draws unblended R=16");
    CHECK_EQ_U32(vram[11 * PSX_VRAM_W + 13], 0x8008u, "PF_SEMI + STP=1 blends ABR2 (24 - 16 = 8) | 0x8000");

    /* 2. CLUT Cache coherency test (clut-cache.exe):
          Overwriting CLUT in VRAM via a solid polygon MUST NOT invalidate the cached CLUT,
          whereas GP0(01h) ClearCache MUST invalidate it! */
    gpu_gp0_write(sys, 0x2000FF00u); /* Draw solid Green triangle right over CLUT at (0..8, 240..244)! */
    gpu_gp0_write(sys, 0u | (240u << 16));
    gpu_gp0_write(sys, 8u | (240u << 16));
    gpu_gp0_write(sys, 0u | (244u << 16));
    CHECK_EQ_U32(vram[clut_y * PSX_VRAM_W + 2], (31u << 5), "CLUT in VRAM overwritten by polygon");

    /* Draw raw 4bpp sprite GP0(65h) using the SAME clut_attr: should still read old cached 0x0010! */
    gpu_gp0_write(sys, 0x65000000u);
    gpu_gp0_write(sys, 20u | (10u << 16));
    gpu_gp0_write(sys, 2u  | (0u << 8) | ((u32)clut_attr << 16));
    gpu_gp0_write(sys, 1u  | (1u << 16));
    CHECK_EQ_U32(vram[10 * PSX_VRAM_W + 20], 0x0010u, "CLUT$ retains cached entry across polygon overwrite");

    /* Issue GP0(01h) ClearCache -> re-fetches new Green (31<<5) CLUT entry! */
    gpu_gp0_write(sys, 0x01000000u);
    gpu_gp0_write(sys, 0x65000000u);
    gpu_gp0_write(sys, 21u | (10u << 16));
    gpu_gp0_write(sys, 2u  | (0u << 8) | ((u32)clut_attr << 16));
    gpu_gp0_write(sys, 1u  | (1u << 16));
    CHECK_EQ_U32(vram[10 * PSX_VRAM_W + 21], (31u << 5), "GP0(01h) invalidates CLUT$ and reloads green");

    /* 3. 8bpp CLUT + Texture Window (E2) repeating 8x8 tile + Sprite X/Y flip! */
    /* Place an 8bpp 8x8 pattern at texpage (320, 0) [tx=5, ty=0] at UV offset (16, 8) -> u=16..23, v=8..15 */
    u32 clut8_y = 242u;
    for (u32 i = 0; i < 256u; i++) {
        vram[clut8_y * PSX_VRAM_W + i] = (u16)(0x100u + i);
    }
    gpu_gp0_write(sys, 0x01000000u); /* Flush CLUT$ after direct test poke */
    u16 clut8_attr = (u16)((0u >> 4) | (clut8_y << 6));

    for (u32 r = 0; r < 8u; r++) {
        for (u32 c = 0; c < 8u; c += 2u) {
            u8 idx0 = (u8)(1u + r * 8u + c);
            u8 idx1 = (u8)(1u + r * 8u + c + 1u);
            vram[(8u + r) * PSX_VRAM_W + 320u + ((16u + c) >> 1)] =
                (u16)idx0 | ((u16)idx1 << 8);
        }
    }

    /* Set E2 Texture Window: 8x8 repeat -> mask_x = 0x1F (invert 0..7 -> mask=31 -> ~(31*8)=~248=7!),
       off_x = 2 (2*8 = 16), mask_y = 0x1F (7), off_y = 1 (1*8 = 8) */
    gpu_gp0_write(sys, 0xE2000000u | 31u | (31u << 5) | (2u << 10) | (1u << 15));
    gpu_gp0_write(sys, 0xE1000000u | 5u | (1u << 7)); /* tx=5, tex_depth=1 (8bpp) */

    /* Draw 16x16 raw sprite GP0(7Dh) at (40, 40) starting at u=0, v=0 -> repeats the 8x8 tile 2x2! */
    gpu_gp0_write(sys, 0x7D000000u);
    gpu_gp0_write(sys, 40u | (40u << 16));
    gpu_gp0_write(sys, 0u  | (0u << 8) | ((u32)clut8_attr << 16));

    CHECK_EQ_U32(vram[40 * PSX_VRAM_W + 40], 0x101u, "8bpp TexWindow (0,0) maps to (16,8)");
    CHECK_EQ_U32(vram[40 * PSX_VRAM_W + 48], 0x101u, "8bpp TexWindow (8,0) wraps to (16,8)");
    CHECK_EQ_U32(vram[48 * PSX_VRAM_W + 48], 0x101u, "8bpp TexWindow (8,8) wraps to (16,8)");
    CHECK_EQ_U32(vram[47 * PSX_VRAM_W + 47], 0x140u, "8bpp TexWindow (7,7) maps to (23,15)");

    /* Reset TexWindow and test E1 bit 12 (X-flip) & bit 13 (Y-flip) on 8x8 raw sprite GP0(75h) */
    gpu_gp0_write(sys, 0xE2000000u);
    gpu_gp0_write(sys, 0xE1000000u | 5u | (1u << 7) | (1u << 12) | (1u << 13)); /* X+Y flip */
    /* UV=(23, 15) stepping -1 in X and -1 in Y covers u=23..16, v=15..8! */
    gpu_gp0_write(sys, 0x75000000u);
    gpu_gp0_write(sys, 70u | (40u << 16));
    gpu_gp0_write(sys, 23u | (15u << 8) | ((u32)clut8_attr << 16));
    CHECK_EQ_U32(vram[40 * PSX_VRAM_W + 70], 0x140u, "Flipped sprite top-left reads (23,15)");
    CHECK_EQ_U32(vram[47 * PSX_VRAM_W + 77], 0x101u, "Flipped sprite bottom-right reads (16,8)");

    /* 4. 15bpp direct texture quad GP0(2Dh) at texpage (512, 0) [tx=8, ty=0, depth=2] */
    gpu_gp0_write(sys, 0xE1000000u);
    for (u32 r = 0; r < 4u; r++) {
        for (u32 c = 0; c < 4u; c++) {
            vram[r * PSX_VRAM_W + 512u + c] = (u16)(0x2000u + r * 4u + c);
        }
    }
    u32 tpage15 = 8u | (2u << 7);
    gpu_gp0_write(sys, 0x2D000000u);
    gpu_gp0_write(sys, 90u | (40u << 16)); gpu_gp0_write(sys, 0u | (0u << 8));
    gpu_gp0_write(sys, 94u | (40u << 16)); gpu_gp0_write(sys, 4u | (0u << 8) | (tpage15 << 16));
    gpu_gp0_write(sys, 90u | (44u << 16)); gpu_gp0_write(sys, 0u | (4u << 8));
    gpu_gp0_write(sys, 94u | (44u << 16)); gpu_gp0_write(sys, 4u | (4u << 8));
    CHECK_EQ_U32(vram[40 * PSX_VRAM_W + 90], 0x2000u, "15bpp direct quad (0,0)");
    CHECK_EQ_U32(vram[43 * PSX_VRAM_W + 93], 0x200Fu, "15bpp direct quad (3,3)");

    /* 4b. Horizontal X-wrapping CLUT at clut_x = 1008 (or 1024-16; note PSX clut_x is in 16-pixel steps,
           and for 8bpp 256-entry CLUT starting at clut_x = 1008 [63*16], entries 16..255 wrap to X=0..239!) */
    u32 wrap_clut_y = 245u;
    vram[wrap_clut_y * PSX_VRAM_W + 1008u + 0u]  = 0x1111u; /* idx 0 (pre-wrap) */
    vram[wrap_clut_y * PSX_VRAM_W + 0u]          = 0x2222u; /* idx 16 (wrapped past 1023 -> 0!) */
    gpu_gp0_write(sys, 0x01000000u); /* ClearCache */
    u16 wrap_clut_attr = (u16)(63u | (wrap_clut_y << 6)); /* 63 * 16 = 1008 */
    vram[0 * PSX_VRAM_W + 320u] = 0x1000u; /* 8bpp: u=0 -> idx 0, u=1 -> idx 16 */
    gpu_gp0_write(sys, 0xE1000000u | 5u | (1u << 7));
    gpu_gp0_write(sys, 0x65000000u);
    gpu_gp0_write(sys, 96u | (40u << 16));
    gpu_gp0_write(sys, 0u  | (0u << 8) | ((u32)wrap_clut_attr << 16));
    gpu_gp0_write(sys, 2u  | (1u << 16));
    CHECK_EQ_U32(vram[40 * PSX_VRAM_W + 96], 0x1111u, "8bpp CLUT pre-wrap entry 0 at X=1008");
    CHECK_EQ_U32(vram[40 * PSX_VRAM_W + 97], 0x2222u, "8bpp CLUT X-wrapped entry 16 at X=0");

    return 0;
}

static int test_dithering_lines_and_mask(PsxSystem *sys) {
    gpu_reset(sys);
    u16 *vram = sys->gpu.vram;
    setup_clip(sys, 0u, 0u, 255u, 255u);

    /* Enable dithering (E1 bit 9 = 1) */
    gpu_gp0_write(sys, 0xE1000200u);

    /* Flat untextured triangle GP0(20h) MUST NOT dither even when E1.bit9 == 1! */
    /* R = 100 (100>>3 = 12; if +3 dithered at (1,2), (100+3)>>3 = 12, so pick R = 101:
       101>>3 = 12; at (1,2) dither=+3 -> (101+3)>>3 = 13!) */
    gpu_gp0_write(sys, 0x20000065u); /* Flat R=101 (0x65) */
    gpu_gp0_write(sys, 0u | (0u << 16));
    gpu_gp0_write(sys, 8u | (0u << 16));
    gpu_gp0_write(sys, 0u | (8u << 16));
    CHECK_EQ_U32(vram[2 * PSX_VRAM_W + 1], 12u, "Flat untextured poly ignores E1 dither bit");

    /* Gouraud triangle GP0(30h) with uniform R=101 MUST apply Sony 4x4 dither matrix:
       at (x=10 -> x&3=2, y=0 -> y&3=0): s_psx_dither[0][2] = -3 -> (101 - 3) >> 3 = 12
       at (x=14 -> x&3=2, y=1 -> y&3=1): s_psx_dither[1][2] = +3 -> (101 + 3) >> 3 = 13
       at (x=12 -> x&3=0, y=3 -> y&3=3): s_psx_dither[3][0] = +3 -> (101 + 3) >> 3 = 13 */
    gpu_gp0_write(sys, 0x30000065u);
    gpu_gp0_write(sys, 10u | (0u << 16));
    gpu_gp0_write(sys, 0x00000065u);
    gpu_gp0_write(sys, 20u | (0u << 16));
    gpu_gp0_write(sys, 0x00000065u);
    gpu_gp0_write(sys, 10u | (10u << 16));
    CHECK_EQ_U32(vram[0 * PSX_VRAM_W + 10], 12u, "Gouraud dither at (10,0) [d=-3] -> 12");
    CHECK_EQ_U32(vram[1 * PSX_VRAM_W + 14], 13u, "Gouraud dither at (14,1) [d=+3] -> 13");
    CHECK_EQ_U32(vram[3 * PSX_VRAM_W + 12], 13u, "Gouraud dither at (12,3) [d=+3] -> 13");

    /* Zero-length single-pixel line GP0(40h) + diagonal Gouraud line GP0(50h) + E6 mask */
    gpu_gp0_write(sys, 0xE1000000u); /* Dither off */
    gpu_gp0_write(sys, 0xE6000001u); /* set_mask = 1 */
    gpu_gp0_write(sys, 0x400000FFu); /* 1-pixel line at (50, 50) */
    gpu_gp0_write(sys, 50u | (50u << 16));
    gpu_gp0_write(sys, 50u | (50u << 16));
    CHECK_EQ_U32(vram[50 * PSX_VRAM_W + 50], 0x801Fu, "Zero-length line draws 1 pixel with set_mask=1");

    /* Now enable check_mask = 1 (E6 = 2) and draw Blue line across (48,50)..(52,50):
       (50,50) has bit15=1 so it MUST stay 0x801F while neighbors become 0x7C00! */
    gpu_gp0_write(sys, 0xE6000002u);
    gpu_gp0_write(sys, 0x40FF0000u);
    gpu_gp0_write(sys, 48u | (50u << 16));
    gpu_gp0_write(sys, 52u | (50u << 16));
    CHECK_EQ_U32(vram[50 * PSX_VRAM_W + 49], 0x7C00u, "Unmasked pixel drawn by line");
    CHECK_EQ_U32(vram[50 * PSX_VRAM_W + 50], 0x801Fu, "Masked pixel protected during line draw");
    CHECK_EQ_U32(vram[50 * PSX_VRAM_W + 51], 0x7C00u, "Unmasked pixel after masked pixel drawn by line");

    return 0;
}

int main(void) {
    memset(&s_sys, 0, sizeof(s_sys));
    psx_config_defaults(&s_sys.cfg, false);
    s_sys.cfg.gpu_backend = GPU_BACKEND_SOFT;

    CHECK_TRUE(arena_init(&s_sys.arena, 4u * 1024u * 1024u) == PSX_OK, "arena_init");
    gpu_init(&s_sys);

    if (test_quad_seam_and_top_left_rule(&s_sys) != 0) return 1;
    if (test_max_span_culling_and_clip_offset(&s_sys) != 0) return 1;
    if (test_textures_clut_modulation_stp_texwin_flip(&s_sys) != 0) return 1;
    if (test_dithering_lines_and_mask(&s_sys) != 0) return 1;

    gpu_shutdown(&s_sys);
    arena_destroy(&s_sys.arena);
    printf("test_gpu_soft PASS\n");
    return 0;
}
