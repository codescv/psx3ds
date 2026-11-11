#include "psx3ds/timers.h"
#include "psx3ds/system.h"

#include <string.h>

#define TMR_MODE_SYNC_EN      (1u << 0)
#define TMR_MODE_SYNC_MASK    (3u << 1)
#define TMR_MODE_RESET_TGT    (1u << 3)
#define TMR_MODE_IRQ_TGT      (1u << 4)
#define TMR_MODE_IRQ_FFFF     (1u << 5)
#define TMR_MODE_IRQ_REPEAT   (1u << 6)
#define TMR_MODE_IRQ_TOGGLE   (1u << 7)
#define TMR_MODE_CLK_MASK     (3u << 8)
#define TMR_MODE_IRQ_N        (1u << 10) /* active-low: 1 = no IRQ, 0 = IRQ active */

/* Internal per-timer latch stored above architectural 16-bit MODE bits */
#define TMR_INTERNAL_FIRED    (1u << 16)

/* Internal HBlank phase flag stored in bit 16 of line_frac_q16 (low 16 bits are Q16 frac) */
#define TMR_LINE_IN_HBLANK    (1u << 16)

PSX_INLINE u32 tmr_width_mask(u32 width) {
    if (width == 1u) {
        return 0xFFu;
    }
    if (width == 2u) {
        return 0xFFFFu;
    }
    return 0xFFFFFFFFu;
}

static bool timers_is_pal(const PsxSystem *sys) {
    return (sys->cfg.region == PSX_REGION_PAL) || (sys->gpu.disp.pal != 0u);
}

static u32 timers_dot_divider(const PsxSystem *sys) {
    u16 hres = sys->gpu.disp.hres;
    if (hres == 256u) return 10u;
    if (hres == 320u) return 8u;
    if (hres == 368u) return 7u;
    if (hres == 512u) return 5u;
    if (hres == 640u) return 4u;

    u32 gpustat = sys->gpu.gpustat;
    if ((gpustat & (1u << 16)) != 0u) {
        return 7u;
    }
    switch ((gpustat >> 17) & 3u) {
        case 0u: return 10u;
        case 1u: return 8u;
        case 2u: return 5u;
        default: return 4u;
    }
}

static void timer_refresh_rate(PsxSystem *sys, u32 idx) {
    u32 src = (sys->timers.t[idx].mode >> 8) & 3u;
    if (idx == 0u) {
        if (src == 1u || src == 3u) {
            u32 vclk_q16 = timers_is_pal(sys) ? PSX_GPU_CLK_RATIO_PAL_Q16
                                              : PSX_GPU_CLK_RATIO_NTSC_Q16;
            u32 div = timers_dot_divider(sys);
            sys->timers.t[idx].rate_num = vclk_q16;
            sys->timers.t[idx].rate_den = div << 16;
        } else {
            sys->timers.t[idx].rate_num = 1u;
            sys->timers.t[idx].rate_den = 1u;
        }
    } else if (idx == 1u) {
        if (src == 1u || src == 3u) {
            /* HBlank clock source: stepped explicitly in EV_HBLANK */
            sys->timers.t[idx].rate_num = 0u;
            sys->timers.t[idx].rate_den = 1u;
        } else {
            sys->timers.t[idx].rate_num = 1u;
            sys->timers.t[idx].rate_den = 1u;
        }
    } else {
        if (src == 2u || src == 3u) {
            sys->timers.t[idx].rate_num = 1u;
            sys->timers.t[idx].rate_den = 8u;
        } else {
            sys->timers.t[idx].rate_num = 1u;
            sys->timers.t[idx].rate_den = 1u;
        }
    }
}

/*
 * Closed-form evaluation of `ticks` counter increments starting from `b` (0..0xFFFF).
 * Returns resulting 16-bit counter value and outputs target / 0xFFFF hit counts.
 */
