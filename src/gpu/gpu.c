#include "psx3ds/gpu.h"

void gpu_init(PsxSystem *sys) {
    (void)sys;
}

void gpu_reset(PsxSystem *sys) {
    (void)sys;
}

void gpu_state(PsxSystem *sys, StateIO *st) {
    (void)sys;
    (void)st;
}

void gpu_shutdown(PsxSystem *sys) {
    (void)sys;
}

void gpu_gp0_write(PsxSystem *sys, u32 word) {
    (void)sys;
    (void)word;
}

void gpu_dma_write_words(PsxSystem *sys, const u32 *src, u32 n) {
    (void)sys;
    (void)src;
    (void)n;
}

u32 gpu_dma_read_words(PsxSystem *sys, u32 *dst, u32 n) {
    (void)sys;
    (void)dst;
    (void)n;
    return 0u;
}

void gpu_gp1_write(PsxSystem *sys, u32 word) {
    (void)sys;
    (void)word;
}

u32 gpu_read(PsxSystem *sys) {
    (void)sys;
    return 0u;
}

u32 gpu_stat(PsxSystem *sys) {
    (void)sys;
    return 0x1C002000u; /* ready for cmd + DMA ready + progressive field=1 */
}
