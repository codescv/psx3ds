#include <3ds.h>
#include <citro3d.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "plat_3ds_internal.h"

/* Override libctru's weak 32 KiB default main-thread stack size with 256 KiB */
PSX_USED u32 __stacksize__ = 256u * 1024u;

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
    char json[256];
    char tty[256];
    char done[256];
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
        } else if (strcmp(key, "json") == 0) {
            snprintf(ar->json, sizeof(ar->json), "%s", val);
        } else if (strcmp(key, "tty") == 0) {
            snprintf(ar->tty, sizeof(ar->tty), "%s", val);
        } else if (strcmp(key, "done") == 0) {
            snprintf(ar->done, sizeof(ar->done), "%s", val);
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
        ui->active_tab = 1u;
    }

    if (ar->hle) {
        cfg.hle_bios = true;
    } else if (ar->bios[0] != '\0') {
        snprintf(cfg.bios_path, sizeof(cfg.bios_path), "%s", ar->bios);
    } else if (ui && ui->bios_path[0] != '\0') {
        snprintf(cfg.bios_path, sizeof(cfg.bios_path), "%s", ui->bios_path);
    } else {
        cfg.hle_bios = true;
    }

    PsxSystem *sys = (PsxSystem *)calloc(1u, sizeof(*sys));
    if (!sys) {
        free(ui);
        return 1;
    }

    PsxResult init_rc = psx_system_init(sys, &cfg);
    if (init_rc != PSX_OK) {
        plat_log("[main_3ds] autorun psx_system_init failed: %d\n", (int)init_rc);
        free(sys);
        free(ui);
        return 1;
    }

    if (ar->stop_tty[0] != '\0') {
        bios_set_stop_pattern(sys, ar->stop_tty);
    }
    if (ar->cd[0] != '\0') {
        (void)psx_load_cd(sys, ar->cd);
    }
    if (ar->exe[0] != '\0') {
        (void)psx_load_exe(sys, ar->exe);
    }

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
    if (ui->bios_path[0] != '\0') {
        cfg.hle_bios = false;
        snprintf(cfg.bios_path, sizeof(cfg.bios_path), "%s", ui->bios_path);
    } else {
        cfg.hle_bios = true;
    }

    PsxResult rc = psx_system_init(sys, &cfg);
    if (rc != PSX_OK) {
        return rc;
    }

    if (boot_path && boot_path[0] != '\0') {
        if (has_exe_ext(boot_path)) {
            rc = psx_load_exe(sys, boot_path);
        } else {
            rc = psx_load_cd(sys, boot_path);
        }
        if (rc != PSX_OK) {
            return rc;
        }
    }

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
        } else if (act == CTR_UI_BOOT_PATH) {
            snprintf(current_game, sizeof(current_game), "%s", ui->pending_boot_path);
            if (boot_game_into_system(sys, ui, &hw, current_game) == PSX_OK) {
                ctr_ui_set_status(ui, current_game[0] ? "Game booted" : "Booted BIOS shell");
            } else {
                ctr_ui_set_status(ui, "Failed to load selected title");
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
                case CTR_UI_RESET_SYSTEM:
                    (void)boot_game_into_system(sys, ui, &hw, current_game);
                    ctr_ui_set_status(ui, "System reset");
                    break;
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
            if ((ui_frame_div++ & 3u) == 0u) {
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