static u32 timer_eval_ticks(u32 b, u32 tgt, bool rst_tgt, u64 ticks,
                            u64 *out_hits_tgt, u64 *out_hits_ffff) {
    b &= 0xFFFFu;
    tgt &= 0xFFFFu;
    if (ticks == 0ULL) {
        *out_hits_tgt = 0ULL;
        *out_hits_ffff = 0ULL;
        return b;
    }

    if (!rst_tgt) {
        u32 dist_tgt = (tgt > b) ? (tgt - b) : (0x10000u - b + tgt);
        u32 dist_ffff = 0x10000u - b;
        *out_hits_tgt = (ticks >= (u64)dist_tgt)
                            ? (1ULL + (ticks - (u64)dist_tgt) / 0x10000ULL)
                            : 0ULL;
        *out_hits_ffff = (ticks >= (u64)dist_ffff)
                             ? (1ULL + (ticks - (u64)dist_ffff) / 0x10000ULL)
                             : 0ULL;
        return (u32)((b + ticks) & 0xFFFFu);
    }

    u32 period = tgt + 1u; /* Reset occurs AFTER reaching target (period = target + 1) */
    if (b <= tgt) {
        u32 dist_tgt = (tgt > b) ? (tgt - b) : period;
        *out_hits_tgt = (ticks >= (u64)dist_tgt)
                            ? (1ULL + (ticks - (u64)dist_tgt) / (u64)period)
                            : 0ULL;
        if (tgt == 0xFFFFu) {
            u32 dist_ffff = 0x10000u - b;
            *out_hits_ffff = (ticks >= (u64)dist_ffff)
                                 ? (1ULL + (ticks - (u64)dist_ffff) / 0x10000ULL)
                                 : 0ULL;
        } else {
            *out_hits_ffff = 0ULL;
        }
        return (u32)((b + ticks) % (u64)period);
    }

    /* b > tgt: wraps past 0xFFFF -> 0x0000 first, then cycles modulo (tgt + 1) */
    u64 to_zero = (u64)(0x10000u - b);
    if (ticks < to_zero) {
        *out_hits_tgt = 0ULL;
        *out_hits_ffff = 0ULL;
        return b + (u32)ticks;
    }
    u64 rem = ticks - to_zero;
    *out_hits_ffff = 1ULL;
    *out_hits_tgt = (rem >= (u64)tgt) ? (1ULL + (rem - (u64)tgt) / (u64)period) : 0ULL;
    return (u32)(rem % (u64)period);
}

static void timer_apply_hits(PsxSystem *sys, u32 idx, u64 hits_tgt, u64 hits_ffff) {
    u32 mode = sys->timers.t[idx].mode;
    if (hits_tgt > 0ULL) {
        sys->timers.t[idx].reached_target = 1u;
    }
    if (hits_ffff > 0ULL) {
        sys->timers.t[idx].reached_ffff = 1u;
    }

    u64 trig = 0ULL;
    bool irq_tgt = (mode & TMR_MODE_IRQ_TGT) != 0u;
    bool irq_ffff = (mode & TMR_MODE_IRQ_FFFF) != 0u;
    if (irq_tgt) {
        trig += hits_tgt;
    }
    if (irq_ffff) {
        if (!(irq_tgt && (sys->timers.t[idx].target & 0xFFFFu) == 0xFFFFu &&
              (mode & TMR_MODE_RESET_TGT) != 0u)) {
            trig += hits_ffff;
        }
    }
    if (trig == 0ULL) {
        return;
    }

    bool repeat = (mode & TMR_MODE_IRQ_REPEAT) != 0u;
    if (!repeat && (mode & TMR_INTERNAL_FIRED) != 0u) {
        return;
    }
    if (!repeat) {
        trig = 1ULL;
        mode |= TMR_INTERNAL_FIRED;
    }

    if ((mode & TMR_MODE_IRQ_TOGGLE) == 0u) {
        /* Pulse mode: bit 10 pulses 1 -> 0 -> 1 and raises IRQ */
        mode |= TMR_MODE_IRQ_N;
        sys->timers.t[idx].mode = mode;
        irq_raise(sys, PSX_IRQ_TMR0 + idx);
    } else {
        /* Toggle mode: bit 10 inverts each condition; IRQ fires on 1 -> 0 edge */
        bool was_high = (mode & TMR_MODE_IRQ_N) != 0u;
        if ((trig & 1ULL) != 0ULL) {
            mode ^= TMR_MODE_IRQ_N;
        }
        sys->timers.t[idx].mode = mode;
        if ((was_high && trig >= 1ULL) || (!was_high && trig >= 2ULL)) {
            irq_raise(sys, PSX_IRQ_TMR0 + idx);
        }
    }
}

