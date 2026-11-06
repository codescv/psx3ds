#include <3ds.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plat_3ds_internal.h"

#define UI_W 320
#define UI_H 240

#define RGB24(r, g, b) (((u32)(r) << 16) | ((u32)(g) << 8) | (u32)(b))

#define COL_BG         RGB24(14, 18, 28)
#define COL_HEADER     RGB24(24, 34, 56)
#define COL_CARD       RGB24(22, 30, 46)
#define COL_BTN        RGB24(34, 48, 76)
#define COL_BTN_ACT    RGB24(48, 108, 184)
#define COL_BORDER     RGB24(64, 88, 128)
#define COL_TEXT       RGB24(236, 242, 250)
#define COL_DIM        RGB24(148, 164, 190)
#define COL_ACCENT     RGB24(92, 196, 255)
#define COL_OK         RGB24(88, 220, 136)
#define COL_WARN       RGB24(250, 196, 76)

static inline void fb_put_px(u8 *fb, int x, int y, u32 rgb) {
    if ((unsigned)x >= UI_W || (unsigned)y >= UI_H) {
        return;
    }
    int idx = (x * 240 + (239 - y)) * 3;
    fb[idx + 0] = (u8)(rgb & 0xFFu);         /* B */
    fb[idx + 1] = (u8)((rgb >> 8) & 0xFFu);  /* G */
    fb[idx + 2] = (u8)((rgb >> 16) & 0xFFu); /* R */
}

static void fb_fill_rect(u8 *fb, int x, int y, int w, int h, u32 rgb) {
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = (x + w) > UI_W ? UI_W : (x + w);
    int y1 = (y + h) > UI_H ? UI_H : (y + h);
    u8 b = (u8)(rgb & 0xFFu);
    u8 g = (u8)((rgb >> 8) & 0xFFu);
    u8 r = (u8)((rgb >> 16) & 0xFFu);

    for (int px = x0; px < x1; px++) {
        u8 *col = fb + px * 240 * 3;
        for (int py = y0; py < y1; py++) {
            int off = (239 - py) * 3;
            col[off + 0] = b;
            col[off + 1] = g;
            col[off + 2] = r;
        }
    }
}

static void fb_draw_border(u8 *fb, int x, int y, int w, int h, u32 rgb) {
    fb_fill_rect(fb, x, y, w, 1, rgb);
    fb_fill_rect(fb, x, y + h - 1, w, 1, rgb);
    fb_fill_rect(fb, x, y, 1, h, rgb);
    fb_fill_rect(fb, x + w - 1, y, 1, h, rgb);
}

static void fb_draw_char(u8 *fb, int x, int y, char ch, u32 fg) {
    u8 code = (u8)ch;
    if (code >= 128u) {
        code = (u8)'?';
    }
    const u8 *glyph = psx3ds_font8x8[code];
    for (int ry = 0; ry < 8; ry++) {
        u8 bits = glyph[ry];
        if (!bits) {
            continue;
        }
        for (int rx = 0; rx < 8; rx++) {
            if (bits & (u8)(0x80u >> rx)) {
                fb_put_px(fb, x + rx, y + ry, fg);
            }
        }
    }
}

static void fb_draw_text(u8 *fb, int x, int y, const char *s, u32 fg, int max_chars) {
    if (!s) {
        return;
    }
    for (int i = 0; s[i] != '\0' && (max_chars <= 0 || i < max_chars); i++) {
        if (x + i * 8 >= UI_W) {
            break;
        }
        fb_draw_char(fb, x + i * 8, y, s[i], fg);
    }
}

static void fb_printf(u8 *fb, int x, int y, u32 fg, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));

static void fb_printf(u8 *fb, int x, int y, u32 fg, const char *fmt, ...) {
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fb_draw_text(fb, x, y, buf, fg, 40);
}

static void fb_draw_button(u8 *fb, int x, int y, int w, int h,
                           const char *label, bool active) {
    fb_fill_rect(fb, x, y, w, h, active ? COL_BTN_ACT : COL_BTN);
    fb_draw_border(fb, x, y, w, h, active ? COL_ACCENT : COL_BORDER);
    int len = (int)strlen(label);
    int tx = x + (w - len * 8) / 2;
    int ty = y + (h - 8) / 2;
    fb_draw_text(fb, tx, ty, label, COL_TEXT, w / 8);
}

