#include <3ds.h>
#include <stdalign.h>
#include <string.h>

#include "plat_3ds_internal.h"

/*
 * Nintendo 3DS NDSP 8-Buffer 44.1 kHz Stereo PCM16 Audio Backend (v0.2.8)
 *
 *  Core 1 (spu_core1_worker) -> ctr_audio_render_core1():
 *      renders ONLY Core-0-queued emulated time (spu_render_queued_async) into the PsxSpu
 *      out_ring. It never touches g_linear_pcm, g_wbufs, DSP_FlushDataCache or ndsp*.
 *
 *  Core 0 (main loop, pacer_wait slices, autorun) -> ctr_audio_step_core1() (alias
 *  ctr_audio_pump_core0()):
 *      pops PCM into the linear wavebufs, cleans Core 0's L1/L2 D-cache with
 *      svcFlushProcessDataCache (Kernel11, no dspHandle IPC racing ndspThreadMain) plus
 *      GSPGPU_FlushDataCache, and submits ndspChnWaveBufAdd — all on Core 0.
 *
 *  libctru stop-race fix: whenever NDSP drained (or stalled) the channel is re-armed with
 *  ndspChnWaveBufClear() BEFORE new wavebufs are queued. Without it ndspiReadChnState can
 *  retire the last wavebuf while leaving chn->playing = 1, and ndspiUpdateChn then queues
 *  new buffers without restarting the stopped DSP channel (silence forever on hardware).
 *
 *  Sync SPU: Core 0 forwards out_ring via plat_audio_push() into a lock-free SPSC staging
 *  ring that Core 0 itself drains here.
 *  Degrades cleanly to silent audio if sdmc:/3ds/dspfirm.cdc is missing.
 *
 *  v0.2.8 smooth-audio pump:
 *   - never splices silence into a wavebuf while NDSP is still playing (v0.2.7 padded
 *     ~700 frames of fade-to-zero whenever only 1 wavebuf remained: ~8 holes/s);
 *   - synchronously drains Core-0-queued SPU commands before checking availability, so a
 *     Core 1 render-lock hold can no longer leave part of a frame unrendered;
 *   - 8 x 735-frame ring, 3-wavebuf (50 ms) prime, re-prime only on a genuine underrun;
 *   - the NDSP hardware resampler rate tracks the paced PS1 sample rate (60.00 Hz pacer x
 *     737.13 frames = 44,228 Hz NTSC; 50.00 Hz x 886.23 = 44,312 Hz PAL) and is nudged
 *     +-0.3% to keep ~3 wavebufs queued, instead of step-resampling or underrunning.
 */

#define CTR_NDSP_CH             0
#define CTR_NDSP_BUF_COUNT      8u /* 8 x 16.67 ms = 133.3 ms NDSP ring */
#define CTR_NDSP_BUF_FRAMES     735u /* 44100 / 60 = 735 stereo frames (16.67 ms) */
#define CTR_NDSP_PRIME_FRAMES   (CTR_NDSP_BUF_FRAMES * 3u) /* 50.0 ms before (re)start */

/*
 * NDSP resampler rates (Hz). The host pacer runs NTSC frames (566,124 cycles = 737.13
 * SPU frames) at 60.00 Hz and PAL frames (680,628 cycles = 886.23 SPU frames) at 50.00 Hz.
 */
#define CTR_NDSP_RATE_NTSC      44228.0f
#define CTR_NDSP_RATE_NTSC_FAST 44360.0f
#define CTR_NDSP_RATE_NTSC_SLOW 44100.0f
#define CTR_NDSP_RATE_PAL       44312.0f
#define CTR_NDSP_RATE_PAL_FAST  44440.0f
#define CTR_NDSP_RATE_PAL_SLOW  44180.0f
/* Rate-steering hysteresis on total queued frames (NDSP wavebufs + SPU rings) */
#define CTR_NDSP_FAST_ENTER     (CTR_NDSP_BUF_FRAMES * 5u)        /* > 83.3 ms: speed up  */
#define CTR_NDSP_FAST_EXIT      (CTR_NDSP_BUF_FRAMES * 4u)        /* <= 66.7 ms: back     */
#define CTR_NDSP_SLOW_ENTER     ((CTR_NDSP_BUF_FRAMES * 3u) / 2u) /* < 25.0 ms: slow down */
#define CTR_NDSP_SLOW_EXIT      ((CTR_NDSP_BUF_FRAMES * 5u) / 2u) /* >= 41.7 ms: back     */
#define CTR_NDSP_RING_FRAMES    8192u
#define CTR_NDSP_RING_MASK      (CTR_NDSP_RING_FRAMES - 1u)

