#include "psx3ds/spu.h"

void spu_init(PsxSystem *sys) {
    (void)sys;
}

void spu_reset(PsxSystem *sys) {
    (void)sys;
}

void spu_state(PsxSystem *sys, StateIO *st) {
    (void)sys;
    (void)st;
}

void spu_shutdown(PsxSystem *sys) {
    (void)sys;
}

void spu_render_batch(PsxSystem *sys, u32 frames) {
    (void)sys;
    (void)frames;
}

void spu_dma_write_words(PsxSystem *sys, const u32 *src, u32 n) {
    (void)sys;
    (void)src;
    (void)n;
}

u32 spu_dma_read_words(PsxSystem *sys, u32 *dst, u32 n) {
    (void)sys;
    (void)dst;
    (void)n;
    return 0u;
}
