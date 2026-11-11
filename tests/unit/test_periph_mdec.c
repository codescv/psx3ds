/*
 * Slice E unit test: MDEC command/status machine (0x1F801820/1824), RLE zig-zag
 * decoder, Sony Q15 2-pass integer IDCT (C vs ARMv6K SMLAD bit-for-bit parity!),
 * and 4bpp / 8bpp / 15bpp RGB5A1 (signed/unsigned/bit15) / 24bpp RGB888 output.
 */

#include "../../src/periph/periph_internal.h"
#include "psx3ds/arena.h"
#include "psx3ds/system.h"

#include <stdio.h>
#include <string.h>
#if PSX3DS_ARCH_ARMV6K && !PSX3DS_PLATFORM_3DS
#include <time.h>
#endif

#define CHECK_TRUE(cond, msg)                                                       \
    do {                                                                            \
        if (!(cond)) {                                                              \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, (msg));       \
            return 1;                                                               \
        }                                                                           \
    } while (0)

static PsxSystem g_sys;

/* Standard PS1 BIOS / mdec-frame-15bit-dma.exe 64-entry Q15 IDCT scale matrix */
static const s16 k_sony_idct_scale[64] = {
     23170,  23170,  23170,  23170,  23170,  23170,  23170,  23170,
     32138,  27245,  18204,   6392,  -6393, -18205, -27246, -32139,
     30273,  12539, -12540, -30274, -30274, -12540,  12539,  30273,
     27245,  -6393, -32139, -18205,  18204,  32138,   6392, -27246,
     23170, -23171, -23171,  23170,  23170, -23171, -23171,  23170,
     18204, -32139,   6392,  27245, -27246,  -6393,  32138, -18205,
     12539, -30274,  30273, -12540, -12540,  30273, -30274,  12539,
      6392, -18205,  27245, -32139,  32138, -27246,  18204,  -6393
};

#if PSX3DS_ARCH_ARMV6K
static u32 prng_next(u32 *state) {
    u32 x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}
#endif