/* NDSP stall watchdog: well past the whole 133.3 ms ring without wavebuf progress */
#define CTR_NDSP_STALL_US       250000ULL
#define CTR_NDSP_STOPPED_US     35000ULL

/* Async rendering: keep ~4 NDSP buffers (~67 ms) of rendered audio queued */
#define CTR_SPU_TARGET_FRAMES   (CTR_NDSP_BUF_FRAMES * 4u)
#define CTR_SPU_CHUNK_FRAMES    64u    /* frames rendered per render-lock hold */
#define CTR_SPU_MAX_CHUNKS      48u    /* bound per render step (~3072 frames) */
#define CTR_SPU_PRESSURE_CMDS   256u   /* render past the target if Core 0 queue backs up */

/*
 * Overflow safety net only (v0.2.8: rate matching is done by the NDSP resampler): above
 * the high-water mark each wavebuf is filled from 735 + 4..8 frames (linear resample,
 * <= 1.1% for one buffer) so latency stays bounded instead of creeping into cmd_ring.
 */
#define CTR_SPU_HIGH_WATER      (CTR_NDSP_BUF_FRAMES * 6u) /* 100.0 ms */
#define CTR_SPU_DRIFT_MAX       8u

static bool        g_ndsp_ready = false;
static PsxSystem  *g_bound_sys = NULL;
static s16        *g_linear_pcm = NULL;
static ndspWaveBuf g_wbufs[CTR_NDSP_BUF_COUNT];
static s16         g_last_l = 0;
static s16         g_last_r = 0;
static u32         g_wbuf_next = 0u; /* FIFO: next wavebuf to refill / resubmit */
static bool        g_stream_active = false; /* game audio is (re)started in NDSP */
static u64         g_last_wb_progress_us = 0ULL;
static alignas(32) u32 g_render_lock = 0u; /* Core 1 render vs Core 0 fallback (trylock) */
static alignas(32) u32 g_pump_lock = 0u;   /* re-entrancy guard for the Core 0 pump */
static s16         g_stretch[(CTR_NDSP_BUF_FRAMES + CTR_SPU_DRIFT_MAX) * 2u];
static float       g_ndsp_rate = CTR_NDSP_RATE_NTSC; /* last rate sent to ndspChnSetRate */
static int         g_rate_mode = 0; /* -1 SLOW, 0 BASE, +1 FAST (hysteresis state) */

/* Live diagnostics for the 2:HUD tab (written on Core 0, read on Core 0) */
static u32  g_wb_submitted = 0u;
static u32  g_wb_active = 0u;
static u32  g_pcm_peak = 0u;
static bool g_logged_first_audio = false;

/* Lock-free SPSC staging ring for sync plat_audio_push() */
static s16              g_push_ring[CTR_NDSP_RING_FRAMES * 2u];
static alignas(32) u32  g_push_w = 0u;
static alignas(32) u32  g_push_r = 0u;

/*
 * (Re)arm NDSP channel 0 from scratch: detach every wavebuf from libctru's queue (bumps
 * chn->sync_count, clears chn->playing), re-apply the output/channel configuration and
 * mark all of our wavebufs FREE. Core 0 only.
 */
static void configure_ndsp_channel_locked(void) {
    ndspChnWaveBufClear(CTR_NDSP_CH);
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspSetClippingMode(NDSP_CLIP_SOFT);
    ndspSetMasterVol(1.0f);
    ndspChnSetInterp(CTR_NDSP_CH, NDSP_INTERP_LINEAR);
    g_ndsp_rate = CTR_NDSP_RATE_NTSC;
    g_rate_mode = 0;
    ndspChnSetRate(CTR_NDSP_CH, g_ndsp_rate);
    ndspChnSetFormat(CTR_NDSP_CH, NDSP_FORMAT_STEREO_PCM16);
    float mix[12];
    memset(mix, 0, sizeof(mix));
    mix[0] = 1.0f; /* Front Left */
    mix[1] = 1.0f; /* Front Right */
    ndspChnSetMix(CTR_NDSP_CH, mix);
    ndspChnSetPaused(CTR_NDSP_CH, false);
    for (u32 i = 0u; i < CTR_NDSP_BUF_COUNT; i++) {
        s16 *buf_ptr = g_linear_pcm + (size_t)i * CTR_NDSP_BUF_FRAMES * 2u;
        memset(&g_wbufs[i], 0, sizeof(g_wbufs[i]));
        g_wbufs[i].data_vaddr = buf_ptr;
        g_wbufs[i].nsamples = CTR_NDSP_BUF_FRAMES;
        g_wbufs[i].status = NDSP_WBUF_FREE;
    }
    g_wbuf_next = 0u;
    g_stream_active = false;
}

