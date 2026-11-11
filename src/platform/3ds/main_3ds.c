#include <3ds.h>
#include <citro3d.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "plat_3ds_internal.h"
#include "psx3ds/ctr_heap.h"

/* Override libctru's weak 32 KiB default main-thread stack size with 256 KiB */
PSX_USED u32 __stacksize__ = 256u * 1024u;

/*
 * Startup heap split (v0.2.1): override libctru's weak `__system_allocateHeaps`.
 *
 * v0.2.0 set `__ctru_linear_heap_size = 20 MiB` with `__ctru_heap_size = 0`, which made
 * libctru compute an *uncapped* normal heap of 102.58 MiB (0x06694000) on New 3DS
 * (124 MiB RESLIMIT_COMMIT). That overflows the Kernel11 heap VA window
 * [0x08000000, 0x0E000000) (96 MiB), svcControlMemory fails, and libctru calls
 * svcBreak(USERBREAK_PANIC) before main() (Luma3DS "prefetch abort / debug event").
 *
 * We now size both heaps ourselves via psx_ctr_compute_heap_split() (unit-tested in
 * tests/unit/test_ctr_heap.c): normal heap = min(remaining - 20 MiB, 80 MiB), linear heap
 * gets the rest. Old 3DS keeps ~42.5 MiB normal + 20 MiB linear; New 3DS gets 80 MiB
 * normal + ~42.5 MiB linear. If the kernel still rejects a size, we shrink and retry
 * instead of panicking. Runs before newlib/malloc init: no libc calls allowed here.
 */
extern u32   __ctru_heap;
extern u32   __ctru_linear_heap;
extern u32   __ctru_heap_size;
extern u32   __ctru_linear_heap_size;
extern char *fake_heap_start;
extern char *fake_heap_end;

#define CTR_HEAP_RETRY_STEP  PSX_CTR_MIB(8)
#define CTR_HEAP_RETRY_FLOOR PSX_CTR_MIB(8)

PSX_USED void __system_allocateHeaps(void) {
    s64 commit_limit = 0;
    s64 commit_current = 0;
    Handle reslimit = 0;
    ResourceLimitType res_type = RESLIMIT_COMMIT;

    if (svcGetResourceLimit(&reslimit, CUR_PROCESS_HANDLE) < 0) {
        svcBreak(USERBREAK_PANIC);
    }
    (void)svcGetResourceLimitLimitValues(&commit_limit, reslimit, &res_type, 1);
    (void)svcGetResourceLimitCurrentValues(&commit_current, reslimit, &res_type, 1);
    svcCloseHandle(reslimit);

    PsxCtrHeapSplit split;
    if (!psx_ctr_compute_heap_split(commit_limit, commit_current, &split)) {
        svcBreak(USERBREAK_PANIC);
    }

    /* Normal heap at OS_HEAP_AREA_BEGIN; never larger than the 96 MiB VA window. */
    u32 heap_size = split.heap_size;
    if (!psx_ctr_heap_fits_va(heap_size)) {
        heap_size = PSX_CTR_HEAP_MAX_BYTES;
    }
    u32 heap_addr = 0u;
    for (;;) {
        Result rc = svcControlMemory(&heap_addr, PSX_CTR_HEAP_AREA_BEGIN, 0x0u, heap_size,
                                     MEMOP_ALLOC, MEMPERM_READ | MEMPERM_WRITE);
        if (rc >= 0) {
            break;
        }
        if (heap_size <= CTR_HEAP_RETRY_FLOOR) {
            svcBreak(USERBREAK_PANIC);
        }
        heap_size = (heap_size > CTR_HEAP_RETRY_STEP + CTR_HEAP_RETRY_FLOOR)
                        ? (heap_size - CTR_HEAP_RETRY_STEP)
                        : CTR_HEAP_RETRY_FLOOR;
    }

    /* Linear heap: everything the normal heap did not take (capped to its VA window). */
    u32 linear_size = split.remaining - heap_size;
    if (linear_size > PSX_CTR_LINEAR_AREA_BYTES) {
        linear_size = PSX_CTR_LINEAR_AREA_BYTES;
    }
    u32 linear_addr = 0u;
    for (;;) {
        Result rc = svcControlMemory(&linear_addr, 0x0u, 0x0u, linear_size, MEMOP_ALLOC_LINEAR,
                                     MEMPERM_READ | MEMPERM_WRITE);
        if (rc >= 0) {
            break;
        }
        if (linear_size <= CTR_HEAP_RETRY_FLOOR) {
            svcBreak(USERBREAK_PANIC);
        }
        linear_size = (linear_size > CTR_HEAP_RETRY_STEP + CTR_HEAP_RETRY_FLOOR)
                          ? (linear_size - CTR_HEAP_RETRY_STEP)
                          : CTR_HEAP_RETRY_FLOOR;
    }

    __ctru_heap = heap_addr;
    __ctru_heap_size = heap_size;
    __ctru_linear_heap = linear_addr;
    __ctru_linear_heap_size = linear_size;

    /* Mappable VA allocator window (svcMapMemoryBlock / svcControlProcessMemory users). */
    mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);

    /* newlib sbrk() window */
    fake_heap_start = (char *)(uintptr_t)__ctru_heap;
    fake_heap_end = (char *)(uintptr_t)(__ctru_heap + __ctru_heap_size);
}

