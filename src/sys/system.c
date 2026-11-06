#include "psx3ds/system.h"

#include <string.h>

#if PSX3DS_PLATFORM_3DS
#include <3ds.h>
extern void jit_mem_3ds_set_allow_hle_exec(bool allow);
#endif

#define PSX_ARENA_BYTES_O3DS     (26u * 1024u * 1024u)
#define PSX_ARENA_BYTES_O3DS_MIN (20u * 1024u * 1024u)
#define PSX_ARENA_BYTES_FULL     (41u * 1024u * 1024u)
#define PSX_ARENA_BYTES_MID      (32u * 1024u * 1024u)

void psx_config_defaults(PsxConfig *cfg, bool is_n3ds) {
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->cpu_engine = CPU_AUTO;
    cfg->gpu_backend = PSX3DS_HAVE_CITRO3D ? GPU_BACKEND_HW : GPU_BACKEND_SOFT;
    cfg->region = PSX_REGION_AUTO;
    cfg->cycle_mult = (u16)PSX3DS_CYCLE_MULT_DEFAULT;
    cfg->cycles_per_mem = 0u;
    cfg->frameskip = 0u;
    cfg->scale = (u8)PSX_SCALE_FIT_4_3;
    cfg->linear_filter = false;

    cfg->jit_ibtc = true;
    cfg->jit_link = true;
    cfg->jit_idle_skip = true;
    cfg->jit_check_align = true;
    cfg->jit_check_kuc = false;
    cfg->jit_inline_gte = true;
    cfg->jit_allow_hle_exec = true;
    cfg->isc_swap_table = false;

    cfg->strict_ram_size = false;
    cfg->gpu_timing = true;
    cfg->spu_async = (PSX3DS_PLATFORM_3DS != 0);
    cfg->spu_reverb = true;
#if defined(PSX3DS_FORCE_SMALL_MEM)
    cfg->small_mem = true;
#else
    cfg->small_mem = !is_n3ds && (PSX3DS_PLATFORM_3DS != 0);
#endif
    cfg->hle_bios = false;
    cfg->dither_hw = false;

    cfg->smc_trap_budget = UINT32_MAX;
    if (PSX3DS_PLATFORM_HOST) {
        cfg->jit_code_bytes = 32u * 1024u * 1024u;
        cfg->jit_max_blocks = 65536u;
    } else if (cfg->small_mem) {
        cfg->jit_code_bytes = 6u * 1024u * 1024u;
        cfg->jit_max_blocks = 32768u;
    } else {
        cfg->jit_code_bytes = 16u * 1024u * 1024u;
        cfg->jit_max_blocks = 65536u;
    }
    cfg->dma_ll_max_nodes_per_tick = 64u;
    cfg->dma_cycles_per_word = 1u;
}

static void psx_sync_cpu_pointers(PsxSystem *sys) {
    PsxCpu *c = &sys->cpu;
    c->sys.p = sys;
    if (!c->ram_base.p && sys->bus.ram) {
        c->ram_base.p = sys->bus.ram;
    }
    if (!c->scratch_base.p && sys->bus.scratch_page) {
        c->scratch_base.p = sys->bus.scratch_page;
    }
    if (!c->fastmem_r.p && sys->bus.fm_r) {
        c->fastmem_r.p = sys->bus.fm_r;
    }
    if (!c->fastmem_w.p && sys->bus.fm_w) {
        if (sys->cfg.isc_swap_table && sys->bus.isolated && sys->bus.fm_w_isc_null) {
            c->fastmem_w.p = sys->bus.fm_w_isc_null;
        } else {
            c->fastmem_w.p = sys->bus.fm_w;
        }
    }
    if (!c->ibtc.p && sys->jit.ibtc) {
        c->ibtc.p = sys->jit.ibtc;
    }
    if (!c->blocktab_ram.p && sys->jit.tab_ram) {
        c->blocktab_ram.p = sys->jit.tab_ram;
    }
    if (!c->blocktab_bios.p && sys->jit.tab_bios) {
        c->blocktab_bios.p = sys->jit.tab_bios;
    }
    sys->gpu.stats = &sys->stats;
}