bool ctr_audio_init(PsxSystem *sys) {
    g_bound_sys = sys;
    g_push_w = 0u;
    g_push_r = 0u;
    g_last_l = 0;
    g_last_r = 0;
    g_wbuf_next = 0u;
    g_stream_active = false;

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
    (void)svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)g_linear_pcm, total_bytes);

    ndspChnReset(CTR_NDSP_CH);
    /*
     * v0.2.7: configure only — do NOT queue silence wavebufs in the ROM menu. They would
     * drain within 67 ms and leave libctru's channel in the stopped-but-"playing" state
     * that swallowed every later ndspChnWaveBufAdd on real hardware.
     */
    configure_ndsp_channel_locked();

    g_ndsp_ready = true;
    plat_log("[audio_ndsp] initialized %uHz stereo PCM16 (%ux%u frames)\n",
             (unsigned)CTR_NDSP_RATE_NTSC, (unsigned)CTR_NDSP_BUF_COUNT,
             (unsigned)CTR_NDSP_BUF_FRAMES);
    return true;
}

/* Called on Core 0 with the Core 1 worker stopped (boot / reset / autorun start) */
void ctr_audio_bind_system(PsxSystem *sys) {
    __atomic_store_n(&g_push_w, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_push_r, 0u, __ATOMIC_RELAXED);
    g_last_l = 0;
    g_last_r = 0;
    if (g_ndsp_ready) {
        configure_ndsp_channel_locked();
    }
    g_wbuf_next = 0u;
    g_stream_active = false;
    g_wb_submitted = 0u;
    g_wb_active = 0u;
    g_pcm_peak = 0u;
    g_logged_first_audio = false;
    g_last_wb_progress_us = 0ULL;
    __atomic_store_n(&g_bound_sys, sys, __ATOMIC_RELEASE);
    plat_log("[audio_ndsp] channel reset for game boot (ndsp=%d)\n", (int)g_ndsp_ready);
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

void ctr_audio_get_diag(u32 *out_wb_submitted, u32 *out_wb_active, u32 *out_pcm_peak) {
    if (out_wb_submitted) {
        *out_wb_submitted = g_wb_submitted;
    }
    if (out_wb_active) {
        *out_wb_active = g_wb_active;
    }
    if (out_pcm_peak) {
        *out_pcm_peak = g_pcm_peak;
    }
}

/* Core 0 only: sole consumer of both the SPU out_ring and the push staging ring */
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

/*
 * Core 1 (spu_core1_worker, every 500 us) — and Core 0 as a fallback: render ONLY emulated
 * time Core 0 already queued (never free-run silence, e.g. while paused in the ROM
 * picker), in short lock holds, until ~4 NDSP buffers are queued — or keep going while
 * Core 0's command ring is backing up. Never touches NDSP, g_wbufs or the DSP cache.
 */
void ctr_audio_render_core1(PsxSystem *sys) {
    if (!sys) {
        sys = __atomic_load_n(&g_bound_sys, __ATOMIC_ACQUIRE);
    }
    if (!sys || !sys->spu.async_mode || sys->spu.irq_enabled) {
        return;
    }
    if (__atomic_exchange_n(&g_render_lock, 1u, __ATOMIC_ACQUIRE) != 0u) {
        return; /* the other core is rendering right now */
    }
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
            break; /* Core 0 holds the SPU render lock: retry next step */
        }
    }
    __atomic_store_n(&g_render_lock, 0u, __ATOMIC_RELEASE);
}

static void reset_wavebufs_for_restart(void) {
    ndspChnWaveBufClear(CTR_NDSP_CH);
    for (u32 i = 0u; i < CTR_NDSP_BUF_COUNT; i++) {
        g_wbufs[i].status = NDSP_WBUF_FREE;
    }
    g_wbuf_next = 0u;
    ndspChnSetPaused(CTR_NDSP_CH, false);
}

