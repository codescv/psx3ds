#include "psx3ds/system.h"

void bios_init(PsxSystem *sys) {
    (void)sys;
}

void bios_reset(PsxSystem *sys) {
    (void)sys;
}

void bios_state(PsxSystem *sys, StateIO *st) {
    (void)sys;
    (void)st;
}

void bios_shutdown(PsxSystem *sys) {
    (void)sys;
}

PsxResult psx_load_bios(PsxSystem *sys, const char *path) {
    (void)sys;
    (void)path;
    return PSX_ERR_NOT_IMPL;
}

bool bios_pc_hook(PsxSystem *sys, u32 pc) {
    (void)sys;
    (void)pc;
    return false;
}
