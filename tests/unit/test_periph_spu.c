/*
 * Slice E unit test: 24-Voice SPU, PS-ADPCM (C vs ARMv6K parity), ADSR,
 * Sony Gaussian pitch interpolation, LFSR noise, PMON, Reverb, IRQ9, DMA4,
 * CD-DA/XA mixing, and SPSC SpuCmd async batch rendering.
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

static int test_adpcm_decoder(void) {
    /* 1. Filter 0, shift 0: nibble +7 -> +7 << 12 = +28672; nibble -8 -> -32768 */
    u8  blk[16];
    s16 out_c[28];
    s16 hist_c[2] = { 0, 0 };

    memset(blk, 0, sizeof(blk));
    blk[0] = 0x00u; /* filter 0, shift 0 */
    blk[1] = 0x00u;
    blk[2] = 0x87u; /* s0 = +7, s1 = -8 */
    blk[3] = 0x1Fu; /* s2 = -1, s3 = +1 */

    spu_adpcm_decode_block_c(blk, out_c, hist_c);
    CHECK_TRUE(out_c[0] == (7 * 4096), "ADPCM filter=0 shift=0 +7");
    CHECK_TRUE(out_c[1] == -32768, "ADPCM filter=0 shift=0 -8");
    CHECK_TRUE(out_c[2] == -4096, "ADPCM filter=0 shift=0 -1");
    CHECK_TRUE(out_c[3] == 4096, "ADPCM filter=0 shift=0 +1");

    /* 2. Invalid shifts 13, 14, 15 must behave identically to shift 9 */
    for (u32 f = 0u; f < 5u; f++) {
        u8  ref_blk[16];
        s16 ref_out[28];
        s16 ref_hist[2] = { 1234, -567 };
        for (u32 i = 2u; i < 16u; i++) {
            ref_blk[i] = (u8)(0x35u + i * 17u);
        }
        ref_blk[0] = (u8)((f << 4) | 9u);
        ref_blk[1] = 0u;
        spu_adpcm_decode_block_c(ref_blk, ref_out, ref_hist);

        for (u32 bad_sh = 13u; bad_sh <= 15u; bad_sh++) {
            s16 got_out[28];
            s16 got_hist[2] = { 1234, -567 };
            ref_blk[0] = (u8)((f << 4) | bad_sh);
            spu_adpcm_decode_block_c(ref_blk, got_out, got_hist);
            CHECK_TRUE(memcmp(ref_out, got_out, sizeof(ref_out)) == 0,
                       "ADPCM invalid shift 13..15 == shift 9");
            CHECK_TRUE(ref_hist[0] == got_hist[0] && ref_hist[1] == got_hist[1],
                       "ADPCM invalid shift hist match");
        }
    }

#if PSX3DS_ARCH_ARMV6K
    /* 3. Exhaustive C vs ARMv6K assembly bit-for-bit parity (aligned & unaligned) */
    u32 seed = 0xC0FFEE11u;
    alignas(4) u8 raw_buf[20];
    s16 guard_c[32];
    s16 guard_a[32];
    s16 hc[4];
    s16 ha[4];

    for (u32 iter = 0u; iter < 2000u; iter++) {
        u8 *bptr = &raw_buf[iter & 3u]; /* exercise all byte alignments 0..3 */
        for (u32 i = 0u; i < 16u; i++) {
            bptr[i] = (u8)(prng_next(&seed) & 0xFFu);
        }
        /* Force extreme saturation cases on early iterations */
        if (iter < 10u) {
            bptr[0] = (u8)(((iter % 5u) << 4) | 0u);
            memset(bptr + 2, (iter & 1u) ? 0x77 : 0x88, 14u);
        }

        for (u32 i = 0u; i < 32u; i++) {
            guard_c[i] = (s16)0x5AA5;
            guard_a[i] = (s16)0x5AA5;
        }
        s16 init_h0 = (iter == 0u) ? 32767 : ((iter == 1u) ? -32768 : (s16)(prng_next(&seed) & 0xFFFFu));
        s16 init_h1 = (iter == 0u) ? -32768 : ((iter == 1u) ? 32767 : (s16)(prng_next(&seed) & 0xFFFFu));
        hc[0] = ha[0] = (s16)0x7E7E;
        hc[1] = ha[1] = init_h0;
        hc[2] = ha[2] = init_h1;
        hc[3] = ha[3] = (s16)0x7E7E;

        spu_adpcm_decode_block_c(bptr, &guard_c[2], &hc[1]);
        spu_adpcm_decode_block_armv6(bptr, &guard_a[2], &ha[1]);

        CHECK_TRUE(memcmp(guard_c, guard_a, sizeof(guard_c)) == 0,
                   "spu_adpcm_decode_block_armv6 vs C sample/canary mismatch");
        CHECK_TRUE(memcmp(hc, ha, sizeof(hc)) == 0,
                   "spu_adpcm_decode_block_armv6 vs C hist/canary mismatch");
    }

