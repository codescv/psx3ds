#if !defined(__3DS__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "psx3ds/system.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if PSX3DS_PLATFORM_3DS
#include <malloc.h>
#endif

static PsxSystem *bench_alloc_system(void) {
    void *mem = NULL;
#if PSX3DS_PLATFORM_3DS
    mem = memalign(32u, sizeof(PsxSystem));
#else
    if (posix_memalign(&mem, 32u, sizeof(PsxSystem)) != 0) {
        mem = NULL;
    }
#endif
    return (PsxSystem *)mem;
}

static PsxResult bench_run_single_pass(const PsxBenchConfig *bcfg,
                                       PsxCpuEngine engine,
                                       PsxGpuBackendKind gpu_kind,
                                       const char *exe_override,
                                       char *pass_json,
                                       u32 pass_cap) {
    PsxSystem *sys = bench_alloc_system();
    if (!sys) {
        return PSX_ERR_NOMEM;
    }

    PsxConfig cfg;
    psx_config_defaults(&cfg, true);
    cfg.cpu_engine = engine;
    cfg.gpu_backend = gpu_kind;
    cfg.hle_bios = bcfg->hle_bios;

    PsxResult rc = psx_system_init(sys, &cfg);
    if (rc != PSX_OK) {
        free(sys);
        return rc;
    }

    if (bcfg->hle_bios) {
        rc = psx_load_bios(sys, NULL);
    } else if (bcfg->bios_path && bcfg->bios_path[0] != '\0') {
        rc = psx_load_bios(sys, bcfg->bios_path);
    } else {
        rc = psx_load_bios(sys, "auto");
    }
    if (rc != PSX_OK) {
        psx_system_shutdown(sys);
        free(sys);
        return rc;
    }

    const char *rom_label = "bios_boot";
    const char *exe_to_load = exe_override ? exe_override : bcfg->exe_path;
    if (exe_to_load && exe_to_load[0] != '\0') {
        rc = psx_load_exe(sys, exe_to_load);
        if (rc != PSX_OK) {
            psx_system_shutdown(sys);
            free(sys);
            return rc;
        }
        rom_label = exe_to_load;
    } else if (bcfg->cd_path && bcfg->cd_path[0] != '\0') {
        rc = psx_load_cd(sys, bcfg->cd_path);
        if (rc != PSX_OK) {
            psx_system_shutdown(sys);
            free(sys);
            return rc;
        }
        rom_label = bcfg->cd_path;
    }

    u32 target_frames = bcfg->frames > 0u ? bcfg->frames : 120u;
    PsxStats accum;
    psx_stats_reset(&accum);

    u64 t0 = plat_time_us();
    u32 ran = 0u;
    for (u32 f = 0u; f < target_frames; f++) {
        psx_run_frame(sys);
        psx_stats_accumulate(&accum, &sys->stats);
        ran++;
        if (sys->quit) {
            break;
        }
    }
    u64 wall_us = plat_time_us() - t0;

    psx_stats_format_json(sys, &accum, rom_label, wall_us, ran, pass_json, pass_cap);
    psx_system_shutdown(sys);
    free(sys);
    return PSX_OK;
}

PsxResult psx_bench_run_json(const PsxBenchConfig *bcfg, char *out_json, u32 out_cap) {
    if (!bcfg || !out_json || out_cap < 128u) {
        return PSX_ERR_BAD_ARG;
    }

    PsxCpuEngine engines[3];
    u32 n_engines = 0u;
    if (bcfg->all_engines) {
        engines[n_engines++] = CPU_INTERP;
        engines[n_engines++] = CPU_CACHED;
#if PSX3DS_HAVE_JIT
        engines[n_engines++] = CPU_JIT;
#endif
    } else {
        engines[n_engines++] = CPU_AUTO;
    }

    PsxGpuBackendKind gpus[2];
    u32 n_gpus = 0u;
    gpus[n_gpus++] = GPU_BACKEND_SOFT;
#if PSX3DS_HAVE_CITRO3D
    if (bcfg->all_gpus) {
        gpus[n_gpus++] = GPU_BACKEND_HW;
    }
#endif

    const char *default_roms[4] = { NULL, NULL, NULL, NULL };
    u32 n_roms = 0u;
    char rom_paths[4][256];
    memset(rom_paths, 0, sizeof(rom_paths));

    if ((bcfg->exe_path && bcfg->exe_path[0] != '\0') ||
        (bcfg->cd_path && bcfg->cd_path[0] != '\0')) {
        default_roms[n_roms++] = bcfg->exe_path;
    } else {
        const char *roms_dir = "tests/roms";
#if PSX3DS_PLATFORM_HOST
        const char *env_roms = getenv("PSX3DS_ROMS_DIR");
        if (env_roms && env_roms[0] != '\0') {
            roms_dir = env_roms;
        }
#endif
        const char *bench_names[4] = {
            "benchmarks/PSXNICCC.exe",
            "benchmarks/Cube.exe",
            "benchmarks/gpu-benchmark.exe",
            "benchmarks/animated-triangle.exe"
        };
        for (int i = 0; i < 4; i++) {
            snprintf(rom_paths[i], sizeof(rom_paths[i]), "%s/%s", roms_dir, bench_names[i]);
            PlatFile *probe = plat_fopen(rom_paths[i], "rb");
            if (probe) {
                plat_fclose(probe);
                default_roms[n_roms++] = rom_paths[i];
            }
        }
        if (n_roms == 0u) {
            default_roms[n_roms++] = NULL;
        }
    }

    const char *target_name = PSX3DS_PLATFORM_3DS
                                  ? "nintendo_3ds"
                                  : (PSX3DS_ARCH_ARMV6K ? "arm11v6k_linux" : "x86_64_host");

    u32 pos = 0u;
    int hdr_n = snprintf(out_json + pos, out_cap - pos,
                         "{\n  \"target\": \"%s\",\n  \"have_jit\": %s,\n  \"passes\": [\n",
                         target_name, PSX3DS_HAVE_JIT ? "true" : "false");
    if (hdr_n < 0 || (u32)hdr_n >= out_cap - pos) {
        return PSX_ERR_NOMEM;
    }
    pos += (u32)hdr_n;

    char pass_buf[4096];
    u32 emitted_passes = 0u;

    for (u32 r = 0u; r < n_roms; r++) {
        for (u32 g = 0u; g < n_gpus; g++) {
            for (u32 e = 0u; e < n_engines; e++) {
                PsxResult rc = bench_run_single_pass(bcfg, engines[e], gpus[g],
                                                     default_roms[r], pass_buf, sizeof(pass_buf));
                if (rc != PSX_OK) {
                    return rc;
                }
                int add = snprintf(out_json + pos, out_cap - pos, "%s%s",
                                   emitted_passes > 0u ? ",\n" : "", pass_buf);
                if (add < 0 || (u32)add >= out_cap - pos) {
                    return PSX_ERR_NOMEM;
                }
                pos += (u32)add;
                emitted_passes++;
            }
        }
    }

    int tail_n = snprintf(out_json + pos, out_cap - pos, "\n  ]\n}\n");
    if (tail_n < 0 || (u32)tail_n >= out_cap - pos) {
        return PSX_ERR_NOMEM;
    }
    return PSX_OK;
}
