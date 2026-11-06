#include <3ds.h>
#include <stdalign.h>
#include <string.h>

#include "plat_3ds_internal.h"

/*
 * Nintendo 3DS NDSP Triple-Buffered 44.1 kHz Stereo PCM16 Audio Backend
 *  - Uses linearMemAlign wave buffers + DSP_FlushDataCache before ndspChnWaveBufAdd
 *  - Fed asynchronously on Core 1 from PsxSpu out_ring / async batch renderer,
 *    plus lock-free SPSC staging ring for direct plat_audio_push() calls
 *  - Degrades cleanly to silent audio if sdmc:/3ds/dspfirm.cdc is missing
 */

#define CTR_NDSP_CH             0
#define CTR_NDSP_RATE           44100u
#define CTR_NDSP_BUF_COUNT      3u
#define CTR_NDSP_BUF_FRAMES     735u /* 44100 / 60 = 735 stereo frames (16.67 ms) */
#define CTR_NDSP_RING_FRAMES    8192u
#define CTR_NDSP_RING_MASK      (CTR_NDSP_RING_FRAMES - 1u)

static bool        g_ndsp_ready = false;
static PsxSystem  *g_bound_sys = NULL;
static s16        *g_linear_pcm = NULL;
static ndspWaveBuf g_wbufs[CTR_NDSP_BUF_COUNT];
static s16         g_last_l = 0;
static s16         g_last_r = 0;

/* Lock-free SPSC staging ring for sync plat_audio_push() */
static s16              g_push_ring[CTR_NDSP_RING_FRAMES * 2u];
static alignas(32) u32  g_push_w = 0u;
static alignas(32) u32  g_push_r = 0u;

bool ctr_audio_init(PsxSystem *sys) {
    g_bound_sys = sys;
    g_push_w = 0u;
    g_push_r = 0u;
    g_last_l = 0;
    g_last_r = 0;

    if (g_ndsp_ready) {
        return true;
    }

    Result rc = ndspInit();
    if (R_FAILED(rc)) {
        plat_log("[audio_ndsp] ndspInit failed (0x%08X) — silent audio fallback\n", (unsigned)rc);
        g_ndsp_ready = false;
        return false;
    }

    u32 total_bytes = CTR_NDSP_BUF_FRAMES * 4u * CTR_NDSP_BUF_COUNT;
    g_linear_pcm = (s16 *)linearMemAlign(total_bytes, 0x80u);
    if (!g_linear_pcm) {
        ndspExit();
        g_ndsp_ready = false;
        return false;
    }
    memset(g_linear_pcm, 0, total_bytes);

    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(CTR_NDSP_CH);
    ndspChnSetInterp(CTR_NDSP_CH, NDSP_INTERP_LINEAR);
    ndspChnSetRate(CTR_NDSP_CH, (float)CTR_NDSP_RATE);
    ndspChnSetFormat(CTR_NDSP_CH, NDSP_FORMAT_STEREO_PCM16);

    float mix[12];
    memset(mix, 0, sizeof(mix));
    mix[0] = 1.0f; /* Front Left */
    mix[1] = 1.0f; /* Front Right */
    ndspChnSetMix(CTR_NDSP_CH, mix);

    memset(g_wbufs, 0, sizeof(g_wbufs));
    for (u32 i = 0u; i < CTR_NDSP_BUF_COUNT; i++) {
        s16 *buf_ptr = g_linear_pcm + (size_t)i * CTR_NDSP_BUF_FRAMES * 2u;
        g_wbufs[i].data_vaddr = buf_ptr;
        g_wbufs[i].nsamples = CTR_NDSP_BUF_FRAMES;
        g_wbufs[i].status = NDSP_WBUF_FREE;
        DSP_FlushDataCache(buf_ptr, CTR_NDSP_BUF_FRAMES * 4u);
        ndspChnWaveBufAdd(CTR_NDSP_CH, &g_wbufs[i]);
    }

    g_ndsp_ready = true;
    plat_log("[audio_ndsp] initialized 44100Hz stereo PCM16 (3x%u frames)\n",
             (unsigned)CTR_NDSP_BUF_FRAMES);
    return true;
}

void ctr_audio_bind_system(PsxSystem *sys) {
    __atomic_store_n(&g_bound_sys, sys, __ATOMIC_RELEASE);
}

u32 ctr_audio_queued_frames(void) {
    u32 w = __atomic_load_n(&g_push_w, __ATOMIC_ACQUIRE);
    u32 r = __atomic_load_n(&g_push_r, __ATOMIC_ACQUIRE);
    u32 n = w - r;
    PsxSystem *sys = __atomic_load_n(&g_bound_sys, __ATOMIC_ACQUIRE);
    if (sys && sys->spu.out_ring && sys->spu.out_cap > 0u) {
        u32 sw = __atomic_load_n(&sys->spu.out_w, __ATOMIC_ACQUIRE);
        u32 sr = __atomic_load_n(&sys->spu.out_r, __ATOMIC_ACQUIRE);
        n += (sw - sr);
    }
    return n;
}