#if !PSX3DS_PLATFORM_3DS
    {
        struct timespec t0, t1, t2;
        s16 bench_out[28];
        s16 bench_hist[2] = { 100, -200 };
        const u32 bench_iters = 200000u;
        raw_buf[0] = 0x24u;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (u32 i = 0u; i < bench_iters; i++) {
            spu_adpcm_decode_block_c(raw_buf, bench_out, bench_hist);
            raw_buf[2] = (u8)bench_out[0];
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        bench_hist[0] = 100;
        bench_hist[1] = -200;
        raw_buf[2] = 0x33u;
        for (u32 i = 0u; i < bench_iters; i++) {
            spu_adpcm_decode_block_armv6(raw_buf, bench_out, bench_hist);
            raw_buf[2] = (u8)bench_out[0];
        }
        clock_gettime(CLOCK_MONOTONIC, &t2);
        double dt_c = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;
        double dt_a = (double)(t2.tv_sec - t1.tv_sec) + (double)(t2.tv_nsec - t1.tv_nsec) * 1e-9;
        if (dt_c > 0.0 && dt_a > 0.0) {
            double msmp_c = ((double)bench_iters * 28.0 * 1e-6) / dt_c;
            double msmp_a = ((double)bench_iters * 28.0 * 1e-6) / dt_a;
            printf("ADPCM bench: C=%.1f MSmp/s, ARMv6K=%.1f MSmp/s (%.2fx)\n",
                   msmp_c, msmp_a, msmp_a / msmp_c);
        }
    }
#endif
#endif

    return 0;
}