static bool has_rom_extension(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) {
        return false;
    }
    char ext[12];
    size_t i = 0u;
    for (; dot[i] != '\0' && i + 1u < sizeof(ext); i++) {
        ext[i] = (char)tolower((unsigned char)dot[i]);
    }
    ext[i] = '\0';
    return strcmp(ext, ".cue") == 0 ||
           strcmp(ext, ".bin") == 0 ||
           strcmp(ext, ".iso") == 0 ||
           strcmp(ext, ".img") == 0 ||
           strcmp(ext, ".exe") == 0 ||
           strcmp(ext, ".psx") == 0;
}

static bool file_exists_512k(const char *path) {
    PlatFile *f = plat_fopen(path, "rb");
    if (!f) {
        return false;
    }
    u64 sz = plat_fsize(f);
    plat_fclose(f);
    return sz == PSX_BIOS_SIZE;
}

static void detect_sdmc_bios(CtrUiState *ui) {
    static const struct {
        const char *path;
        const char *badge;
    } k_bios_candidates[] = {
        { "sdmc:/psx3ds/bios/SCPH1001.BIN", "SCPH1001" },
        { "sdmc:/psx3ds/bios/scph1001.bin", "SCPH1001" },
        { "sdmc:/psx3ds/bios/openbios.bin", "OPENBIOS" },
        { "sdmc:/psx/SCPH1001.BIN",         "SCPH1001" },
        { "sdmc:/psx/scph1001.bin",         "SCPH1001" },
        { "sdmc:/psx/openbios.bin",         "OPENBIOS" },
        { "sdmc:/bios/SCPH1001.BIN",        "SCPH1001" },
        { "sdmc:/bios/openbios.bin",        "OPENBIOS" },
        { "sdmc:/SCPH1001.BIN",             "SCPH1001" },
        { "sdmc:/scph1001.bin",             "SCPH1001" },
        { "sdmc:/openbios.bin",             "OPENBIOS" },
        { "romfs:/openbios.bin",            "OPENBIOS" }
    };
    for (size_t i = 0u; i < sizeof(k_bios_candidates) / sizeof(k_bios_candidates[0]); i++) {
        if (file_exists_512k(k_bios_candidates[i].path)) {
            snprintf(ui->bios_path, sizeof(ui->bios_path), "%s", k_bios_candidates[i].path);
            snprintf(ui->bios_badge, sizeof(ui->bios_badge), "%s", k_bios_candidates[i].badge);
            return;
        }
    }
    ui->bios_path[0] = '\0';
    strcpy(ui->bios_badge, "HLE BIOS");
}

void ctr_ui_scan_roms(CtrUiState *ui) {
    if (!ui) {
        return;
    }
    detect_sdmc_bios(ui);
    ui->rom_count = 0u;
    ui->rom_sel = 0u;
    ui->rom_scroll = 0u;

    static const char *const k_dirs[] = {
        "sdmc:/psx3ds/roms",
        "sdmc:/psx",
        "sdmc:/roms/psx",
        "sdmc:/"
    };

    char (*entries)[256] = (char (*)[256])malloc(128u * 256u);
    if (!entries) {
        return;
    }

    for (size_t d = 0u; d < sizeof(k_dirs) / sizeof(k_dirs[0]) && ui->rom_count < 128u; d++) {
        u32 n = plat_listdir(k_dirs[d], entries, 128u);
        for (u32 i = 0u; i < n && ui->rom_count < 128u; i++) {
            if (!has_rom_extension(entries[i])) {
                continue;
            }
            /* Skip BIOS binaries when scanning root directories */
            if (strstr(entries[i], "SCPH") != NULL || strstr(entries[i], "scph") != NULL ||
                strstr(entries[i], "openbios") != NULL) {
                continue;
            }
            char full[256];
            size_t dlen = strlen(k_dirs[d]);
            if (dlen > 0u && k_dirs[d][dlen - 1u] == '/') {
                snprintf(full, sizeof(full), "%s%.220s", k_dirs[d], entries[i]);
            } else {
                snprintf(full, sizeof(full), "%s/%.220s", k_dirs[d], entries[i]);
            }
            bool dup = false;
            for (u32 k = 0u; k < ui->rom_count; k++) {
                if (strcmp(ui->rom_paths[k], full) == 0) {
                    dup = true;
                    break;
                }
            }
            if (dup) {
                continue;
            }
            snprintf(ui->rom_paths[ui->rom_count], 256u, "%s", full);
            snprintf(ui->rom_labels[ui->rom_count], 64u, "%.63s", entries[i]);
            ui->rom_count++;
        }
    }
    free(entries);
}