/*
 * Computes current counter value at `now`. If `reanchor` is true (or elapsed CPU ticks
 * are large, or an IRQ/flag transition occurs that needs committing), commits `base_value`
 * and advances `base_tick` by exact whole timer ticks so sub-tick phase is never lost.
 */
static u16 timer_sync(PsxSystem *sys, u32 idx, psx_tick_t now, bool force_reanchor) {
    timer_refresh_rate(sys, idx);
    u32 rate_num = sys->timers.t[idx].rate_num;
    u32 rate_den = sys->timers.t[idx].rate_den;

    if (sys->timers.t[idx].gated != 0u || rate_num == 0u) {
        if (force_reanchor) {
            sys->timers.t[idx].base_tick = now;
        }
        return (u16)(sys->timers.t[idx].base_value & 0xFFFFu);
    }

    u32 delta_cpu = (u32)(now - sys->timers.t[idx].base_tick);
    u64 ticks = ((u64)delta_cpu * (u64)rate_num) / (u64)rate_den;
    if (ticks == 0ULL) {
        return (u16)(sys->timers.t[idx].base_value & 0xFFFFu);
    }

    u64 hits_tgt = 0ULL;
    u64 hits_ffff = 0ULL;
    bool rst_tgt = (sys->timers.t[idx].mode & TMR_MODE_RESET_TGT) != 0u;
    u32 cur = timer_eval_ticks(sys->timers.t[idx].base_value,
                               sys->timers.t[idx].target,
                               rst_tgt, ticks, &hits_tgt, &hits_ffff);

    if (force_reanchor || hits_tgt > 0ULL || hits_ffff > 0ULL || delta_cpu >= 0x20000000u) {
        timer_apply_hits(sys, idx, hits_tgt, hits_ffff);
        sys->timers.t[idx].base_value = cur;
        if (rate_num == 1u) {
            /* Exact remainder preservation for sysclk (1/1) and sysclk/8 (1/8) */
            sys->timers.t[idx].base_tick += (u32)(ticks * (u64)rate_den);
        } else {
            u64 consumed = ((ticks * (u64)rate_den) + (u64)rate_num - 1ULL) / (u64)rate_num;
            if (consumed > (u64)delta_cpu) {
                consumed = (u64)delta_cpu;
            }
            sys->timers.t[idx].base_tick += (u32)consumed;
        }
    }

    return (u16)cur;
}

static u32 timer_ticks_to_next_irq(const PsxSystem *sys, u32 idx, u32 cur_val) {
    u32 mode = sys->timers.t[idx].mode;
    bool irq_tgt = (mode & TMR_MODE_IRQ_TGT) != 0u;
    bool irq_ffff = (mode & TMR_MODE_IRQ_FFFF) != 0u;
    if (!irq_tgt && !irq_ffff) {
        return 0u;
    }
    if ((mode & TMR_MODE_IRQ_REPEAT) == 0u && (mode & TMR_INTERNAL_FIRED) != 0u) {
        return 0u;
    }

    u32 b = cur_val & 0xFFFFu;
    u32 tgt = sys->timers.t[idx].target & 0xFFFFu;
    bool rst_tgt = (mode & TMR_MODE_RESET_TGT) != 0u;
    u32 best = UINT32_MAX;

    if (!rst_tgt) {
        if (irq_tgt) {
            u32 d = (tgt > b) ? (tgt - b) : (0x10000u - b + tgt);
            if (d < best) best = d;
        }
        if (irq_ffff) {
            u32 d = 0x10000u - b;
            if (d < best) best = d;
        }
    } else {
        u32 period = tgt + 1u;
        if (b <= tgt) {
            if (irq_tgt) {
                u32 d = (tgt > b) ? (tgt - b) : period;
                if (d < best) best = d;
            }
            if (irq_ffff && tgt == 0xFFFFu) {
                u32 d = 0x10000u - b;
                if (d < best) best = d;
            }
        } else {
            u32 to_zero = 0x10000u - b;
            if (irq_ffff && to_zero < best) {
                best = to_zero;
            }
            if (irq_tgt && (to_zero + tgt) < best) {
                best = to_zero + tgt;
            }
        }
    }

    return (best == UINT32_MAX) ? 0u : best;
}

