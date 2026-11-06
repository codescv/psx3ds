/*
 * Unit tests for Slice D (gpu_morton.c + gpu_texcache.c):
 *   1. 8x8 Z-order Morton LUT & 256x256 coordinate bijection
 *   2. PSX ABGR1555 -> PICA200 RGBA5551 / RGBA8888 (0x0000 vs 0x8000 vs STP bits)
 *   3. 4bpp / 8bpp / 15bpp Morton TexPage decoding, STP classification & Texture Window
 *   4. 24bpp RGB display swizzling
 *   5. 64-slot strict LRU eviction, FNV-1a CLUT hash hits & palette-swap zero-redecode
 *   6. Dirty-rect partial & wrapped VRAM invalidation
 */

#include "psx3ds/gpu_hw.h"
#include "psx3ds/stats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, (msg));       \
            return 1;                                                               \
        }                                                                           \
    } while (0)

static int test_morton_bijection(void) {
    u8 seen64[64];
    memset(seen64, 0, sizeof(seen64));

    for (u32 y = 0; y < 8u; ++y) {
        for (u32 x = 0; x < 8u; ++x) {
            u32 expected = ((x & 1u) << 0) | ((y & 1u) << 1) |
                           ((x & 2u) << 1) | ((y & 2u) << 2) |
                           ((x & 4u) << 2) | ((y & 4u) << 3);
            u32 got = gpu_morton8x8_lut[y][x];
            CHECK(got == expected && got < 64u, "8x8 Morton LUT formula mismatch");
            seen64[got]++;
        }
    }
    for (u32 i = 0; i < 64u; ++i) {
        CHECK(seen64[i] == 1u, "8x8 Morton LUT not bijective");
    }

    u8 *seen_page = (u8 *)calloc(GPU_HW_TEXPAGE_PIXELS, 1u);
    CHECK(seen_page != NULL, "calloc seen_page");
    for (u32 y = 0; y < 256u; ++y) {
        for (u32 x = 0; x < 256u; ++x) {
            u32 idx = gpu_morton_index(x, y, 256u);
            CHECK(idx < GPU_HW_TEXPAGE_PIXELS, "gpu_morton_index out of range");
            seen_page[idx]++;
        }
    }
    for (u32 i = 0; i < GPU_HW_TEXPAGE_PIXELS; ++i) {
        if (seen_page[i] != 1u) {
            free(seen_page);
            CHECK(false, "256x256 Morton index not bijective");
        }
    }
    free(seen_page);
    return 0;
}

static int test_pixel_conversions(void) {
    /* PSX 0x0000 -> transparent (0,0,0,0) */
    CHECK(gpu_psx_to_rgba5551(0x0000u) == 0x0000u, "PSX 0x0000 -> RGBA5551 transparent");
    CHECK(gpu_psx_to_rgba8888(0x0000u) == 0x00000000u, "PSX 0x0000 -> RGBA8888 transparent");

    /* PSX 0x8000 (STP black!) -> NON-transparent black (A=1 in 5551, A=0x80 in 8888) */
    CHECK(gpu_psx_to_rgba5551(0x8000u) == 0x0001u, "PSX 0x8000 STP black -> RGBA5551 A=1");
    CHECK(gpu_psx_to_rgba8888(0x8000u) == 0x00000080u, "PSX 0x8000 STP black -> RGBA8888 A=0x80");

    /* Untextured/VRAM display black 0x0000 -> opaque A=1 */
    CHECK(gpu_psx_to_rgba5551_opaque(0x0000u) == 0x0001u, "opaque 0x0000 -> 0x0001");

    /* Pure R, G, B channel positions */
    CHECK(gpu_psx_to_rgba5551(0x001Fu) == 0xF801u, "PSX pure red -> 0xF801");
    CHECK(gpu_psx_to_rgba5551(0x03E0u) == 0x07C1u, "PSX pure green -> 0x07C1");
    CHECK(gpu_psx_to_rgba5551(0x7C00u) == 0x003Fu, "PSX pure blue -> 0x003F");

    /* RGBA8888 STP=0 vs STP=1 */
    CHECK(gpu_psx_to_rgba8888(0x001Fu) == 0xFF0000FFu, "PSX STP=0 red -> RGBA8888 A=0xFF");
    CHECK(gpu_psx_to_rgba8888(0x801Fu) == 0xFF000080u, "PSX STP=1 red -> RGBA8888 A=0x80");
    CHECK(gpu_psx_to_rgba8888(0x83E0u) == 0x00FF0080u, "PSX STP=1 green -> RGBA8888 A=0x80");
    CHECK(gpu_psx_to_rgba8888(0x7C00u) == 0x0000FFFFu, "PSX STP=0 blue -> RGBA8888 A=0xFF");

    /* Exact round-trip for all 32,768 15-bit RGB combinations */
    for (u32 c = 0u; c < 0x8000u; ++c) {
        u16 pica = gpu_psx_to_rgba5551_opaque((u16)c);
        u16 back = gpu_rgba5551_to_psx(pica);
        CHECK(back == (u16)c, "RGBA5551 <-> PSX round-trip mismatch");
    }
    return 0;
}