#define AUTORUN_CFG_PATH "sdmc:/psx3ds_autorun.cfg"

typedef struct CtrAutorunCfg {
    bool present;
    char bios[256];
    bool hle;
    char exe[256];
    char cd[256];
    char gpu[16];
    char cpu[16];
    u32  frames;
    char stop_tty[64];
    char ppm[256];
    char bottom_ppm[256]; /* optional 320x240 bottom-screen (UI) capture */
    bool fast_boot;       /* cfg.cd_fast_boot: sideload disc EXE instead of native BIOS CD boot */
    u32  ui_tab;          /* 1..4 = bottom tab shown during autorun (default 2:HUD) */
    char json[256];
    char tty[256];
    char done[256];
    char state[256];      /* optional savestate loaded after the disc/exe is mounted */
    int  scale;           /* -1 = default, else PsxVideoScale (0 = 1:1, 1 = stretch, 2 = 4:3) */
    int  small_mem; /* -1 = auto (O3DS), 0 = force off, 1 = force Old-3DS memory profile */
} CtrAutorunCfg;

static char *trim_ws(char *s) {
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    size_t n = strlen(s);
    while (n > 0u && isspace((unsigned char)s[n - 1u])) {
        s[--n] = '\0';
    }
    return s;
}

static bool parse_autorun_cfg(CtrAutorunCfg *ar) {
    memset(ar, 0, sizeof(*ar));
    ar->frames = 120u;
    ar->small_mem = -1;
    ar->scale = -1;

    FILE *fp = fopen(AUTORUN_CFG_PATH, "r");
    if (!fp) {
        return false;
    }
    ar->present = true;

    char line[512];
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *p = trim_ws(line);
        if (*p == '\0' || *p == '#') {
            continue;
        }
        char *eq = strchr(p, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = trim_ws(p);
        char *val = trim_ws(eq + 1);

        if (strcmp(key, "bios") == 0) {
            if (strcmp(val, "hle") == 0) {
                ar->hle = true;
            } else {
                snprintf(ar->bios, sizeof(ar->bios), "%s", val);
            }
        } else if (strcmp(key, "hle") == 0) {
            ar->hle = (strcmp(val, "1") == 0 || strcmp(val, "true") == 0);
        } else if (strcmp(key, "exe") == 0) {
            snprintf(ar->exe, sizeof(ar->exe), "%s", val);
        } else if (strcmp(key, "cd") == 0) {
            snprintf(ar->cd, sizeof(ar->cd), "%s", val);
        } else if (strcmp(key, "gpu") == 0) {
            snprintf(ar->gpu, sizeof(ar->gpu), "%s", val);
        } else if (strcmp(key, "cpu") == 0) {
            snprintf(ar->cpu, sizeof(ar->cpu), "%s", val);
        } else if (strcmp(key, "frames") == 0) {
            ar->frames = (u32)strtoul(val, NULL, 0);
        } else if (strcmp(key, "stop_on_tty") == 0 || strcmp(key, "until_printf") == 0) {
            snprintf(ar->stop_tty, sizeof(ar->stop_tty), "%s", val);
        } else if (strcmp(key, "ppm") == 0) {
            snprintf(ar->ppm, sizeof(ar->ppm), "%s", val);
        } else if (strcmp(key, "fast_boot") == 0) {
            ar->fast_boot = (strcmp(val, "1") == 0 || strcmp(val, "true") == 0);
        } else if (strcmp(key, "bottom_ppm") == 0) {
            snprintf(ar->bottom_ppm, sizeof(ar->bottom_ppm), "%s", val);
        } else if (strcmp(key, "ui_tab") == 0) {
            ar->ui_tab = (u32)strtoul(val, NULL, 0);
        } else if (strcmp(key, "json") == 0) {
            snprintf(ar->json, sizeof(ar->json), "%s", val);
        } else if (strcmp(key, "tty") == 0) {
            snprintf(ar->tty, sizeof(ar->tty), "%s", val);
        } else if (strcmp(key, "done") == 0) {
            snprintf(ar->done, sizeof(ar->done), "%s", val);
        } else if (strcmp(key, "savestate") == 0 || strcmp(key, "state") == 0) {
            snprintf(ar->state, sizeof(ar->state), "%s", val);
        } else if (strcmp(key, "scale") == 0) {
            if (strcmp(val, "1x1") == 0) {
                ar->scale = (int)PSX_SCALE_1X1;
            } else if (strcmp(val, "stretch") == 0) {
                ar->scale = (int)PSX_SCALE_STRETCH;
            } else if (strcmp(val, "fit") == 0 || strcmp(val, "4:3") == 0) {
                ar->scale = (int)PSX_SCALE_FIT_4_3;
            } else {
                ar->scale = (int)strtol(val, NULL, 0);
            }
        } else if (strcmp(key, "small_mem") == 0) {
            ar->small_mem = (strcmp(val, "1") == 0 || strcmp(val, "true") == 0) ? 1 : 0;
        }
    }
    fclose(fp);
    return true;
}