static int test_spu_mmio_and_dma(void) {
    memset(&g_sys, 0, sizeof(g_sys));
    psx_config_defaults(&g_sys.cfg, true);
    g_sys.cfg.spu_async  = false;
    g_sys.cfg.spu_reverb = true;
    CHECK_TRUE(arena_init(&g_sys.arena, 2u * 1024u * 1024u) == PSX_OK, "arena_init SPU");

    spu_init(&g_sys);

    /* SPUSTAT[5:0] must track SPUCNT[5:0] so mode-wait loops never spinlock */
    spu_mmio_write(&g_sys, PSX_REG_SPUCNT, 0xC025u, 2u);
    u16 stat = (u16)spu_mmio_read(&g_sys, PSX_REG_SPUSTAT, 2u);
    CHECK_TRUE((stat & 0x003Fu) == 0x0025u, "SPUSTAT[5:0] mirrors SPUCNT[5:0]");
    CHECK_TRUE(((stat >> 7) & 1u) == 1u, "SPUSTAT[7] DMA R/W request bit");
    CHECK_TRUE(((stat >> 8) & 1u) == 1u, "SPUSTAT[8] DMA write request");

    /* SPUSTAT and ENDX writes must be ignored (read-only) */
    g_sys.spu.endx = 0x00ABCDEFu;
    spu_mmio_write(&g_sys, PSX_REG_SPU_ENDX_LO, 0x0000u, 2u);
    spu_mmio_write(&g_sys, PSX_REG_SPU_ENDX_HI, 0x0000u, 2u);
    CHECK_TRUE(spu_mmio_read(&g_sys, PSX_REG_SPU_ENDX_LO, 2u) == 0xCDEFu, "ENDX_LO read-only");
    CHECK_TRUE(spu_mmio_read(&g_sys, PSX_REG_SPU_ENDX_HI, 2u) == 0x00ABu, "ENDX_HI read-only");

    /* Manual FIFO transfer + latched 0x1DA6 readback + DMA4 readback */
    spu_mmio_write(&g_sys, PSX_REG_SPU_XFER_ADDR, 0x0200u, 2u); /* byte 0x1000 */
    for (u32 i = 0u; i < 8u; i++) {
        spu_mmio_write(&g_sys, PSX_REG_SPU_XFER_FIFO, 0x1100u + i, 2u);
    }
    spu_mmio_write(&g_sys, PSX_REG_SPUCNT, 0x8010u, 2u); /* Manual write flush */
    CHECK_TRUE(spu_mmio_read(&g_sys, PSX_REG_SPU_XFER_ADDR, 2u) == 0x0200u,
               "0x1DA6 returns latched write value");

    /* Read back those 4 32-bit words via DMA4 */
    spu_mmio_write(&g_sys, PSX_REG_SPU_XFER_ADDR, 0x0200u, 2u);
    spu_mmio_write(&g_sys, PSX_REG_SPUCNT, 0x8030u, 2u); /* DMA read */
    u32 dma_rd[4] = { 0 };
    CHECK_TRUE(spu_dma_read_words(&g_sys, dma_rd, 4u) == 4u, "spu_dma_read_words count");
    CHECK_TRUE(dma_rd[0] == 0x11011100u && dma_rd[1] == 0x11031102u &&
               dma_rd[2] == 0x11051104u && dma_rd[3] == 0x11071106u,
               "Manual FIFO -> SPU RAM -> DMA4 readback exact match");

    /* DMA4 write + IRQ9 address hit test */
    spu_mmio_write(&g_sys, PSX_REG_SPU_IRQ_ADDR, 0x0402u, 2u); /* byte 0x2010 */
    spu_mmio_write(&g_sys, PSX_REG_SPUCNT, 0x8060u, 2u);       /* SPU enable + IRQ9 enable + DMA write */
    CHECK_TRUE((spu_mmio_read(&g_sys, PSX_REG_SPUSTAT, 2u) & 0x0040u) == 0u, "IRQ9 initially clear");

    spu_mmio_write(&g_sys, PSX_REG_SPU_XFER_ADDR, 0x0400u, 2u); /* byte 0x2000..0x201F */
    u32 dma_wr[8] = { 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u };
    spu_dma_write_words(&g_sys, dma_wr, 8u);
    CHECK_TRUE((spu_mmio_read(&g_sys, PSX_REG_SPUSTAT, 2u) & 0x0040u) != 0u,
               "DMA4 crossing SPU_IRQ_ADDR sets SPUSTAT.bit6");

    /* Clearing SPUCNT.bit6 acknowledges and clears SPUSTAT.bit6 */
    spu_mmio_write(&g_sys, PSX_REG_SPUCNT, 0x8020u, 2u);
    CHECK_TRUE((spu_mmio_read(&g_sys, PSX_REG_SPUSTAT, 2u) & 0x0040u) == 0u,
               "Clearing SPUCNT.bit6 clears SPUSTAT.bit6");

    arena_destroy(&g_sys.arena);
    return 0;
}

static void write_adpcm_block(u8 *ram, u32 addr, u8 shift_filter, u8 flags, u8 nibble_pair) {
    ram[addr + 0u] = shift_filter;
    ram[addr + 1u] = flags;
    memset(&ram[addr + 2u], nibble_pair, 14u);
}