static u32 psx_resolve_cpu_mode(const PsxSystem *sys) {
    bool jit_ok = false;
#if PSX3DS_HAVE_JIT
    jit_ok = (sys->jit.mem.mode != JIT_MEM_NONE) && (sys->jit.mem.rx != NULL);
#endif
    bool cached_ok = (sys->jit.mem.rw != NULL) || (sys->jit.blocks != NULL);

    switch (sys->cfg.cpu_engine) {
        case CPU_INTERP:
            return (u32)CPU_INTERP;
        case CPU_CACHED:
            return cached_ok ? (u32)CPU_CACHED : (u32)CPU_INTERP;
        case CPU_JIT:
            if (jit_ok) {
                return (u32)CPU_JIT;
            }
            return cached_ok ? (u32)CPU_CACHED : (u32)CPU_INTERP;
        case CPU_AUTO:
        default:
            if (jit_ok) {
                return (u32)CPU_JIT;
            }
            return cached_ok ? (u32)CPU_CACHED : (u32)CPU_INTERP;
    }
}

static void psx_wire_gpu_backends(PsxSystem *sys) {
    /* Capture any backend already created inside gpu_init() */
    if (sys->gpu.be != NULL) {
        if (sys->gpu.be->name &&
            (strstr(sys->gpu.be->name, "citro") != NULL ||
             strstr(sys->gpu.be->name, "hw") != NULL ||
             strstr(sys->gpu.be->name, "HW") != NULL)) {
            sys->hw_gpu_be = sys->gpu.be;
            sys->hw_gpu_self = sys->gpu.be_self;
        } else {
            sys->soft_gpu_be = sys->gpu.be;
            sys->soft_gpu_self = sys->gpu.be_self;
        }
    }

    if (sys->cfg.gpu_backend == GPU_BACKEND_HW && PSX3DS_HAVE_CITRO3D) {
        if (!sys->hw_gpu_be) {
            const GpuBackend *be = NULL;
            void *self = NULL;
            if (gpu_hw_backend_create(sys, &be, &self) == PSX_OK && be != NULL) {
                sys->hw_gpu_be = be;
                sys->hw_gpu_self = self;
            }
        }
        if (sys->hw_gpu_be) {
            sys->gpu.be = sys->hw_gpu_be;
            sys->gpu.be_self = sys->hw_gpu_self;
            return;
        }
        sys->cfg.gpu_backend = GPU_BACKEND_SOFT;
    }

    if (!sys->soft_gpu_be) {
        const GpuBackend *be = NULL;
        void *self = NULL;
        if (gpu_soft_backend_create(sys, &be, &self) == PSX_OK && be != NULL) {
            sys->soft_gpu_be = be;
            sys->soft_gpu_self = self;
        }
    }
    if (sys->soft_gpu_be) {
        sys->gpu.be = sys->soft_gpu_be;
        sys->gpu.be_self = sys->soft_gpu_self;
    }
}

PsxResult psx_system_set_gpu_backend(PsxSystem *sys, PsxGpuBackendKind kind) {
    if (!sys) {
        return PSX_ERR_BAD_ARG;
    }
    if (kind == GPU_BACKEND_HW && !PSX3DS_HAVE_CITRO3D) {
        return PSX_ERR_NOT_IMPL;
    }
    if (sys->gpu.be && sys->gpu.be->sync_to_shadow) {
        sys->gpu.be->sync_to_shadow(sys->gpu.be_self, 0u, 0u, PSX_VRAM_W, PSX_VRAM_H);
    }

    if (kind == GPU_BACKEND_HW) {
        if (!sys->hw_gpu_be) {
            const GpuBackend *be = NULL;
            void *self = NULL;
            PsxResult rc = gpu_hw_backend_create(sys, &be, &self);
            if (rc != PSX_OK || !be) {
                return rc != PSX_OK ? rc : PSX_ERR_NOT_IMPL;
            }
            sys->hw_gpu_be = be;
            sys->hw_gpu_self = self;
        }
        sys->gpu.be = sys->hw_gpu_be;
        sys->gpu.be_self = sys->hw_gpu_self;
        sys->cfg.gpu_backend = GPU_BACKEND_HW;
    } else {
        if (!sys->soft_gpu_be) {
            const GpuBackend *be = NULL;
            void *self = NULL;
            PsxResult rc = gpu_soft_backend_create(sys, &be, &self);
            if (rc != PSX_OK || !be) {
                return rc != PSX_OK ? rc : PSX_ERR_NOT_IMPL;
            }
            sys->soft_gpu_be = be;
            sys->soft_gpu_self = self;
        }
        sys->gpu.be = sys->soft_gpu_be;
        sys->gpu.be_self = sys->soft_gpu_self;
        sys->cfg.gpu_backend = GPU_BACKEND_SOFT;
    }

    if (sys->gpu.be) {
        if (sys->gpu.be->set_draw_env) {
            sys->gpu.be->set_draw_env(sys->gpu.be_self, &sys->gpu.env);
        }
        if (sys->gpu.be->vram_written) {
            sys->gpu.be->vram_written(sys->gpu.be_self, 0u, 0u, PSX_VRAM_W, PSX_VRAM_H);
        }
    }
    return PSX_OK;
}

