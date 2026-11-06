#include "psx3ds/system.h"

#include <stdio.h>
#include <string.h>

void psx_stats_reset(PsxStats *s) {
    if (!s) {
        return;
    }
    memset(s, 0, sizeof(*s));
}

void psx_stats_frame_begin(PsxStats *s) {
    if (!s) {
        return;
    }
    u64 cycles_total    = s->cycles_total;
    u64 insns_total     = s->insns_total;
    u32 frames          = s->frames;
    u32 jit_blocks_live = s->jit_blocks_live;
    u32 jit_code_bytes  = s->jit_code_bytes;
    u32 jit_flushes     = s->jit_flushes;
    u32 jit_mem_mode    = s->jit_mem_mode;

    memset(s, 0, sizeof(*s));

    s->cycles_total    = cycles_total;
    s->insns_total     = insns_total;
    s->frames          = frames;
    s->jit_blocks_live = jit_blocks_live;
    s->jit_code_bytes  = jit_code_bytes;
    s->jit_flushes     = jit_flushes;
    s->jit_mem_mode    = jit_mem_mode;
}

void psx_stats_accumulate(PsxStats *accum, const PsxStats *frame_stats) {
    if (!accum || !frame_stats) {
        return;
    }
    accum->cycles_total    = frame_stats->cycles_total;
    accum->insns_total     = frame_stats->insns_total;
    accum->frames          = frame_stats->frames;
    accum->jit_blocks_live = frame_stats->jit_blocks_live;
    accum->jit_code_bytes  = frame_stats->jit_code_bytes;
    accum->jit_flushes     = frame_stats->jit_flushes;
    accum->jit_mem_mode    = frame_stats->jit_mem_mode;

    accum->jit_blocks_compiled_frame += frame_stats->jit_blocks_compiled_frame;
    accum->ibtc_hits          += frame_stats->ibtc_hits;
    accum->ibtc_misses        += frame_stats->ibtc_misses;
    accum->dispatch_slow      += frame_stats->dispatch_slow;
    accum->slow_reads         += frame_stats->slow_reads;
    accum->slow_writes        += frame_stats->slow_writes;
    accum->smc_traps          += frame_stats->smc_traps;
    accum->smc_invalidations  += frame_stats->smc_invalidations;
    accum->step_one_fallbacks += frame_stats->step_one_fallbacks;

    accum->gte_inline += frame_stats->gte_inline;
    accum->gte_helper += frame_stats->gte_helper;
    accum->idle_skips += frame_stats->idle_skips;
    accum->exceptions += frame_stats->exceptions;
    accum->irqs       += frame_stats->irqs;

    accum->gpu_prims       += frame_stats->gpu_prims;
    accum->gpu_batches     += frame_stats->gpu_batches;
    accum->gpu_tex_uploads += frame_stats->gpu_tex_uploads;
    accum->gpu_tex_hits    += frame_stats->gpu_tex_hits;
    accum->gpu_readbacks   += frame_stats->gpu_readbacks;

    accum->spu_batches   += frame_stats->spu_batches;
    accum->spu_underruns += frame_stats->spu_underruns;
    accum->cd_ra_hits    += frame_stats->cd_ra_hits;
    accum->cd_ra_misses  += frame_stats->cd_ra_misses;

    for (u32 i = 0; i < 16u; i++) {
        accum->events_fired[i] += frame_stats->events_fired[i];
    }

    accum->us_cpu     += frame_stats->us_cpu;
    accum->us_gpu     += frame_stats->us_gpu;
    accum->us_present += frame_stats->us_present;
    accum->us_events  += frame_stats->us_events;
    accum->us_frame   += frame_stats->us_frame;
}

static const char *cpu_engine_str(u32 mode) {
    switch ((PsxCpuEngine)mode) {
        case CPU_INTERP: return "interp";
        case CPU_CACHED: return "cached";
        case CPU_JIT:    return "jit";
        default:         return "auto";
    }
}