static void timer_reschedule(PsxSystem *sys, u32 idx, psx_tick_t now) {
    PsxEventId ev = (PsxEventId)(EV_TIMER0 + idx);
    timer_refresh_rate(sys, idx);
    u32 rate_num = sys->timers.t[idx].rate_num;
    u32 rate_den = sys->timers.t[idx].rate_den;

    if (sys->timers.t[idx].gated != 0u || rate_num == 0u) {
        sched_cancel(sys, ev);
        return;
    }

    u16 cur = timer_sync(sys, idx, now, false);
    u32 next_ticks = timer_ticks_to_next_irq(sys, idx, cur);
    if (next_ticks == 0u) {
        sched_cancel(sys, ev);
        return;
    }

    u32 delta_cpu = (u32)(now - sys->timers.t[idx].base_tick);
    u64 cur_ticks = ((u64)delta_cpu * (u64)rate_num) / (u64)rate_den;
    u64 target_ticks = cur_ticks + (u64)next_ticks;
    u64 due_cpu = ((target_ticks * (u64)rate_den) + (u64)rate_num - 1ULL) / (u64)rate_num;
    u32 rem_cycles = (due_cpu > (u64)delta_cpu) ? (u32)(due_cpu - (u64)delta_cpu) : 1u;
    if (rem_cycles == 0u) {
        rem_cycles = 1u;
    }
    sched_schedule(sys, ev, rem_cycles);
}

static void timer_step_explicit_one(PsxSystem *sys, u32 idx) {
    u64 hits_tgt = 0ULL;
    u64 hits_ffff = 0ULL;
    bool rst_tgt = (sys->timers.t[idx].mode & TMR_MODE_RESET_TGT) != 0u;
    u32 cur = timer_eval_ticks(sys->timers.t[idx].base_value,
                               sys->timers.t[idx].target,
                               rst_tgt, 1ULL, &hits_tgt, &hits_ffff);
    sys->timers.t[idx].base_value = cur;
    timer_apply_hits(sys, idx, hits_tgt, hits_ffff);
}

static void timer_apply_initial_sync_gate(PsxSystem *sys, u32 idx, psx_tick_t now) {
    u32 mode = sys->timers.t[idx].mode;
    if ((mode & TMR_MODE_SYNC_EN) == 0u) {
        sys->timers.t[idx].gated = 0u;
        return;
    }
    u32 sync_mode = (mode >> 1) & 3u;
    if (idx == 2u) {
        /* Timer 2: sync_mode 0 or 3 stops counter; 1 or 2 free-runs */
        sys->timers.t[idx].gated = (sync_mode == 0u || sync_mode == 3u) ? 1u : 0u;
    } else if (idx == 0u) {
        bool in_hb = (sys->timers.line_frac_q16 & TMR_LINE_IN_HBLANK) != 0u;
        if (sync_mode == 0u) sys->timers.t[0].gated = in_hb ? 1u : 0u;
        else if (sync_mode == 1u) sys->timers.t[0].gated = 0u;
        else if (sync_mode == 2u) sys->timers.t[0].gated = in_hb ? 0u : 1u;
        else sys->timers.t[0].gated = 1u; /* pause until next HBlank */
    } else {
        bool in_vb = sys->timers.in_vblank;
        if (sync_mode == 0u) sys->timers.t[1].gated = in_vb ? 1u : 0u;
        else if (sync_mode == 1u) sys->timers.t[1].gated = 0u;
        else if (sync_mode == 2u) sys->timers.t[1].gated = in_vb ? 0u : 1u;
        else sys->timers.t[1].gated = 1u; /* pause until next VBlank */
    }
    sys->timers.t[idx].base_tick = now;
}

