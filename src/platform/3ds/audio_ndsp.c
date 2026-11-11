#include <3ds.h>
#include <stdalign.h>
#include <string.h>

#include "plat_3ds_internal.h"

/*
 * Nintendo 3DS NDSP Quad-Buffered 44.1 kHz Stereo PCM16 Audio Backend
 *  - Uses linearMemAlign wave buffers + DSP_FlushDataCache before ndspChnWaveBufAdd
 *  - Async SPU (Core 1): renders only Core-0-queued emulated time, then drains the
 *    PsxSpu out_ring (SPSC consumer) — every SPU frame reaches NDSP exactly once
 *  - Sync SPU: Core 0 forwards out_ring via plat_audio_push() into a lock-free SPSC
 *    staging ring that Core 1 drains (out_ring is never touched from Core 1 then)
 *  - Wavebufs are refilled in strict FIFO order and only with a FULL 735-frame buffer of
 *    real samples (v0.2.6: no more DC-padded partial buffers locking NDSP for 50 ms)
 *  - ctr_audio_step_core1() is trylock-guarded: the Core 1 worker AND Core 0 (once per
 *    emulated frame) both pump it, so audio keeps flowing if Core 1 is throttled
 *  - Degrades cleanly to silent audio if sdmc:/3ds/dspfirm.cdc is missing
 */

#define CTR_NDSP_CH             0
#define CTR_NDSP_RATE           44100u
#define CTR_NDSP_BUF_COUNT      4u /* 4 x 16.67 ms = 66.7 ms NDSP ring */
#define CTR_NDSP_BUF_FRAMES     735u /* 44100 / 60 = 735 stereo frames (16.67 ms) */
#define CTR_NDSP_RING_FRAMES    8192u
#define CTR_NDSP_RING_MASK      (CTR_NDSP_RING_FRAMES - 1u)

/* Async rendering: keep ~4 NDSP buffers (~67 ms) of rendered audio queued */
#define CTR_SPU_TARGET_FRAMES   (CTR_NDSP_BUF_FRAMES * 4u)
#define CTR_SPU_CHUNK_FRAMES    64u    /* frames rendered per render-lock hold */
#define CTR_SPU_MAX_CHUNKS      48u    /* bound per Core 1 step (~3072 frames) */
#define CTR_SPU_PRESSURE_CMDS   256u   /* render past the target if Core 0 queue backs up */

/*
 * Drift control: the 60.00 Hz pacer runs the emulated ~59.8 Hz PS1 slightly fast (~0.3%
 * more samples than NDSP consumes). Above the high-water mark each wavebuf is filled from
 * 735 + 4..8 frames (linear resample, <= 1.1% / ~19 cents for one buffer) so latency stays
 * bounded instead of creeping into the command ring.
 */
#define CTR_SPU_HIGH_WATER      (CTR_SPU_TARGET_FRAMES + CTR_NDSP_BUF_FRAMES * 2u)
#define CTR_SPU_DRIFT_MAX       8u

static bool        g_ndsp_ready = false;
static PsxSystem  *g_bound_sys = NULL;
static s16        *g_linear_pcm = NULL;
static ndspWaveBuf g_wbufs[CTR_NDSP_BUF_COUNT];
static s16         g_last_l = 0;
static s16         g_last_r = 0;
static u32         g_wbuf_next = 0u; /* FIFO: next wavebuf to refill / resubmit */
static alignas(32) u32 g_step_lock = 0u; /* Core 0 / Core 1 mutual exclusion (trylock) */
static s16         g_stretch[(CTR_NDSP_BUF_FRAMES + CTR_SPU_DRIFT_MAX) * 2u];

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
    g_wbuf_next = 0u;

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
    g_wbuf_next = 0u;
    plat_log("[audio_ndsp] initialized 44100Hz stereo PCM16 (%ux%u frames)\n",
             (unsigned)CTR_NDSP_BUF_COUNT, (unsigned)CTR_NDSP_BUF_FRAMES);
    return true;
}

void ctr_audio_bind_system(PsxSystem *sys) {
    __atomic_store_n(&g_bound_sys, sys, __ATOMIC_RELEASE);
}

static u32 spu_out_queued(const PsxSystem *sys) {
    if (!sys || !sys->spu.out_ring || sys->spu.out_cap == 0u) {
        return 0u;
    }
    u32 sw = __atomic_load_n(&sys->spu.out_w, __ATOMIC_ACQUIRE);
    u32 sr = __atomic_load_n(&sys->spu.out_r, __ATOMIC_ACQUIRE);
    return sw - sr;
}

/* Frames pop_samples(sys, ...) can deliver right now (async out_ring + push ring) */
static u32 queued_frames_for(const PsxSystem *sys) {
    u32 w = __atomic_load_n(&g_push_w, __ATOMIC_ACQUIRE);
    u32 r = __atomic_load_n(&g_push_r, __ATOMIC_ACQUIRE);
    u32 n = w - r;
    if (sys && sys->spu.async_mode && sys->spu.out_ring && sys->spu.out_cap > 0u) {
        u32 sw = __atomic_load_n(&sys->spu.out_w, __ATOMIC_ACQUIRE);
        u32 sr = __atomic_load_n(&sys->spu.out_r, __ATOMIC_ACQUIRE);
        u32 q = sw - sr;
        n += (q > sys->spu.out_cap) ? sys->spu.out_cap : q;
    }
    return n;
}

u32 ctr_audio_queued_frames(void) {
    return queued_frames_for(__atomic_load_n(&g_bound_sys, __ATOMIC_ACQUIRE));
}

