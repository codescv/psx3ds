#ifndef PSX3DS_PLATFORM_H
#define PSX3DS_PLATFORM_H

#include "psx3ds/types.h"

typedef struct PsxPadState {
    u16 buttons; /* active-low PS1 button layout */
    u8  lx, ly, rx, ry;
    u8  analog_mode;
    u8  connected;
} PsxPadState;

typedef struct PsxHostKeys {
    u8 menu;
    u8 save_state;
    u8 load_state;
    u8 toggle_renderer;
    u8 fast_forward;
    u8 quit;
    u8 slot;
} PsxHostKeys;

u64  plat_time_us(void);

void plat_video_present(const u16 *rgb555, u16 w, u16 h, u16 pitch_px, bool is24);
void plat_audio_push(const s16 *stereo, u32 frames);
void plat_input_poll(PsxPadState pads[2], PsxHostKeys *keys);

typedef struct PlatThread PlatThread;
PlatThread *plat_thread_create(void (*fn)(void *), void *arg, u32 stack, int prio, int core);
void        plat_thread_join(PlatThread *th);
void        plat_sleep_us(u32 us);
void        plat_yield(void);

typedef struct PlatFile PlatFile;
PlatFile *plat_fopen(const char *path, const char *mode);
u32       plat_fread(PlatFile *f, void *dst, u32 bytes);
u32       plat_fwrite(PlatFile *f, const void *src, u32 bytes);
bool      plat_fseek(PlatFile *f, u64 off);
u64       plat_fsize(PlatFile *f);
void      plat_fclose(PlatFile *f);

bool plat_mkdir_p(const char *path);
u32  plat_listdir(const char *path, char (*names)[256], u32 max);
void plat_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif /* PSX3DS_PLATFORM_H */