static void timer_on_blank_edge(PsxSystem *sys, u32 idx, bool entering_blank, psx_tick_t now) {
    u32 mode = sys->timers.t[idx].mode;
    if ((mode & TMR_MODE_SYNC_EN) == 0u) {
        return;
    }
    u32 sync_mode = (mode >> 1) & 3u;
    (void)timer_sync(sys, idx, now, true);

    if (entering_blank) {
        if (sync_mode == 0u) {
            sys->timers.t[idx].gated = 1u;
            sys->timers.t[idx].base_tick = now;
        } else if (sync_mode == 1u) {
            sys->timers.t[idx].base_value = 0u;
            sys->timers.t[idx].gated = 0u;
            sys->timers.t[idx].base_tick = now;
        } else if (sync_mode == 2u) {
            sys->timers.t[idx].base_value = 0u;
            sys->timers.t[idx].gated = 0u;
            sys->timers.t[idx].base_tick = now;
        } else {
            /* sync_mode == 3: unpause permanently into free-run */
            sys->timers.t[idx].mode &= ~TMR_MODE_SYNC_EN;
            sys->timers.t[idx].gated = 0u;
            sys->timers.t[idx].base_tick = now;
        }
    } else {
        if (sync_mode == 0u) {
            sys->timers.t[idx].gated = 0u;
            sys->timers.t[idx].base_tick = now;
        } else if (sync_mode == 2u) {
            sys->timers.t[idx].gated = 1u;
            sys->timers.t[idx].base_tick = now;
        }
    }
    timer_reschedule(sys, idx, now);
}

static u32 timers_next_line_cycles(PsxSystem *sys) {
    bool pal = timers_is_pal(sys);
    u32 line_q16 = pal ? PSX_CYCLES_PER_LINE_PAL_Q16 : PSX_CYCLES_PER_LINE_NTSC_Q16;
    u32 frac = sys->timers.line_frac_q16 & 0xFFFFu;
    u32 hb_flag = sys->timers.line_frac_q16 & TMR_LINE_IN_HBLANK;
    u32 sum = frac + line_q16;
    sys->timers.line_frac_q16 = (sum & 0xFFFFu) | hb_flag;
    return sum >> 16;
}

/*
 * Standalone (GPU-less) GPUSTAT bit 31 approximation. When gpu_init() has hooked EV_HBLANK,
 * gpu_refresh_scanline_bits() recomputes bits 13/31 right after this, so both must agree:
 * bit 31 is 0 during VBlank, and in 480i (bit 22 interlace + bit 19 vres=480) it tracks
 * the field parity (frame & 1) during active video, otherwise the scanline parity.
 */
static void timers_update_gpustat_line(PsxSystem *sys) {
    u32 gs = sys->gpu.gpustat;
    bool interlace_480 = ((gs & (1u << 22)) != 0u) && ((gs & (1u << 19)) != 0u);
    u32 bit = 0u;
    if (!sys->timers.in_vblank) {
        bit = interlace_480 ? (sys->gpu.frame & 1u) : (sys->timers.scanline & 1u);
    }
    if (bit != 0u) sys->gpu.gpustat |= (1u << 31);
    else sys->gpu.gpustat &= ~(1u << 31);
}

/*
 * Fallback EV_VBLANK handler when no GPU is attached. Frame statistics are counted once
 * per psx_run_frame() (src/sys/system.c), never here, so a GPU chaining to this callback
 * from gpu_on_vblank() does not double-count frames.
 */
