#include "psx3ds/timers.h"

void timers_init(PsxSystem *sys) {
    (void)sys;
}

void timers_reset(PsxSystem *sys) {
    (void)sys;
}

void timers_state(PsxSystem *sys, StateIO *st) {
    (void)sys;
    (void)st;
}

void timers_shutdown(PsxSystem *sys) {
    (void)sys;
}

u16 timers_read_counter(PsxSystem *sys, u32 idx) {
    (void)sys;
    (void)idx;
    return 0u;
}