static const char *jit_mem_mode_str(u32 mode) {
    switch ((JitMemMode)mode) {
        case JIT_MEM_MIRROR_RWRX: return "mirror_rwrx";
        case JIT_MEM_RWX_INPLACE: return "rwx_inplace";
        case JIT_MEM_HOST_MMAP:   return "host_mmap";
        case JIT_MEM_NONE:
        default:                  return "none";
    }
}

static void json_escape_str(const char *in, char *out, size_t out_cap) {
    if (out_cap == 0u) {
        return;
    }
    size_t j = 0u;
    if (in) {
        for (size_t i = 0u; in[i] != '\0' && j + 2u < out_cap; i++) {
            char c = in[i];
            if (c == '"' || c == '\\') {
                if (j + 3u >= out_cap) {
                    break;
                }
                out[j++] = '\\';
                out[j++] = c;
            } else if ((unsigned char)c >= 0x20u) {
                out[j++] = c;
            }
        }
    }
    out[j] = '\0';
}

u32 psx_stats_format_json(const PsxSystem *sys, const PsxStats *accum,
                          const char *rom_name, u64 wall_us, u32 frames_ran,
                          char *buf, u32 cap) {
    if (!sys || !buf || cap == 0u) {
        return 0u;
    }
    const PsxStats *st = accum ? accum : &sys->stats;
    u32 f = frames_ran > 0u ? frames_ran : (st->frames > 0u ? st->frames : 1u);
    double wall_sec = (wall_us > 0u) ? ((double)wall_us / 1000000.0) : 0.000001;
    double emu_fps = (double)frames_ran / wall_sec;
    double host_ms_per_frame = (wall_us > 0u && frames_ran > 0u)
                                   ? ((double)wall_us / 1000.0 / (double)frames_ran)
                                   : 0.0;
    double speed_pct = (emu_fps / 59.826) * 100.0;

    u64 insns = st->insns_total > 0u ? st->insns_total
                                     : ((st->cycles_total * 256ULL) /
                                        (sys->cfg.cycle_mult ? sys->cfg.cycle_mult : 448u));
    double mips = ((double)insns / 1000000.0) / wall_sec;

    u64 ibtc_tot = (u64)st->ibtc_hits + (u64)st->ibtc_misses;
    double ibtc_hit_pct = ibtc_tot > 0u ? (100.0 * (double)st->ibtc_hits / (double)ibtc_tot) : 0.0;

    u64 gte_tot = (u64)st->gte_inline + (u64)st->gte_helper;
    double gte_inline_pct = gte_tot > 0u ? (100.0 * (double)st->gte_inline / (double)gte_tot) : 0.0;

    u64 tex_tot = (u64)st->gpu_tex_hits + (u64)st->gpu_tex_uploads;
    double tex_hit_pct = tex_tot > 0u ? (100.0 * (double)st->gpu_tex_hits / (double)tex_tot) : 0.0;

    char rom_esc[256];
    json_escape_str(rom_name ? rom_name : "", rom_esc, sizeof(rom_esc));

    const char *gpu_name = (sys->cfg.gpu_backend == GPU_BACKEND_HW) ? "hw" : "soft";
    if (sys->gpu.be && sys->gpu.be->name && sys->gpu.be->name[0] != '\0') {
        gpu_name = sys->gpu.be->name;
    }
    char gpu_esc[64];
    json_escape_str(gpu_name, gpu_esc, sizeof(gpu_esc));

    int n = snprintf(
        buf, cap,
        "{\n"
        "  \"rom\": \"%s\",\n"
        "  \"cpu_engine\": \"%s\",\n"
        "  \"gpu_backend\": \"%s\",\n"
        "  \"jit_mem_mode\": \"%s\",\n"
        "  \"frames\": %u,\n"
        "  \"wall_ms\": %.3f,\n"
        "  \"emu_fps\": %.2f,\n"
        "  \"speed_pct\": %.2f,\n"
        "  \"host_ms_per_frame\": %.3f,\n"
        "  \"cycles_total\": %llu,\n"
        "  \"cycles_per_frame\": %llu,\n"
        "  \"insns_total\": %llu,\n"
        "  \"mips_insns_per_frame\": %llu,\n"
        "  \"host_mips\": %.2f,\n"
        "  \"blocks_live\": %u,\n"
        "  \"blocks_compiled\": %u,\n"
        "  \"code_kib\": %.2f,\n"
        "  \"jit_flushes\": %u,\n"
        "  \"ibtc_hits\": %u,\n"
        "  \"ibtc_misses\": %u,\n"
        "  \"ibtc_hit_pct\": %.2f,\n"
        "  \"slow_rd_per_frame\": %.2f,\n"
        "  \"slow_wr_per_frame\": %.2f,\n"
        "  \"smc_traps\": %u,\n"
        "  \"smc_invalidations\": %u,\n"
        "  \"idle_skips\": %u,\n"
        "  \"gte_inline\": %u,\n"
        "  \"gte_helper\": %u,\n"
        "  \"gte_inline_pct\": %.2f,\n"
        "  \"gpu_prims\": %u,\n"
        "  \"gpu_batches\": %u,\n"
        "  \"gpu_tex_uploads\": %u,\n"
        "  \"gpu_tex_hits\": %u,\n"
        "  \"gpu_tex_hit_pct\": %.2f,\n"
        "  \"spu_batches\": %u,\n"
        "  \"spu_underruns\": %u,\n"
        "  \"cd_ra_hits\": %u,\n"
        "  \"cd_ra_misses\": %u,\n"
        "  \"breakdown_us\": {\n"
        "    \"cpu\": %u,\n"
        "    \"gpu\": %u,\n"
        "    \"present\": %u,\n"
        "    \"events\": %u,\n"
        "    \"frame\": %u\n"
        "  }\n"
        "}",
        rom_esc,
        cpu_engine_str(sys->cpu_mode),
        gpu_esc,
        jit_mem_mode_str(st->jit_mem_mode),
        (unsigned)frames_ran,
        (double)wall_us / 1000.0,
        emu_fps,
        speed_pct,
        host_ms_per_frame,
        (unsigned long long)st->cycles_total,
        (unsigned long long)(st->cycles_total / f),
        (unsigned long long)insns,
        (unsigned long long)(insns / f),
        mips,
        (unsigned)st->jit_blocks_live,
        (unsigned)st->jit_blocks_compiled_frame,
        (double)st->jit_code_bytes / 1024.0,
        (unsigned)st->jit_flushes,
        (unsigned)st->ibtc_hits,
        (unsigned)st->ibtc_misses,
        ibtc_hit_pct,
        (double)st->slow_reads / (double)f,
        (double)st->slow_writes / (double)f,
        (unsigned)st->smc_traps,
        (unsigned)st->smc_invalidations,
        (unsigned)st->idle_skips,
        (unsigned)st->gte_inline,
        (unsigned)st->gte_helper,
        gte_inline_pct,
        (unsigned)st->gpu_prims,
        (unsigned)st->gpu_batches,
        (unsigned)st->gpu_tex_uploads,
        (unsigned)st->gpu_tex_hits,
        tex_hit_pct,
        (unsigned)st->spu_batches,
        (unsigned)st->spu_underruns,
        (unsigned)st->cd_ra_hits,
        (unsigned)st->cd_ra_misses,
        (unsigned)st->us_cpu,
        (unsigned)st->us_gpu,
        (unsigned)st->us_present,
        (unsigned)st->us_events,
        (unsigned)st->us_frame);

    if (n < 0) {
        return 0u;
    }
    return ((u32)n < cap) ? (u32)n : (cap - 1u);
}