static void timers_vblank_default_cb(PsxSystem *sys, u32 late_cycles) {
    (void)late_cycles;
    irq_raise(sys, PSX_IRQ_VBLANK);
    sys->frame_done = true;
}

static void timers_hblank_cb(PsxSystem *sys, u32 late_cycles) {
    psx_tick_t now = sys->sched.now;
    bool split_t0 = ((sys->timers.t[0].mode & TMR_MODE_SYNC_EN) != 0u) &&
                    ((((sys->timers.t[0].mode >> 1) & 3u) == 0u) ||
                     (((sys->timers.t[0].mode >> 1) & 3u) == 2u));

    if (split_t0 && (sys->timers.line_frac_q16 & TMR_LINE_IN_HBLANK) == 0u) {
        /* Entering HBlank (last ~1/4 of current scanline) */
        sys->timers.line_frac_q16 |= TMR_LINE_IN_HBLANK;

        u32 t1_src = (sys->timers.t[1].mode >> 8) & 3u;
        if ((t1_src == 1u || t1_src == 3u) && sys->timers.t[1].gated == 0u) {
            timer_step_explicit_one(sys, 1u);
        }
        timer_on_blank_edge(sys, 0u, true, now);

        u32 line_cyc = timers_is_pal(sys) ? PSX_CYCLES_PER_LINE_PAL : PSX_CYCLES_PER_LINE_NTSC;
        u32 hb_cyc = line_cyc >> 2;
        u32 delay = (hb_cyc > late_cycles) ? (hb_cyc - late_cycles) : 1u;
        sched_schedule(sys, EV_HBLANK, delay);
        return;
    }

    bool was_in_hb = (sys->timers.line_frac_q16 & TMR_LINE_IN_HBLANK) != 0u;
    sys->timers.line_frac_q16 &= ~TMR_LINE_IN_HBLANK;

    if (!was_in_hb) {
        /* Single-event scanline path (normal 15.8 KHz O3DS fast path): HBlank pulse now */
        u32 t1_src = (sys->timers.t[1].mode >> 8) & 3u;
        if ((t1_src == 1u || t1_src == 3u) && sys->timers.t[1].gated == 0u) {
            timer_step_explicit_one(sys, 1u);
        }
        if ((sys->timers.t[0].mode & TMR_MODE_SYNC_EN) != 0u) {
            timer_on_blank_edge(sys, 0u, true, now);
        }
    } else {
        /* Exiting HBlank at start of new scanline */
        timer_on_blank_edge(sys, 0u, false, now);
    }

    bool pal = timers_is_pal(sys);
    u32 total_lines = pal ? PSX_LINES_PAL : PSX_LINES_NTSC;
    u32 vblank_line = pal ? PSX_VBLANK_LINE_PAL : PSX_VBLANK_LINE_NTSC;

    sys->timers.scanline++;
    if (sys->timers.scanline >= total_lines) {
        sys->timers.scanline = 0u;
        if (sys->timers.in_vblank) {
            sys->timers.in_vblank = false;
            timer_on_blank_edge(sys, 1u, false, now);
        }
    } else if (sys->timers.scanline == vblank_line) {
        sys->timers.in_vblank = true;
        timer_on_blank_edge(sys, 1u, true, now);
        PsxEventFn vb_fn = sys->sched.fn[EV_VBLANK];
        /*
         * The interlace field counter advances exactly once per frame at VBlank entry.
         * When the GPU owns EV_VBLANK, gpu_on_vblank() does it (before raising IRQ0);
         * only advance it here when no GPU handler is hooked. Advancing it both here and
         * in gpu_on_vblank() kept (frame & 1) constant during active video, freezing
         * GPUSTAT bit 31 in 480i and hanging Psy-Q VSync(0) forever.
         */
        if (vb_fn == NULL || vb_fn == timers_vblank_default_cb) {
            sys->gpu.frame++;
        }
        if ((sys->sched.active_mask & (1u << EV_VBLANK)) == 0u && vb_fn != NULL) {
            vb_fn(sys, late_cycles);
        }
    }

    timers_update_gpustat_line(sys);

    u32 full_line = timers_next_line_cycles(sys);
    bool split_next = ((sys->timers.t[0].mode & TMR_MODE_SYNC_EN) != 0u) &&
                      ((((sys->timers.t[0].mode >> 1) & 3u) == 0u) ||
                       (((sys->timers.t[0].mode >> 1) & 3u) == 2u));
    u32 target_cyc = split_next ? (full_line - (full_line >> 2)) : full_line;
    u32 delay = (target_cyc > late_cycles) ? (target_cyc - late_cycles) : 1u;
    sched_schedule(sys, EV_HBLANK, delay);
}

