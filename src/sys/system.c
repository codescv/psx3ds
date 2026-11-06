#include "psx3ds/system.h"

#include <string.h>

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
    cfg->scale = (u8)PSX_SCALE_1X1;
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

PsxResult psx_system_init(PsxSystem *sys, const PsxConfig *cfg) {
    (void)sys;
    (void)cfg;
    return PSX_ERR_NOT_IMPL;
}

void psx_system_reset(PsxSystem *sys) {
    (void)sys;
}

PsxResult psx_load_cd(PsxSystem *sys, const char *path) {
    return cdrom_load(sys, path);
}

void psx_run_frame(PsxSystem *sys) {
    (void)sys;
}

void psx_system_shutdown(PsxSystem *sys) {
    (void)sys;
}