static int test_spu_voices_reverb_xa_async(void) {
    memset(&g_sys, 0, sizeof(g_sys));
    psx_config_defaults(&g_sys.cfg, true);
    g_sys.cfg.spu_async  = false;
    g_sys.cfg.spu_reverb = true;
    CHECK_TRUE(arena_init(&g_sys.arena, 2u * 1024u * 1024u) == PSX_OK, "arena_init SPU voices");

    spu_init(&g_sys);

    /*
     * Build a 3-block looping instrument at 0x2000 (0x0400 * 8):
     *   0x2000: flags = 0x04 (Loop Start), filter 0, shift 1, nibbles = +6/+6
     *   0x2010: flags = 0x00 (Body),       filter 0, shift 1, nibbles = -6/-6
     *   0x2020: flags = 0x03 (Loop End + Repeat), filter 0, shift 1, nibbles = +4/+4
     * And a 1-block one-shot instrument at 0x3000 (0x0600 * 8):
     *   0x3000: flags = 0x01 (Loop End, Mute)
     */
    write_adpcm_block(g_sys.spu.ram, 0x2000u, 0x01u, 0x04u, 0x66u);
    write_adpcm_block(g_sys.spu.ram, 0x2010u, 0x01u, 0x00u, 0xAAu);
    write_adpcm_block(g_sys.spu.ram, 0x2020u, 0x01u, 0x03u, 0x44u);
    write_adpcm_block(g_sys.spu.ram, 0x3000u, 0x01u, 0x01u, 0x55u);

    /* Master volume + SPU enable + unmute */
    spu_mmio_write(&g_sys, PSX_REG_SPU_MAIN_VOL_L, 0x3FFFu, 2u);
    spu_mmio_write(&g_sys, PSX_REG_SPU_MAIN_VOL_R, 0x3FFFu, 2u);
    spu_mmio_write(&g_sys, PSX_REG_SPUCNT, 0xC000u, 2u);

    /* Configure Voice 0 (looping) & Voice 1 (one-shot + PMON) & Voice 2 (LFSR Noise) */
    u32 v0 = PSX_REG_SPU_BASE + 0x00u;
    spu_mmio_write(&g_sys, v0 + 0x00u, 0x3FFFu, 2u); /* Vol L */
    spu_mmio_write(&g_sys, v0 + 0x02u, 0x3FFFu, 2u); /* Vol R */
    spu_mmio_write(&g_sys, v0 + 0x04u, 0x1000u, 2u); /* Pitch 44.1 kHz */
    spu_mmio_write(&g_sys, v0 + 0x06u, 0x0400u, 2u); /* Start = 0x2000 */
    /* Fast Attack (rate=0), fast Decay (rate=0), Sustain level = 8 (0x4800), Sustain/Release */
    spu_mmio_write(&g_sys, v0 + 0x08u, 0x0008u, 2u);
    spu_mmio_write(&g_sys, v0 + 0x0Au, 0x0002u, 2u);

    u32 v1 = PSX_REG_SPU_BASE + 0x10u;
    spu_mmio_write(&g_sys, v1 + 0x00u, 0x2000u, 2u);
    spu_mmio_write(&g_sys, v1 + 0x02u, 0x2000u, 2u);
    spu_mmio_write(&g_sys, v1 + 0x04u, 0xFFFFu, 2u); /* Clamped to 0x3FFF */
    spu_mmio_write(&g_sys, v1 + 0x06u, 0x0600u, 2u); /* Start = 0x3000 */
    spu_mmio_write(&g_sys, v1 + 0x08u, 0x000Fu, 2u);
    spu_mmio_write(&g_sys, v1 + 0x0Au, 0x0001u, 2u);

    u32 v2 = PSX_REG_SPU_BASE + 0x20u;
    spu_mmio_write(&g_sys, v2 + 0x00u, 0x2000u, 2u);
    spu_mmio_write(&g_sys, v2 + 0x02u, 0x2000u, 2u);
    spu_mmio_write(&g_sys, v2 + 0x08u, 0x000Fu, 2u);
    spu_mmio_write(&g_sys, v2 + 0x0Au, 0x0001u, 2u);

    spu_mmio_write(&g_sys, PSX_REG_SPU_PMON_LO, 0x0002u, 2u); /* Voice 1 modulated by Voice 0 */
    spu_mmio_write(&g_sys, PSX_REG_SPU_NON_LO, 0x0004u, 2u);  /* Voice 2 uses LFSR noise */

    /* Configure Reverb work area at 0x40000 (0x8000 * 8) with audible reflection */
    spu_mmio_write(&g_sys, 0x1F801D84u, 0x3FFFu, 2u); /* vLOUT */
    spu_mmio_write(&g_sys, 0x1F801D86u, 0x3FFFu, 2u); /* vROUT */
    spu_mmio_write(&g_sys, 0x1F801DA2u, 0x8000u, 2u); /* mBASE = 0x40000 */
    spu_mmio_write(&g_sys, 0x1F801DC4u, 0x4000u, 2u); /* vIIR */
    spu_mmio_write(&g_sys, 0x1F801DC6u, 0x4000u, 2u); /* vCOMB1 */
    spu_mmio_write(&g_sys, 0x1F801DD4u, 0x0020u, 2u); /* mLSAME */
    spu_mmio_write(&g_sys, 0x1F801DD6u, 0x0040u, 2u); /* mRSAME */
    spu_mmio_write(&g_sys, 0x1F801DD8u, 0x0010u, 2u); /* mLCOMB1 */
    spu_mmio_write(&g_sys, 0x1F801DDAu, 0x0030u, 2u); /* mRCOMB1 */
    spu_mmio_write(&g_sys, 0x1F801DFAu, 0x4000u, 2u); /* vLIN */
    spu_mmio_write(&g_sys, 0x1F801DFCu, 0x4000u, 2u); /* vRIN */
    spu_mmio_write(&g_sys, PSX_REG_SPU_EON_LO, 0x0001u, 2u);
    spu_mmio_write(&g_sys, PSX_REG_SPUCNT, 0xC080u, 2u); /* Enable reverb writes */

    /* Key ON voices 0, 1, 2 */
    spu_mmio_write(&g_sys, PSX_REG_SPU_KON_LO, 0x0007u, 2u);

    spu_render_batch(&g_sys, 256u);
    CHECK_TRUE(g_sys.spu.out_w - g_sys.spu.out_r == 256u, "256 stereo frames rendered");

    bool any_nonzero = false;
    for (u32 i = 0u; i < 512u; i++) {
        if (g_sys.spu.out_ring[i] != 0) {
            any_nonzero = true;
            break;
        }
    }
    CHECK_TRUE(any_nonzero, "SPU rendered non-zero stereo waveform");
    CHECK_TRUE((g_sys.spu.endx & 0x01u) != 0u, "Looping voice 0 set ENDX bit 0 on block 2");
    CHECK_TRUE(g_sys.spu.v[0].repeat_addr == 0x2000u, "Loop start flag=4 latched repeat_addr=0x2000");
    CHECK_TRUE(g_sys.spu.v[0].adsr_phase != 0u, "Looping voice 0 remains active");
    CHECK_TRUE((g_sys.spu.endx & 0x02u) != 0u && g_sys.spu.v[1].adsr_phase == 0u,
               "One-shot voice 1 set ENDX bit 1 and muted");

    /* Verify capture buffers (0x0000..0x0FFF) received Voice 1 / Voice 3 data */
    CHECK_TRUE(g_sys.spu.reverb_pos > 0u, "Reverb advanced work-area cursor");

    /* Key OFF all voices -> verify Release decays voice 0 & 2 to silence */
    spu_mmio_write(&g_sys, PSX_REG_SPU_KOFF_LO, 0x0007u, 2u);
    spu_render_batch(&g_sys, 256u);
    CHECK_TRUE(g_sys.spu.v[0].adsr_phase == 0u && g_sys.spu.v[0].env_level == 0,
               "KOFF released Voice 0 envelope to 0");

    /* Test XA/CD-DA mixing path */
    g_sys.spu.out_r = g_sys.spu.out_w;
    spu_mmio_write(&g_sys, 0x1F801DB0u, 0x3FFFu, 2u); /* CD Vol L */
    spu_mmio_write(&g_sys, 0x1F801DB2u, 0x3FFFu, 2u); /* CD Vol R */
    spu_mmio_write(&g_sys, PSX_REG_SPUCNT, 0xC001u, 2u); /* CD Audio Enable */
    s16 xa_test[64];
    for (u32 i = 0u; i < 64u; i++) {
        xa_test[i] = 12000;
    }
    spu_push_xa(&g_sys, xa_test, 32u);
    spu_render_batch(&g_sys, 32u);
    u32 base_idx = (g_sys.spu.out_r & (g_sys.spu.out_cap - 1u)) * 2u;
    CHECK_TRUE(g_sys.spu.out_ring[base_idx] > 2000 && g_sys.spu.out_ring[base_idx + 1u] > 2000,
               "CD/XA audio mixed into SPU stereo output");

    /* Test SPSC async SpuCmd queue mode */
    g_sys.cfg.spu_async  = true;
    g_sys.cfg.spu_reverb = false; /* also exercise O3DS reverb bypass knob */
    spu_reset(&g_sys);
    CHECK_TRUE(g_sys.spu.async_mode, "SPU entered async SPSC mode");
    write_adpcm_block(g_sys.spu.ram, 0x2000u, 0x01u, 0x06u, 0x66u);

    spu_mmio_write(&g_sys, PSX_REG_SPU_MAIN_VOL_L, 0x3FFFu, 2u);
    spu_mmio_write(&g_sys, PSX_REG_SPU_MAIN_VOL_R, 0x3FFFu, 2u);
    spu_mmio_write(&g_sys, PSX_REG_SPUCNT, 0xC000u, 2u);
    spu_mmio_write(&g_sys, v0 + 0x00u, 0x3FFFu, 2u);
    spu_mmio_write(&g_sys, v0 + 0x02u, 0x3FFFu, 2u);
    spu_mmio_write(&g_sys, v0 + 0x04u, 0x1000u, 2u);
    spu_mmio_write(&g_sys, v0 + 0x06u, 0x0400u, 2u);
    spu_mmio_write(&g_sys, v0 + 0x08u, 0x000Fu, 2u);
    spu_mmio_write(&g_sys, v0 + 0x0Au, 0x0002u, 2u);
    spu_mmio_write(&g_sys, PSX_REG_SPU_KON_LO, 0x0001u, 2u);
    spu_event_cb(&g_sys, 0u); /* pushes SPU_CMD_REG_ADVANCE(32) */

    CHECK_TRUE(spsc_consumer_count(&g_sys.spu.cmd_head, &g_sys.spu.cmd_tail) > 0u,
               "Async SPU queued SpuCmd entries in SPSC ring");
    spu_render_batch(&g_sys, 32u);
    CHECK_TRUE(spsc_consumer_count(&g_sys.spu.cmd_head, &g_sys.spu.cmd_tail) == 0u,
               "Core 1 worker spu_render_batch drained SpuCmd ring");
    CHECK_TRUE(g_sys.spu.out_w - g_sys.spu.out_r == 32u,
               "Async SPU worker rendered 32 frames");

    spu_shutdown(&g_sys);
    arena_destroy(&g_sys.arena);
    return 0;
}