PsxResult psx_system_init(PsxSystem *sys, const PsxConfig *cfg) {
    if (!sys) {
        return PSX_ERR_BAD_ARG;
    }
    memset(sys, 0, sizeof(*sys));

    if (cfg) {
        sys->cfg = *cfg;
    } else {
        bool is_n3ds = true;
#if PSX3DS_PLATFORM_3DS
        is_n3ds = false;
        APT_CheckNew3DS(&is_n3ds);
#endif
        psx_config_defaults(&sys->cfg, is_n3ds);
    }

#if PSX3DS_PLATFORM_3DS
    /* Auto-detect constrained Old-3DS / launcher FCRAM envelope */
    u32 free_fcram = (u32)osGetMemRegionFree(MEMREGION_APPLICATION);
    if (free_fcram > 0u && free_fcram < (68u * 1024u * 1024u) && !sys->cfg.small_mem) {
        sys->cfg.small_mem = true;
        if (sys->cfg.jit_code_bytes > 6u * 1024u * 1024u) {
            sys->cfg.jit_code_bytes = 6u * 1024u * 1024u;
        }
        if (sys->cfg.jit_max_blocks > 32768u) {
            sys->cfg.jit_max_blocks = 32768u;
        }
    }
#endif

    if (sys->cfg.small_mem && sys->cfg.jit_max_blocks > 32768u) {
        sys->cfg.jit_max_blocks = 32768u;
    }

    u32 arena_bytes = sys->cfg.small_mem ? PSX_ARENA_BYTES_O3DS : PSX_ARENA_BYTES_FULL;
    if (sizeof(void *) > 4u) {
        /* 64-bit host pointer tables (fm_r, fm_w, fm_w_isc_null, tab_ram, tab_bios, ibtc)
         * are 2x wider than 32-bit ARM11v6K/3DS targets. */
        arena_bytes += 20u * 1024u * 1024u;
    }
    PsxResult arc = arena_init(&sys->arena, arena_bytes);
#if PSX3DS_PLATFORM_3DS
    if (arc != PSX_OK && arena_bytes > PSX_ARENA_BYTES_MID) {
        arc = arena_init(&sys->arena, PSX_ARENA_BYTES_MID);
    }
    if (arc != PSX_OK && arena_bytes > PSX_ARENA_BYTES_O3DS) {
        sys->cfg.small_mem = true;
        arc = arena_init(&sys->arena, PSX_ARENA_BYTES_O3DS);
    }
    if (arc != PSX_OK) {
        arc = arena_init(&sys->arena, PSX_ARENA_BYTES_O3DS_MIN);
    }
#endif
    if (arc != PSX_OK) {
        return arc;
    }

    sys->cpu.sys.p = sys;
    sys->gpu.stats = &sys->stats;

    sys->pads[0].buttons = 0xFFFFu;
    sys->pads[0].lx = 0x80u;
    sys->pads[0].ly = 0x80u;
    sys->pads[0].rx = 0x80u;
    sys->pads[0].ry = 0x80u;
    sys->pads[0].connected = 1u;

    sys->pads[1].buttons = 0xFFFFu;
    sys->pads[1].lx = 0x80u;
    sys->pads[1].ly = 0x80u;
    sys->pads[1].rx = 0x80u;
    sys->pads[1].ry = 0x80u;
    sys->pads[1].connected = 0u;

    psx_stats_reset(&sys->stats);

    /* Normative initialization sequence (ARCHITECTURE.md §3.8) */
    bus_init(sys);
    if (!sys->bus.ram) {
        sys->bus.ram = (u8 *)arena_alloc(&sys->arena, PSX_RAM_SIZE, 4096u);
    }
    if (!sys->bus.scratch_page) {
        sys->bus.scratch_page = (u8 *)arena_alloc(&sys->arena, PSX_SCRATCH_PAGE_SIZE, 4096u);
    }
    if (!sys->bus.bios) {
        sys->bus.bios = (u8 *)arena_alloc(&sys->arena, PSX_BIOS_SIZE, 4096u);
    }

    sched_init(&sys->sched);
    irq_init(sys);
    cpu_init(sys);
    gte_init(sys);
#if PSX3DS_PLATFORM_3DS
    jit_mem_3ds_set_allow_hle_exec(sys->cfg.jit_allow_hle_exec);
#endif
    jit_init(sys);
    if (!sys->jit.mem.rw && sys->cfg.jit_code_bytes > 0u) {
        (void)jit_mem_alloc(&sys->jit.mem, sys->cfg.jit_code_bytes);
    }
    dma_init(sys);
    timers_init(sys);

    gpu_init(sys);
    if (!sys->gpu.vram) {
        sys->gpu.vram = (u16 *)arena_alloc(&sys->arena, PSX_VRAM_BYTES, 4096u);
    }
    psx_wire_gpu_backends(sys);

    spu_init(sys);
    if (!sys->spu.ram) {
        sys->spu.ram = (u8 *)arena_alloc(&sys->arena, PSX_SPU_RAM_BYTES, 4096u);
    }

    cdrom_init(sys);
    mdec_init(sys);
    sio_init(sys);
    bios_init(sys);

    psx_sync_cpu_pointers(sys);
    sys->cpu_mode = psx_resolve_cpu_mode(sys);
    sys->stats.jit_mem_mode = (u32)sys->jit.mem.mode;

    psx_system_reset(sys);

    if (sys->cfg.hle_bios) {
        PsxResult brc = psx_load_bios(sys, NULL);
        if (brc != PSX_OK) {
            psx_system_shutdown(sys);
            return brc;
        }
    } else if (sys->cfg.bios_path[0] != '\0') {
        PsxResult brc = psx_load_bios(sys, sys->cfg.bios_path);
        if (brc != PSX_OK) {
            psx_system_shutdown(sys);
            return brc;
        }
    }

    return PSX_OK;
}