static u32 pop_samples(PsxSystem *sys, s16 *dst, u32 want_frames) {
    u32 got = 0u;

    /* 1. Async SPU only: drain the out_ring (sync mode forwards it via plat_audio_push) */
    if (sys && sys->spu.async_mode && sys->spu.out_ring && sys->spu.out_cap > 0u) {
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

/* Linear-resample n_src stereo frames into exactly n_dst frames (n_src >= n_dst) */
static u32 resample_into(s16 *dst, u32 n_dst, const s16 *src, u32 n_src) {
    if (n_src <= n_dst) {
        memcpy(dst, src, (size_t)n_src * 4u);
        return n_src;
    }
    u32 step = (u32)((((u64)(n_src - 1u)) << 16) / (u64)(n_dst - 1u));
    u32 pos  = 0u;
    for (u32 i = 0u; i < n_dst; i++, pos += step) {
        u32 j = pos >> 16;
        s32 f = (s32)(pos & 0xFFFFu);
        if (j >= n_src - 1u) {
            dst[i * 2u + 0u] = src[(n_src - 1u) * 2u + 0u];
            dst[i * 2u + 1u] = src[(n_src - 1u) * 2u + 1u];
            continue;
        }
        s32 l0 = src[j * 2u + 0u], l1 = src[j * 2u + 2u];
        s32 r0 = src[j * 2u + 1u], r1 = src[j * 2u + 3u];
        dst[i * 2u + 0u] = (s16)(l0 + (s32)(((s64)(l1 - l0) * f) >> 16));
        dst[i * 2u + 1u] = (s16)(r0 + (s32)(((s64)(r1 - r0) * f) >> 16));
    }
    return n_dst;
}

static void audio_step_locked(PsxSystem *sys) {
    /*
     * Async SPU: render ONLY emulated time Core 0 already queued (never free-run silence,
     * e.g. while paused in the ROM picker), in short lock holds, until ~4 NDSP buffers are
     * queued — or keep going while Core 0's command ring is backing up.
     */
    if (sys && sys->spu.async_mode && !sys->spu.irq_enabled) {
        for (u32 chunk = 0u; chunk < CTR_SPU_MAX_CHUNKS; chunk++) {
            u32 backlog = spu_cmd_backlog(sys);
            if (backlog == 0u) {
                break;
            }
            if (spu_out_queued(sys) >= CTR_SPU_TARGET_FRAMES && backlog < CTR_SPU_PRESSURE_CMDS) {
                break;
            }
            if (spu_render_queued_async(sys, CTR_SPU_CHUNK_FRAMES) == 0u &&
                spu_cmd_backlog(sys) == backlog) {
                break; /* Core 0 holds the render lock: retry next step */
            }
        }
    }

    /*
     * Refill wavebufs in strict FIFO order, and only with a FULL buffer of real samples:
     * a partially-filled buffer would be padded with flat DC and lock NDSP for 16.67 ms
     * of silence per buffer (v0.2.5 bug). Leftover frames wait for the next step.
     */
    while (g_wbufs[g_wbuf_next].status == NDSP_WBUF_DONE ||
           g_wbufs[g_wbuf_next].status == NDSP_WBUF_FREE) {
        u32 avail = queued_frames_for(sys);
        if (avail < CTR_NDSP_BUF_FRAMES) {
            /* Count an underrun only if ALL wavebufs have drained while game audio is active */
            bool any_active = false;
            for (u32 k = 0u; k < CTR_NDSP_BUF_COUNT; k++) {
                if (g_wbufs[k].status == NDSP_WBUF_PLAYING ||
                    g_wbufs[k].status == NDSP_WBUF_QUEUED) {
                    any_active = true;
                    break;
                }
            }
            if (!any_active && sys && avail > 0u) {
                sys->stats.spu_underruns++;
            }
            break;
        }
        ndspWaveBuf *wb  = &g_wbufs[g_wbuf_next];
        s16         *dst = (s16 *)wb->data_vaddr;
        u32          got;
        if (avail >= CTR_SPU_HIGH_WATER + CTR_NDSP_BUF_FRAMES) {
            u32 extra = (avail >= CTR_SPU_HIGH_WATER + CTR_NDSP_BUF_FRAMES * 3u)
                            ? CTR_SPU_DRIFT_MAX
                            : (CTR_SPU_DRIFT_MAX / 2u);
            u32 n = pop_samples(sys, g_stretch, CTR_NDSP_BUF_FRAMES + extra);
            got = resample_into(dst, CTR_NDSP_BUF_FRAMES, g_stretch, n);
        } else {
            got = pop_samples(sys, dst, CTR_NDSP_BUF_FRAMES);
        }
        if (got == 0u) {
            break;
        }
        g_last_l = dst[(got - 1u) * 2u + 0u];
        g_last_r = dst[(got - 1u) * 2u + 1u];
        wb->nsamples = got;
        DSP_FlushDataCache(dst, got * 4u);
        ndspChnWaveBufAdd(CTR_NDSP_CH, wb);
        g_wbuf_next = (g_wbuf_next + 1u) % CTR_NDSP_BUF_COUNT;
    }
}

void ctr_audio_step_core1(PsxSystem *sys) {
    if (!g_ndsp_ready) {
        return;
    }
    /* Safe from Core 1 (worker) and Core 0 (end of frame): whoever loses simply skips */
    if (__atomic_exchange_n(&g_step_lock, 1u, __ATOMIC_ACQUIRE) != 0u) {
        return;
    }
    if (!sys) {
        sys = __atomic_load_n(&g_bound_sys, __ATOMIC_ACQUIRE);
    }
    audio_step_locked(sys);
    __atomic_store_n(&g_step_lock, 0u, __ATOMIC_RELEASE);
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
    g_wbuf_next = 0u;
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