void ctr_ui_init(CtrUiState *ui) {
    if (!ui) {
        return;
    }
    memset(ui, 0, sizeof(*ui));
    ui->active_tab = 0u;
    ui->state_slot = 0u;
    ctr_ui_scan_roms(ui);
    ctr_ui_set_status(ui, "Select a PS1 disc or PS-EXE to boot");
}

void ctr_ui_set_status(CtrUiState *ui, const char *msg) {
    if (!ui || !msg) {
        return;
    }
    snprintf(ui->status_msg, sizeof(ui->status_msg), "%s", msg);
    ui->status_timer = 180u;
}

static bool hit_rect(int tx, int ty, int x, int y, int w, int h) {
    return tx >= x && tx < (x + w) && ty >= y && ty < (y + h);
}

CtrUiAction ctr_ui_update(CtrUiState *ui, PsxSystem *sys, const CtrHwInfo *hw,
                          u32 keys_down, u32 keys_held, const touchPosition *touch) {
    (void)hw;
    if (!ui) {
        return CTR_UI_NONE;
    }
    if (ui->status_timer > 0u) {
        ui->status_timer--;
    }

    /* Update Old-3DS touch L2/L3/R3/R2 pads along bottom strip (y = 208..236) */
    u16 touch_pads = 0u;
    if ((keys_held & KEY_TOUCH) && touch && ui->active_tab != 0u) {
        int tx = touch->px;
        int ty = touch->py;
        if (hit_rect(tx, ty, 6, 208, 72, 26))   touch_pads |= 0x0100u; /* L2 */
        if (hit_rect(tx, ty, 84, 208, 72, 26))  touch_pads |= 0x0002u; /* L3 */
        if (hit_rect(tx, ty, 164, 208, 72, 26)) touch_pads |= 0x0004u; /* R3 */
        if (hit_rect(tx, ty, 242, 208, 72, 26)) touch_pads |= 0x0200u; /* R2 */
    }
    ui->touch_extra_buttons = touch_pads;
    ctr_input_set_touch_buttons(touch_pads);

    /* START+SELECT chord switches to Quick Controls tab */
    if ((keys_held & (KEY_START | KEY_SELECT)) == (KEY_START | KEY_SELECT) &&
        (keys_down & (KEY_START | KEY_SELECT))) {
        ui->active_tab = 2u;
        return CTR_UI_NONE;
    }

    bool tapped = (keys_down & KEY_TOUCH) && touch != NULL;
    int tx = tapped ? touch->px : -1;
    int ty = tapped ? touch->py : -1;

    /* Top tab bar (y = 20..40) */
    if (tapped && ty >= 20 && ty < 40) {
        if (tx < 106)      ui->active_tab = 0u;
        else if (tx < 212) ui->active_tab = 1u;
        else               ui->active_tab = 2u;
        return CTR_UI_NONE;
    }

    /* Tab 0: ROM Browser */
    if (ui->active_tab == 0u) {
        if (!ui->game_loaded && (keys_down & KEY_START)) {
            return CTR_UI_QUIT_APP;
        }
        if ((keys_down & KEY_X)) {
            ctr_ui_scan_roms(ui);
            ctr_ui_set_status(ui, "Rescanned SD card ROM directories");
            return CTR_UI_NONE;
        }
        if (ui->rom_count > 0u) {
            if (keys_down & KEY_DUP) {
                ui->rom_sel = (ui->rom_sel > 0u) ? (ui->rom_sel - 1u) : (ui->rom_count - 1u);
            }
            if (keys_down & KEY_DDOWN) {
                ui->rom_sel = (ui->rom_sel + 1u) % ui->rom_count;
            }
            if (ui->rom_sel < ui->rom_scroll) {
                ui->rom_scroll = ui->rom_sel;
            } else if (ui->rom_sel >= ui->rom_scroll + 10u) {
                ui->rom_scroll = ui->rom_sel - 9u;
            }

            if (keys_down & KEY_A) {
                snprintf(ui->pending_boot_path, sizeof(ui->pending_boot_path),
                         "%s", ui->rom_paths[ui->rom_sel]);
                return CTR_UI_BOOT_PATH;
            }
            if (tapped && ty >= 48 && ty < 188) {
                u32 row = ui->rom_scroll + (u32)((ty - 48) / 14);
                if (row < ui->rom_count) {
                    if (ui->rom_sel == row) {
                        snprintf(ui->pending_boot_path, sizeof(ui->pending_boot_path),
                                 "%s", ui->rom_paths[row]);
                        return CTR_UI_BOOT_PATH;
                    }
                    ui->rom_sel = row;
                }
            }
        }
        /* Boot BIOS Shell button at bottom of ROM browser */
        if (tapped && hit_rect(tx, ty, 8, 196, 148, 24)) {
            ui->pending_boot_path[0] = '\0';
            return CTR_UI_BOOT_PATH;
        }
        if (tapped && hit_rect(tx, ty, 164, 196, 148, 24)) {
            ctr_ui_scan_roms(ui);
            ctr_ui_set_status(ui, "Rescanned SD card");
        }
        return CTR_UI_NONE;
    }

    /* Tab 2: Quick Controls */
    if (ui->active_tab == 2u && tapped && sys) {
        if (hit_rect(tx, ty, 8, 48, 148, 28))   return CTR_UI_TOGGLE_GPU;
        if (hit_rect(tx, ty, 164, 48, 148, 28)) return CTR_UI_CYCLE_SCALE;
        if (hit_rect(tx, ty, 8, 82, 148, 28))   return CTR_UI_CYCLE_FRAMESKIP;
        if (hit_rect(tx, ty, 164, 82, 148, 28)) return CTR_UI_TOGGLE_REVERB;

        for (u8 s = 0u; s < 4u; s++) {
            if (hit_rect(tx, ty, 8 + (int)s * 78, 116, 72, 24)) {
                ui->state_slot = s;
                return CTR_UI_NONE;
            }
        }
        if (hit_rect(tx, ty, 8, 146, 148, 26))   return CTR_UI_SAVE_STATE;
        if (hit_rect(tx, ty, 164, 146, 148, 26)) return CTR_UI_LOAD_STATE;
        if (hit_rect(tx, ty, 8, 176, 148, 26))   return CTR_UI_RESET_SYSTEM;
        if (hit_rect(tx, ty, 164, 176, 148, 26)) {
            ui->active_tab = 0u;
            return CTR_UI_OPEN_BROWSER;
        }
    }

    return CTR_UI_NONE;
}