static u32 pop_samples(PsxSystem *sys, s16 *dst, u32 want_frames) {
    u32 got = 0u;

    /* 1. Drain SPU output ring if populated (async or sync SPU ring) */
    if (sys && sys->spu.out_ring && sys->spu.out_cap > 0u) {
        u32 sw = __atomic_load_n(&sys->spu.out_w, __ATOMIC_ACQUIRE);
        u32 sr = sys->spu.out_r;
        if ((sw - sr) > sys->spu.out_cap) {
            sr = sw - sys->spu.out_cap;
        }
        while (got < want_frames && sr != sw) {
            u32 idx = sr % sys->spu.out_cap;
            dst[got * 2u + 0u] = sys->spu.out_ring[idx * 2u + 0u];
            dst[got * 2u + 1u] = sys->spu.out_ring[idx * 2u + 1u];
            sr++;
            got++;
        }
        __atomic_store_n(&sys->spu.out_r, sr, __ATOMIC_RELEASE);
    }

    /* 2. Drain direct plat_audio_push() SPSC staging ring */
    u32 pw = __atomic_load_n(&g_push_w, __ATOMIC_ACQUIRE);
    u32 pr = g_push_r;
    while (got < want_frames && pr != pw) {
        u32 idx = pr & CTR_NDSP_RING_MASK;
        dst[got * 2u + 0u] = g_push_ring[idx * 2u + 0u];
        dst[got * 2u + 1u] = g_push_ring[idx * 2u + 1u];
        pr++;
        got++;
    }
    __atomic_store_n(&g_push_r, pr, __ATOMIC_RELEASE);

    return got;
}

void ctr_audio_step_core1(PsxSystem *sys) {
    if (!g_ndsp_ready) {
        return;
    }
    if (!sys) {
        sys = __atomic_load_n(&g_bound_sys, __ATOMIC_ACQUIRE);
    }

    /* In async SPU mode, render pending SPU command batches on Core 1 */
    if (sys && sys->spu.async_mode) {
        u32 sw = __atomic_load_n(&sys->spu.out_w, __ATOMIC_ACQUIRE);
        u32 sr = __atomic_load_n(&sys->spu.out_r, __ATOMIC_ACQUIRE);
        if ((sw - sr) < CTR_NDSP_BUF_FRAMES * 2u) {
            spu_render_batch(sys, PSX_SPU_BATCH_FRAMES);
        }
    }

    for (u32 i = 0u; i < CTR_NDSP_BUF_COUNT; i++) {
        if (g_wbufs[i].status == NDSP_WBUF_DONE || g_wbufs[i].status == NDSP_WBUF_FREE) {
            s16 *dst = (s16 *)g_wbufs[i].data_vaddr;
            u32 got = pop_samples(sys, dst, CTR_NDSP_BUF_FRAMES);
            if (got > 0u) {
                g_last_l = dst[(got - 1u) * 2u + 0u];
                g_last_r = dst[(got - 1u) * 2u + 1u];
            }
            if (got < CTR_NDSP_BUF_FRAMES) {
                if (sys && got > 0u) {
                    sys->stats.spu_underruns++;
                }
                for (u32 k = got; k < CTR_NDSP_BUF_FRAMES; k++) {
                    dst[k * 2u + 0u] = g_last_l;
                    dst[k * 2u + 1u] = g_last_r;
                }
            }
            g_wbufs[i].nsamples = CTR_NDSP_BUF_FRAMES;
            DSP_FlushDataCache(dst, CTR_NDSP_BUF_FRAMES * 4u);
            ndspChnWaveBufAdd(CTR_NDSP_CH, &g_wbufs[i]);
        }
    }
}

void plat_audio_push(const s16 *stereo, u32 frames) {
    if (!g_ndsp_ready || !stereo || frames == 0u) {
        return;
    }
    u32 w = g_push_w;
    u32 r = __atomic_load_n(&g_push_r, __ATOMIC_ACQUIRE);
    u32 free_frames = CTR_NDSP_RING_FRAMES - (w - r);
    if (frames > free_frames) {
        frames = free_frames;
    }
    for (u32 i = 0u; i < frames; i++) {
        u32 idx = (w + i) & CTR_NDSP_RING_MASK;
        g_push_ring[idx * 2u + 0u] = stereo[i * 2u + 0u];
        g_push_ring[idx * 2u + 1u] = stereo[i * 2u + 1u];
    }
    __atomic_store_n(&g_push_w, w + frames, __ATOMIC_RELEASE);
}

void ctr_audio_shutdown(void) {
    __atomic_store_n(&g_bound_sys, NULL, __ATOMIC_RELEASE);
    if (!g_ndsp_ready) {
        return;
    }
    ndspChnWaveBufClear(CTR_NDSP_CH);
    ndspChnReset(CTR_NDSP_CH);
    ndspExit();
    if (g_linear_pcm) {
        linearFree(g_linear_pcm);
        g_linear_pcm = NULL;
    }
    g_ndsp_ready = false;
}