static int test_decoders_and_texwindow(u16 *vram) {
    u16 *buf5551 = (u16 *)malloc(GPU_HW_TEXPAGE_PIXELS * sizeof(u16));
    u32 *buf8888 = (u32 *)malloc(GPU_HW_TEXPAGE_PIXELS * sizeof(u32));
    CHECK(buf5551 != NULL && buf8888 != NULL, "alloc decode buffers");

    /* --- 4bpp MIXED test (transparent + STP=0 + STP=1 + 0x8000 STP black) --- */
    memset(vram, 0, GPU_HW_VRAM_W * GPU_HW_VRAM_H * sizeof(u16));
    u16 clut_y = 480u;
    vram[(clut_y << 10) + 0u] = 0x0000u; /* transparent */
    vram[(clut_y << 10) + 1u] = 0x001Fu; /* opaque red (STP=0) */
    vram[(clut_y << 10) + 2u] = 0x83E0u; /* semi green (STP=1) */
    vram[(clut_y << 10) + 3u] = 0x8000u; /* semi black (STP=1) */

    /* Write nibbles (0, 1, 2, 3) into word (x=0, y=0), and nibbles (3, 2, 1, 0) into (x=1, y=5) */
    vram[0] = 0x3210u;
    vram[(5u << 10) + 1u] = 0x0123u;

    GpuTexAlphaClass cls4 = gpu_morton_decode_4bpp_8888(buf8888, vram, 0u, 0u, 0u, clut_y, NULL);
    CHECK(cls4 == GPU_TEX_ALPHA_MIXED, "4bpp MIXED alpha classification");
    CHECK(buf8888[gpu_morton_index(0u, 0u, 256u)] == 0x00000000u, "4bpp texel(0,0) == transparent");
    CHECK(buf8888[gpu_morton_index(1u, 0u, 256u)] == 0xFF0000FFu, "4bpp texel(1,0) == opaque red");
    CHECK(buf8888[gpu_morton_index(2u, 0u, 256u)] == 0x00FF0080u, "4bpp texel(2,0) == STP green");
    CHECK(buf8888[gpu_morton_index(3u, 0u, 256u)] == 0x00000080u, "4bpp texel(3,0) == STP black");
    CHECK(buf8888[gpu_morton_index(4u, 5u, 256u)] == 0x00000080u, "4bpp texel(4,5) == STP black");
    CHECK(buf8888[gpu_morton_index(6u, 5u, 256u)] == 0xFF0000FFu, "4bpp texel(6,5) == opaque red");

    /* --- 8bpp ALL_OPAQUE with VRAM X wrap (tp_x = 960 wraps past 1024) --- */
    memset(vram, 0, GPU_HW_VRAM_W * GPU_HW_VRAM_H * sizeof(u16));
    vram[(490u << 10) + 42u]  = 0x7C00u; /* pure blue, STP=0 */
    vram[(490u << 10) + 200u] = 0x03FFu; /* yellow, STP=0 */
    /* u = 130 -> word offset 65 -> (960 + 65) & 1023 == 1! u = 131 -> high byte of word 1 */
    vram[(256u + 9u) * 1024u + 1u] = (u16)((200u << 8) | 42u);

    GpuTexAlphaClass cls8 = gpu_morton_decode_8bpp_5551(buf5551, vram, 960u, 256u, 0u, 490u, NULL);
    CHECK(cls8 == GPU_TEX_ALPHA_ALL_OPAQUE, "8bpp ALL_OPAQUE classification");
    CHECK(buf5551[gpu_morton_index(130u, 9u, 256u)] == gpu_psx_to_rgba5551(0x7C00u),
          "8bpp wrapped u=130 sample");
    CHECK(buf5551[gpu_morton_index(131u, 9u, 256u)] == gpu_psx_to_rgba5551(0x03FFu),
          "8bpp wrapped u=131 sample");

    /* --- 15bpp ALL_STP & Texture Window repeat test --- */
    memset(vram, 0, GPU_HW_VRAM_W * GPU_HW_VRAM_H * sizeof(u16));
    /* Populate a 16x16 window at (u=16..31, v=32..47) inside TexPage (128, 0) */
    for (u32 v = 0; v < 16u; ++v) {
        for (u32 u = 0; u < 16u; ++u) {
            u16 stp_col = (u16)(0x8000u | ((v & 0x1Fu) << 5) | (u & 0x1Fu));
            vram[((32u + v) << 10) + (128u + 16u + u)] = stp_col;
        }
    }
    /* mask = 0x1E -> ~(0x1E * 8) = ~0xF0 (16px repeat); off = (2, 4) -> (16, 32) */
    GpuTexWindowKey tw = { .mask_x = 0x1Eu, .mask_y = 0x1Eu, .off_x = 0x02u, .off_y = 0x04u };
    GpuTexAlphaClass cls15 = gpu_morton_decode_15bpp_5551(buf5551, vram, 128u, 0u, &tw);
    CHECK(cls15 == GPU_TEX_ALPHA_ALL_STP, "15bpp ALL_STP classification");
    u16 expect_5_7 = gpu_psx_to_rgba5551((u16)(0x8000u | (7u << 5) | 5u));
    CHECK(buf5551[gpu_morton_index(5u, 7u, 256u)] == expect_5_7, "15bpp tw (5,7)");
    CHECK(buf5551[gpu_morton_index(16u + 5u, 16u + 7u, 256u)] == expect_5_7, "15bpp tw repeat (21,23)");
    CHECK(buf5551[gpu_morton_index(160u + 5u, 208u + 7u, 256u)] == expect_5_7, "15bpp tw repeat (165,215)");

    /* --- 24bpp RGB display swizzle test --- */
    memset(vram, 0, GPU_HW_VRAM_W * GPU_HW_VRAM_H * sizeof(u16));
    u8 *row10 = (u8 *)(vram + (10u << 10));
    /* Pixel 0 at byte 0: (R=0x12, G=0x34, B=0x56), Pixel 1 at byte 3: (R=0xAB, G=0xCD, B=0xEF) */
    row10[0] = 0x12u; row10[1] = 0x34u; row10[2] = 0x56u;
    row10[3] = 0xABu; row10[4] = 0xCDu; row10[5] = 0xEFu;
    gpu_morton_swizzle_vram_24bpp_8888(buf8888, 256u, 256u, vram, 0u, 10u, 16u, 8u);
    CHECK(buf8888[gpu_morton_index(0u, 0u, 256u)] == 0x123456FFu, "24bpp pixel 0");
    CHECK(buf8888[gpu_morton_index(1u, 0u, 256u)] == 0xABCDEFffu, "24bpp pixel 1");

    free(buf8888);
    free(buf5551);
    return 0;
}

