#include "psx3ds/platform.h"

PsxResult audio_wav_write_stub(const char *path, const s16 *stereo, u32 frames);

PsxResult audio_wav_write_stub(const char *path, const s16 *stereo, u32 frames) {
    (void)path;
    (void)stereo;
    (void)frames;
    return PSX_ERR_NOT_IMPL;
}