static const char *cpu_engine_badge(const PsxSystem *sys) {
    if (!sys) {
        return "IDLE";
    }
    if (sys->cpu_mode == (u32)CPU_JIT) {
        if (sys->jit.mem.mode == JIT_MEM_MIRROR_RWRX) {
            return "JIT MIRROR";
        }
        return "JIT RWX";
    }
    if (sys->cpu_mode == (u32)CPU_CACHED) {
        return "CACHED";
    }
    return "INTERP";
}

void ctr_ui_render(const CtrUiState *ui, const PsxSystem *sys, const CtrHwInfo *hw) {
    if (!ui) {
        return;
    }
    u16 fb_w = 0u;
    u16 fb_h = 0u;
    u8 *fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &fb_w, &fb_h);
    if (!fb) {
        return;
    }

    fb_fill_rect(fb, 0, 0, UI_W, UI_H, COL_BG);

    /* Top Header & Hardware Badges */
    fb_fill_rect(fb, 0, 0, UI_W, 19, COL_HEADER);
    fb_draw_text(fb, 6, 6, "PSX3DS", COL_ACCENT, 8);
    if (hw) {
        fb_printf(fb, 62, 6, hw->is_n3ds ? COL_OK : COL_WARN,
                  "%s %uMHz", hw->is_n3ds ? "N3DS" : "O3DS", (unsigned)hw->cpu_mhz);
    }
    fb_printf(fb, 184, 6, ui->bios_path[0] ? COL_OK : COL_WARN, "[%s]", ui->bios_badge);

    /* Tab Bar */
    fb_draw_button(fb, 2,   20, 104, 19, "1:ROMS", ui->active_tab == 0u);
    fb_draw_button(fb, 108, 20, 104, 19, "2:HUD",  ui->active_tab == 1u);
    fb_draw_button(fb, 214, 20, 104, 19, "3:CTRL", ui->active_tab == 2u);

    if (ui->active_tab == 0u) {
        /* SD Card ROM Browser */
        fb_fill_rect(fb, 6, 44, 308, 146, COL_CARD);
        fb_draw_border(fb, 6, 44, 308, 146, COL_BORDER);

        if (ui->rom_count == 0u) {
            fb_draw_text(fb, 16, 68, "No PS1 discs/EXEs found on SD.", COL_WARN, 36);
            fb_draw_text(fb, 16, 86, "Place .cue/.bin/.iso/.exe in:", COL_DIM, 36);
            fb_draw_text(fb, 16, 102, "  sdmc:/psx/ or sdmc:/psx3ds/roms/", COL_TEXT, 36);
            fb_draw_text(fb, 16, 122, "Place SCPH1001.BIN in sdmc:/psx/", COL_DIM, 36);
            fb_draw_text(fb, 16, 138, "(Built-in HLE BIOS ready!)", COL_OK, 36);
        } else {
            for (u32 i = 0u; i < 10u && (ui->rom_scroll + i) < ui->rom_count; i++) {
                u32 idx = ui->rom_scroll + i;
                int ry = 48 + (int)i * 14;
                bool sel = (idx == ui->rom_sel);
                if (sel) {
                    fb_fill_rect(fb, 8, ry - 1, 304, 13, COL_BTN_ACT);
                    fb_draw_char(fb, 10, ry + 1, '\x10', COL_ACCENT);
                }
                fb_draw_text(fb, 20, ry + 1, ui->rom_labels[idx],
                             sel ? COL_TEXT : COL_DIM, 36);
            }
        }
        fb_draw_button(fb, 8,   196, 148, 24, "Boot BIOS/HLE", false);
        fb_draw_button(fb, 164, 196, 148, 24, "Rescan SD (X)", false);
        fb_draw_text(fb, 8, 226, ui->status_msg, COL_DIM, 38);
    } else if (ui->active_tab == 1u) {
        /* Live HUD & Multi-Core Telemetry */
        fb_fill_rect(fb, 6, 44, 308, 158, COL_CARD);
        fb_draw_border(fb, 6, 44, 308, 158, COL_BORDER);

        u32 us_frame = sys ? sys->stats.us_frame : 0u;
        double fps = us_frame > 0u ? (1000000.0 / (double)us_frame) : 0.0;
        double ms_frame = (double)us_frame / 1000.0;
        double ms_cpu = sys ? ((double)sys->stats.us_cpu / 1000.0) : 0.0;
        double ms_gpu = sys ? ((double)sys->stats.us_gpu / 1000.0) : 0.0;

        u64 tc_total = sys ? ((u64)sys->stats.gpu_tex_hits + (u64)sys->stats.gpu_tex_uploads) : 0ULL;
        double tc_pct = tc_total ? (100.0 * (double)sys->stats.gpu_tex_hits / (double)tc_total) : 100.0;

        u64 ibtc_tot = sys ? ((u64)sys->stats.ibtc_hits + (u64)sys->stats.ibtc_misses) : 0ULL;
        double ibtc_pct = ibtc_tot ? (100.0 * (double)sys->stats.ibtc_hits / (double)ibtc_tot) : 0.0;

        const char *gpu_name = (sys && sys->cfg.gpu_backend == GPU_BACKEND_HW)
                                   ? "PICA200 HW"
                                   : "ARMv6 SW";

        fb_printf(fb, 12, 50, COL_OK,     "FPS: %5.1f  Frame: %5.2f ms", fps, ms_frame);
        fb_printf(fb, 12, 64, COL_TEXT,   "CPU: %-10s (%u MHz %s)",
                  cpu_engine_badge(sys),
                  hw ? (unsigned)hw->cpu_mhz : 268u,
                  (hw && hw->is_n3ds) ? "N3DS" : "O3DS");
        fb_printf(fb, 12, 78, COL_DIM,    "JIT: %u blks  %u KiB  IBTC %4.1f%%",
                  sys ? (unsigned)sys->stats.jit_blocks_live : 0u,
                  sys ? (unsigned)(sys->stats.jit_code_bytes / 1024u) : 0u,
                  ibtc_pct);
        fb_printf(fb, 12, 92, COL_TEXT,   "GPU: %-10s  Batches: %u",
                  gpu_name,
                  sys ? (unsigned)sys->stats.gpu_batches : 0u);
        fb_printf(fb, 12, 106, COL_DIM,   "Prims: %u  TexCache Hit: %5.1f%%",
                  sys ? (unsigned)sys->stats.gpu_prims : 0u,
                  tc_pct);
        fb_printf(fb, 12, 120, COL_DIM,   "Time: CPU %4.1fms  GPU %4.1fms", ms_cpu, ms_gpu);
        fb_printf(fb, 12, 136, (hw && hw->spu_core1_active) ? COL_OK : COL_WARN,
                  "Core1 SPU: %-4s  NDSP: %-4s",
                  (hw && hw->spu_core1_active) ? "ASYNC" : "SYNC",
                  (hw && hw->ndsp_ok) ? "OK" : "MUTE");
        fb_printf(fb, 12, 150, (hw && hw->cd_worker_core >= 0) ? COL_OK : COL_DIM,
                  "CD Readahead: Core %d (Miss %u)",
                  hw ? hw->cd_worker_core : -1,
                  sys ? (unsigned)sys->stats.cd_ra_misses : 0u);
        fb_printf(fb, 12, 166, COL_ACCENT, "%s", ui->status_msg);

        /* Old-3DS Touch L2/L3/R3/R2 Virtual Pads */
        fb_draw_button(fb, 6,   208, 72, 26, "L2", (ui->touch_extra_buttons & 0x0100u) != 0u);
        fb_draw_button(fb, 84,  208, 72, 26, "L3", (ui->touch_extra_buttons & 0x0002u) != 0u);
        fb_draw_button(fb, 164, 208, 72, 26, "R3", (ui->touch_extra_buttons & 0x0004u) != 0u);
        fb_draw_button(fb, 242, 208, 72, 26, "R2", (ui->touch_extra_buttons & 0x0200u) != 0u);
    } else {
        /* Tab 2: Touch & D-Pad Quick Controls */
        char gpu_lbl[24];
        char scl_lbl[24];
        char skp_lbl[24];
        char rev_lbl[24];

        bool is_hw = sys && sys->cfg.gpu_backend == GPU_BACKEND_HW;
        snprintf(gpu_lbl, sizeof(gpu_lbl), "GPU: %s", is_hw ? "PICA HW" : "ARM SW");

        const char *sname = "4:3";
        if (sys && sys->cfg.scale == PSX_SCALE_STRETCH)     sname = "Wide";
        else if (sys && sys->cfg.scale == PSX_SCALE_1X1)    sname = "1:1";
        snprintf(scl_lbl, sizeof(scl_lbl), "Aspect: %s", sname);

        snprintf(skp_lbl, sizeof(skp_lbl), "Frameskip: %u",
                 sys ? (unsigned)sys->cfg.frameskip : 0u);
        snprintf(rev_lbl, sizeof(rev_lbl), "Reverb: %s",
                 (sys && sys->cfg.spu_reverb) ? "ON" : "OFF");

        fb_draw_button(fb, 8,   48, 148, 28, gpu_lbl, is_hw);
        fb_draw_button(fb, 164, 48, 148, 28, scl_lbl, false);
        fb_draw_button(fb, 8,   82, 148, 28, skp_lbl, false);
        fb_draw_button(fb, 164, 82, 148, 28, rev_lbl, sys && sys->cfg.spu_reverb);

        for (u8 s = 0u; s < 4u; s++) {
            char sl[16];
            snprintf(sl, sizeof(sl), "Slot %u", (unsigned)s);
            fb_draw_button(fb, 8 + (int)s * 78, 116, 72, 24, sl, ui->state_slot == s);
        }

        fb_draw_button(fb, 8,   146, 148, 26, "Save State", false);
        fb_draw_button(fb, 164, 146, 148, 26, "Load State", false);
        fb_draw_button(fb, 8,   176, 148, 26, "Reset PS1",  false);
        fb_draw_button(fb, 164, 176, 148, 26, "Load Game",  false);

        /* Old-3DS Touch L2/L3/R3/R2 Virtual Pads */
        fb_draw_button(fb, 6,   208, 72, 26, "L2", (ui->touch_extra_buttons & 0x0100u) != 0u);
        fb_draw_button(fb, 84,  208, 72, 26, "L3", (ui->touch_extra_buttons & 0x0002u) != 0u);
        fb_draw_button(fb, 164, 208, 72, 26, "R3", (ui->touch_extra_buttons & 0x0004u) != 0u);
        fb_draw_button(fb, 242, 208, 72, 26, "R2", (ui->touch_extra_buttons & 0x0200u) != 0u);
    }

    GSPGPU_FlushDataCache(fb, UI_W * UI_H * 3u);
    gfxScreenSwapBuffers(GFX_BOTTOM, false);
}