/*
 * Frames queued in NDSP (submitted, not yet played) + frames ready in the SPU rings.
 * ndspChnGetSamplePos() is the play position inside the wavebuf currently playing.
 */
static u32 total_queued_frames(PsxSystem *sys, u32 active_wbufs) {
    u32 q = active_wbufs * CTR_NDSP_BUF_FRAMES;
    if (active_wbufs > 0u) {
        u32 pos = ndspChnGetSamplePos(CTR_NDSP_CH);
        q -= (pos < CTR_NDSP_BUF_FRAMES) ? pos : CTR_NDSP_BUF_FRAMES;
    }
    return q + queued_frames_for(sys);
}

/*
 * v0.2.8 rate matching: steer the TeakLite DSP resampler around the paced PS1 sample rate
 * so ~3 wavebufs stay queued (no pitch steps, no overruns, no underruns). FAST / SLOW are
 * entered and left with hysteresis so the rate does not flap between the pacer_wait
 * slices and psx_run_frame (each frame adds ~737 frames at once). Only issues
 * ndspChnSetRate when the target changes.
 */
static void update_ndsp_rate(PsxSystem *sys, u32 queued) {
    if (!sys) {
        return;
    }
    if (g_rate_mode > 0) {
        if (queued <= CTR_NDSP_FAST_EXIT) {
            g_rate_mode = 0;
        }
    } else if (g_rate_mode < 0) {
        if (queued >= CTR_NDSP_SLOW_EXIT) {
            g_rate_mode = 0;
        }
    }
    if (g_rate_mode == 0) {
        if (queued > CTR_NDSP_FAST_ENTER) {
            g_rate_mode = 1;
        } else if (queued < CTR_NDSP_SLOW_ENTER) {
            g_rate_mode = -1;
        }
    }
    bool  pal  = (sys->gpu.disp.pal != 0u); /* the same flag pacer_wait() paces by */
    float rate = pal ? CTR_NDSP_RATE_PAL : CTR_NDSP_RATE_NTSC;
    if (g_rate_mode > 0) {
        rate = pal ? CTR_NDSP_RATE_PAL_FAST : CTR_NDSP_RATE_NTSC_FAST;
    } else if (g_rate_mode < 0) {
        rate = pal ? CTR_NDSP_RATE_PAL_SLOW : CTR_NDSP_RATE_NTSC_SLOW;
    }
    if (rate != g_ndsp_rate) {
        g_ndsp_rate = rate;
        ndspChnSetRate(CTR_NDSP_CH, rate);
    }
}

