#ifndef PSX3DS_TIMERS_H
#define PSX3DS_TIMERS_H

#include "psx3ds/hw_regs.h"
#include "psx3ds/types.h"

typedef struct PsxTimers {
    struct {
        u32        mode;
        u32        target;
        u32        base_value;
        psx_tick_t base_tick;
        u32        rate_num;
        u32        rate_den;
        u8         gated;
        u8         reached_target;
        u8         reached_ffff;
    } t[3];
    u32  scanline;
    u32  line_frac_q16;
    bool in_vblank;
} PsxTimers;

void timers_init(PsxSystem *sys);
void timers_reset(PsxSystem *sys);
void timers_state(PsxSystem *sys, StateIO *st);
void timers_shutdown(PsxSystem *sys);

u16  timers_read_counter(PsxSystem *sys, u32 idx);

#endif /* PSX3DS_TIMERS_H */