static void timer0_cb(PsxSystem *sys, u32 late) {
    (void)late;
    psx_tick_t now = sys->sched.now;
    (void)timer_sync(sys, 0u, now, true);
    timer_reschedule(sys, 0u, now);
}

static void timer1_cb(PsxSystem *sys, u32 late) {
    (void)late;
    psx_tick_t now = sys->sched.now;
    (void)timer_sync(sys, 1u, now, true);
    timer_reschedule(sys, 1u, now);
}

static void timer2_cb(PsxSystem *sys, u32 late) {
    (void)late;
    psx_tick_t now = sys->sched.now;
    (void)timer_sync(sys, 2u, now, true);
    timer_reschedule(sys, 2u, now);
}

u16 timers_read_counter(PsxSystem *sys, u32 idx) {
    if (!sys || idx >= 3u) {
        return 0u;
    }
    return timer_sync(sys, idx, sched_now(sys), false);
}

static u32 timers_mmio_read(PsxSystem *sys, u32 addr, u32 width) {
    u32 off = addr - PSX_REG_TIMERS_BASE;
    u32 idx = off >> 4;
    if (idx >= 3u) {
        return 0u;
    }
    u32 reg = (off >> 2) & 3u;
    u32 shift = (addr & 3u) * 8u;
    psx_tick_t now = sched_now(sys);
    u32 val = 0u;

    if (reg == 0u) {
        val = (u32)timer_sync(sys, idx, now, false);
    } else if (reg == 1u) {
        u16 cur = timer_sync(sys, idx, now, false);
        u32 flags = (sys->timers.t[idx].reached_target ? (1u << 11) : 0u) |
                    (sys->timers.t[idx].reached_ffff ? (1u << 12) : 0u);
        val = (sys->timers.t[idx].mode & 0x07FFu) | flags;
        /* Reading MODE clears reached_target & reached_ffff; re-anchor if flags were set */
        if (flags != 0u && ((addr & 3u) <= 1u) && ((addr & 3u) + width > 1u)) {
            sys->timers.t[idx].base_value = cur;
            if (sys->timers.t[idx].rate_num == 1u) {
                u32 d = (u32)(now - sys->timers.t[idx].base_tick);
                u32 den = sys->timers.t[idx].rate_den;
                sys->timers.t[idx].base_tick += (d / den) * den;
            } else {
                sys->timers.t[idx].base_tick = now;
            }
            sys->timers.t[idx].reached_target = 0u;
            sys->timers.t[idx].reached_ffff = 0u;
        }
    } else if (reg == 2u) {
        val = sys->timers.t[idx].target & 0xFFFFu;
    }

    return (val >> shift) & tmr_width_mask(width);
}