void psx_system_reset(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    bus_reset(sys);
    sched_reset(sys);
    irq_reset(sys);
    cpu_reset(sys);
    gte_reset(sys);
    jit_reset(sys);
    dma_reset(sys);
    timers_reset(sys);
    gpu_reset(sys);
    spu_reset(sys);
    cdrom_reset(sys);
    mdec_reset(sys);
    sio_reset(sys);
    bios_reset(sys);

    psx_sync_cpu_pointers(sys);
    if (sys->cpu.pc == 0u) {
        sys->cpu.pc = PSX_RESET_PC;
        sys->cpu.next_pc = PSX_RESET_PC + 4u;
    }

    sio_set_pad(sys, 0, &sys->pads[0]);
    sio_set_pad(sys, 1, &sys->pads[1]);

    sys->frame_done = false;
    sys->quit = false;
    sys->cpu_mode = psx_resolve_cpu_mode(sys);
    sys->stats.jit_mem_mode = (u32)sys->jit.mem.mode;
}

PsxResult psx_load_cd(PsxSystem *sys, const char *path) {
    if (!sys) {
        return PSX_ERR_BAD_ARG;
    }
    return cdrom_load(sys, path);
}

static void psx_drain_sync_spu_ring(PsxSystem *sys) {
    PsxSpu *spu = &sys->spu;
    if (spu->async_mode || !spu->out_ring || spu->out_cap == 0u) {
        return;
    }
    u32 w = __atomic_load_n(&spu->out_w, __ATOMIC_ACQUIRE);
    u32 r = spu->out_r;
    if (w == r) {
        return;
    }
    if ((w - r) > spu->out_cap) {
        r = w - spu->out_cap;
    }
    s16 batch[512 * 2];
    while (r != w) {
        u32 avail = w - r;
        u32 chunk = avail > 512u ? 512u : avail;
        for (u32 i = 0; i < chunk; i++) {
            u32 idx = (r + i) % spu->out_cap;
            batch[i * 2u + 0u] = spu->out_ring[idx * 2u + 0u];
            batch[i * 2u + 1u] = spu->out_ring[idx * 2u + 1u];
        }
        plat_audio_push(batch, chunk);
        r += chunk;
    }
    __atomic_store_n(&spu->out_r, r, __ATOMIC_RELEASE);
}

