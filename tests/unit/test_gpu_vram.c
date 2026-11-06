/*
 * Unit tests for Slice C 1MB Shadow VRAM operations:
 * - GP0(02h) FillRect 16-pixel X alignment & 1024x512 wraparound
 * - GP0(A0h) CPU->VRAM & GP0(C0h) VRAM->CPU transfers (even/odd counts, mask bits, GPUSTAT.27)
 * - GP0(80h) VRAM->VRAM overlap-safe copies in all 4 directions + wraparound + mask bits
 * - Active display 15bpp RGB555 & 24bpp RGB888 extraction & presentation
 */
#include "../../src/gpu/gpu_internal.h"
#include "psx3ds/arena.h"
#include "psx3ds/gpu.h"
#include "psx3ds/system.h"

#include <stdio.h>
#include <string.h>

static PsxSystem s_sys;
static u16       s_out555[64 * 64];
static u8        s_out888[64 * 64 * 3];

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

static int test_fill_rect_alignment_and_wrap(PsxSystem *sys) {
    gpu_reset(sys);
    u16 *vram = sys->gpu.vram;

    /* Set restrictive clip, offset, and check_mask=1/set_mask=1 — FillRect MUST ignore all of them! */
    gpu_gp0_write(sys, 0xE3000000u | 500u | (200u << 10));
    gpu_gp0_write(sys, 0xE4000000u | 510u | (210u << 10));
    gpu_gp0_write(sys, 0xE5000000u | 100u | (100u << 11));
    gpu_gp0_write(sys, 0xE6000003u);

    /* Pre-seed VRAM with 0xFFFF (bit 15 = 1) */
    for (u32 i = 0; i < PSX_VRAM_W * 8u; i++) {
        vram[i] = 0xFFFFu;
    }

    /* GP0(02h): X=23 (-> rounds down to 16), Y=2, W=17 (-> rounds up to 32: X=16..47), H=3
       Color BGR24 = 0xF8FCFF -> R=0xFF>>3=31, G=0xFC>>3=31, B=0xF8>>3=31 -> 0x7FFF (bit 15 cleared!) */
    gpu_gp0_write(sys, 0x02F8FCFFu);
    gpu_gp0_write(sys, 23u | (2u << 16));
    gpu_gp0_write(sys, 17u | (3u << 16));

    CHECK_EQ_U32(vram[2 * PSX_VRAM_W + 15], 0xFFFFu, "FillRect left of X&~0xF untouched");
    CHECK_EQ_U32(vram[2 * PSX_VRAM_W + 16], 0x7FFFu, "FillRect starts at X&~0xF == 16 with bit15=0");
    CHECK_EQ_U32(vram[4 * PSX_VRAM_W + 47], 0x7FFFu, "FillRect ends at 16 + 32 - 1 == 47");
    CHECK_EQ_U32(vram[4 * PSX_VRAM_W + 48], 0xFFFFu, "FillRect right of 47 untouched");
    CHECK_EQ_U32(vram[5 * PSX_VRAM_W + 20], 0xFFFFu, "FillRect below Y+H untouched");

    /* Wraparound across X=1024 and Y=512: X=1008, Y=510, W=32, H=4 -> wraps to X=0..15, Y=0..1 */
    gpu_gp0_write(sys, 0x020000FFu); /* Red 0x001F */
    gpu_gp0_write(sys, 1008u | (510u << 16));
    gpu_gp0_write(sys, 32u   | (4u << 16));

    CHECK_EQ_U32(vram[510 * PSX_VRAM_W + 1023], 0x001Fu, "FillRect pre-wrap corner (1023,510)");
    CHECK_EQ_U32(vram[510 * PSX_VRAM_W + 0],    0x001Fu, "FillRect X-wrapped (0,510)");
    CHECK_EQ_U32(vram[1   * PSX_VRAM_W + 15],   0x001Fu, "FillRect X+Y-wrapped (15,1)");
    CHECK_EQ_U32(vram[1   * PSX_VRAM_W + 16],   0xFFFFu, "FillRect X+Y-wrapped boundary (16,1)");

    return 0;
}

