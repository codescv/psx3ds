#ifndef PSX3DS_PLAT_HOST_INTERNAL_H
#define PSX3DS_PLAT_HOST_INTERNAL_H

#include "psx3ds/platform.h"

/* Host CLI capture & deterministic input hooks (implemented in plat_host.c) */
void      plat_host_reset_capture(void);
void      plat_host_set_video_capture(bool enable, const char *periodic_prefix, u32 every_n);
bool      plat_host_has_presented_frame(void);
PsxResult plat_host_write_last_ppm(const char *path);

void      plat_host_set_audio_capture(bool enable);
u32       plat_host_audio_captured_frames(void);
PsxResult plat_host_write_captured_wav(const char *path);

void      plat_host_input_clear(void);
bool      plat_host_input_add_event(u32 start_frame, u32 end_frame, u16 press_mask);
void      plat_host_input_set_frame(u32 frame_idx);

#endif /* PSX3DS_PLAT_HOST_INTERNAL_H */
