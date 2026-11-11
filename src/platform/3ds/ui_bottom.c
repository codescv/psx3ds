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

static bool is_chd_filename(const char *name) {
    if (!name) {
        return false;
    }
    const char *dot = strrchr(name, '.');
    if (!dot || dot[1] == '\0') {
        return false;
    }
    return tolower((unsigned char)dot[1]) == 'c' &&
           tolower((unsigned char)dot[2]) == 'h' &&
           tolower((unsigned char)dot[3]) == 'd' &&
           dot[4] == '\0';
}

/* Case-insensitive prefix/substring checks to keep BIOS images out of the ROM list. */
static bool name_contains_ci(const char *name, const char *needle) {
    size_t nl = strlen(needle);
    for (const char *p = name; *p != '\0'; p++) {
        size_t i = 0u;
        while (i < nl && p[i] != '\0' &&
               tolower((unsigned char)p[i]) == (unsigned char)needle[i]) {
            i++;
        }
        if (i == nl) {
            return true;
        }
    }
    return false;
}

static bool looks_like_bios_file(const char *name) {
    return name_contains_ci(name, "scph") || name_contains_ci(name, "psxonpsp") ||
           name_contains_ci(name, "openbios") ||
           (tolower((unsigned char)name[0]) == 'b' && tolower((unsigned char)name[1]) == 'i' &&
            tolower((unsigned char)name[2]) == 'o' && tolower((unsigned char)name[3]) == 's');
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
    return strcmp(ext, ".chd") == 0 ||
           strcmp(ext, ".cue") == 0 ||
           strcmp(ext, ".bin") == 0 ||
           strcmp(ext, ".iso") == 0 ||
           strcmp(ext, ".img") == 0 ||
           strcmp(ext, ".exe") == 0 ||
           strcmp(ext, ".psx") == 0;
}

/*
 * BIOS selection (v0.2.2): an external 512 KiB SD BIOS (SCPH*.BIN, PSXONPSP*.BIN, BIOS*.BIN,
 * openbios.bin in sdmc:/psx3ds/bios, sdmc:/bios, sdmc:/psx3ds/roms, sdmc:/roms/psx, sdmc:/psx) wins;
 * the SD root is never scanned (v0.2.3); else the
 * embedded OpenBIOS ("builtin") is used. HLE Direct is an explicit opt-in (ui->bios_mode).
 */
static void detect_sdmc_bios(CtrUiState *ui) {
    char path[256];
    char badge[32];
    path[0] = '\0';
    badge[0] = '\0';
    if (psx_bios_find_external(path, (u32)sizeof(path), badge, (u32)sizeof(badge)) &&
        path[0] != '\0') {
        snprintf(ui->bios_path, sizeof(ui->bios_path), "%s", path);
        snprintf(ui->bios_badge, sizeof(ui->bios_badge), "%s",
                 badge[0] != '\0' ? badge : "EXT BIOS");
    } else {
        snprintf(ui->bios_path, sizeof(ui->bios_path), "%s", PSX_BIOS_BUILTIN_NAME);
        snprintf(ui->bios_badge, sizeof(ui->bios_badge), "%s", "OPENBIOS");
    }
}

/* Badge text + colour shown in the header for the current BIOS mode. */
static const char *bios_badge_text(const CtrUiState *ui) {
    return ui->bios_mode == CTR_BIOS_MODE_HLE ? "HLE BIOS" : ui->bios_badge;
}