static int test_cpu_vram_transfers_and_mask(PsxSystem *sys) {
    gpu_reset(sys);
    u16 *vram = sys->gpu.vram;

    /* 1. Odd pixel count CPU->VRAM (3x3 = 9 pixels = 5 words; upper 16 bits of 5th word ignored!) */
    vram[10 * PSX_VRAM_W + 10 + 3] = 0xBEEFu; /* Neighbor pixel */
    vram[12 * PSX_VRAM_W + 12]     = 0xCAFEu;
    vram[13 * PSX_VRAM_W + 10]     = 0xDEADu;

    gpu_gp0_write(sys, 0xA0000000u);
    gpu_gp0_write(sys, 10u | (10u << 16));
    gpu_gp0_write(sys, 3u  | (3u << 16));

    /* Mid-IMG_LOAD: GPUSTAT.26 == 0, GPUSTAT.27 == 0, GPUSTAT.28 == 1 */
    u32 st = gpu_stat(sys);
    CHECK_TRUE(((st >> 26) & 1u) == 0u && ((st >> 27) & 1u) == 0u && ((st >> 28) & 1u) == 1u,
               "GPUSTAT during GP0_IMG_LOAD: bit26=0, bit27=0, bit28=1");

    const u32 load_words[5] = {
        0x10021001u,
        0x10041003u,
        0x10061005u,
        0x10081007u,
        0x99991009u /* 0x9999 padding must be ignored! */
    };
    gpu_dma_write_words(sys, load_words, 5u);

    CHECK_EQ_U32((gpu_stat(sys) >> 26) & 1u, 1u, "GP0_IMG_LOAD finishes cleanly after ceil(9/2)=5 words");
    CHECK_EQ_U32(vram[10 * PSX_VRAM_W + 10], 0x1001u, "A0h pixel 0");
    CHECK_EQ_U32(vram[12 * PSX_VRAM_W + 12], 0x1009u, "A0h pixel 8 (last odd pixel)");
    CHECK_EQ_U32(vram[13 * PSX_VRAM_W + 10], 0xDEADu, "A0h odd padding word did NOT overwrite next row");

    /* 2. VRAM->CPU readout GP0(C0h) of the same 3x3 odd region */
    gpu_gp1_write(sys, 0x04000003u); /* dma_dir = 3 (GPUREAD -> CPU) */
    CHECK_EQ_U32((gpu_stat(sys) >> 25) & 1u, 0u, "dma_req == 0 before C0h starts");

    gpu_gp0_write(sys, 0xC0000000u);
    gpu_gp0_write(sys, 10u | (10u << 16));
    gpu_gp0_write(sys, 3u  | (3u << 16));

    st = gpu_stat(sys);
    CHECK_TRUE(((st >> 26) & 1u) == 0u && ((st >> 27) & 1u) == 1u &&
               ((st >> 28) & 1u) == 0u && ((st >> 25) & 1u) == 1u,
               "GPUSTAT during GP0_IMG_STORE: bit26=0, bit27=1, bit28=0, bit25=1");

    u32 read_words[5] = { 0 };
    CHECK_EQ_U32(gpu_dma_read_words(sys, read_words, 5u), 5u, "gpu_dma_read_words count");
    CHECK_EQ_U32(read_words[0], 0x10021001u, "C0h word 0");
    CHECK_EQ_U32(read_words[3], 0x10081007u, "C0h word 3");
    CHECK_EQ_U32(read_words[4] & 0xFFFFu, 0x1009u, "C0h word 4 low halfword");

    st = gpu_stat(sys);
    CHECK_TRUE(((st >> 26) & 1u) == 1u && ((st >> 27) & 1u) == 0u && ((st >> 28) & 1u) == 1u,
               "GPUSTAT after GP0_IMG_STORE completes: bit26=1, bit27=0, bit28=1");

    /* 3. CPU->VRAM honoring check_mask & set_mask (E6 = 3) */
    vram[20 * PSX_VRAM_W + 20] = 0x8123u; /* Protected (bit 15 = 1) */
    vram[20 * PSX_VRAM_W + 21] = 0x0123u; /* Unprotected (bit 15 = 0) */
    gpu_gp0_write(sys, 0xE6000003u);      /* set_mask=1, check_mask=1 */

    gpu_gp0_write(sys, 0xA0000000u);
    gpu_gp0_write(sys, 20u | (20u << 16));
    gpu_gp0_write(sys, 2u  | (1u << 16));
    gpu_gp0_write(sys, 0x00220011u);

    CHECK_EQ_U32(vram[20 * PSX_VRAM_W + 20], 0x8123u, "A0h check_mask protects dst with bit15=1");
    CHECK_EQ_U32(vram[20 * PSX_VRAM_W + 21], 0x8022u, "A0h writes unprotected dst and sets bit15=1");

    return 0;
}

