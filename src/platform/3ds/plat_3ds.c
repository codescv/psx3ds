#include <3ds.h>

#include "plat_3ds_internal.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static u16           g_touch_extra_mask = 0u;
static PsxVideoScale g_scale_mode = PSX_SCALE_FIT_4_3;

void ctr_input_set_touch_buttons(u16 active_high_mask) {
    g_touch_extra_mask = active_high_mask;
}

void ctr_input_set_scale_mode(PsxVideoScale mode) {
    g_scale_mode = mode;
}

u64 plat_time_us(void) {
    return (u64)(svcGetSystemTick() * 1000000ULL / SYSCLOCK_ARM11);
}

/*
 * Software rasterizer presenter onto 3DS 400x240 Top Screen (rotated 240x400 BGR8).
 * Supports both 15bpp PSX 0BGR555 and packed 24bpp PSX RGB888 with pitch_px*2 byte stride.
 */
void plat_video_present(const u16 *rgb555, u16 w, u16 h, u16 pitch_px, bool is24) {
    if (!rgb555 || w == 0u || h == 0u) {
        return;
    }
    u16 fb_w = 0u;
    u16 fb_h = 0u;
    u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fb_w, &fb_h);
    if (!fb) {
        return;
    }

    u32 dst_w = 320u;
    u32 dst_h = 240u;
    if (g_scale_mode == PSX_SCALE_STRETCH) {
        dst_w = 400u;
        dst_h = 240u;
    } else if (g_scale_mode == PSX_SCALE_1X1) {
        dst_w = w < 400u ? w : 400u;
        dst_h = h < 240u ? h : 240u;
    } else {
        dst_w = 320u;
        dst_h = 240u;
    }

    u32 off_x = (400u - dst_w) / 2u;
    u32 off_y = (240u - dst_h) / 2u;

    /* Clear letterbox borders if narrower/shorter than 400x240 */
    if (off_x > 0u) {
        memset(fb, 0, (size_t)off_x * 240u * 3u);
        u32 right_x = off_x + dst_w;
        if (right_x < 400u) {
            memset(fb + (size_t)right_x * 240u * 3u, 0, (size_t)(400u - right_x) * 240u * 3u);
        }
    }

    size_t stride_bytes = pitch_px != 0u
                              ? ((size_t)pitch_px * 2u)
                              : (is24 ? (((size_t)w * 3u + 1u) & ~1u) : ((size_t)w * 2u));
    const u8 *src_base = (const u8 *)rgb555;

    for (u32 dy = 0u; dy < dst_h; dy++) {
        u32 sy = (dy * (u32)h) / dst_h;
        const u8 *src_row = src_base + (size_t)sy * stride_bytes;
        u32 screen_y = off_y + dy;
        u32 rot_y = 239u - screen_y;

        for (u32 dx = 0u; dx < dst_w; dx++) {
            u32 sx = (dx * (u32)w) / dst_w;
            u8 r8, g8, b8;
            if (is24) {
                const u8 *p = src_row + (size_t)sx * 3u;
                r8 = p[0];
                g8 = p[1];
                b8 = p[2];
            } else {
                u16 px = (u16)((u16)src_row[(size_t)sx * 2u] |
                               ((u16)src_row[(size_t)sx * 2u + 1u] << 8));
                u8 r5 = (u8)(px & 0x1Fu);
                u8 g5 = (u8)((px >> 5) & 0x1Fu);
                u8 b5 = (u8)((px >> 10) & 0x1Fu);
                r8 = (u8)((r5 << 3) | (r5 >> 2));
                g8 = (u8)((g5 << 3) | (g5 >> 2));
                b8 = (u8)((b5 << 3) | (b5 >> 2));
            }
            size_t idx = ((size_t)(off_x + dx) * 240u + (size_t)rot_y) * 3u;
            fb[idx + 0u] = b8;
            fb[idx + 1u] = g8;
            fb[idx + 2u] = r8;
        }
    }

    GSPGPU_FlushDataCache(fb, 400u * 240u * 3u);
    gfxScreenSwapBuffers(GFX_TOP, false);
}

/*
 * Un-rotated 400x240 top-screen readback (docs/toolchain_env.md §6.3).
 */
