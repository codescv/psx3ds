/*
 * Exhaustive pixel-exact parity & throughput benchmark for Slice C software spans:
 * - Verifies all 4 PSX ABR blend formulas (0.5B+0.5F, B+F, B-F, B+0.25F)
 * - Verifies Portable C fast spans (*_c) vs gpu_soft_span_general reference
 * - On ARMv6K (T2 QEMU arm11mpcore & T1 3DS): verifies bit-for-bit parity + canary bounds
 *   across all 13 *_armv6 assembly kernels vs *_c across aligned/unaligned pointers,
 *   odd/even/peeled counts (1..129), saturation edges, UV wraparound, STP gating, & ABR 0..3
 * - Measures MPix/s throughput (and ARMv6K SIMD vs Portable C speedup on ARMv6K)
 */
#include "../../src/gpu/gpu_internal.h"
#include "psx3ds/config.h"
#include "psx3ds/platform.h"

#include <stdio.h>
#include <string.h>

#if !PSX3DS_PLATFORM_3DS
#include <time.h>
#endif

/* Static BSS buffers (safe on 3DS small stack) */
static u16 s_vram[PSX_VRAM_W * PSX_VRAM_H];
static u16 s_clut4[16];
static u16 s_clut8[256];
static u16 s_buf_ref[520] __attribute__((aligned(8)));
static u16 s_buf_dut[520] __attribute__((aligned(8)));
static u16 s_bench_dst[512] __attribute__((aligned(8)));

#define CHECK_TRUE(cond, msg)                                                       \
    do {                                                                            \
        if (!(cond)) {                                                              \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, (msg));       \
            return 1;                                                               \
        }                                                                           \
    } while (0)

static u64 now_ns(void) {
#if !PSX3DS_PLATFORM_3DS
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
#else
    return plat_time_us() * 1000ULL;
#endif
}