static bool has_exe_ext(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) {
        return false;
    }
    return (strcasecmp(dot, ".exe") == 0) || (strcasecmp(dot, ".psx") == 0);
}

static void format_savestate_path(char *out, size_t cap, const char *boot_path, u8 slot) {
    const char *base = "bios";
    if (boot_path && boot_path[0] != '\0') {
        const char *slash = strrchr(boot_path, '/');
        base = slash ? (slash + 1) : boot_path;
    }
    char stem[64];
    size_t i = 0u;
    for (; base[i] != '\0' && base[i] != '.' && i + 1u < sizeof(stem); i++) {
        stem[i] = base[i];
    }
    stem[i] = '\0';
    snprintf(out, cap, "sdmc:/psx3ds/saves/%s_slot%u.st", stem, (unsigned)slot);
}

/* Guest TTY (BIOS putchar / printf) -> on-screen LOG tab + sdmc:/psx3ds/psx3ds.log */
static void tty_to_ctr_log(const char *data, u32 len, void *user) {
    (void)user;
    for (u32 i = 0u; i < len; i++) {
        ctr_log_tty_putc(data[i]);
    }
}

static const char *cfg_bios_label(const PsxConfig *cfg) {
    if (cfg->hle_bios) {
        return "hle";
    }
    return cfg->bios_path[0] != '\0' ? cfg->bios_path : PSX_BIOS_BUILTIN_NAME;
}

