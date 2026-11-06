#include "psx3ds/cdrom.h"

void cdrom_init(PsxSystem *sys) {
    (void)sys;
}

void cdrom_reset(PsxSystem *sys) {
    (void)sys;
}

void cdrom_state(PsxSystem *sys, StateIO *st) {
    (void)sys;
    (void)st;
}

void cdrom_shutdown(PsxSystem *sys) {
    (void)sys;
}

PsxResult cdrom_load(PsxSystem *sys, const char *path) {
    (void)sys;
    (void)path;
    return PSX_ERR_NOT_IMPL;
}

u32 cdrom_dma_read_words(PsxSystem *sys, u32 *dst, u32 n) {
    (void)sys;
    (void)dst;
    (void)n;
    return 0u;
}