static int test_vram_to_vram_overlap_and_mask(PsxSystem *sys) {
    gpu_reset(sys);
    u16 *vram = sys->gpu.vram;

    /* Test all 4 self-overlapping shift directions against a reference snapshot! */
    static const struct { int dx; int dy; } shifts[4] = {
        { +3,  0 }, /* Right overlap (dx > sx, same rows) */
        { -3,  0 }, /* Left overlap  (dx < sx, same rows) */
        { +2, +2 }, /* Down-right overlap (dy > sy) */
        { -2, -2 }  /* Up-left overlap    (dy < sy) */
    };

    u16 ref_block[8 * 8];
    for (int dir = 0; dir < 4; dir++) {
        gpu_vram_clear(vram);
        u16 sx = 40u, sy = 40u, w = 8u, h = 8u;
        u16 dx = (u16)((int)sx + shifts[dir].dx);
        u16 dy = (u16)((int)sy + shifts[dir].dy);

        for (u16 r = 0; r < h; r++) {
            for (u16 c = 0; c < w; c++) {
                u16 val = (u16)(0x100u + r * 16u + c);
                vram[(sy + r) * PSX_VRAM_W + (sx + c)] = val;
                ref_block[r * w + c] = val;
            }
        }

        gpu_gp0_write(sys, 0x80000000u);
        gpu_gp0_write(sys, sx | ((u32)sy << 16));
        gpu_gp0_write(sys, dx | ((u32)dy << 16));
        gpu_gp0_write(sys, w  | ((u32)h  << 16));

        for (u16 r = 0; r < h; r++) {
            for (u16 c = 0; c < w; c++) {
                u16 got = vram[(dy + r) * PSX_VRAM_W + (dx + c)];
                CHECK_EQ_U32(got, ref_block[r * w + c], "Self-overlapping VRAM->VRAM copy exact match");
            }
        }
    }

    /* Non-sticky mask bit test (mask-bit.exe): when check_mask=0 && set_mask=0,
       copying 0x001F over 0x8000 overwrites bit 15 to 0! */
    gpu_gp0_write(sys, 0xE6000000u);
    vram[100 * PSX_VRAM_W + 100] = 0x001Fu;
    vram[100 * PSX_VRAM_W + 101] = 0x83E0u;
    gpu_gp0_write(sys, 0x80000000u);
    gpu_gp0_write(sys, 100u | (100u << 16));
    gpu_gp0_write(sys, 101u | (100u << 16));
    gpu_gp0_write(sys, 1u   | (1u   << 16));
    CHECK_EQ_U32(vram[100 * PSX_VRAM_W + 101], 0x001Fu, "Dst bit 15 is NOT sticky when E6==0");

    /* When check_mask=1 && set_mask=1, masked dst survives while unmasked dst gets src | 0x8000 */
    gpu_gp0_write(sys, 0xE6000003u);
    vram[102 * PSX_VRAM_W + 10] = 0x001Fu;
    vram[102 * PSX_VRAM_W + 11] = 0x03E0u;
    vram[103 * PSX_VRAM_W + 10] = 0x87FFu; /* Protected */
    vram[103 * PSX_VRAM_W + 11] = 0x0000u; /* Unprotected */
    gpu_gp0_write(sys, 0x80000000u);
    gpu_gp0_write(sys, 10u | (102u << 16));
    gpu_gp0_write(sys, 10u | (103u << 16));
    gpu_gp0_write(sys, 2u  | (1u   << 16));
    CHECK_EQ_U32(vram[103 * PSX_VRAM_W + 10], 0x87FFu, "VRAM->VRAM check_mask preserves bit15==1 dst");
    CHECK_EQ_U32(vram[103 * PSX_VRAM_W + 11], 0x83E0u, "VRAM->VRAM set_mask sets bit15 on written dst");

    return 0;
}