PsxResult ctr_capture_top_screen_ppm(const char *path) {
    if (!path || path[0] == '\0') {
        return PSX_ERR_BAD_ARG;
    }
    u16 fb_w = 0u;
    u16 fb_h = 0u;
    u8 *gfx_fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fb_w, &fb_h);
    if (!gfx_fb) {
        return PSX_ERR_IO;
    }
    (void)GSPGPU_InvalidateDataCache(gfx_fb, 400u * 240u * 3u);

    FILE *ppm = fopen(path, "wb");
    if (!ppm) {
        return PSX_ERR_IO;
    }
    if (fprintf(ppm, "P6\n400 240\n255\n") < 0) {
        fclose(ppm);
        return PSX_ERR_IO;
    }
    u8 row[400u * 3u];
    for (int y = 0; y < 240; y++) {
        for (int x = 0; x < 400; x++) {
            int idx = (x * 240 + (239 - y)) * 3;
            row[x * 3 + 0] = gfx_fb[idx + 2]; /* R */
            row[x * 3 + 1] = gfx_fb[idx + 1]; /* G */
            row[x * 3 + 2] = gfx_fb[idx + 0]; /* B */
        }
        if (fwrite(row, 1u, sizeof(row), ppm) != sizeof(row)) {
            fclose(ppm);
            return PSX_ERR_IO;
        }
    }
    if (fclose(ppm) != 0) {
        return PSX_ERR_IO;
    }
    return PSX_OK;
}

static u8 scale_stick_axis(s16 v, bool invert) {
    s32 s = invert ? -(s32)v : (s32)v;
    /* Circle pad nominal range is ~[-154, +154] */
    s32 mapped = 128 + (s * 127) / 150;
    if (mapped < 0) mapped = 0;
    if (mapped > 255) mapped = 255;
    return (u8)mapped;
}

void plat_input_poll(PsxPadState pads[2], PsxHostKeys *keys) {
    u32 held = hidKeysHeld();
    u32 down = hidKeysDown();

    circlePosition cpad = { 0, 0 };
    hidCircleRead(&cpad);

    circlePosition cstick = { 0, 0 };
    irrstCstickRead(&cstick);

    if (pads) {
        memset(pads, 0, sizeof(PsxPadState) * 2u);
        u16 pressed = g_touch_extra_mask;

        if (held & KEY_SELECT) pressed |= 0x0001u;
        if (held & KEY_START)  pressed |= 0x0008u;
        if ((held & KEY_DUP)    || cpad.dy > 55)  pressed |= 0x0010u;
        if ((held & KEY_DRIGHT) || cpad.dx > 55)  pressed |= 0x0020u;
        if ((held & KEY_DDOWN)  || cpad.dy < -55) pressed |= 0x0040u;
        if ((held & KEY_DLEFT)  || cpad.dx < -55) pressed |= 0x0080u;
        if (held & KEY_ZL)     pressed |= 0x0100u; /* L2 (N3DS) */
        if (held & KEY_ZR)     pressed |= 0x0200u; /* R2 (N3DS) */
        if (held & KEY_L)      pressed |= 0x0400u; /* L1 */
        if (held & KEY_R)      pressed |= 0x0800u; /* R1 */
        if (held & KEY_X)      pressed |= 0x1000u; /* Triangle */
        if (held & KEY_A)      pressed |= 0x2000u; /* Circle */
        if (held & KEY_B)      pressed |= 0x4000u; /* Cross */
        if (held & KEY_Y)      pressed |= 0x8000u; /* Square */

        /* L+R+Y chord maps to L3+R3 on Old 3DS */
        if ((held & (KEY_L | KEY_R | KEY_Y)) == (KEY_L | KEY_R | KEY_Y)) {
            pressed |= (0x0002u | 0x0004u);
        }

        pads[0].buttons = (u16)(0xFFFFu & ~pressed);
        pads[0].lx = scale_stick_axis(cpad.dx, false);
        pads[0].ly = scale_stick_axis(cpad.dy, true);
        pads[0].rx = scale_stick_axis(cstick.dx, false);
        pads[0].ry = scale_stick_axis(cstick.dy, true);
        pads[0].connected = 1u;

        pads[1].buttons = 0xFFFFu;
        pads[1].lx = 0x80u;
        pads[1].ly = 0x80u;
        pads[1].rx = 0x80u;
        pads[1].ry = 0x80u;
        pads[1].connected = 0u;
    }

    if (keys) {
        memset(keys, 0, sizeof(*keys));
        if ((held & (KEY_START | KEY_SELECT)) == (KEY_START | KEY_SELECT)) {
            keys->menu = 1u;
        }
        if ((held & (KEY_L | KEY_R)) == (KEY_L | KEY_R) && (down & KEY_X)) {
            keys->save_state = 1u;
        }
    }
}

