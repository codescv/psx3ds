#include <3ds.h>

#include "psx3ds/platform.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

u64 plat_time_us(void) {
    return (u64)(svcGetSystemTick() * 1000000ULL / SYSCLOCK_ARM11);
}

void plat_video_present(const u16 *rgb555, u16 w, u16 h, u16 pitch_px, bool is24) {
    (void)rgb555;
    (void)w;
    (void)h;
    (void)pitch_px;
    (void)is24;
}

void plat_input_poll(PsxPadState pads[2], PsxHostKeys *keys) {
    if (pads) {
        memset(pads, 0, sizeof(PsxPadState) * 2u);
        pads[0].buttons = 0xFFFFu;
        pads[0].connected = 1u;
        pads[1].buttons = 0xFFFFu;
    }
    if (keys) {
        memset(keys, 0, sizeof(*keys));
    }
}

PlatFile *plat_fopen(const char *path, const char *mode) {
    (void)path;
    (void)mode;
    return NULL;
}

u32 plat_fread(PlatFile *f, void *dst, u32 bytes) {
    (void)f;
    (void)dst;
    (void)bytes;
    return 0u;
}

u32 plat_fwrite(PlatFile *f, const void *src, u32 bytes) {
    (void)f;
    (void)src;
    (void)bytes;
    return 0u;
}

bool plat_fseek(PlatFile *f, u64 off) {
    (void)f;
    (void)off;
    return false;
}

u64 plat_fsize(PlatFile *f) {
    (void)f;
    return 0u;
}

void plat_fclose(PlatFile *f) {
    (void)f;
}

bool plat_mkdir_p(const char *path) {
    (void)path;
    return false;
}

u32 plat_listdir(const char *path, char (*names)[256], u32 max) {
    (void)path;
    (void)names;
    (void)max;
    return 0u;
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