void psx_run_frame(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    u64 t_frame_start = plat_time_us();
    psx_stats_frame_begin(&sys->stats);
    sys->frame_done = false;

    sio_set_pad(sys, 0, &sys->pads[0]);
    sio_set_pad(sys, 1, &sys->pads[1]);

    u64 start_sched_total = sys->sched.total;
    u64 start_cpu_total = sys->cpu.total_cycles;
    u32 zero_progress_iters = 0u;
    u32 us_events_acc = 0u;
    u32 us_cpu_acc = 0u;

    const u64 max_cycles_guard = (u64)PSX_CYCLES_PER_FRAME_PAL * 2ULL;

    for (;;) {
        u64 t0 = plat_time_us();
        u64 prev_sched = sys->sched.total;
        u64 prev_cpu = sys->cpu.total_cycles;

        sched_run(sys);
        u64 t1 = plat_time_us();
        us_events_acc += (u32)(t1 - t0);

        if (sys->frame_done || sys->quit || sys->bios.until_matched) {
            break;
        }

        switch ((PsxCpuEngine)sys->cpu_mode) {
            case CPU_JIT:
                jit_run(sys);
                break;
            case CPU_CACHED:
                cpu_cached_run(sys);
                break;
            case CPU_INTERP:
            default:
                cpu_run_interp(sys);
                break;
        }
        u64 t2 = plat_time_us();
        us_cpu_acc += (u32)(t2 - t1);

        if (sys->frame_done || sys->quit || sys->bios.until_matched) {
            break;
        }

        u64 cur_sched = sys->sched.total;
        u64 cur_cpu = sys->cpu.total_cycles;
        if (cur_sched == prev_sched && cur_cpu == prev_cpu) {
            zero_progress_iters++;
            if (zero_progress_iters >= 16u) {
                /* Stubbed core/scheduler guard: avoid infinite loop in isolated unit tests */
                sys->frame_done = true;
                break;
            }
        } else {
            zero_progress_iters = 0u;
        }

        u64 elapsed_sched = cur_sched - start_sched_total;
        u64 elapsed_cpu = cur_cpu - start_cpu_total;
        if (elapsed_sched >= max_cycles_guard || elapsed_cpu >= max_cycles_guard) {
            sys->frame_done = true;
            break;
        }
    }

    psx_drain_sync_spu_ring(sys);

    u64 t_frame_end = plat_time_us();
    sys->stats.us_events += us_events_acc;
    sys->stats.us_cpu += us_cpu_acc;
    sys->stats.us_frame = (u32)(t_frame_end - t_frame_start);
    sys->stats.frames++;

    if (sys->sched.total > sys->stats.cycles_total) {
        sys->stats.cycles_total = sys->sched.total;
    }
    if (sys->cpu.total_cycles > sys->stats.cycles_total) {
        sys->stats.cycles_total = sys->cpu.total_cycles;
    }
    sys->stats.jit_blocks_live = sys->jit.n_blocks;
    if (sys->jit.code_ptr && sys->jit.mem.rw && sys->jit.code_ptr >= sys->jit.mem.rw) {
        sys->stats.jit_code_bytes = (u32)(sys->jit.code_ptr - sys->jit.mem.rw);
    }
    sys->stats.jit_mem_mode = (u32)sys->jit.mem.mode;
}

void psx_system_shutdown(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    sys->quit = true;

    const GpuBackend *active_be = sys->gpu.be;

    bios_shutdown(sys);
    sio_shutdown(sys);
    mdec_shutdown(sys);
    cdrom_shutdown(sys);
    spu_shutdown(sys);
    gpu_shutdown(sys);

    /* Destroy any cached GPU backends not already torn down by gpu_shutdown() */
    if (sys->gpu.be != NULL) {
        if (sys->gpu.be->destroy) {
            sys->gpu.be->destroy(sys->gpu.be_self);
        }
        if (sys->soft_gpu_be == sys->gpu.be) {
            sys->soft_gpu_be = NULL;
            sys->soft_gpu_self = NULL;
        }
        if (sys->hw_gpu_be == sys->gpu.be) {
            sys->hw_gpu_be = NULL;
            sys->hw_gpu_self = NULL;
        }
        sys->gpu.be = NULL;
        sys->gpu.be_self = NULL;
    } else {
        if (sys->soft_gpu_be == active_be) {
            sys->soft_gpu_be = NULL;
            sys->soft_gpu_self = NULL;
        }
        if (sys->hw_gpu_be == active_be) {
            sys->hw_gpu_be = NULL;
            sys->hw_gpu_self = NULL;
        }
    }
    if (sys->hw_gpu_be && sys->hw_gpu_be->destroy) {
        sys->hw_gpu_be->destroy(sys->hw_gpu_self);
    }
    if (sys->soft_gpu_be && sys->soft_gpu_be->destroy) {
        sys->soft_gpu_be->destroy(sys->soft_gpu_self);
    }
    sys->hw_gpu_be = NULL;
    sys->hw_gpu_self = NULL;
    sys->soft_gpu_be = NULL;
    sys->soft_gpu_self = NULL;

    timers_shutdown(sys);
    dma_shutdown(sys);
    jit_shutdown(sys);
    if (sys->jit.mem.rw) {
        jit_mem_free(&sys->jit.mem);
    }
    gte_shutdown(sys);
    cpu_shutdown(sys);
    irq_shutdown(sys);
    sched_shutdown(sys);
    bus_shutdown(sys);

    arena_destroy(&sys->arena);
}