static void timers_mmio_write(PsxSystem *sys, u32 addr, u32 v, u32 width) {
    u32 off = addr - PSX_REG_TIMERS_BASE;
    u32 idx = off >> 4;
    if (idx >= 3u) {
        return;
    }
    u32 reg = (off >> 2) & 3u;
    u32 shift = (addr & 3u) * 8u;
    u32 wmask = tmr_width_mask(width) << shift;
    u32 wval = (v & tmr_width_mask(width)) << shift;
    psx_tick_t now = sched_now(sys);

    if (reg == 0u) {
        u16 cur = timer_sync(sys, idx, now, true);
        u32 next = ((u32)cur & ~wmask) | (wval & wmask);
        sys->timers.t[idx].base_value = next & 0xFFFFu;
        sys->timers.t[idx].base_tick = now;
        timer_reschedule(sys, idx, now);
    } else if (reg == 1u) {
        /* Latch any pending target/0xFFFF flags BEFORE resetting counter to 0 */
        (void)timer_sync(sys, idx, now, true);
        u32 prev_cfg = sys->timers.t[idx].mode & 0x03FFu;
        u32 next_cfg = (prev_cfg & ~wmask) | (wval & wmask);
        /* Writing MODE sets bit 10 = 1 (no IRQ), resets counter to 0, preserves bits 11/12 */
        sys->timers.t[idx].mode = (next_cfg & 0x03FFu) | TMR_MODE_IRQ_N;
        sys->timers.t[idx].base_value = 0u;
        sys->timers.t[idx].base_tick = now;
        timer_refresh_rate(sys, idx);
        timer_apply_initial_sync_gate(sys, idx, now);
        timer_reschedule(sys, idx, now);
    } else if (reg == 2u) {
        (void)timer_sync(sys, idx, now, true);
        u32 next = (sys->timers.t[idx].target & ~wmask) | (wval & wmask);
        sys->timers.t[idx].target = next & 0xFFFFu;
        timer_reschedule(sys, idx, now);
    }
}

void timers_init(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    bus_mmio_register(sys, PSX_REG_TIMERS_BASE, 0x30u, timers_mmio_read, timers_mmio_write);
    sched_set_handler(&sys->sched, EV_HBLANK, timers_hblank_cb);
    if (sys->sched.fn[EV_VBLANK] == NULL) {
        sched_set_handler(&sys->sched, EV_VBLANK, timers_vblank_default_cb);
    }
    sched_set_handler(&sys->sched, EV_TIMER0, timer0_cb);
    sched_set_handler(&sys->sched, EV_TIMER1, timer1_cb);
    sched_set_handler(&sys->sched, EV_TIMER2, timer2_cb);
    timers_reset(sys);
}

void timers_reset(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    PsxTimers *tm = &sys->timers;
    memset(tm, 0, sizeof(*tm));
    psx_tick_t now = sched_now(sys);
    for (u32 i = 0u; i < 3u; i++) {
        tm->t[i].mode = TMR_MODE_IRQ_N;
        tm->t[i].target = 0u;
        tm->t[i].base_value = 0u;
        tm->t[i].base_tick = now;
        tm->t[i].rate_num = 1u;
        tm->t[i].rate_den = 1u;
        tm->t[i].gated = 0u;
        tm->t[i].reached_target = 0u;
        tm->t[i].reached_ffff = 0u;
        sched_cancel(sys, (PsxEventId)(EV_TIMER0 + i));
    }
    u32 first_line = timers_next_line_cycles(sys);
    sched_schedule(sys, EV_HBLANK, first_line);
}

void timers_state(PsxSystem *sys, StateIO *st) {
    if (!sys || !st) {
        return;
    }
    PsxTimers *tm = &sys->timers;
    for (u32 i = 0u; i < 3u; i++) {
        state_u32(st, &tm->t[i].mode);
        state_u32(st, &tm->t[i].target);
        state_u32(st, &tm->t[i].base_value);
        state_u32(st, &tm->t[i].base_tick);
        state_u32(st, &tm->t[i].rate_num);
        state_u32(st, &tm->t[i].rate_den);
        state_u8(st, &tm->t[i].gated);
        state_u8(st, &tm->t[i].reached_target);
        state_u8(st, &tm->t[i].reached_ffff);
    }
    state_u32(st, &tm->scanline);
    state_u32(st, &tm->line_frac_q16);
    state_bool(st, &tm->in_vblank);
}

void timers_shutdown(PsxSystem *sys) {
    (void)sys;
}