static int run_autorun_mode(const CtrAutorunCfg *ar, CtrHwInfo *hw) {
    plat_log("[main_3ds] autorun config detected: exe='%s' cd='%s' gpu='%s' cpu='%s' frames=%u\n",
             ar->exe, ar->cd, ar->gpu, ar->cpu, (unsigned)ar->frames);

    /* Single-buffer top & bottom screens in autorun mode for exact frame readback */
    gfxSetDoubleBuffering(GFX_TOP, false);
    gfxSetDoubleBuffering(GFX_BOTTOM, false);

    PsxConfig cfg;
    psx_config_defaults(&cfg, hw->is_n3ds);
    if (strcmp(ar->gpu, "soft") == 0) {
        cfg.gpu_backend = GPU_BACKEND_SOFT;
    } else if (strcmp(ar->gpu, "hw") == 0) {
        cfg.gpu_backend = GPU_BACKEND_HW;
    }
    if (ar->small_mem >= 0) {
        /* Mirror psx_config_defaults() O3DS/N3DS budgets for an explicit override */
        cfg.small_mem      = (ar->small_mem != 0);
        cfg.jit_code_bytes = cfg.small_mem ? 6u * 1024u * 1024u : 16u * 1024u * 1024u;
        cfg.jit_max_blocks = cfg.small_mem ? 32768u : 65536u;
    }
    if (ar->scale >= (int)PSX_SCALE_1X1 && ar->scale <= (int)PSX_SCALE_FIT_4_3) {
        cfg.scale = (u8)ar->scale;
    }
    if (strcmp(ar->cpu, "interp") == 0) {
        cfg.cpu_engine = CPU_INTERP;
    } else if (strcmp(ar->cpu, "cached") == 0) {
        cfg.cpu_engine = CPU_CACHED;
    } else if (strcmp(ar->cpu, "jit") == 0) {
        cfg.cpu_engine = CPU_JIT;
    }

    CtrUiState *ui = (CtrUiState *)calloc(1u, sizeof(*ui));
    if (ui) {
        ctr_ui_init(ui);
        ui->active_tab = (ar->ui_tab >= 1u && ar->ui_tab <= CTR_UI_TAB_COUNT)
                             ? (u8)(ar->ui_tab - 1u)
                             : (u8)CTR_UI_TAB_HUD;
        ui->game_loaded = true;
    }

    if (ar->hle) {
        cfg.hle_bios = true;
    } else if (ar->bios[0] != '\0') {
        snprintf(cfg.bios_path, sizeof(cfg.bios_path), "%s", ar->bios);
    } else if (ui && ui->bios_path[0] != '\0') {
        /* External SD BIOS if detected, else "builtin" OpenBIOS */
        snprintf(cfg.bios_path, sizeof(cfg.bios_path), "%s", ui->bios_path);
    } else {
        snprintf(cfg.bios_path, sizeof(cfg.bios_path), "%s", PSX_BIOS_BUILTIN_NAME);
    }


    PsxSystem *sys = (PsxSystem *)calloc(1u, sizeof(*sys));
    if (!sys) {
        free(ui);
        return 1;
    }

    cfg.cd_fast_boot = ar->fast_boot;
    if (ui) {
        ui->cd_fast_boot = ar->fast_boot;
    }
    char bios_req[256];
    snprintf(bios_req, sizeof(bios_req), "%s", cfg_bios_label(&cfg));
    PsxResult init_rc = psx_system_init(sys, &cfg);
    plat_log("[boot] autorun bios=%s -> %s cd_boot=%s rc=%d\n", bios_req,
             cfg_bios_label(&sys->cfg), cfg.cd_fast_boot ? "fast" : "native", (int)init_rc);
    if (init_rc != PSX_OK) {
        plat_log("[main_3ds] autorun psx_system_init failed: %d\n", (int)init_rc);
        free(sys);
        free(ui);
        return 1;
    }

    bios_set_tty_callback(sys, tty_to_ctr_log, NULL);
    if (ar->stop_tty[0] != '\0') {
        bios_set_stop_pattern(sys, ar->stop_tty);
    }
    if (ar->cd[0] != '\0') {
        PsxResult crc = psx_load_cd(sys, ar->cd);
        plat_log("[boot] autorun cd=%s rc=%d\n", ar->cd, (int)crc);
    }
    if (ar->exe[0] != '\0') {
        PsxResult erc = psx_load_exe(sys, ar->exe);
        plat_log("[boot] autorun exe=%s rc=%d\n", ar->exe, (int)erc);
    }
    if (ar->state[0] != '\0') {
        PsxResult src = psx_state_load_file(sys, ar->state);
        plat_log("[autorun] loaded savestate %s (rc=%d)\n", ar->state, (int)src);
    }

    ctr_input_set_scale_mode((PsxVideoScale)sys->cfg.scale);
    ctr_audio_bind_system(sys);
    ctr_threads_start_workers(sys, hw);

    PsxStats accum;
    psx_stats_reset(&accum);
    u32 frames_ran = 0u;
    u64 t0 = plat_time_us();

    while (frames_ran < ar->frames && aptMainLoop() && !sys->quit && !sys->bios.until_matched) {
        hidScanInput();
        PsxHostKeys hkeys;
        plat_input_poll(sys->pads, &hkeys);

        psx_run_frame(sys);
        psx_stats_accumulate(&accum, &sys->stats);
        frames_ran++;

        if (ui && ((frames_ran & 3u) == 1u || frames_ran == ar->frames)) {
            ctr_ui_render(ui, sys, hw);
        }
    }

    u64 wall_us = plat_time_us() - t0;
    ctr_threads_stop_workers(sys, hw);

    /* 1. Capture un-rotated 400x240 top screen PPM */
    if (ar->ppm[0] != '\0') {
        PsxResult prc = ctr_capture_top_screen_ppm(ar->ppm);
        plat_log("[main_3ds] autorun ppm '%s' rc=%d\n", ar->ppm, (int)prc);
    }
    if (ar->bottom_ppm[0] != '\0') {
        if (ui) {
            ctr_ui_render(ui, sys, hw); /* fresh frame incl. the lines logged above */
        }
        PsxResult brc = ctr_capture_bottom_screen_ppm(ar->bottom_ppm);
        plat_log("[main_3ds] autorun bottom_ppm '%s' rc=%d\n", ar->bottom_ppm, (int)brc);
    }

    /* 2. Write TTY stdout capture */
    if (ar->tty[0] != '\0') {
        FILE *tfp = fopen(ar->tty, "wb");
        if (tfp) {
            if (sys->bios.tty_buf && sys->bios.tty_buf_len > 0u) {
                (void)fwrite(sys->bios.tty_buf, 1u, sys->bios.tty_buf_len, tfp);
            }
            fclose(tfp);
        }
    }

    /* 3. Write structured telemetry JSON */
    if (ar->json[0] != '\0') {
        char jbuf[4096];
        const char *label = ar->exe[0] ? ar->exe : (ar->cd[0] ? ar->cd : "autorun");
        u32 jlen = psx_stats_format_json(sys, &accum, label, wall_us, frames_ran,
                                         jbuf, sizeof(jbuf));
        FILE *jfp = fopen(ar->json, "wb");
        if (jfp) {
            (void)fwrite(jbuf, 1u, jlen, jfp);
            fclose(jfp);
        }
    }

    plat_log("[main_3ds] autorun completed cleanly (%u frames, jit_mem=%u, cpu=%u)\n",
             (unsigned)frames_ran, (unsigned)sys->jit.mem.mode, (unsigned)sys->cpu_mode);
    /* Allow HLE debug logger thread to drain OutputDebugString before done sentinel triggers exit */
    plat_sleep_us(50000u);

    /* 4. Write completion sentinel LAST so azahar-headless sees flushed artifacts */
    if (ar->done[0] != '\0') {
        FILE *dfp = fopen(ar->done, "wb");
        if (dfp) {
            fprintf(dfp, "OK frames=%u jit_mem_mode=%u cpu_mode=%u\n",
                    (unsigned)frames_ran,
                    (unsigned)sys->jit.mem.mode,
                    (unsigned)sys->cpu_mode);
            fclose(dfp);
        }
    }

    psx_system_shutdown(sys);
    free(sys);
    free(ui);
    return 0;
}