static int test_idct_exactness_and_armv6_parity(void) {
    /* 1. DC-only impulse: blk[0] = 64 -> 64 / sqrt(8)^2 = +8 everywhere */
    s16 dc_blk[64];
    memset(dc_blk, 0, sizeof(dc_blk));
    dc_blk[0] = 64;
    mdec_idct_c(dc_blk, k_sony_idct_scale);
    for (u32 i = 0u; i < 64u; i++) {
        CHECK_TRUE(dc_blk[i] == 8, "DC=64 -> uniform spatial +8 across all 64 pixels");
    }

    /* Saturation clamp [-128, +127] */
    memset(dc_blk, 0, sizeof(dc_blk));
    dc_blk[0] = 2048;
    mdec_idct_c(dc_blk, k_sony_idct_scale);
    for (u32 i = 0u; i < 64u; i++) {
        CHECK_TRUE(dc_blk[i] == 127, "Positive spatial clamp +127");
    }
    memset(dc_blk, 0, sizeof(dc_blk));
    dc_blk[0] = -2048;
    mdec_idct_c(dc_blk, k_sony_idct_scale);
    for (u32 i = 0u; i < 64u; i++) {
        CHECK_TRUE(dc_blk[i] == -128, "Negative spatial clamp -128");
    }

#if PSX3DS_ARCH_ARMV6K
    /* 2. Exhaustive C vs ARMv6K SMLAD assembly bit-for-bit parity */
    alignas(4) s16 guard_c[72];
    alignas(4) s16 guard_a[72];
    alignas(4) s16 scale_buf[68];
    memcpy(&scale_buf[1], k_sony_idct_scale, sizeof(k_sony_idct_scale));

    /* 2a. All 64 basis impulses at +512 and -512 */
    for (u32 basis = 0u; basis < 64u; basis++) {
        for (int sign = -1; sign <= 1; sign += 2) {
            for (u32 i = 0u; i < 72u; i++) {
                guard_c[i] = guard_a[i] = (s16)0x3C3C;
            }
            s16 *bc = &guard_c[2];
            s16 *ba = &guard_a[2];
            memset(bc, 0, 64u * sizeof(s16));
            bc[basis] = (s16)(sign * 512);
            memcpy(ba, bc, 64u * sizeof(s16));

            mdec_idct_c(bc, k_sony_idct_scale);
            mdec_idct_armv6(ba, k_sony_idct_scale);
            CHECK_TRUE(memcmp(guard_c, guard_a, sizeof(guard_c)) == 0,
                       "mdec_idct_armv6 vs C basis impulse mismatch");
        }
    }

    /* 2b. 2,000 random [-1024, +1023] DCT blocks (aligned & 2-byte unaligned) */
    u32 seed = 0x9E3779B9u;
    for (u32 iter = 0u; iter < 2000u; iter++) {
        u32 off = (iter & 1u) ? 3u : 2u; /* off=3 tests 2-byte-only alignment! */
        for (u32 i = 0u; i < 72u; i++) {
            guard_c[i] = guard_a[i] = (s16)0x5AA5;
        }
        s16 *bc = &guard_c[off];
        s16 *ba = &guard_a[off];
        for (u32 i = 0u; i < 64u; i++) {
            if (iter == 0u) {
                bc[i] = 1023;
            } else if (iter == 1u) {
                bc[i] = -1024;
            } else if (iter == 2u) {
                bc[i] = (i & 1u) ? 1023 : -1024;
            } else {
                s32 r = (s32)(prng_next(&seed) & 0x7FFu) - 1024;
                bc[i] = (s16)r;
            }
        }
        memcpy(ba, bc, 64u * sizeof(s16));

        const s16 *sc = (iter & 2u) ? &scale_buf[1] : k_sony_idct_scale;
        mdec_idct_c(bc, sc);
        mdec_idct_armv6(ba, sc);

        if (memcmp(guard_c, guard_a, sizeof(guard_c)) != 0) {
            fprintf(stderr, "mdec_idct_armv6 mismatch on iter %u\n", (unsigned)iter);
            return 1;
        }
    }

#if !PSX3DS_PLATFORM_3DS
    {
        struct timespec t0, t1, t2;
        alignas(4) s16 bblk[64];
        const u32 bench_iters = 50000u;
        memset(bblk, 1, sizeof(bblk));
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (u32 i = 0u; i < bench_iters; i++) {
            bblk[0] = (s16)(i & 0xFFu);
            mdec_idct_c(bblk, k_sony_idct_scale);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        memset(bblk, 1, sizeof(bblk));
        for (u32 i = 0u; i < bench_iters; i++) {
            bblk[0] = (s16)(i & 0xFFu);
            mdec_idct_armv6(bblk, k_sony_idct_scale);
        }
        clock_gettime(CLOCK_MONOTONIC, &t2);
        double dt_c = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;
        double dt_a = (double)(t2.tv_sec - t1.tv_sec) + (double)(t2.tv_nsec - t1.tv_nsec) * 1e-9;
        if (dt_c > 0.0 && dt_a > 0.0) {
            double mblk_c = ((double)bench_iters * 1e-6) / dt_c;
            double mblk_a = ((double)bench_iters * 1e-6) / dt_a;
            printf("IDCT bench: C=%.2f MBlk/s, ARMv6K=%.2f MBlk/s (%.2fx)\n",
                   mblk_c, mblk_a, mblk_a / mblk_c);
        }
    }
#endif
#endif

    return 0;
}

static void upload_tables(PsxSystem *sys) {
    /* Reset MDEC via MDEC_CTRL bit 31 and enable DMA0/DMA1 bits 30..29 */
    mdec_mmio_write(sys, PSX_REG_MDEC_CTRL, 0x80000000u, 4u);
    mdec_mmio_write(sys, PSX_REG_MDEC_CTRL, 0x60000000u, 4u);

    /* cmd 2 (bit 0 = 1 -> 64B luma quant + 64B chroma quant = 32 words) */
    mdec_mmio_write(sys, PSX_REG_MDEC_CMD, (2u << 29) | 1u, 4u);
    u8 q_table[128];
    for (u32 i = 0u; i < 128u; i++) {
        q_table[i] = 8u; /* uniform quantizer = 8 so q_scale=8, iq=8 -> (val*8*8+4)>>3 = val*8 */
    }
    u32 q_words[32];
    memcpy(q_words, q_table, sizeof(q_words));
    (void)mdec_dma_in_words(sys, q_words, 32u);

    /* cmd 3 (32 words = 64 s16 scale entries) */
    mdec_mmio_write(sys, PSX_REG_MDEC_CMD, (3u << 29), 4u);
    u32 s_words[32];
    memcpy(s_words, k_sony_idct_scale, sizeof(s_words));
    (void)mdec_dma_in_words(sys, s_words, 32u);
}

static u16 pack_dc(u32 q_scale, s32 dc10) {
    return (u16)(((q_scale & 0x3Fu) << 10) | ((u32)dc10 & 0x03FFu));
}

static int test_mdec_macroblock_depths_and_dma(void) {
    memset(&g_sys, 0, sizeof(g_sys));
    psx_config_defaults(&g_sys.cfg, true);
    CHECK_TRUE(arena_init(&g_sys.arena, 2u * 1024u * 1024u) == PSX_OK, "arena_init MDEC");

    mdec_init(&g_sys);

    /* Verify exact power-on / reset MDEC_STAT = 0x8004FFFF */
    mdec_mmio_write(&g_sys, PSX_REG_MDEC_CTRL, 0x80000000u, 4u);
    CHECK_TRUE(mdec_mmio_read(&g_sys, PSX_REG_MDEC_STAT, 4u) == 0x8004FFFFu,
               "Reset MDEC_STAT == 0x8004FFFF");

    upload_tables(&g_sys);
    CHECK_TRUE((mdec_mmio_read(&g_sys, PSX_REG_MDEC_STAT, 4u) & (1u << 29)) == 0u,
               "MDEC idle after cmd 2 & cmd 3 table uploads");

    /*
     * 1. Test 15bpp RGB5A1 decode (unsigned, bit15=1) on 2 back-to-back macroblocks
     *    streamed in one DMA0 burst before DMA1 reads them out (mdec-frame-15bit-dma pattern):
     *    MB0: Cr=0, Cb=0, Y1..Y4=0 -> neutral mid-gray (128,128,128) -> R=16,G=16,B=16 + bit15 -> 0xC210
     *    MB1: Cr=+64, Cb=0, Y1..Y4=0 -> warm red-shifted macroblock (R > G, R > B)
     */
    u16 mb_hw[24];
    /* MB0: 6 blocks * (DC=0, EOB=0xFE00) = 12 halfwords */
    for (u32 b = 0u; b < 6u; b++) {
        mb_hw[b * 2u + 0u] = pack_dc(8u, 0);
        mb_hw[b * 2u + 1u] = 0xFE00u;
    }
    /* MB1: Cr DC = +64, Cb=0, Y1..Y4 = +32 */
    mb_hw[12] = pack_dc(8u, 64);
    mb_hw[13] = 0xFE00u;
    mb_hw[14] = pack_dc(8u, 0);
    mb_hw[15] = 0xFE00u;
    for (u32 b = 2u; b < 6u; b++) {
        mb_hw[12u + b * 2u + 0u] = pack_dc(8u, 32);
        mb_hw[12u + b * 2u + 1u] = 0xFE00u;
    }

    u32 in_words[12];
    memcpy(in_words, mb_hw, sizeof(in_words));

    /* cmd 1: depth=3 (15bpp), signed=0 (bit 26), bit15=1 (bit 25), param_words=12 */
    u32 cmd1_15bpp = (1u << 29) | (3u << 27) | (1u << 25) | 12u;
    mdec_mmio_write(&g_sys, PSX_REG_MDEC_CMD, cmd1_15bpp, 4u);
    CHECK_TRUE(mdec_dma_in_words(&g_sys, in_words, 12u) == 12u, "DMA0 push 2 macroblocks");

    u32 stat = mdec_mmio_read(&g_sys, PSX_REG_MDEC_STAT, 4u);
    CHECK_TRUE((stat & (1u << 31)) == 0u, "MDEC_STAT bit 31 cleared (Data-Out FIFO not empty)");
    CHECK_TRUE(((stat >> 23) & 0xFu) == ((3u << 2) | 1u), "MDEC_STAT reflects depth=3, signed=0, bit15=1");

    u32 out_15[256]; /* 2 macroblocks * 128 words (256 16-bit pixels each) */
    CHECK_TRUE(mdec_dma_out_words(&g_sys, out_15, 256u) == 256u, "DMA1 read 256 words (2 15bpp MBs)");

    for (u32 i = 0u; i < 128u; i++) {
        CHECK_TRUE(out_15[i] == 0xC210C210u,
                   "MB0 15bpp neutral gray == 0xC210 (R=16,G=16,B=16,A=1)");
    }
    u16 px_mb1 = (u16)(out_15[128] & 0xFFFFu);
    u32 r5     = px_mb1 & 0x1Fu;
    u32 g5     = (px_mb1 >> 5) & 0x1Fu;
    u32 b5     = (px_mb1 >> 10) & 0x1Fu;
    CHECK_TRUE((px_mb1 & 0x8000u) != 0u && r5 > g5 && r5 > b5,
               "MB1 positive Cr boosts Red channel above Green/Blue with bit15=1");
    CHECK_TRUE((mdec_mmio_read(&g_sys, PSX_REG_MDEC_STAT, 4u) & (1u << 31)) != 0u,
               "MDEC_STAT bit 31 set again after draining all output words");

    /* 2. Test 24bpp RGB888 decode (192 words = 768 bytes per macroblock) */
    u32 cmd1_24bpp = (1u << 29) | (2u << 27) | 6u;
    mdec_mmio_write(&g_sys, PSX_REG_MDEC_CMD, cmd1_24bpp, 4u);
    CHECK_TRUE(mdec_dma_in_words(&g_sys, in_words, 6u) == 6u, "DMA0 push 24bpp MB");
    u32 out_24[192];
    CHECK_TRUE(mdec_dma_out_words(&g_sys, out_24, 192u) == 192u, "DMA1 read 192 words (24bpp MB)");
    const u8 *rgb = (const u8 *)out_24;
    for (u32 i = 0u; i < 768u; i++) {
        CHECK_TRUE(rgb[i] == 0x80u, "24bpp neutral gray byte == 0x80");
    }

    /* 3. Test 8bpp mono (16 words = 64 bytes) and 4bpp mono (8 words = 32 bytes) */
    u16 mono_hw[2] = { pack_dc(8u, 0), 0xFE00u };
    u32 mono_w[1];
    memcpy(mono_w, mono_hw, 4u);

    mdec_mmio_write(&g_sys, PSX_REG_MDEC_CMD, (1u << 29) | (1u << 27) | 1u, 4u); /* 8bpp unsigned */
    (void)mdec_dma_in_words(&g_sys, mono_w, 1u);
    u32 out_8[16];
    CHECK_TRUE(mdec_dma_out_words(&g_sys, out_8, 16u) == 16u, "8bpp mono -> 16 words");
    CHECK_TRUE(out_8[0] == 0x80808080u, "8bpp unsigned zero block -> 0x80808080");

    mdec_mmio_write(&g_sys, PSX_REG_MDEC_CMD, (1u << 29) | (0u << 27) | 1u, 4u); /* 4bpp unsigned */
    (void)mdec_dma_in_words(&g_sys, mono_w, 1u);
    u32 out_4[8];
    CHECK_TRUE(mdec_dma_out_words(&g_sys, out_4, 8u) == 8u, "4bpp mono -> 8 words");
    CHECK_TRUE(out_4[0] == 0x88888888u, "4bpp unsigned zero block -> 0x88888888");

    /*
     * 4. Real full-frame 300-macroblock (320x240 15bpp) decode test from
     *    tests/roms/dma_timers_io/mdec-frame-15bit-dma.exe when present!
     */
    {
        const char *paths[2] = {
            "../tests/roms/dma_timers_io/mdec-frame-15bit-dma.exe",
            "tests/roms/dma_timers_io/mdec-frame-15bit-dma.exe"
        };
        FILE *rom = fopen(paths[0], "rb");
        if (!rom) {
            rom = fopen(paths[1], "rb");
        }
        if (rom) {
            static u32 rom_bs[14240];
            static u32 rom_out[38400];
            u32 rom_qt[32];
            u32 rom_sc[32];

            CHECK_TRUE(fseek(rom, 0x3550L, SEEK_SET) == 0 &&
                       fread(rom_bs, 4u, 14240u, rom) == 14240u,
                       "read mdec-frame-15bit-dma bitstream");
            CHECK_TRUE(fseek(rom, 0x113D0L, SEEK_SET) == 0 &&
                       fread(rom_qt, 4u, 32u, rom) == 32u,
                       "read mdec-frame-15bit-dma quant tables");
            CHECK_TRUE(fseek(rom, 0x11450L, SEEK_SET) == 0 &&
                       fread(rom_sc, 4u, 32u, rom) == 32u,
                       "read mdec-frame-15bit-dma scale table");
            fclose(rom);

            mdec_mmio_write(&g_sys, PSX_REG_MDEC_CTRL, 0x80000000u, 4u);
            mdec_mmio_write(&g_sys, PSX_REG_MDEC_CTRL, 0x60000000u, 4u);
            mdec_mmio_write(&g_sys, PSX_REG_MDEC_CMD, (2u << 29) | 1u, 4u);
            (void)mdec_dma_in_words(&g_sys, rom_qt, 32u);
            mdec_mmio_write(&g_sys, PSX_REG_MDEC_CMD, (3u << 29), 4u);
            (void)mdec_dma_in_words(&g_sys, rom_sc, 32u);

            /* Exact command issued by mdec-frame-15bit-dma.exe: 15bpp unsigned, bit15=0, 14240 words */
            mdec_mmio_write(&g_sys, PSX_REG_MDEC_CMD, (1u << 29) | (3u << 27) | 14240u, 4u);
            CHECK_TRUE(mdec_dma_in_words(&g_sys, rom_bs, 14240u) == 14240u,
                       "DMA0 full-frame 14,240 words");
            CHECK_TRUE((mdec_mmio_read(&g_sys, PSX_REG_MDEC_STAT, 4u) & (1u << 31)) == 0u,
                       "Full-frame output ready before DMA1");
            CHECK_TRUE(mdec_dma_out_words(&g_sys, rom_out, 38400u) == 38400u,
                       "DMA1 full-frame 38,400 words (300 macroblocks = 320x240 15bpp)");

            u32 fnv = 2166136261u;
            const u8 *ob = (const u8 *)rom_out;
            for (u32 i = 0u; i < sizeof(rom_out); i++) {
                fnv ^= ob[i];
                fnv *= 16777619u;
            }
            printf("mdec-frame-15bit-dma 320x240 frame FNV1a=0x%08lX\n", (unsigned long)fnv);
        }
    }

    mdec_shutdown(&g_sys);
    arena_destroy(&g_sys.arena);
    return 0;
}

static void mdec_dma_advance(u32 cyc) {
    PsxCpu *c = &g_sys.cpu;
    u32 rem = cyc;
    while (rem > 0u) {
        u32 step = (rem > 1000u) ? 1000u : rem;
        c->slice_len = (s32)step;
        c->cycles_left = 0;
        sched_run(&g_sys);
        rem -= step;
    }
}

/*
 * v0.2.4 regression (Winning Eleven 3 FMV "MDEC_in_sync timeout"): Psy-Q arms
 * DecDCTout() = DMA1 (MDEC-Out, SyncMode 1) BEFORE DecDCTin() = DMA0. DMA1 must stay armed
 * until the MDEC has decoded output, instead of instantly draining zeros from an empty
 * Data-Out FIFO and leaving a decoded slice stranded (Command Busy stuck at 1).
 */
static int test_mdec_dma1_armed_before_dma0(void) {
    memset(&g_sys, 0, sizeof(g_sys));
    psx_config_defaults(&g_sys.cfg, true);
    g_sys.cfg.cycle_mult = 256u;
    CHECK_TRUE(arena_init(&g_sys.arena, 20u * 1024u * 1024u) == PSX_OK, "arena_init MDEC+DMA");
    sched_init(&g_sys.sched);
    bus_init(&g_sys);
    irq_init(&g_sys);
    cpu_init(&g_sys);
    dma_init(&g_sys);
    mdec_init(&g_sys);
    cpu_set_sr(&g_sys, 0u);
    PsxCpu *c = &g_sys.cpu;

    upload_tables(&g_sys);
    psx_write32(c, PSX_REG_DPCR, PSX_DPCR_DEFAULT | 0x08888888u);

    /* Bitstream: 2 x 15bpp macroblocks (12 words) padded with 0xFE00 to one 32-word block */
    u16 mb_hw[64];
    for (u32 i = 0u; i < 64u; i++) {
        mb_hw[i] = 0xFE00u;
    }
    for (u32 b = 0u; b < 6u; b++) {
        mb_hw[b * 2u + 0u] = pack_dc(8u, 0);
        mb_hw[b * 2u + 1u] = 0xFE00u;
    }
    mb_hw[12] = pack_dc(8u, 64);
    mb_hw[14] = pack_dc(8u, 0);
    for (u32 b = 2u; b < 6u; b++) {
        mb_hw[12u + b * 2u] = pack_dc(8u, 32);
    }
    u32 in_words[32];
    memcpy(in_words, mb_hw, sizeof(in_words));
    const u32 src_phys = 0x00010000u;
    const u32 dst_phys = 0x00020000u;
    bus_dma_write_words(&g_sys, src_phys, in_words, 32u);

    static u32 sentinel[256];
    for (u32 i = 0u; i < 256u; i++) {
        sentinel[i] = 0xDEADBEEFu;
    }
    bus_dma_write_words(&g_sys, dst_phys, sentinel, 256u);

    /* 1. DecDCTout first: DMA1 to RAM, SyncMode 1, 8 blocks x 32 words (2 MBs = 256 words) */
    psx_write32(c, PSX_REG_DMA_MADR(PSX_DMA_CH_MDEC_OUT), dst_phys);
    psx_write32(c, PSX_REG_DMA_BCR(PSX_DMA_CH_MDEC_OUT), (8u << 16) | 32u);
    psx_write32(c, PSX_REG_DMA_CHCR(PSX_DMA_CH_MDEC_OUT), 0x01000200u);
    mdec_dma_advance(20000u);

    CHECK_TRUE((psx_read32(c, PSX_REG_DMA_CHCR(PSX_DMA_CH_MDEC_OUT)) & (1u << 24)) != 0u,
               "DMA1 stays armed (busy) while MDEC Data-Out FIFO is empty");
    CHECK_TRUE(psx_read32(c, PSX_REG_DMA_MADR(PSX_DMA_CH_MDEC_OUT)) == dst_phys,
               "DMA1 MADR not advanced before DMA0 delivers input");
    u32 probe[256];
    bus_dma_read_words(&g_sys, dst_phys, probe, 256u);
    for (u32 i = 0u; i < 256u; i++) {
        CHECK_TRUE(probe[i] == 0xDEADBEEFu, "DMA1 wrote no zeros before MDEC output exists");
    }

    /* 2. DecDCTin second: MDEC cmd 1 (15bpp, bit15=1, 32 words) + DMA0 1 x 32-word block */
    psx_write32(c, PSX_REG_MDEC_CMD, (1u << 29) | (3u << 27) | (1u << 25) | 32u);
    psx_write32(c, PSX_REG_DMA_MADR(PSX_DMA_CH_MDEC_IN), src_phys);
    psx_write32(c, PSX_REG_DMA_BCR(PSX_DMA_CH_MDEC_IN), (1u << 16) | 32u);
    psx_write32(c, PSX_REG_DMA_CHCR(PSX_DMA_CH_MDEC_IN), 0x01000201u);

    for (u32 spin = 0u; spin < 200u; spin++) {
        if ((psx_read32(c, PSX_REG_DMA_CHCR(PSX_DMA_CH_MDEC_OUT)) & (1u << 24)) == 0u &&
            (psx_read32(c, PSX_REG_DMA_CHCR(PSX_DMA_CH_MDEC_IN)) & (1u << 24)) == 0u) {
            break;
        }
        mdec_dma_advance(10000u);
    }
    CHECK_TRUE((psx_read32(c, PSX_REG_DMA_CHCR(PSX_DMA_CH_MDEC_IN)) & (1u << 24)) == 0u,
               "DMA0 (DecDCTin) completed");
    CHECK_TRUE((psx_read32(c, PSX_REG_DMA_CHCR(PSX_DMA_CH_MDEC_OUT)) & (1u << 24)) == 0u,
               "DMA1 (DecDCTout) completed once MDEC produced output");
    CHECK_TRUE(psx_read32(c, PSX_REG_DMA_MADR(PSX_DMA_CH_MDEC_OUT)) == dst_phys + 256u * 4u,
               "DMA1 MADR advanced by exactly 256 words");

    bus_dma_read_words(&g_sys, dst_phys, probe, 256u);
    for (u32 i = 0u; i < 128u; i++) {
        CHECK_TRUE(probe[i] == 0xC210C210u, "slice word 0..127 = MB0 neutral gray (not zeros)");
    }
    u16 px_mb1 = (u16)(probe[128] & 0xFFFFu);
    CHECK_TRUE((px_mb1 & 0x8000u) != 0u && (px_mb1 & 0x1Fu) > ((px_mb1 >> 5) & 0x1Fu),
               "slice word 128 = MB1 red-shifted pixel");

    u32 stat = psx_read32(c, PSX_REG_MDEC_STAT);
    CHECK_TRUE((stat & (1u << 31)) != 0u, "MDEC Data-Out FIFO empty after DMA1 drained it");
    CHECK_TRUE((stat & (1u << 29)) == 0u,
               "MDEC Command Busy clear -> DecDCTinSync(0) cannot time out");

    mdec_shutdown(&g_sys);
    arena_destroy(&g_sys.arena);
    return 0;
}

int main(void) {
    if (test_idct_exactness_and_armv6_parity() != 0) {
        return 1;
    }
    if (test_mdec_macroblock_depths_and_dma() != 0) {
        return 1;
    }
    if (test_mdec_dma1_armed_before_dma0() != 0) {
        return 1;
    }
    printf("test_periph_mdec PASS (arch_armv6k=%d)\n", PSX3DS_ARCH_ARMV6K);
    return 0;
}