static int test_display_extraction_15_and_24bpp(PsxSystem *sys) {
    gpu_reset(sys);
    u16 *vram = sys->gpu.vram;

    /* 15bpp extraction with X/Y wraparound */
    vram[511 * PSX_VRAM_W + 1023] = 0x1111u;
    vram[511 * PSX_VRAM_W + 0]    = 0x2222u;
    vram[0   * PSX_VRAM_W + 1023] = 0x3333u;
    vram[0   * PSX_VRAM_W + 0]    = 0x4444u;

    gpu_vram_extract_rgb555(vram, 1023u, 511u, 2u, 2u, false, s_out555, 2u);
    CHECK_EQ_U32(s_out555[0], 0x1111u, "extract_rgb555 (0,0)");
    CHECK_EQ_U32(s_out555[1], 0x2222u, "extract_rgb555 (1,0) X-wrap");
    CHECK_EQ_U32(s_out555[2], 0x3333u, "extract_rgb555 (0,1) Y-wrap");
    CHECK_EQ_U32(s_out555[3], 0x4444u, "extract_rgb555 (1,1) XY-wrap");

    /* Blanked extraction outputs pure black */
    gpu_vram_extract_rgb555(vram, 1023u, 511u, 2u, 2u, true, s_out555, 2u);
    CHECK_EQ_U32(s_out555[0] | s_out555[3], 0u, "extract_rgb555 blanked == 0");

    /* 24bpp RGB888 extraction: 2 pixels = 3 consecutive 16-bit VRAM words:
       w0 = G0<<8 | R0, w1 = R1<<8 | B0, w2 = B1<<8 | G1 */
    vram[50 * PSX_VRAM_W + 100] = 0x2211u; /* R0=0x11, G0=0x22 */
    vram[50 * PSX_VRAM_W + 101] = 0x4433u; /* B0=0x33, R1=0x44 */
    vram[50 * PSX_VRAM_W + 102] = 0x6655u; /* G1=0x55, B1=0x66 */

    gpu_vram_extract_rgb888(vram, 100u, 50u, 2u, 1u, false, s_out888, 6u);
    CHECK_TRUE(s_out888[0] == 0x11u && s_out888[1] == 0x22u && s_out888[2] == 0x33u &&
               s_out888[3] == 0x44u && s_out888[4] == 0x55u && s_out888[5] == 0x66u,
               "extract_rgb888 24-bit 3-halfword pair unpacking");

    /* Smoke test gpu_Soft present() across 15bpp, 24bpp, wrapped, and blanked */
    sys->gpu.disp.blank   = 0u;
    sys->gpu.disp.hres    = 320u;
    sys->gpu.disp.vres    = 240u;
    sys->gpu.disp.color24 = 0u;
    sys->gpu.disp.vram_x  = 0u;
    sys->gpu.disp.vram_y  = 0u;
    sys->gpu.be->present(sys->gpu.be_self, &sys->gpu.disp); /* Zero-copy fast path */

    sys->gpu.disp.vram_x  = 900u;
    sys->gpu.be->present(sys->gpu.be_self, &sys->gpu.disp); /* Wrapped staging path */

    sys->gpu.disp.color24 = 1u;
    sys->gpu.be->present(sys->gpu.be_self, &sys->gpu.disp); /* 24bpp staging path */

    return 0;
}

int main(void) {
    memset(&s_sys, 0, sizeof(s_sys));
    psx_config_defaults(&s_sys.cfg, false);
    s_sys.cfg.gpu_backend = GPU_BACKEND_SOFT;

    CHECK_TRUE(arena_init(&s_sys.arena, 4u * 1024u * 1024u) == PSX_OK, "arena_init");
    gpu_init(&s_sys);

    if (test_fill_rect_alignment_and_wrap(&s_sys) != 0) return 1;
    if (test_cpu_vram_transfers_and_mask(&s_sys) != 0) return 1;
    if (test_vram_to_vram_overlap_and_mask(&s_sys) != 0) return 1;
    if (test_display_extraction_15_and_24bpp(&s_sys) != 0) return 1;

    gpu_shutdown(&s_sys);
    arena_destroy(&s_sys.arena);
    printf("test_gpu_vram PASS\n");
    return 0;
}