/* Save/load with Core 1 (SPU) and Core 2 (CD readahead) workers quiesced */
static PsxResult state_io_quiesced(PsxSystem *sys, CtrHwInfo *hw, const char *path, bool save) {
    ctr_threads_stop_workers(sys, hw);
    PsxResult rc = save ? psx_state_save_file(sys, path) : psx_state_load_file(sys, path);
    ctr_threads_start_workers(sys, hw);
    return rc;
}

static PsxResult boot_game_into_system(PsxSystem *sys, CtrUiState *ui, CtrHwInfo *hw,
                                       const char *boot_path) {
    ctr_threads_stop_workers(sys, hw);
    /*
     * Mark the system unloaded BEFORE tearing it down: if init/load below fails, the main
     * loop must not keep calling psx_run_frame() on the destroyed arena.
     */
    ui->game_loaded = false;
    psx_system_shutdown(sys);

    PsxConfig cfg;
    psx_config_defaults(&cfg, hw->is_n3ds);
    if (ui->bios_mode == CTR_BIOS_MODE_HLE) {
        cfg.hle_bios = true;
        cfg.bios_path[0] = '\0';
    } else {
        /* Auto: external SD BIOS found at scan time, else the embedded OpenBIOS */
        cfg.hle_bios = false;
        snprintf(cfg.bios_path, sizeof(cfg.bios_path), "%s",
                 ui->bios_path[0] != '\0' ? ui->bios_path : PSX_BIOS_BUILTIN_NAME);
    }
    cfg.cd_fast_boot = ui->cd_fast_boot;
    const char *path_lbl = (boot_path && boot_path[0] != '\0') ? boot_path : "(bios shell)";
    char bios_req[256];
    snprintf(bios_req, sizeof(bios_req), "%s", cfg_bios_label(&cfg));

    PsxResult rc = psx_system_init(sys, &cfg);
    if (rc != PSX_OK) {
        plat_log("[boot] path=%s bios=%s rc=%d (system init failed)\n", path_lbl, bios_req,
                 (int)rc);
        return rc;
    }
    bios_set_tty_callback(sys, tty_to_ctr_log, NULL);

    if (boot_path && boot_path[0] != '\0') {
        if (has_exe_ext(boot_path)) {
            rc = psx_load_exe(sys, boot_path);
        } else {
            rc = psx_load_cd(sys, boot_path);
        }
        if (rc != PSX_OK) {
            plat_log("[boot] path=%s bios=%s rc=%d (load failed)\n", path_lbl,
                     cfg_bios_label(&sys->cfg), (int)rc);
            return rc;
        }
    }
    plat_log("[boot] path=%s bios=%s cd_boot=%s rc=%d\n", path_lbl, cfg_bios_label(&sys->cfg),
             sys->cfg.cd_fast_boot ? "fast" : "native", (int)rc);

    ctr_input_set_scale_mode((PsxVideoScale)sys->cfg.scale);
    ctr_audio_bind_system(sys);
    ctr_threads_start_workers(sys, hw);
    ui->game_loaded = true;
    ui->active_tab = 1u;
    return PSX_OK;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    CtrHwInfo hw;
    ctr_threads_init_hardware(&hw);
    plat_log("[main_3ds] psx3ds v%s starting (n3ds=%d, cpu=%u MHz)\n",
             PSX3DS_VERSION_STR, (int)hw.is_n3ds, (unsigned)hw.cpu_mhz);
    plat_log("[main_3ds] heap=0x%08lX (%lu KiB) @0x%08lX linear=0x%08lX (%lu KiB) @0x%08lX\n",
             (unsigned long)__ctru_heap_size, (unsigned long)(__ctru_heap_size >> 10),
             (unsigned long)__ctru_heap, (unsigned long)__ctru_linear_heap_size,
             (unsigned long)(__ctru_linear_heap_size >> 10), (unsigned long)__ctru_linear_heap);

    gfxInitDefault();
    C3D_Init(0x40000u);
    (void)romfsInit();

    (void)plat_mkdir_p("sdmc:/psx3ds/bios");
    (void)plat_mkdir_p("sdmc:/psx3ds/roms");
    (void)plat_mkdir_p("sdmc:/psx3ds/saves");
    (void)plat_mkdir_p("sdmc:/psx3ds/memcards");

    hw.ndsp_ok = ctr_audio_init(NULL);

    CtrAutorunCfg autorun;
    if (parse_autorun_cfg(&autorun)) {
        int rc = run_autorun_mode(&autorun, &hw);
        ctr_audio_shutdown();
        romfsExit();
        C3D_Fini();
        gfxExit();
        return rc;
    }

    /* Interactive 3DS Frontend */
    CtrUiState *ui = (CtrUiState *)calloc(1u, sizeof(*ui));
    PsxSystem  *sys = (PsxSystem *)calloc(1u, sizeof(*sys));
    if (!ui || !sys) {
        free(ui);
        free(sys);
        ctr_audio_shutdown();
        romfsExit();
        C3D_Fini();
        gfxExit();
        return 1;
    }

    ctr_ui_init(ui);
    plat_log("[main_3ds] BIOS: %s (%s), %u ROM(s) found; log: sdmc:/psx3ds/psx3ds.log\n",
             ui->bios_badge, ui->bios_path, (unsigned)ui->rom_count);
    char current_game[256] = { 0 };
    u32 ui_frame_div = 0u;

    while (aptMainLoop()) {
        hidScanInput();
        u32 kdown = hidKeysDown();
        u32 kheld = hidKeysHeld();
        touchPosition touch = { 0, 0 };
        if (kheld & KEY_TOUCH) {
            hidTouchRead(&touch);
        }

        CtrUiAction act = ctr_ui_update(ui, ui->game_loaded ? sys : NULL, &hw,
                                        kdown, kheld, (kheld & KEY_TOUCH) ? &touch : NULL);
        if (act == CTR_UI_QUIT_APP) {
            break;
        } else if (act == CTR_UI_TOGGLE_BIOS_MODE) {
            ui->bios_mode = (ui->bios_mode == CTR_BIOS_MODE_HLE) ? CTR_BIOS_MODE_AUTO
                                                                 : CTR_BIOS_MODE_HLE;
            plat_log("[ui] BIOS mode -> %s\n", ui->bios_mode == CTR_BIOS_MODE_HLE
                                                   ? "HLE Direct"
                                                   : ui->bios_path);
            ctr_ui_set_status(ui, "BIOS mode applies on next boot/reset");
        } else if (act == CTR_UI_TOGGLE_CD_BOOT) {
            ui->cd_fast_boot = !ui->cd_fast_boot;
            plat_log("[ui] CD boot -> %s\n", ui->cd_fast_boot ? "Fast (sideload EXE)"
                                                               : "Native (BIOS reads SYSTEM.CNF)");
            ctr_ui_set_status(ui, ui->bios_mode == CTR_BIOS_MODE_HLE
                                      ? "CD Boot mode only used with OpenBIOS/SD BIOS"
                                      : "CD Boot mode applies on next boot/reset");
        } else if (act == CTR_UI_BOOT_PATH) {
            snprintf(current_game, sizeof(current_game), "%s", ui->pending_boot_path);
            PsxResult brc = boot_game_into_system(sys, ui, &hw, current_game);
            if (brc == PSX_OK) {
                ctr_ui_set_status(ui, current_game[0] ? "Game booted" : "Booted BIOS shell");
            } else {
                char msg[64];
                snprintf(msg, sizeof(msg), "Boot failed rc=%d (see 4:LOG)", (int)brc);
                ctr_ui_set_status(ui, msg);
            }
        } else if (ui->game_loaded) {
            char st_path[256];
            switch (act) {
                case CTR_UI_TOGGLE_GPU: {
                    PsxGpuBackendKind next_be = (sys->cfg.gpu_backend == GPU_BACKEND_HW)
                                                    ? GPU_BACKEND_SOFT
                                                    : GPU_BACKEND_HW;
                    if (psx_system_set_gpu_backend(sys, next_be) == PSX_OK) {
                        ctr_ui_set_status(ui, next_be == GPU_BACKEND_HW
                                                  ? "Switched to PICA200 HW GPU"
                                                  : "Switched to ARMv6 SW GPU");
                    } else {
                        ctr_ui_set_status(ui, "GPU backend unavailable");
                    }
                    break;
                }
                case CTR_UI_CYCLE_SCALE:
                    sys->cfg.scale = (u8)((sys->cfg.scale + 1u) % 3u);
                    ctr_input_set_scale_mode((PsxVideoScale)sys->cfg.scale);
                    ctr_ui_set_status(ui, "Updated top-screen aspect ratio");
                    break;
                case CTR_UI_CYCLE_FRAMESKIP:
                    sys->cfg.frameskip = (u8)((sys->cfg.frameskip + 1u) % 3u);
                    ctr_ui_set_status(ui, "Updated frameskip");
                    break;
                case CTR_UI_TOGGLE_REVERB:
                    sys->cfg.spu_reverb = !sys->cfg.spu_reverb;
                    ctr_ui_set_status(ui, sys->cfg.spu_reverb ? "SPU Reverb ON" : "SPU Reverb OFF");
                    break;
                case CTR_UI_SAVE_STATE:
                    format_savestate_path(st_path, sizeof(st_path), current_game, ui->state_slot);
                    if (state_io_quiesced(sys, &hw, st_path, true) == PSX_OK) {
                        ctr_ui_set_status(ui, "Saved state to SD card");
                    } else {
                        ctr_ui_set_status(ui, "Save state failed");
                    }
                    break;
                case CTR_UI_LOAD_STATE:
                    format_savestate_path(st_path, sizeof(st_path), current_game, ui->state_slot);
                    if (state_io_quiesced(sys, &hw, st_path, false) == PSX_OK) {
                        ctr_ui_set_status(ui, "Loaded state from SD card");
                    } else {
                        ctr_ui_set_status(ui, "State slot empty or invalid");
                    }
                    break;
                case CTR_UI_RESET_SYSTEM: {
                    PsxResult rrc = boot_game_into_system(sys, ui, &hw, current_game);
                    if (rrc == PSX_OK) {
                        ctr_ui_set_status(ui, "System reset");
                    } else {
                        char msg[64];
                        snprintf(msg, sizeof(msg), "Reset failed rc=%d (see 4:LOG)", (int)rrc);
                        ctr_ui_set_status(ui, msg);
                    }
                    break;
                }
                default:
                    break;
            }
        }

        if (ui->game_loaded && ui->active_tab != 0u) {
            PsxHostKeys hkeys;
            plat_input_poll(sys->pads, &hkeys);
            if (hkeys.save_state) {
                char st_path[256];
                format_savestate_path(st_path, sizeof(st_path), current_game, ui->state_slot);
                (void)state_io_quiesced(sys, &hw, st_path, true);
                ctr_ui_set_status(ui, "Quick-saved state (L+R+X)");
            }
            psx_run_frame(sys);
            /*
             * v0.2.5: the bottom-screen UI is software-rendered on the ARM11 (~1-2 ms per
             * redraw on real hardware). While idle during gameplay redraw it only every 16th
             * frame (~3.75 Hz); any new button press or touch redraws it immediately.
             */
            bool ui_input = (kdown != 0u) || ((kheld & KEY_TOUCH) != 0u);
            if (ui_input || (ui_frame_div++ & 15u) == 0u) {
                ctr_ui_render(ui, sys, &hw);
            }
        } else {
            ctr_ui_render(ui, ui->game_loaded ? sys : NULL, &hw);
            gspWaitForVBlank();
        }
    }

    ctr_threads_stop_workers(sys, &hw);
    if (ui->game_loaded) {
        psx_system_shutdown(sys);
    }
    free(sys);
    free(ui);
    ctr_audio_shutdown();
    romfsExit();
    C3D_Fini();
    gfxExit();
    return 0;
}
