#ifndef PSX3DS_STATS_H
#define PSX3DS_STATS_H

#include "psx3ds/types.h"

/*
 * Telemetry counters (ARCHITECTURE.md §3.8).
 * Per-frame counters are zeroed by psx_stats_frame_begin(); cumulative
 * counters (cycles_total, insns_total, frames, jit_blocks_live, jit_code_bytes,
 * jit_flushes, jit_mem_mode) persist across frames.
 */
typedef struct PsxStats {
    u64 cycles_total;
    u64 insns_total;
    u32 frames;

    u32 jit_blocks_live;
    u32 jit_blocks_compiled_frame;
    u32 jit_code_bytes;
    u32 jit_flushes;
    u32 jit_mem_mode;

    u32 ibtc_hits;
    u32 ibtc_misses;
    u32 dispatch_slow;
    u32 slow_reads;
    u32 slow_writes;
    u32 smc_traps;
    u32 smc_invalidations;
    u32 step_one_fallbacks;

    u32 gte_inline;
    u32 gte_helper;
    u32 idle_skips;
    u32 exceptions;
    u32 irqs;

    u32 gpu_prims;
    u32 gpu_batches;
    u32 gpu_tex_uploads;
    u32 gpu_tex_hits;
    u32 gpu_readbacks;

    u32 spu_batches;
    u32 spu_underruns;
    u32 cd_ra_hits;
    u32 cd_ra_misses;
    u32 events_fired[16];

    u32 us_cpu;
    u32 us_gpu;
    u32 us_present;
    u32 us_events;
    u32 us_frame;
} PsxStats;

void psx_stats_reset(PsxStats *s);
void psx_stats_frame_begin(PsxStats *s);

#endif /* PSX3DS_STATS_H */
