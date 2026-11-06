#include "psx3ds/mdec.h"

void mdec_init(PsxSystem *sys) {
    (void)sys;
}

void mdec_reset(PsxSystem *sys) {
    (void)sys;
}

void mdec_state(PsxSystem *sys, StateIO *st) {
    (void)sys;
    (void)st;
}

void mdec_shutdown(PsxSystem *sys) {
    (void)sys;
}

u32 mdec_dma_in_words(PsxSystem *sys, const u32 *src, u32 n) {
    (void)sys;
    (void)src;
    (void)n;
    return 0u;
}

u32 mdec_dma_out_words(PsxSystem *sys, u32 *dst, u32 n) {
    (void)sys;
    (void)dst;
    (void)n;
    return 0u;
}