static int test_texcache_lru_clut_and_invalidation(u16 *vram) {
    GpuTexCache tc;
    PsxStats stats;
    memset(&stats, 0, sizeof(stats));
    memset(vram, 0, GPU_HW_VRAM_W * GPU_HW_VRAM_H * sizeof(u16));

    CHECK(gpu_texcache_init(&tc, true), "gpu_texcache_init");

    /* Setup Page 0 (4bpp at 0,0) with Palette A at (0, 480) */
    u16 clut_a = (u16)(480u << 6);
    vram[(480u << 10) + 1u] = 0x001Fu; /* red */
    vram[0] = 0x1111u;

    bool hit = true;
    const GpuTexSlot *s0 = gpu_texcache_lookup(&tc, vram, 0u, 0u, clut_a, NULL, &stats, &hit);
    CHECK(s0 != NULL && !hit, "initial lookup is miss");
    CHECK(stats.gpu_tex_uploads == 1u && stats.gpu_tex_hits == 0u, "stats after 1st miss");
    CHECK(((const u16 *)s0->pixels)[0] == 0xF801u, "decoded red texel");

    /* Static repeat lookup: instant zero-redecode hit */
    s0 = gpu_texcache_lookup(&tc, vram, 0u, 0u, clut_a, NULL, &stats, &hit);
    CHECK(s0 != NULL && hit, "2nd lookup is hit");
    CHECK(stats.gpu_tex_uploads == 1u && stats.gpu_tex_hits == 1u, "stats after hit");

    /* Invalidate CLUT row WITHOUT changing CLUT contents -> FNV-1a hash matches -> ZERO redecode! */
    gpu_texcache_invalidate_vram(&tc, 0u, 480u, 16u, 1u);
    s0 = gpu_texcache_lookup(&tc, vram, 0u, 0u, clut_a, NULL, &stats, &hit);
    CHECK(s0 != NULL && hit, "same-content CLUT rewrite hits via FNV-1a");
    CHECK(stats.gpu_tex_uploads == 1u && stats.gpu_tex_hits == 2u, "zero upload on identical CLUT");

    /* In-place Palette Animation: overwrite (0, 480) with Palette B (blue), then swap back to A! */
    vram[(480u << 10) + 1u] = 0x7C00u; /* blue */
    gpu_texcache_invalidate_vram(&tc, 0u, 480u, 16u, 1u);
    const GpuTexSlot *sB = gpu_texcache_lookup(&tc, vram, 0u, 0u, clut_a, NULL, &stats, &hit);
    CHECK(sB != NULL && !hit && sB != s0, "Palette B allocates distinct slot, preserving Palette A");
    CHECK(((const u16 *)sB->pixels)[0] == 0x003Fu, "Palette B decoded blue");
    CHECK(stats.gpu_tex_uploads == 2u, "uploads == 2 after Palette B");

    /* Swap back to Palette A in place -> MUST HIT existing s0 with ZERO re-decoding! */
    vram[(480u << 10) + 1u] = 0x001Fu; /* red again */
    gpu_texcache_invalidate_vram(&tc, 0u, 480u, 16u, 1u);
    const GpuTexSlot *sA2 = gpu_texcache_lookup(&tc, vram, 0u, 0u, clut_a, NULL, &stats, &hit);
    CHECK(sA2 == s0 && hit, "in-place swap back to Palette A hits cached slot with zero redecode");
    CHECK(((const u16 *)sA2->pixels)[0] == 0xF801u, "Palette A still red");
    CHECK(stats.gpu_tex_uploads == 2u && stats.gpu_tex_hits == 3u, "zero upload on palette cycle");

    /* Partial Dirty-Rect Invalidation test */
    /* Populate Page 5 (tp_x = 320..383, tp_y = 0..255) */
    vram[320u] = 0x1111u;
    const GpuTexSlot *sP5 = gpu_texcache_lookup(&tc, vram, 5u, 0u, clut_a, NULL, &stats, &hit);
    CHECK(sP5 != NULL && !hit && stats.gpu_tex_uploads == 3u, "Page 5 initial upload");

    /* Dirty rect at (128, 0, 64, 64) touches NEITHER Page 0 (0..63) NOR Page 5 (320..383) */
    gpu_texcache_invalidate_vram(&tc, 128u, 0u, 64u, 64u);
    (void)gpu_texcache_lookup(&tc, vram, 0u, 0u, clut_a, NULL, &stats, &hit);
    CHECK(hit, "Page 0 unaffected by disjoint dirty rect");
    (void)gpu_texcache_lookup(&tc, vram, 5u, 0u, clut_a, NULL, &stats, &hit);
    CHECK(hit, "Page 5 unaffected by disjoint dirty rect");

    /* Dirty rect wrapping X from 1016 width 24 -> wraps into [0..15], dirtying Page 0 but NOT Page 5 */
    vram[(480u << 10) + 2u] = 0x03E0u; /* green at index 2 */
    gpu_texcache_invalidate_vram(&tc, 0u, 480u, 16u, 1u);
    vram[0] = 0x2222u;
    gpu_texcache_invalidate_vram(&tc, 1016u, 10u, 24u, 8u);

    /* Page 5 must only re-upload if its texels dirtied (CLUT changed so Page 5 uploads new CLUT, test clean CLUT first) */
    gpu_texcache_reset(&tc);
    memset(&stats, 0, sizeof(stats));
    (void)gpu_texcache_lookup(&tc, vram, 0u, 0u, clut_a, NULL, &stats, &hit);
    (void)gpu_texcache_lookup(&tc, vram, 5u, 0u, clut_a, NULL, &stats, &hit);
    CHECK(stats.gpu_tex_uploads == 2u, "seeded Page 0 and Page 5");

    vram[0] = 0x1111u;
    gpu_texcache_invalidate_vram(&tc, 1016u, 10u, 24u, 8u); /* wraps X into 0..15 -> hits Page 0 only */
    (void)gpu_texcache_lookup(&tc, vram, 5u, 0u, clut_a, NULL, &stats, &hit);
    CHECK(hit && stats.gpu_tex_uploads == 2u, "Page 5 clean after wrapped rect at X=1016..15");
    const GpuTexSlot *s0_re = gpu_texcache_lookup(&tc, vram, 0u, 0u, clut_a, NULL, &stats, &hit);
    CHECK(!hit && stats.gpu_tex_uploads == 3u, "Page 0 invalidated by wrapped rect at X=1016..15");
    CHECK(((const u16 *)s0_re->pixels)[0] == 0xF801u, "Page 0 re-decoded updated texel");

    /* --- 64-Slot Strict LRU Eviction Test --- */
    gpu_texcache_reset(&tc);
    memset(&stats, 0, sizeof(stats));
    for (u32 i = 0; i < 64u; ++i) {
        u16 row_y = (u16)(300u + i);
        vram[(row_y << 10) + 1u] = (u16)(0x0001u + i * 7u);
        u16 c_attr = (u16)(row_y << 6);
        const GpuTexSlot *si = gpu_texcache_lookup(&tc, vram, 0u, 0u, c_attr, NULL, &stats, &hit);
        CHECK(si != NULL && !hit, "filling 64 unique CLUT slots");
    }
    CHECK(stats.gpu_tex_uploads == 64u && stats.gpu_tex_hits == 0u, "64 slots filled");

    /* Touch slot 0 (row 300) so row 301 becomes the oldest LRU entry */
    (void)gpu_texcache_lookup(&tc, vram, 0u, 0u, (u16)(300u << 6), NULL, &stats, &hit);
    CHECK(hit && stats.gpu_tex_hits == 1u, "touch slot 0 hit");

    /* Insert 65th unique CLUT (row 364) -> must evict row 301! */
    vram[(364u << 10) + 1u] = 0x7FFFu;
    (void)gpu_texcache_lookup(&tc, vram, 0u, 0u, (u16)(364u << 6), NULL, &stats, &hit);
    CHECK(!hit && stats.gpu_tex_uploads == 65u, "65th entry triggers LRU eviction");

    /* Row 300 (touched) and rows 302..364 must ALL still be cache hits! */
    (void)gpu_texcache_lookup(&tc, vram, 0u, 0u, (u16)(300u << 6), NULL, &stats, &hit);
    CHECK(hit, "touched row 300 survived LRU eviction");
    for (u32 i = 2u; i <= 64u; ++i) {
        u16 row_y = (u16)(300u + i);
        (void)gpu_texcache_lookup(&tc, vram, 0u, 0u, (u16)(row_y << 6), NULL, &stats, &hit);
        CHECK(hit, "recent slot survived LRU eviction");
    }

    /* Row 301 was evicted, so looking it up now must be a miss! */
    (void)gpu_texcache_lookup(&tc, vram, 0u, 0u, (u16)(301u << 6), NULL, &stats, &hit);
    CHECK(!hit, "oldest LRU entry (row 301) was evicted as expected");

    gpu_texcache_shutdown(&tc);
    return 0;
}

int main(void) {
    u16 *vram = (u16 *)calloc(GPU_HW_VRAM_W * GPU_HW_VRAM_H, sizeof(u16));
    CHECK(vram != NULL, "calloc 1 MiB shadow VRAM");

    if (test_morton_bijection() != 0 ||
        test_pixel_conversions() != 0 ||
        test_decoders_and_texwindow(vram) != 0 ||
        test_texcache_lru_clut_and_invalidation(vram) != 0) {
        free(vram);
        return 1;
    }

    free(vram);
    printf("test_gpuhw_texcache PASS\n");
    return 0;
}
