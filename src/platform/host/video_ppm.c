#include "psx3ds/platform.h"

PsxResult video_ppm_write_stub(const char *path, const u16 *rgb555, u16 w, u16 h, u16 pitch_px);

PsxResult video_ppm_write_stub(const char *path, const u16 *rgb555, u16 w, u16 h, u16 pitch_px) {
    (void)path;
    (void)rgb555;
    (void)w;
    (void)h;
    (void)pitch_px;
    return PSX_ERR_NOT_IMPL;
}