void ctr_ui_scan_roms(CtrUiState *ui) {
    if (!ui) {
        return;
    }
    detect_sdmc_bios(ui);
    ui->rom_count = 0u;
    ui->rom_sel = 0u;
    ui->rom_scroll = 0u;

    /* v0.2.3: only dedicated PS1 ROM directories; never walk the whole SD card root. */
    static const char *const k_dirs[] = {
        "sdmc:/psx3ds/roms",
        "sdmc:/roms/psx",
        "sdmc:/psx",
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
            /* Skip BIOS binaries that share a ROM directory */
            if (looks_like_bios_file(entries[i])) {
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

void ctr_ui_reset_frame_stats(CtrUiState *ui) {
    if (!ui) {
        return;
    }
    memset(ui->frame_us_ring, 0, sizeof(ui->frame_us_ring));
    memset(ui->cpu_us_ring, 0, sizeof(ui->cpu_us_ring));
    memset(ui->gpu_us_ring, 0, sizeof(ui->gpu_us_ring));
    ui->frame_ring_pos = 0u;
    ui->frame_ring_count = 0u;
}

void ctr_ui_record_frame_time(CtrUiState *ui, u32 paced_us, u32 cpu_us, u32 gpu_us) {
    if (!ui) {
        return;
    }
    u32 pos = ui->frame_ring_pos % CTR_UI_FRAME_RING;
    ui->frame_us_ring[pos] = paced_us;
    ui->cpu_us_ring[pos] = cpu_us;
    ui->gpu_us_ring[pos] = gpu_us;
    ui->frame_ring_pos = (u8)((pos + 1u) % CTR_UI_FRAME_RING);
    if (ui->frame_ring_count < CTR_UI_FRAME_RING) {
        ui->frame_ring_count++;
    }
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

/*
 * Bottom-screen geometry (320x240):
 *   y  0..18  header (version, N3DS/O3DS, BIOS badge)
 *   y 20..38  tab bar: 4 tabs at x = 2 + i*79, w = 77  (1:ROMS | 2:HUD | 3:CTRL | 4:LOG)
 *   y 44..202 tab body
 *   y 208..233 HUD/CTRL: Old-3DS L2/L3/R3/R2 touch pads; LOG: Dump/Clear/Up/Down buttons
 */
#define TAB_X0      2
#define TAB_PITCH   79
#define TAB_BTN_W   77
#define PAD_Y       208
#define PAD_H       26
#define PAD_W       72
#define LOG_ROWS    12u
#define LOG_ROW_Y0  47
#define LOG_ROW_DY  12
#define LOG_COLS    39
#define LOG_PAGE    6u

static const int k_pad_x[4] = { 6, 84, 164, 242 };

static u32 log_max_scroll(void) {
    u32 n = ctr_log_count();
    return n > LOG_ROWS ? (n - LOG_ROWS) : 0u;
}

static void log_scroll_by(CtrUiState *ui, s32 delta) {
    s32 s = (s32)ui->log_scroll + delta;
    if (s < 0) {
        s = 0;
    }
    u32 max_s = log_max_scroll();
    ui->log_scroll = ((u32)s > max_s) ? max_s : (u32)s;
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

    /* Old-3DS touch L2/L3/R3/R2 pads along bottom strip (y = 208..233), HUD + CTRL tabs only */
    u16 touch_pads = 0u;
    bool pads_tab = (ui->active_tab == CTR_UI_TAB_HUD || ui->active_tab == CTR_UI_TAB_CTRL);
    if ((keys_held & KEY_TOUCH) && touch && pads_tab) {
        int tx = touch->px;
        int ty = touch->py;
        if (hit_rect(tx, ty, k_pad_x[0], PAD_Y, PAD_W, PAD_H)) touch_pads |= 0x0100u; /* L2 */
        if (hit_rect(tx, ty, k_pad_x[1], PAD_Y, PAD_W, PAD_H)) touch_pads |= 0x0002u; /* L3 */
        if (hit_rect(tx, ty, k_pad_x[2], PAD_Y, PAD_W, PAD_H)) touch_pads |= 0x0004u; /* R3 */
        if (hit_rect(tx, ty, k_pad_x[3], PAD_Y, PAD_W, PAD_H)) touch_pads |= 0x0200u; /* R2 */
    }
    ui->touch_extra_buttons = touch_pads;
    ctr_input_set_touch_buttons(touch_pads);

    /* START+SELECT chord switches to Quick Controls tab */
    if ((keys_held & (KEY_START | KEY_SELECT)) == (KEY_START | KEY_SELECT) &&
        (keys_down & (KEY_START | KEY_SELECT))) {
        ui->active_tab = CTR_UI_TAB_CTRL;
        return CTR_UI_NONE;
    }

    /* SELECT alone cycles tabs while no game is running (SELECT is a PS1 button otherwise) */
    if (!ui->game_loaded && (keys_down & KEY_SELECT) && !(keys_held & KEY_START)) {
        ui->active_tab = (u8)((ui->active_tab + 1u) % CTR_UI_TAB_COUNT);
        return CTR_UI_NONE;
    }

    bool tapped = (keys_down & KEY_TOUCH) && touch != NULL;
    int tx = tapped ? touch->px : -1;
    int ty = tapped ? touch->py : -1;

    /* Top tab bar (y = 20..39) */
    if (tapped && ty >= 20 && ty < 40) {
        int t = (tx - TAB_X0) / TAB_PITCH;
        if (t < 0) t = 0;
        if (t >= (int)CTR_UI_TAB_COUNT) t = (int)CTR_UI_TAB_COUNT - 1;
        ui->active_tab = (u8)t;
        return CTR_UI_NONE;
    }

    /* Tab 1: ROM Browser */
    if (ui->active_tab == CTR_UI_TAB_ROMS) {
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

    /* Tab 3: Quick Controls */
    if (ui->active_tab == CTR_UI_TAB_CTRL && tapped) {
        /* These work without a running game */
        if (hit_rect(tx, ty, 8, 182, 148, 20)) {
            return CTR_UI_TOGGLE_BIOS_MODE;
        }
        if (hit_rect(tx, ty, 164, 182, 148, 20)) {
            return CTR_UI_TOGGLE_CD_BOOT;
        }
        for (u8 s = 0u; s < 4u; s++) {
            if (hit_rect(tx, ty, 8 + (int)s * 78, 102, 72, 22)) {
                ui->state_slot = s;
                return CTR_UI_NONE;
            }
        }
        if (hit_rect(tx, ty, 164, 156, 148, 22)) {
            ui->active_tab = CTR_UI_TAB_ROMS;
            return CTR_UI_OPEN_BROWSER;
        }
        if (sys) {
            if (hit_rect(tx, ty, 8, 46, 148, 24))    return CTR_UI_TOGGLE_GPU;
            if (hit_rect(tx, ty, 164, 46, 148, 24))  return CTR_UI_CYCLE_SCALE;
            if (hit_rect(tx, ty, 8, 74, 148, 24))    return CTR_UI_CYCLE_FRAMESKIP;
            if (hit_rect(tx, ty, 164, 74, 148, 24))  return CTR_UI_TOGGLE_REVERB;
            if (hit_rect(tx, ty, 8, 128, 148, 24))   return CTR_UI_SAVE_STATE;
            if (hit_rect(tx, ty, 164, 128, 148, 24)) return CTR_UI_LOAD_STATE;
            if (hit_rect(tx, ty, 8, 156, 148, 22))   return CTR_UI_RESET_SYSTEM;
        }
        return CTR_UI_NONE;
    }

    /* Tab 4: Log viewer (usable with or without a game) */
    if (ui->active_tab == CTR_UI_TAB_LOG) {
        /* D-Pad scrolls only while no game runs: with a game it belongs to the PS1 pad. */
        if (!ui->game_loaded) {
            if (keys_down & KEY_DUP)    log_scroll_by(ui, 1);
            if (keys_down & KEY_DDOWN)  log_scroll_by(ui, -1);
            if (keys_down & KEY_DLEFT)  log_scroll_by(ui, (s32)LOG_ROWS);
            if (keys_down & KEY_DRIGHT) log_scroll_by(ui, -(s32)LOG_ROWS);
        }
        if (tapped) {
            if (hit_rect(tx, ty, k_pad_x[0], PAD_Y, PAD_W, PAD_H)) {
                if (ctr_log_dump(CTR_LOG_DUMP_PATH)) {
                    ctr_ui_set_status(ui, "Saved " CTR_LOG_DUMP_PATH);
                    plat_log("[ui] log dumped to %s\n", CTR_LOG_DUMP_PATH);
                } else {
                    ctr_ui_set_status(ui, "Log dump failed (SD card?)");
                }
            } else if (hit_rect(tx, ty, k_pad_x[1], PAD_Y, PAD_W, PAD_H)) {
                ctr_log_clear();
                ui->log_scroll = 0u;
                ctr_ui_set_status(ui, "Log cleared (SD log file kept)");
            } else if (hit_rect(tx, ty, k_pad_x[2], PAD_Y, PAD_W, PAD_H)) {
                log_scroll_by(ui, (s32)LOG_PAGE);
            } else if (hit_rect(tx, ty, k_pad_x[3], PAD_Y, PAD_W, PAD_H)) {
                log_scroll_by(ui, -(s32)LOG_PAGE);
            }
        }
        log_scroll_by(ui, 0); /* re-clamp after ring changes */
        return CTR_UI_NONE;
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
    fb_draw_text(fb, 58, 6, "v" PSX3DS_VERSION_STR, COL_DIM, 8);
    if (hw) {
        fb_printf(fb, 120, 6, hw->is_n3ds ? COL_OK : COL_WARN,
                  "%s %uMHz", hw->is_n3ds ? "N3DS" : "O3DS", (unsigned)hw->cpu_mhz);
    }
    fb_printf(fb, 232, 6, ui->bios_mode == CTR_BIOS_MODE_HLE ? COL_WARN : COL_OK,
              "[%.8s]", bios_badge_text(ui));

    /* Tab Bar */
    static const char *const k_tab_labels[CTR_UI_TAB_COUNT] = {
        "1:ROMS", "2:HUD", "3:CTRL", "4:LOG"
    };
    for (u32 t = 0u; t < CTR_UI_TAB_COUNT; t++) {
        fb_draw_button(fb, TAB_X0 + (int)t * TAB_PITCH, 20, TAB_BTN_W, 19, k_tab_labels[t],
                       ui->active_tab == t);
    }

    if (ui->active_tab == CTR_UI_TAB_ROMS) {
        /* SD Card ROM Browser */
        fb_fill_rect(fb, 6, 44, 308, 146, COL_CARD);
        fb_draw_border(fb, 6, 44, 308, 146, COL_BORDER);

        if (ui->rom_count == 0u) {
            bool external = strcmp(ui->bios_path, PSX_BIOS_BUILTIN_NAME) != 0 &&
                            ui->bios_path[0] != '\0';
            fb_draw_text(fb, 16, 64, "No PS1 discs/EXEs found on SD.", COL_WARN, 36);
            fb_draw_text(fb, 16, 82, "Place .chd/.cue/.bin/.iso/.exe in:", COL_DIM, 36);
            fb_draw_text(fb, 16, 98, "  sdmc:/psx3ds/roms/ sdmc:/roms/psx/", COL_TEXT, 36);
            if (ui->bios_mode == CTR_BIOS_MODE_HLE) {
                fb_draw_text(fb, 16, 118, "HLE Direct BIOS (3:CTRL to change)", COL_WARN, 36);
            } else if (external) {
                fb_printf(fb, 16, 118, COL_OK, "SD BIOS ready: %.20s", ui->bios_badge);
            } else {
                fb_draw_text(fb, 16, 118, "Built-in OpenBIOS ready!", COL_OK, 36);
            }
            fb_draw_text(fb, 16, 136, "Optional: SCPH*.BIN in", COL_DIM, 36);
            fb_draw_text(fb, 16, 152, "  sdmc:/psx3ds/bios/", COL_TEXT, 36);
            fb_draw_text(fb, 16, 172, "SELECT: next tab  4:LOG = debug log", COL_DIM, 36);
        } else {
            for (u32 i = 0u; i < 10u && (ui->rom_scroll + i) < ui->rom_count; i++) {
                u32 idx = ui->rom_scroll + i;
                int ry = 48 + (int)i * 14;
                bool sel = (idx == ui->rom_sel);
                bool chd = is_chd_filename(ui->rom_paths[idx]);
                if (sel) {
                    fb_fill_rect(fb, 8, ry - 1, 304, 13, COL_BTN_ACT);
                    fb_draw_char(fb, 10, ry + 1, '\x10', COL_ACCENT);
                }
                fb_draw_text(fb, 20, ry + 1, ui->rom_labels[idx],
                             sel ? COL_TEXT : COL_DIM, chd ? 30 : 36);
                if (chd) {
                    fb_draw_text(fb, 268, ry + 1, "[CHD]", sel ? COL_OK : COL_ACCENT, 5);
                }
            }
        }
        fb_draw_button(fb, 8,   196, 148, 24, "Boot BIOS Shell", false);
        fb_draw_button(fb, 164, 196, 148, 24, "Rescan SD (X)", false);
        fb_draw_text(fb, 8, 226, ui->status_msg, COL_DIM, 38);
    } else if (ui->active_tab == CTR_UI_TAB_HUD) {
        /* Live HUD & Multi-Core Telemetry (12 px rows, y 48..196) */
        fb_fill_rect(fb, 6, 44, 308, 158, COL_CARD);
        fb_draw_border(fb, 6, 44, 308, 158, COL_BORDER);

        u32 us_frame = sys ? sys->stats.us_frame : 0u;
        double fps = us_frame > 0u ? (1000000.0 / (double)us_frame) : 0.0;
        double ms_frame = (double)us_frame / 1000.0;
        double ms_cpu = sys ? ((double)sys->stats.us_cpu / 1000.0) : 0.0;
        double ms_gpu = sys ? ((double)sys->stats.us_gpu / 1000.0) : 0.0;
        if (sys && ui->frame_ring_count > 0u) {
            /*
             * v0.2.6: rolling average of the last <= 32 PACED frames, so the heavy even and
             * empty odd frames of 30 Hz VSync(2) games are weighted equally.
             */
            u64 sum_f = 0u, sum_c = 0u, sum_g = 0u;
            u32 n = ui->frame_ring_count;
            for (u32 i = 0u; i < n; i++) {
                sum_f += ui->frame_us_ring[i];
                sum_c += ui->cpu_us_ring[i];
                sum_g += ui->gpu_us_ring[i];
            }
            ms_frame = (double)sum_f / (double)n / 1000.0;
            ms_cpu = (double)sum_c / (double)n / 1000.0;
            ms_gpu = (double)sum_g / (double)n / 1000.0;
            fps = sum_f > 0u ? (1000000.0 * (double)n / (double)sum_f) : 0.0;
            /* Sleep wake-up jitter must never read as 60.2 FPS under the 60 Hz lock */
            if (fps >= 60.0) {
                fps = 60.0;
                if (ms_frame < 16.67) {
                    ms_frame = 16.67;
                }
            }
        }

        u64 tc_total = sys ? ((u64)sys->stats.gpu_tex_hits + (u64)sys->stats.gpu_tex_uploads) : 0ULL;
        double tc_pct = tc_total ? (100.0 * (double)sys->stats.gpu_tex_hits / (double)tc_total) : 100.0;

        u64 ibtc_tot = sys ? ((u64)sys->stats.ibtc_hits + (u64)sys->stats.ibtc_misses) : 0ULL;
        double ibtc_pct = ibtc_tot ? (100.0 * (double)sys->stats.ibtc_hits / (double)ibtc_tot) : 0.0;

        const char *gpu_name = (sys && sys->cfg.gpu_backend == GPU_BACKEND_HW)
                                   ? "PICA200 HW"
                                   : "ARMv6 SW";

        fb_printf(fb, 12, 48, COL_OK,     "FPS: %5.1f  Frame: %5.2f ms", fps, ms_frame);
        fb_printf(fb, 12, 60, COL_TEXT,   "CPU: %-10s (%u MHz %s)",
                  cpu_engine_badge(sys),
                  hw ? (unsigned)hw->cpu_mhz : 268u,
                  (hw && hw->is_n3ds) ? "N3DS" : "O3DS");
        fb_printf(fb, 12, 72, COL_DIM,    "JIT: %u blks  %u KiB  IBTC %4.1f%%",
                  sys ? (unsigned)sys->stats.jit_blocks_live : 0u,
                  sys ? (unsigned)(sys->stats.jit_code_bytes / 1024u) : 0u,
                  ibtc_pct);
        fb_printf(fb, 12, 84, COL_TEXT,   "GPU: %-10s  Batches: %u",
                  gpu_name,
                  sys ? (unsigned)sys->stats.gpu_batches : 0u);
        fb_printf(fb, 12, 96, COL_DIM,    "Prims: %u  TexCache Hit: %5.1f%%",
                  sys ? (unsigned)sys->stats.gpu_prims : 0u,
                  tc_pct);
        fb_printf(fb, 12, 108, COL_DIM,   "Time: CPU %4.1fms  GPU %4.1fms", ms_cpu, ms_gpu);
        /* v0.2.7: live NDSP diagnostics — wavebufs submitted / in flight, PCM peak */
        u32 wb_sub = 0u, wb_act = 0u, pcm_pk = 0u;
        ctr_audio_get_diag(&wb_sub, &wb_act, &pcm_pk);
        fb_printf(fb, 12, 120, (hw && hw->ndsp_ok && wb_sub > 0u) ? COL_OK : COL_WARN,
                  "SPU:%-3s NDSP:%-2s WB:%u/%u Pk:%u",
                  !(hw && hw->spu_core1_active) ? "SYN"
                      : (hw->spu_worker_core == 2) ? "C2" : "C1",
                  (hw && hw->ndsp_ok) ? "OK" : "NO",
                  (unsigned)wb_sub, (unsigned)wb_act, (unsigned)pcm_pk);
        fb_printf(fb, 12, 132, (hw && hw->cd_worker_core >= 0) ? COL_OK : COL_DIM,
                  cdrom_is_chd(sys) ? "CD [CHD]: Core %d (Miss %u)"
                                    : "CD Readahead: Core %d (Miss %u)",
                  hw ? hw->cd_worker_core : -1,
                  sys ? (unsigned)sys->stats.cd_ra_misses : 0u);
        if (sys) {
            const GpuDisplay *d = &sys->gpu.disp;
            fb_printf(fb, 12, 144, COL_TEXT, "PC:%08X CD Rd:%u IRQ:%03X/%03X",
                      (unsigned)sys->cpu.pc, (unsigned)sys->cdrom.sectors_read,
                      (unsigned)(sys->irq.stat & 0x7FFu), (unsigned)(sys->irq.mask & 0x7FFu));
            fb_printf(fb, 12, 156, d->blank ? COL_WARN : COL_OK,
                      "Disp:%-3s %ux%u%s %s VRAM %u,%u",
                      d->blank ? "OFF" : "ON",
                      (unsigned)d->hres, (unsigned)d->vres, d->interlace ? "i" : "",
                      d->color24 ? "24b" : "15b",
                      (unsigned)d->vram_x, (unsigned)d->vram_y);
        } else {
            fb_draw_text(fb, 12, 144, "PC:-------- CD Rd:- IRQ:---/---", COL_DIM, 38);
            fb_draw_text(fb, 12, 156, "Disp: (no game running)", COL_DIM, 38);
        }
        fb_printf(fb, 12, 172, COL_ACCENT, "%.37s", ui->status_msg);
        fb_printf(fb, 12, 186, COL_DIM, "BIOS: %s  CD Boot: %s",
                  ui->bios_mode == CTR_BIOS_MODE_HLE ? "HLE Direct" : ui->bios_badge,
                  ui->bios_mode == CTR_BIOS_MODE_HLE ? "HLE"
                                                     : (ui->cd_fast_boot ? "Fast" : "Native"));

        /* Old-3DS Touch L2/L3/R3/R2 Virtual Pads */
        fb_draw_button(fb, k_pad_x[0], PAD_Y, PAD_W, PAD_H, "L2", (ui->touch_extra_buttons & 0x0100u) != 0u);
        fb_draw_button(fb, k_pad_x[1], PAD_Y, PAD_W, PAD_H, "L3", (ui->touch_extra_buttons & 0x0002u) != 0u);
        fb_draw_button(fb, k_pad_x[2], PAD_Y, PAD_W, PAD_H, "R3", (ui->touch_extra_buttons & 0x0004u) != 0u);
        fb_draw_button(fb, k_pad_x[3], PAD_Y, PAD_W, PAD_H, "R2", (ui->touch_extra_buttons & 0x0200u) != 0u);
    } else if (ui->active_tab == CTR_UI_TAB_CTRL) {
        /* Tab 3: Touch & D-Pad Quick Controls */
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

        fb_draw_button(fb, 8,   46, 148, 24, gpu_lbl, is_hw);
        fb_draw_button(fb, 164, 46, 148, 24, scl_lbl, false);
        fb_draw_button(fb, 8,   74, 148, 24, skp_lbl, false);
        fb_draw_button(fb, 164, 74, 148, 24, rev_lbl, sys && sys->cfg.spu_reverb);

        for (u8 s = 0u; s < 4u; s++) {
            char sl[16];
            snprintf(sl, sizeof(sl), "Slot %u", (unsigned)s);
            fb_draw_button(fb, 8 + (int)s * 78, 102, 72, 22, sl, ui->state_slot == s);
        }

        fb_draw_button(fb, 8,   128, 148, 24, "Save State", false);
        fb_draw_button(fb, 164, 128, 148, 24, "Load State", false);
        fb_draw_button(fb, 8,   156, 148, 22, "Reset PS1",  false);
        fb_draw_button(fb, 164, 156, 148, 22, "Load Game",  false);
        /* Both apply on next boot/reset; CD Boot only matters with a real/OpenBIOS BIOS */
        fb_draw_button(fb, 8,   182, 148, 20,
                       ui->bios_mode == CTR_BIOS_MODE_HLE ? "BIOS: HLE Direct" : "BIOS:Auto/OpenBIOS",
                       ui->bios_mode == CTR_BIOS_MODE_HLE);
        fb_draw_button(fb, 164, 182, 148, 20,
                       ui->cd_fast_boot ? "CD Boot: Fast" : "CD Boot: Native",
                       ui->cd_fast_boot);

        /* Old-3DS Touch L2/L3/R3/R2 Virtual Pads */
        fb_draw_button(fb, k_pad_x[0], PAD_Y, PAD_W, PAD_H, "L2", (ui->touch_extra_buttons & 0x0100u) != 0u);
        fb_draw_button(fb, k_pad_x[1], PAD_Y, PAD_W, PAD_H, "L3", (ui->touch_extra_buttons & 0x0002u) != 0u);
        fb_draw_button(fb, k_pad_x[2], PAD_Y, PAD_W, PAD_H, "R3", (ui->touch_extra_buttons & 0x0004u) != 0u);
        fb_draw_button(fb, k_pad_x[3], PAD_Y, PAD_W, PAD_H, "R2", (ui->touch_extra_buttons & 0x0200u) != 0u);
    } else {
        /* Tab 4: on-screen log (newest at the bottom) */
        fb_fill_rect(fb, 2, 44, 316, 148, COL_CARD);
        fb_draw_border(fb, 2, 44, 316, 148, COL_BORDER);

        u32 n = ctr_log_count();
        u32 max_s = n > LOG_ROWS ? (n - LOG_ROWS) : 0u;
        u32 scroll = ui->log_scroll > max_s ? max_s : ui->log_scroll;
        u32 first = n > LOG_ROWS ? (n - LOG_ROWS - scroll) : 0u;
        if (n == 0u) {
            fb_draw_text(fb, 8, LOG_ROW_Y0, "(log empty)", COL_DIM, LOG_COLS);
        }
        for (u32 i = 0u; i < LOG_ROWS && first + i < n; i++) {
            char line[48];
            (void)ctr_log_get(first + i, line, (u32)sizeof(line));
            u32 col = COL_TEXT;
            if (strncmp(line, "[tty]", 5) == 0) {
                col = COL_ACCENT;
            } else if (strstr(line, "fail") || strstr(line, "FAIL") || strstr(line, "rror") ||
                       strstr(line, "ERR") || strstr(line, "rc=-")) {
                col = COL_WARN;
            } else if (strncmp(line, "[boot]", 6) == 0 || strncmp(line, "[cdboot]", 8) == 0 ||
                       strncmp(line, "[bios]", 6) == 0 || strncmp(line, "[audio_ndsp]", 12) == 0) {
                col = COL_OK;
            }
            fb_draw_text(fb, 4, LOG_ROW_Y0 + (int)i * LOG_ROW_DY, line, col, LOG_COLS);
        }

        if (ui->status_timer > 0u) {
            fb_printf(fb, 4, 196, COL_ACCENT, "%.39s", ui->status_msg);
        } else if (scroll > 0u) {
            fb_printf(fb, 4, 196, COL_WARN, "Scrolled up %u of %u lines", (unsigned)scroll,
                      (unsigned)n);
        } else {
            fb_printf(fb, 4, 196, COL_DIM, "%u lines  %s", (unsigned)n,
                      ui->game_loaded ? "Up/Down buttons scroll" : "D-Pad/Up/Down scroll");
        }

        fb_draw_button(fb, k_pad_x[0], PAD_Y, PAD_W, PAD_H, "Dump Log", false);
        fb_draw_button(fb, k_pad_x[1], PAD_Y, PAD_W, PAD_H, "Clear", false);
        fb_draw_button(fb, k_pad_x[2], PAD_Y, PAD_W, PAD_H, "Up", false);
        fb_draw_button(fb, k_pad_x[3], PAD_Y, PAD_W, PAD_H, "Down", false);
    }

    GSPGPU_FlushDataCache(fb, UI_W * UI_H * 3u);
    gfxScreenSwapBuffers(GFX_BOTTOM, false);
}