static void audio_pump_locked(PsxSystem *sys) {
    if (sys && sys->spu.async_mode && !sys->spu.irq_enabled && spu_cmd_backlog(sys) > 0u) {
        if (queued_frames_for(sys) < CTR_NDSP_PRIME_FRAMES) {
            /*
             * v0.2.8: render 100% of the audio Core 0 already emulated (blocking on the
             * SPU lock if Core 1 is mid-chunk) before deciding whether a full wavebuf is
             * available — a trylock miss used to leave part of the frame in cmd_ring.
             */
            spu_drain_all_cmds_sync(sys);
        } else if (spu_cmd_backlog(sys) >= CTR_SPU_PRESSURE_CMDS) {
            /* Core 0 fallback render in case Core 1 is throttled by the SysCore quota */
            ctr_audio_render_core1(sys);
        }
    }

    u64 now_us = plat_time_us();
    u32 active_wbufs = 0u;
    for (u32 k = 0u; k < CTR_NDSP_BUF_COUNT; k++) {
        if (g_wbufs[k].status == NDSP_WBUF_PLAYING || g_wbufs[k].status == NDSP_WBUF_QUEUED) {
            active_wbufs++;
        }
    }

    if (active_wbufs == 0u) {
        /*
         * Genuine hardware underrun: NDSP fully drained. Count ONE underrun per
         * active->drained transition, then re-prime with 3 full buffers (50 ms) before
         * restarting. The channel is re-armed with ndspChnWaveBufClear() first, so libctru
         * restarts the DSP channel instead of appending to a stopped one.
         */
        if (g_stream_active) {
            g_stream_active = false;
            if (sys) {
                __atomic_fetch_add(&sys->stats.spu_underruns, 1u, __ATOMIC_RELAXED);
            }
        }
        if (queued_frames_for(sys) < CTR_NDSP_PRIME_FRAMES) {
            g_wb_active = 0u;
            return;
        }
        reset_wavebufs_for_restart();
        g_stream_active = true;
        g_last_wb_progress_us = now_us;
    } else {
        u64 since = now_us - g_last_wb_progress_us;
        if (since > CTR_NDSP_STALL_US ||
            (!ndspChnIsPlaying(CTR_NDSP_CH) && since > CTR_NDSP_STOPPED_US)) {
            /* Wavebufs stuck QUEUED/PLAYING on a stopped DSP channel: hard reset */
            plat_log("[audio_ndsp] NDSP stall (active=%u playing=%d %ums) - channel reset\n",
                     (unsigned)active_wbufs, (int)ndspChnIsPlaying(CTR_NDSP_CH),
                     (unsigned)(since / 1000ULL));
            configure_ndsp_channel_locked();
            active_wbufs = 0u;
            if (sys) {
                __atomic_fetch_add(&sys->stats.spu_underruns, 1u, __ATOMIC_RELAXED);
            }
            if (queued_frames_for(sys) < CTR_NDSP_PRIME_FRAMES) {
                g_wb_active = 0u;
                return;
            }
            g_stream_active = true;
            g_last_wb_progress_us = now_us;
        }
    }

    if (g_stream_active) {
        update_ndsp_rate(sys, total_queued_frames(sys, active_wbufs));
    }

    /*
     * Refill wavebufs in strict FIFO order (Core 0 only), FULL buffers only. While at
     * least one wavebuf is still playing there is no reason to pad: the next
     * psx_run_frame() supplies ~737 more frames well before NDSP needs them.
     */
    while (g_wbufs[g_wbuf_next].status == NDSP_WBUF_DONE ||
           g_wbufs[g_wbuf_next].status == NDSP_WBUF_FREE) {
        u32          avail = queued_frames_for(sys);
        ndspWaveBuf *wb    = &g_wbufs[g_wbuf_next];
        s16         *dst   = (s16 *)wb->data_vaddr;
        u32          got;
        if (avail < CTR_NDSP_BUF_FRAMES) {
            break; /* wait for the next frame's samples — never splice silence */
        }
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

        u32 buf_peak = 0u;
        for (u32 i = 0u; i < got * 2u; i++) {
            s32 smp = dst[i];
            u32 a = (u32)(smp < 0 ? -smp : smp);
            if (a > buf_peak) {
                buf_peak = a;
            }
        }
        g_pcm_peak = (buf_peak >= g_pcm_peak) ? buf_peak : ((g_pcm_peak * 15u + buf_peak) >> 4);

        /* Clean Core 0's L1/L2 D-cache to FCRAM before the DSP reads the buffer */
        (void)svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)dst, got * 4u);
        GSPGPU_FlushDataCache(dst, got * 4u);

        wb->nsamples = got;
        ndspChnWaveBufAdd(CTR_NDSP_CH, wb);
        g_wbuf_next = (g_wbuf_next + 1u) % CTR_NDSP_BUF_COUNT;
        active_wbufs++;
        g_wb_submitted++;
        g_last_wb_progress_us = now_us;

        if (!g_logged_first_audio && buf_peak > 16u) {
            g_logged_first_audio = true;
            plat_log("[audio_ndsp] audio active: wb=%u active=%u peak=%u spucnt=0x%04X\n",
                     (unsigned)g_wb_submitted, (unsigned)active_wbufs, (unsigned)buf_peak,
                     sys ? (unsigned)sys->spu.spucnt : 0u);
        }
    }
    g_wb_active = active_wbufs;
}

/*
 * Core 0 NDSP pump (historical name kept for all callers): main loop after psx_run_frame,
 * every <= 4 ms slice of pacer_wait, and the autorun loop.
 */
void ctr_audio_step_core1(PsxSystem *sys) {
    if (!g_ndsp_ready) {
        return;
    }
    if (__atomic_exchange_n(&g_pump_lock, 1u, __ATOMIC_ACQUIRE) != 0u) {
        return;
    }
    if (!sys) {
        sys = __atomic_load_n(&g_bound_sys, __ATOMIC_ACQUIRE);
    }
    audio_pump_locked(sys);
    __atomic_store_n(&g_pump_lock, 0u, __ATOMIC_RELEASE);
}

void ctr_audio_pump_core0(PsxSystem *sys) {
    ctr_audio_step_core1(sys);
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
    g_stream_active = false;
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