/*
 * v0.2.6: queue `batches` SPU_CMD_REG_ADVANCE(32) commands on "Core 0" and drain them the
 * way the 3DS NDSP feeder does (spu_render_queued_async in 64-frame chunks).
 */
static u32 run_async_batches(u32 batches) {
    u32 rendered = 0u;
    for (u32 b = 0u; b < batches; b++) {
        spu_event_cb(&g_sys, 0u);
        if ((b & 7u) == 7u) {
            u32 n;
            while ((n = spu_render_queued_async(&g_sys, 64u)) != 0u) {
                rendered += n;
            }
        }
    }
    u32 n;
    while ((n = spu_render_queued_async(&g_sys, 64u)) != 0u) {
        rendered += n;
    }
    return rendered;
}

static void start_dc_voice(void) {
    const u32 v0 = PSX_REG_SPU_BASE;
    /* One self-looping block (Loop Start | Loop End | Repeat), constant +6 << 11 */
    write_adpcm_block(g_sys.spu.ram, 0x2000u, 0x01u, 0x07u, 0x66u);
    spu_mmio_write(&g_sys, PSX_REG_SPU_MAIN_VOL_L, 0x3FFFu, 2u);
    spu_mmio_write(&g_sys, PSX_REG_SPU_MAIN_VOL_R, 0x3FFFu, 2u);
    spu_mmio_write(&g_sys, PSX_REG_SPUCNT, 0xC000u, 2u);
    spu_mmio_write(&g_sys, v0 + 0x00u, 0x3FFFu, 2u); /* vol L */
    spu_mmio_write(&g_sys, v0 + 0x02u, 0x3FFFu, 2u); /* vol R */
    spu_mmio_write(&g_sys, v0 + 0x04u, 0x1000u, 2u); /* pitch 1.0 */
    spu_mmio_write(&g_sys, v0 + 0x06u, 0x0400u, 2u); /* start 0x2000 */
    spu_mmio_write(&g_sys, v0 + 0x08u, 0x000Fu, 2u); /* ADSR1: fastest attack */
    spu_mmio_write(&g_sys, v0 + 0x0Au, 0x0000u, 2u); /* ADSR2: hold sustain */
    spu_mmio_write(&g_sys, PSX_REG_SPU_KON_LO, 0x0001u, 2u);
}