/* --------------------------------------------------------------------------
 * SDMC / RomFS File & Directory I/O
 * -------------------------------------------------------------------------- */
struct PlatFile {
    FILE *fp;
};

PlatFile *plat_fopen(const char *path, const char *mode) {
    if (!path || !mode) {
        return NULL;
    }
    FILE *fp = fopen(path, mode);
    if (!fp) {
        return NULL;
    }
    PlatFile *pf = (PlatFile *)malloc(sizeof(*pf));
    if (!pf) {
        fclose(fp);
        return NULL;
    }
    pf->fp = fp;
    return pf;
}

u32 plat_fread(PlatFile *f, void *dst, u32 bytes) {
    if (!f || !f->fp || (!dst && bytes > 0u) || bytes == 0u) {
        return 0u;
    }
    return (u32)fread(dst, 1u, (size_t)bytes, f->fp);
}

u32 plat_fwrite(PlatFile *f, const void *src, u32 bytes) {
    if (!f || !f->fp || (!src && bytes > 0u) || bytes == 0u) {
        return 0u;
    }
    return (u32)fwrite(src, 1u, (size_t)bytes, f->fp);
}

bool plat_fseek(PlatFile *f, u64 off) {
    if (!f || !f->fp) {
        return false;
    }
    return fseek(f->fp, (long)off, SEEK_SET) == 0;
}

u64 plat_fsize(PlatFile *f) {
    if (!f || !f->fp) {
        return 0u;
    }
    long cur = ftell(f->fp);
    if (cur < 0) {
        return 0u;
    }
    if (fseek(f->fp, 0L, SEEK_END) != 0) {
        return 0u;
    }
    long end = ftell(f->fp);
    (void)fseek(f->fp, cur, SEEK_SET);
    return end >= 0 ? (u64)end : 0u;
}

void plat_fclose(PlatFile *f) {
    if (!f) {
        return;
    }
    if (f->fp) {
        fclose(f->fp);
    }
    free(f);
}

bool plat_mkdir_p(const char *path) {
    if (!path || path[0] == '\0') {
        return false;
    }
    char tmp[512];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) {
        return false;
    }
    memcpy(tmp, path, len + 1u);

    char *start = strstr(tmp, ":/");
    size_t idx = start ? (size_t)((start - tmp) + 2) : 1u;
    for (size_t i = idx; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
                return false;
            }
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
        return false;
    }
    return true;
}

static int cmp_entry_name(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

u32 plat_listdir(const char *path, char (*names)[256], u32 max) {
    if (!path || !names || max == 0u) {
        return 0u;
    }
    DIR *dir = opendir(path);
    if (!dir) {
        return 0u;
    }
    u32 count = 0u;
    struct dirent *ent = NULL;
    while (count < max && (ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        snprintf(names[count], 256u, "%.255s", ent->d_name);
        count++;
    }
    closedir(dir);
    if (count > 1u) {
        qsort(names, count, 256u, cmp_entry_name);
    }
    return count;
}

void plat_log(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        svcOutputDebugString(buf, n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1);
    }
}

/* --------------------------------------------------------------------------
 * Portable PPM & WAV Writers (used by 3DS autorun & cross-compiled unit tests)
 * -------------------------------------------------------------------------- */
PsxResult video_ppm_write_rgb24_raw(const char *path, const u8 *rgb24,
                                    u16 w, u16 h, u32 pitch_bytes) {
    if (!path || !rgb24 || w == 0u || h == 0u) {
        return PSX_ERR_BAD_ARG;
    }
    u32 row_bytes = (u32)w * 3u;
    if (pitch_bytes == 0u) {
        pitch_bytes = row_bytes;
    }
    if (pitch_bytes < row_bytes) {
        return PSX_ERR_BAD_ARG;
    }
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return PSX_ERR_IO;
    }
    if (fprintf(fp, "P6\n%u %u\n255\n", (unsigned)w, (unsigned)h) < 0) {
        fclose(fp);
        return PSX_ERR_IO;
    }
    for (u16 y = 0u; y < h; y++) {
        if (fwrite(rgb24 + (size_t)y * pitch_bytes, 1u, row_bytes, fp) != row_bytes) {
            fclose(fp);
            return PSX_ERR_IO;
        }
    }
    return fclose(fp) == 0 ? PSX_OK : PSX_ERR_IO;
}