static u32 xorshift32(u32 *state) {
    u32 x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static void init_test_textures(void) {
    u32 rng = 0x13579BDFu;
    /* Populate CLUTs with explicit edge cases:
       0 = 0x0000 (transparent), 1 = 0x8000 (STP black), 2 = 0x7FFF (max opaque),
       3 = 0xFFFF (max STP), 4.. = deterministic pseudorandom mix of opaque & STP */
    s_clut4[0] = 0x0000u;
    s_clut4[1] = 0x8000u;
    s_clut4[2] = 0x7FFFu;
    s_clut4[3] = 0xFFFFu;
    for (u32 i = 4u; i < 16u; i++) {
        u16 v = (u16)(xorshift32(&rng) & 0xFFFFu);
        s_clut4[i] = (i == 8u) ? 0x0000u : v;
    }

    s_clut8[0] = 0x0000u;
    s_clut8[1] = 0x8000u;
    s_clut8[2] = 0x7FFFu;
    s_clut8[3] = 0xFFFFu;
    for (u32 i = 4u; i < 256u; i++) {
        u16 v = (u16)(xorshift32(&rng) & 0xFFFFu);
        s_clut8[i] = ((i & 31u) == 0u) ? 0x0000u : v;
    }

    /* Populate VRAM with mixed 4bpp/8bpp/15bpp texels including 0x0000 and 0x8000 */
    for (u32 i = 0u; i < PSX_VRAM_W * PSX_VRAM_H; i++) {
        u32 r = xorshift32(&rng);
        if ((r & 15u) == 0u) {
            s_vram[i] = 0x0000u;
        } else if ((r & 15u) == 1u) {
            s_vram[i] = 0x8000u;
        } else {
            s_vram[i] = (u16)(r & 0xFFFFu);
        }
    }
}

static int test_abr_blend_formulas(void) {
    /* Bg = (R=20, G=10, B=4), Fg = (R=16, G=24, B=8) */
    u16 bg = (u16)(20u | (10u << 5) | (4u << 10));
    u16 fg = (u16)(16u | (24u << 5) | (8u << 10));

    /* ABR 0: 0.5B + 0.5F -> (18, 17, 6) */
    u16 b0 = gpu_blend_abr15(bg, fg, 0u);
    CHECK_TRUE(b0 == (u16)(18u | (17u << 5) | (6u << 10)), "ABR 0 (0.5B + 0.5F)");

    /* ABR 1: 1.0B + 1.0F saturated to 31 -> (31, 31, 12) */
    u16 b1 = gpu_blend_abr15(bg, fg, 1u);
    CHECK_TRUE(b1 == (u16)(31u | (31u << 5) | (12u << 10)), "ABR 1 (1.0B + 1.0F saturated)");

    /* ABR 2: 1.0B - 1.0F clamped to 0 -> (4, 0, 0) */
    u16 b2 = gpu_blend_abr15(bg, fg, 2u);
    CHECK_TRUE(b2 == (u16)(4u | (0u << 5) | (0u << 10)), "ABR 2 (1.0B - 1.0F clamped)");

    /* ABR 3: 1.0B + 0.25F -> (20+4=24, 10+6=16, 4+2=6) */
    u16 b3 = gpu_blend_abr15(bg, fg, 3u);
    CHECK_TRUE(b3 == (u16)(24u | (16u << 5) | (6u << 10)), "ABR 3 (1.0B + 0.25F)");

    return 0;
}

static int verify_buffers_equal(const char *label, u32 align_off, u32 count) {
    if (s_buf_dut[align_off - 1u] != 0xCAFEu || s_buf_dut[align_off + count] != 0xBABEu) {
        fprintf(stderr, "FAIL [%s] canary clobbered (align=%u count=%u: head=0x%04X tail=0x%04X)\n",
                label, (unsigned)align_off, (unsigned)count,
                (unsigned)s_buf_dut[align_off - 1u], (unsigned)s_buf_dut[align_off + count]);
        return 1;
    }
    for (u32 i = 0u; i < count; i++) {
        u16 r = s_buf_ref[align_off + i];
        u16 d = s_buf_dut[align_off + i];
        if (r != d) {
            fprintf(stderr, "FAIL [%s] mismatch at idx=%u/%u (align=%u): ref=0x%04X dut=0x%04X\n",
                    label, (unsigned)i, (unsigned)count, (unsigned)align_off,
                    (unsigned)r, (unsigned)d);
            return 1;
        }
    }
    return 0;
}

static void seed_pair(u32 *rng, u32 align_off, u32 count) {
    s_buf_ref[align_off - 1u] = 0xCAFEu;
    s_buf_dut[align_off - 1u] = 0xCAFEu;
    for (u32 i = 0u; i < count; i++) {
        u16 bg = (u16)(xorshift32(rng) & 0xFFFFu);
        s_buf_ref[align_off + i] = bg;
        s_buf_dut[align_off + i] = bg;
    }
    s_buf_ref[align_off + count] = 0xBABEu;
    s_buf_dut[align_off + count] = 0xBABEu;
}

static int test_c_fast_spans_vs_general(void) {
    u32 rng = 0x2468ACE1u;
    const u32 count = 37u;
    const u32 off   = 2u;
    const u16 tx_base = 256u;
    const u16 ty_base = 0u;
    const u16 *tex_base = &s_vram[ty_base * PSX_VRAM_W + tx_base];

    for (u32 abr = 0u; abr < 4u; abr++) {
        for (u32 set_m = 0u; set_m <= 1u; set_m++) {
            u16 mask_or = set_m ? 0x8000u : 0x0000u;

            /* 1. flat_abr_c vs general */
            seed_pair(&rng, off, count);
            GpuSpanParams sp;
            memset(&sp, 0, sizeof(sp));
            sp.dst_row = &s_buf_ref[off];
            sp.vram    = s_vram;
            sp.count   = count;
            sp.r_q16   = 200 << 16;
            sp.g_q16   = 104 << 16;
            sp.b_q16   = 48  << 16;
            sp.abr     = (u8)abr;
            sp.flags   = PF_SEMI;
            sp.set_mask = (u8)set_m;
            sp.tw_and_x = 0xFFu;
            sp.tw_and_y = 0xFFu;
            gpu_soft_span_general(&sp);

            u16 col15 = (u16)((200u >> 3) | ((104u >> 3) << 5) | ((48u >> 3) << 10));
            gpu_soft_span_flat_abr_c(&s_buf_dut[off], count, col15, abr, mask_or);
            if (verify_buffers_equal("c_vs_gen:flat_abr", off, count) != 0) return 1;

            /* 2. gouraud_opaque_c & gouraud_abr_c vs general */
            s32 r0 = -10 * 65536, g0 = 128 * 65536, b0 = 260 * 65536;
            s32 dr =   7 * 65536, dg =  -4 * 65536, db =  -5 * 65536;
            seed_pair(&rng, off, count);
            sp.dst_row = &s_buf_ref[off];
            sp.r_q16 = r0; sp.g_q16 = g0; sp.b_q16 = b0;
            sp.dr_q16 = dr; sp.dg_q16 = dg; sp.db_q16 = db;
            sp.flags = PF_GOURAUD;
            gpu_soft_span_general(&sp);
            gpu_soft_span_gouraud_opaque_c(&s_buf_dut[off], count, r0, g0, b0, dr, dg, db, mask_or);
            if (verify_buffers_equal("c_vs_gen:gouraud_opaque", off, count) != 0) return 1;

            seed_pair(&rng, off, count);
            sp.flags = PF_GOURAUD | PF_SEMI;
            gpu_soft_span_general(&sp);
            gpu_soft_span_gouraud_abr_c(&s_buf_dut[off], count, r0, g0, b0, dr, dg, db, abr, mask_or);
            if (verify_buffers_equal("c_vs_gen:gouraud_abr", off, count) != 0) return 1;

            /* 3. Textured 4bpp / 8bpp / 15bpp (raw, mod_opaque, mod_abr) vs general */
            u16 u_q8 = 0xF480u, v_q8 = 0x1240u;
            u16 du_q8 = 0x0140u, dv_q8 = 0xFF80u; /* wraps across 255->0 and steps negative V */
            u32 uv_pack  = ((u32)v_q8 << 16) | (u32)u_q8;
            u32 duv_pack = ((u32)dv_q8 << 16) | (u32)du_q8;
            u32 rgb_mod  = 220u | (128u << 8) | (64u << 16);

            for (u32 depth = 0u; depth < 3u; depth++) {
                memset(&sp, 0, sizeof(sp));
                sp.vram      = s_vram;
                sp.clut      = (depth == 0u) ? s_clut4 : s_clut8;
                sp.count     = count;
                sp.tx_base   = tx_base;
                sp.ty_base   = ty_base;
                sp.tex_depth = (u8)depth;
                sp.abr       = (u8)abr;
                sp.set_mask  = (u8)set_m;
                sp.tw_and_x  = 0xFFu;
                sp.tw_and_y  = 0xFFu;
                sp.u_q16     = (s32)((u32)u_q8 << 8);
                sp.v_q16     = (s32)((u32)v_q8 << 8);
                sp.du_q16    = (s32)((s16)du_q8) * 256;
                sp.dv_q16    = (s32)((s16)dv_q8) * 256;
                sp.r_q16     = 220 << 16;
                sp.g_q16     = 128 << 16;
                sp.b_q16     = 64  << 16;

                /* Raw opaque */
                seed_pair(&rng, off, count);
                sp.dst_row = &s_buf_ref[off];
                sp.flags   = PF_TEXTURED | PF_RAW_TEX;
                gpu_soft_span_general(&sp);
                if (depth == 0u) {
                    gpu_soft_span_tex4_raw_opaque_c(&s_buf_dut[off], count, tex_base, s_clut4, uv_pack, duv_pack, mask_or);
                } else if (depth == 1u) {
                    gpu_soft_span_tex8_raw_opaque_c(&s_buf_dut[off], count, tex_base, s_clut8, uv_pack, duv_pack, mask_or);
                } else {
                    gpu_soft_span_tex16_raw_opaque_c(&s_buf_dut[off], count, tex_base, uv_pack, duv_pack, mask_or);
                }
                if (verify_buffers_equal("c_vs_gen:tex_raw", off, count) != 0) return 1;

                /* Modulated flat opaque */
                seed_pair(&rng, off, count);
                sp.flags = PF_TEXTURED;
                gpu_soft_span_general(&sp);
                if (depth == 0u) {
                    gpu_soft_span_tex4_mod_flat_opaque_c(&s_buf_dut[off], count, tex_base, s_clut4, uv_pack, duv_pack, rgb_mod, mask_or);
                } else if (depth == 1u) {
                    gpu_soft_span_tex8_mod_flat_opaque_c(&s_buf_dut[off], count, tex_base, s_clut8, uv_pack, duv_pack, rgb_mod, mask_or);
                } else {
                    gpu_soft_span_tex16_mod_flat_opaque_c(&s_buf_dut[off], count, tex_base, uv_pack, duv_pack, rgb_mod, mask_or);
                }
                if (verify_buffers_equal("c_vs_gen:tex_mod_opaque", off, count) != 0) return 1;

                /* Modulated flat ABR */
                seed_pair(&rng, off, count);
                sp.flags = PF_TEXTURED | PF_SEMI;
                gpu_soft_span_general(&sp);
                if (depth == 0u) {
                    gpu_soft_span_tex4_mod_flat_abr_c(&s_buf_dut[off], count, tex_base, s_clut4, uv_pack, duv_pack, rgb_mod, abr, mask_or);
                } else if (depth == 1u) {
                    gpu_soft_span_tex8_mod_flat_abr_c(&s_buf_dut[off], count, tex_base, s_clut8, uv_pack, duv_pack, rgb_mod, abr, mask_or);
                } else {
                    gpu_soft_span_tex16_mod_flat_abr_c(&s_buf_dut[off], count, tex_base, uv_pack, duv_pack, rgb_mod, abr, mask_or);
                }
                if (verify_buffers_equal("c_vs_gen:tex_mod_abr", off, count) != 0) return 1;
            }
        }
    }
    return 0;
}

#if PSX3DS_ARCH_ARMV6K
static int test_armv6_vs_c_parity(void) {
    static const u32 counts[] = { 1u, 2u, 3u, 4u, 7u, 8u, 9u, 15u, 16u, 17u, 31u, 32u, 33u, 64u, 127u, 128u, 129u };
    u32 rng = 0xCAFEBABEu;
    const u16 *tex_base = &s_vram[0 * PSX_VRAM_W + 128u];

    for (size_t ci = 0; ci < sizeof(counts) / sizeof(counts[0]); ci++) {
        u32 count = counts[ci];
        /* Test both 32-bit aligned (off=2) and 16-bit odd-unaligned (off=3) pointers! */
        for (u32 off = 2u; off <= 3u; off++) {
            /* 1. fill16 */
            u16 fill_val = (u16)(xorshift32(&rng) & 0xFFFFu);
            seed_pair(&rng, off, count);
            gpu_soft_span_fill16_c(&s_buf_ref[off], fill_val, count);
            gpu_soft_span_fill16_armv6(&s_buf_dut[off], fill_val, count);
            if (verify_buffers_equal("armv6:fill16", off, count) != 0) return 1;

            for (u32 abr = 0u; abr < 4u; abr++) {
                u16 mask_or = (xorshift32(&rng) & 1u) ? 0x8000u : 0x0000u;
                u16 col15   = (u16)(xorshift32(&rng) & 0x7FFFu);

                /* 2. flat_abr */
                seed_pair(&rng, off, count);
                gpu_soft_span_flat_abr_c(&s_buf_ref[off], count, col15, abr, mask_or);
                gpu_soft_span_flat_abr_armv6(&s_buf_dut[off], count, col15, abr, mask_or);
                if (verify_buffers_equal("armv6:flat_abr", off, count) != 0) return 1;

                /* 3 & 4. gouraud_opaque & gouraud_abr (including out-of-range saturation!) */
                s32 r0 = ((s32)(xorshift32(&rng) % 320u) - 32) * 4096;
                s32 g0 = ((s32)(xorshift32(&rng) % 320u) - 32) * 4096;
                s32 b0 = ((s32)(xorshift32(&rng) % 320u) - 32) * 4096;
                s32 dr = ((s32)(xorshift32(&rng) % 16u) - 8) * 32768;
                s32 dg = ((s32)(xorshift32(&rng) % 16u) - 8) * 32768;
                s32 db = ((s32)(xorshift32(&rng) % 16u) - 8) * 32768;

                seed_pair(&rng, off, count);
                gpu_soft_span_gouraud_opaque_c(&s_buf_ref[off], count, r0, g0, b0, dr, dg, db, mask_or);
                gpu_soft_span_gouraud_opaque_armv6(&s_buf_dut[off], count, r0, g0, b0, dr, dg, db, mask_or);
                if (verify_buffers_equal("armv6:gouraud_opaque", off, count) != 0) return 1;

                seed_pair(&rng, off, count);
                gpu_soft_span_gouraud_abr_c(&s_buf_ref[off], count, r0, g0, b0, dr, dg, db, abr, mask_or);
                gpu_soft_span_gouraud_abr_armv6(&s_buf_dut[off], count, r0, g0, b0, dr, dg, db, abr, mask_or);
                if (verify_buffers_equal("armv6:gouraud_abr", off, count) != 0) return 1;

                /* 5..13. All 9 textured kernels (4/8/16bpp x raw/mod_opaque/mod_abr) */
                u32 uv_pack  = xorshift32(&rng);
                u32 duv_pack = xorshift32(&rng);
                u32 rgb_mod  = xorshift32(&rng) & 0xFFFFFFu;

                seed_pair(&rng, off, count);
                gpu_soft_span_tex4_raw_opaque_c(&s_buf_ref[off], count, tex_base, s_clut4, uv_pack, duv_pack, mask_or);
                gpu_soft_span_tex4_raw_opaque_armv6(&s_buf_dut[off], count, tex_base, s_clut4, uv_pack, duv_pack, mask_or);
                if (verify_buffers_equal("armv6:tex4_raw_opaque", off, count) != 0) return 1;

                seed_pair(&rng, off, count);
                gpu_soft_span_tex8_raw_opaque_c(&s_buf_ref[off], count, tex_base, s_clut8, uv_pack, duv_pack, mask_or);
                gpu_soft_span_tex8_raw_opaque_armv6(&s_buf_dut[off], count, tex_base, s_clut8, uv_pack, duv_pack, mask_or);
                if (verify_buffers_equal("armv6:tex8_raw_opaque", off, count) != 0) return 1;

                seed_pair(&rng, off, count);
                gpu_soft_span_tex16_raw_opaque_c(&s_buf_ref[off], count, tex_base, uv_pack, duv_pack, mask_or);
                gpu_soft_span_tex16_raw_opaque_armv6(&s_buf_dut[off], count, tex_base, uv_pack, duv_pack, mask_or);
                if (verify_buffers_equal("armv6:tex16_raw_opaque", off, count) != 0) return 1;

                seed_pair(&rng, off, count);
                gpu_soft_span_tex4_mod_flat_opaque_c(&s_buf_ref[off], count, tex_base, s_clut4, uv_pack, duv_pack, rgb_mod, mask_or);
                gpu_soft_span_tex4_mod_flat_opaque_armv6(&s_buf_dut[off], count, tex_base, s_clut4, uv_pack, duv_pack, rgb_mod, mask_or);
                if (verify_buffers_equal("armv6:tex4_mod_flat_opaque", off, count) != 0) return 1;

                seed_pair(&rng, off, count);
                gpu_soft_span_tex8_mod_flat_opaque_c(&s_buf_ref[off], count, tex_base, s_clut8, uv_pack, duv_pack, rgb_mod, mask_or);
                gpu_soft_span_tex8_mod_flat_opaque_armv6(&s_buf_dut[off], count, tex_base, s_clut8, uv_pack, duv_pack, rgb_mod, mask_or);
                if (verify_buffers_equal("armv6:tex8_mod_flat_opaque", off, count) != 0) return 1;

                seed_pair(&rng, off, count);
                gpu_soft_span_tex16_mod_flat_opaque_c(&s_buf_ref[off], count, tex_base, uv_pack, duv_pack, rgb_mod, mask_or);
                gpu_soft_span_tex16_mod_flat_opaque_armv6(&s_buf_dut[off], count, tex_base, uv_pack, duv_pack, rgb_mod, mask_or);
                if (verify_buffers_equal("armv6:tex16_mod_flat_opaque", off, count) != 0) return 1;

                seed_pair(&rng, off, count);
                gpu_soft_span_tex4_mod_flat_abr_c(&s_buf_ref[off], count, tex_base, s_clut4, uv_pack, duv_pack, rgb_mod, abr, mask_or);
                gpu_soft_span_tex4_mod_flat_abr_armv6(&s_buf_dut[off], count, tex_base, s_clut4, uv_pack, duv_pack, rgb_mod, abr, mask_or);
                if (verify_buffers_equal("armv6:tex4_mod_flat_abr", off, count) != 0) return 1;

                seed_pair(&rng, off, count);
                gpu_soft_span_tex8_mod_flat_abr_c(&s_buf_ref[off], count, tex_base, s_clut8, uv_pack, duv_pack, rgb_mod, abr, mask_or);
                gpu_soft_span_tex8_mod_flat_abr_armv6(&s_buf_dut[off], count, tex_base, s_clut8, uv_pack, duv_pack, rgb_mod, abr, mask_or);
                if (verify_buffers_equal("armv6:tex8_mod_flat_abr", off, count) != 0) return 1;

                seed_pair(&rng, off, count);
                gpu_soft_span_tex16_mod_flat_abr_c(&s_buf_ref[off], count, tex_base, uv_pack, duv_pack, rgb_mod, abr, mask_or);
                gpu_soft_span_tex16_mod_flat_abr_armv6(&s_buf_dut[off], count, tex_base, uv_pack, duv_pack, rgb_mod, abr, mask_or);
                if (verify_buffers_equal("armv6:tex16_mod_flat_abr", off, count) != 0) return 1;
            }
        }
    }
    return 0;
}
#endif /* PSX3DS_ARCH_ARMV6K */

static void run_throughput_benchmark(void) {
    const u32 span_len = 256u;
    const u32 iters    = 8192u;
    const double total_mpix = ((double)span_len * (double)iters) / 1e6;
    const u16 *tex_base = &s_vram[128u];

    for (u32 i = 0u; i < span_len; i++) {
        s_bench_dst[i] = (u16)(0x2108u + i);
    }

    u64 t0, t1;

#if PSX3DS_ARCH_ARMV6K
#define BENCH_PAIR(name, call_c, call_asm)                                          \
    do {                                                                            \
        t0 = now_ns();                                                              \
        for (u32 it = 0u; it < iters; it++) { call_c; }                             \
        t1 = now_ns();                                                              \
        double dt_c = (t1 > t0) ? ((double)(t1 - t0) * 1e-9) : 1e-9;                \
        t0 = now_ns();                                                              \
        for (u32 it = 0u; it < iters; it++) { call_asm; }                           \
        t1 = now_ns();                                                              \
        double dt_a = (t1 > t0) ? ((double)(t1 - t0) * 1e-9) : 1e-9;                \
        printf("BENCH %-24s C=%8.1f MPix/s  ARMv6K=%8.1f MPix/s  (%.2fx)\n",         \
               (name), total_mpix / dt_c, total_mpix / dt_a,                        \
               (total_mpix / dt_a) / (total_mpix / dt_c));                          \
    } while (0)

    BENCH_PAIR("fill16",
               gpu_soft_span_fill16_c(s_bench_dst, 0x3DEFu, span_len),
               gpu_soft_span_fill16_armv6(s_bench_dst, 0x3DEFu, span_len));
    BENCH_PAIR("flat_abr0",
               gpu_soft_span_flat_abr_c(s_bench_dst, span_len, 0x4210u, 0u, 0u),
               gpu_soft_span_flat_abr_armv6(s_bench_dst, span_len, 0x4210u, 0u, 0u));
    BENCH_PAIR("flat_abr1",
               gpu_soft_span_flat_abr_c(s_bench_dst, span_len, 0x4210u, 1u, 0u),
               gpu_soft_span_flat_abr_armv6(s_bench_dst, span_len, 0x4210u, 1u, 0u));
    BENCH_PAIR("gouraud_opaque",
               gpu_soft_span_gouraud_opaque_c(s_bench_dst, span_len, 16<<16, 64<<16, 128<<16, 256, 512, -256, 0u),
               gpu_soft_span_gouraud_opaque_armv6(s_bench_dst, span_len, 16<<16, 64<<16, 128<<16, 256, 512, -256, 0u));
    BENCH_PAIR("gouraud_abr0",
               gpu_soft_span_gouraud_abr_c(s_bench_dst, span_len, 16<<16, 64<<16, 128<<16, 256, 512, -256, 0u, 0u),
               gpu_soft_span_gouraud_abr_armv6(s_bench_dst, span_len, 16<<16, 64<<16, 128<<16, 256, 512, -256, 0u, 0u));
    BENCH_PAIR("tex4_raw_opaque",
               gpu_soft_span_tex4_raw_opaque_c(s_bench_dst, span_len, tex_base, s_clut4, 0x00000000u, 0x00800100u, 0u),
               gpu_soft_span_tex4_raw_opaque_armv6(s_bench_dst, span_len, tex_base, s_clut4, 0x00000000u, 0x00800100u, 0u));
    BENCH_PAIR("tex4_mod_flat_opaque",
               gpu_soft_span_tex4_mod_flat_opaque_c(s_bench_dst, span_len, tex_base, s_clut4, 0x00000000u, 0x00800100u, 0x8090A0u, 0u),
               gpu_soft_span_tex4_mod_flat_opaque_armv6(s_bench_dst, span_len, tex_base, s_clut4, 0x00000000u, 0x00800100u, 0x8090A0u, 0u));
    BENCH_PAIR("tex8_mod_flat_abr1",
               gpu_soft_span_tex8_mod_flat_abr_c(s_bench_dst, span_len, tex_base, s_clut8, 0x00000000u, 0x00800100u, 0x8090A0u, 1u, 0u),
               gpu_soft_span_tex8_mod_flat_abr_armv6(s_bench_dst, span_len, tex_base, s_clut8, 0x00000000u, 0x00800100u, 0x8090A0u, 1u, 0u));
    BENCH_PAIR("tex16_mod_flat_opaque",
               gpu_soft_span_tex16_mod_flat_opaque_c(s_bench_dst, span_len, tex_base, 0x00000000u, 0x00800100u, 0x8090A0u, 0u),
               gpu_soft_span_tex16_mod_flat_opaque_armv6(s_bench_dst, span_len, tex_base, 0x00000000u, 0x00800100u, 0x8090A0u, 0u));
#undef BENCH_PAIR
#else
#define BENCH_C(name, call_c)                                                       \
    do {                                                                            \
        t0 = now_ns();                                                              \
        for (u32 it = 0u; it < iters; it++) { call_c; }                             \
        t1 = now_ns();                                                              \
        double dt_c = (t1 > t0) ? ((double)(t1 - t0) * 1e-9) : 1e-9;                \
        printf("BENCH %-24s C=%8.1f MPix/s\n", (name), total_mpix / dt_c);          \
    } while (0)

    BENCH_C("fill16", gpu_soft_span_fill16_c(s_bench_dst, 0x3DEFu, span_len));
    BENCH_C("flat_abr0", gpu_soft_span_flat_abr_c(s_bench_dst, span_len, 0x4210u, 0u, 0u));
    BENCH_C("gouraud_opaque", gpu_soft_span_gouraud_opaque_c(s_bench_dst, span_len, 16<<16, 64<<16, 128<<16, 256, 512, -256, 0u));
    BENCH_C("tex4_mod_flat_opaque", gpu_soft_span_tex4_mod_flat_opaque_c(s_bench_dst, span_len, tex_base, s_clut4, 0u, 0x00800100u, 0x8090A0u, 0u));
    BENCH_C("tex8_mod_flat_abr1", gpu_soft_span_tex8_mod_flat_abr_c(s_bench_dst, span_len, tex_base, s_clut8, 0u, 0x00800100u, 0x8090A0u, 1u, 0u));
#undef BENCH_C
#endif
}

int main(void) {
    gpu_soft_span_init_tables();
    init_test_textures();

    if (test_abr_blend_formulas() != 0) return 1;
    if (test_c_fast_spans_vs_general() != 0) return 1;
#if PSX3DS_ARCH_ARMV6K
    if (test_armv6_vs_c_parity() != 0) return 1;
#endif

    run_throughput_benchmark();
    printf("test_gpu_spans PASS (armv6k=%d, sink=0x%04X)\n",
           PSX3DS_ARCH_ARMV6K, s_bench_dst[0]);
    return 0;
}