static u8 g_state_buf[1536u * 1024u];

static int test_spu_async_savestate_and_ring(void) {
    memset(&g_sys, 0, sizeof(g_sys));
    psx_config_defaults(&g_sys.cfg, true);
    g_sys.cfg.spu_async  = false; /* the savestate comes from a sync (host CLI) system */
    g_sys.cfg.spu_reverb = false;
    CHECK_TRUE(arena_init(&g_sys.arena, 2u * 1024u * 1024u) == PSX_OK, "arena_init SPU state");
    spu_init(&g_sys);
    CHECK_TRUE(!g_sys.spu.async_mode, "sync SPU before save");
    start_dc_voice();
    spu_render_batch(&g_sys, 64u);
    g_sys.spu.out_r = g_sys.spu.out_w;

    StateIO st;
    memset(&st, 0, sizeof(st));
    st.writing = true;
    st.buf     = g_state_buf;
    st.cap     = (u32)sizeof(g_state_buf);
    spu_state(&g_sys, &st);
    CHECK_TRUE(!st.error && st.pos > 0u, "spu_state save (async_mode = false)");
    u32 saved_len = st.pos;

    /* 3DS system: cfg.spu_async = true; load the sync savestate into it */
    g_sys.cfg.spu_async = true;
    spu_reset(&g_sys);
    CHECK_TRUE(g_sys.spu.async_mode, "3DS config runs async SPU");
    memset(&st, 0, sizeof(st));
    st.writing = false;
    st.buf     = g_state_buf;
    st.cap     = saved_len;
    spu_state(&g_sys, &st);
    CHECK_TRUE(!st.error, "spu_state load");
    CHECK_TRUE(g_sys.spu.async_mode, "savestate load keeps async_mode = cfg.spu_async (true)");
    CHECK_TRUE(!g_sys.spu.irq_enabled, "IRQ9 off -> SpuCmd queue path");
    CHECK_TRUE(g_sys.spu.out_w == g_sys.spu.out_r, "out_ring empty after load");

    /* Loaded voice keeps playing through queued SpuCmd batches (4 NDSP buffers' worth) */
    const u32 batches = (735u * 4u + 31u) / 32u;
    CHECK_TRUE(spu_cmd_backlog(&g_sys) == 0u, "no backlog after load");
    u32 rendered = run_async_batches(batches);
    CHECK_TRUE(rendered == batches * 32u, "spu_render_queued_async rendered every queued frame");
    CHECK_TRUE(spu_cmd_backlog(&g_sys) == 0u, "SpuCmd ring fully drained");
    u32 queued = g_sys.spu.out_w - g_sys.spu.out_r;
    CHECK_TRUE(queued == rendered, "out_ring holds every rendered frame (no drops)");

    /* Contiguous non-zero stereo: once the voice is audible, no silent gaps */
    u32 first_nz = queued;
    u32 nonzero  = 0u;
    s32 max_abs  = 0;
    for (u32 i = 0u; i < queued; i++) {
        u32 idx = (g_sys.spu.out_r + i) & (g_sys.spu.out_cap - 1u);
        s32 l = g_sys.spu.out_ring[idx * 2u + 0u];
        s32 r = g_sys.spu.out_ring[idx * 2u + 1u];
        if (l != 0 || r != 0) {
            if (first_nz == queued) {
                first_nz = i;
            }
            nonzero++;
        } else if (first_nz != queued) {
            fprintf(stderr, "FAIL: silent frame %u after audio started at %u\n", (unsigned)i,
                    (unsigned)first_nz);
            return 1;
        }
        if (l < 0) l = -l;
        if (r < 0) r = -r;
        if (l > max_abs) max_abs = l;
        if (r > max_abs) max_abs = r;
    }
    CHECK_TRUE(first_nz < 32u, "voice audible within the first batch");
    CHECK_TRUE(nonzero + first_nz == queued, "contiguous non-zero stereo samples");
    CHECK_TRUE(max_abs > 1000, "loud async output after savestate load");

    /* Consumer drains (NDSP feeder) and producer keeps going: still contiguous, no drops */
    g_sys.spu.out_r = g_sys.spu.out_w;
    u32 before = g_sys.spu.out_w;
    rendered = run_async_batches(64u);
    CHECK_TRUE(g_sys.spu.out_w - before == 64u * 32u && rendered == 64u * 32u,
               "second async run rendered 2048 contiguous frames");

    spu_shutdown(&g_sys);
    arena_destroy(&g_sys.arena);
    return 0;
}

int main(void) {
    if (test_adpcm_decoder() != 0) {
        return 1;
    }
    if (test_spu_mmio_and_dma() != 0) {
        return 1;
    }
    if (test_spu_voices_reverb_xa_async() != 0) {
        return 1;
    }
    if (test_spu_async_savestate_and_ring() != 0) {
        return 1;
    }
    printf("test_periph_spu PASS (arch_armv6k=%d)\n", PSX3DS_ARCH_ARMV6K);
    return 0;
}