PsxResult video_ppm_write_rgb555(const char *path, const u16 *vram,
                                 u16 w, u16 h, u16 pitch_px, bool is24) {
    if (!path || !vram || w == 0u || h == 0u) {
        return PSX_ERR_BAD_ARG;
    }
    size_t row_rgb = (size_t)w * 3u;
    size_t stride = pitch_px != 0u ? ((size_t)pitch_px * 2u)
                                   : (is24 ? ((row_rgb + 1u) & ~1u) : ((size_t)w * 2u));
    if ((is24 && stride < row_rgb) || (!is24 && stride < (size_t)w * 2u)) {
        return PSX_ERR_BAD_ARG;
    }
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return PSX_ERR_IO;
    }
    if (fprintf(fp, "P6\n%u %u\n255\n", (unsigned)w, (unsigned)h) < 0) {
        fclose(fp);
        return PSX_ERR_IO;
    }
    u8 *row_buf = (u8 *)malloc(row_rgb);
    if (!row_buf) {
        fclose(fp);
        return PSX_ERR_NOMEM;
    }
    const u8 *base = (const u8 *)vram;
    for (u16 y = 0u; y < h; y++) {
        const u8 *src = base + (size_t)y * stride;
        if (is24) {
            if (fwrite(src, 1u, row_rgb, fp) != row_rgb) {
                free(row_buf);
                fclose(fp);
                return PSX_ERR_IO;
            }
        } else {
            for (u16 x = 0u; x < w; x++) {
                u16 px = (u16)((u16)src[(size_t)x * 2u] |
                               ((u16)src[(size_t)x * 2u + 1u] << 8));
                u8 r5 = (u8)(px & 0x1Fu);
                u8 g5 = (u8)((px >> 5) & 0x1Fu);
                u8 b5 = (u8)((px >> 10) & 0x1Fu);
                row_buf[(size_t)x * 3u + 0u] = (u8)((r5 << 3) | (r5 >> 2));
                row_buf[(size_t)x * 3u + 1u] = (u8)((g5 << 3) | (g5 >> 2));
                row_buf[(size_t)x * 3u + 2u] = (u8)((b5 << 3) | (b5 >> 2));
            }
            if (fwrite(row_buf, 1u, row_rgb, fp) != row_rgb) {
                free(row_buf);
                fclose(fp);
                return PSX_ERR_IO;
            }
        }
    }
    free(row_buf);
    return fclose(fp) == 0 ? PSX_OK : PSX_ERR_IO;
}

PsxResult audio_wav_write(const char *path, const s16 *stereo,
                          u32 frames, u32 sample_rate) {
    if (!path || (frames > 0u && !stereo)) {
        return PSX_ERR_BAD_ARG;
    }
    if (sample_rate == 0u) {
        sample_rate = 44100u;
    }
    u32 data_bytes = frames * 4u;
    u32 riff_size = 36u + data_bytes;
    u32 byte_rate = sample_rate * 4u;
    u16 fmt_pcm = 1u, chans = 2u, align4 = 4u, bps16 = 16u;
    u32 fmt_sz = 16u;

    u8 hdr[44];
    memcpy(&hdr[0], "RIFF", 4u);
    memcpy(&hdr[4], &riff_size, 4u);
    memcpy(&hdr[8], "WAVE", 4u);
    memcpy(&hdr[12], "fmt ", 4u);
    memcpy(&hdr[16], &fmt_sz, 4u);
    memcpy(&hdr[20], &fmt_pcm, 2u);
    memcpy(&hdr[22], &chans, 2u);
    memcpy(&hdr[24], &sample_rate, 4u);
    memcpy(&hdr[28], &byte_rate, 4u);
    memcpy(&hdr[32], &align4, 2u);
    memcpy(&hdr[34], &bps16, 2u);
    memcpy(&hdr[36], "data", 4u);
    memcpy(&hdr[40], &data_bytes, 4u);

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return PSX_ERR_IO;
    }
    if (fwrite(hdr, 1u, sizeof(hdr), fp) != sizeof(hdr)) {
        fclose(fp);
        return PSX_ERR_IO;
    }
    if (data_bytes > 0u && fwrite(stereo, 1u, data_bytes, fp) != data_bytes) {
        fclose(fp);
        return PSX_ERR_IO;
    }
    return fclose(fp) == 0 ? PSX_OK : PSX_ERR_IO;
}
