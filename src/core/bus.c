#include "psx3ds/bus.h"

void bus_init(PsxSystem *sys) {
    (void)sys;
}

void bus_reset(PsxSystem *sys) {
    (void)sys;
}

void bus_state(PsxSystem *sys, StateIO *st) {
    (void)sys;
    (void)st;
}

void bus_shutdown(PsxSystem *sys) {
    (void)sys;
}

void bus_mmio_register(PsxSystem *sys, u32 base, u32 size,
                       MmioRead read_fn, MmioWrite write_fn) {
    (void)sys;
    (void)base;
    (void)size;
    (void)read_fn;
    (void)write_fn;
}

void bus_dma_write_words(PsxSystem *sys, u32 phys, const u32 *src, u32 n) {
    (void)sys;
    (void)phys;
    (void)src;
    (void)n;
}

void bus_dma_read_words(PsxSystem *sys, u32 phys, u32 *dst, u32 n) {
    (void)sys;
    (void)phys;
    (void)dst;
    (void)n;
}

void bus_dma_fill_desc(PsxSystem *sys, u32 phys_last, u32 n) {
    (void)sys;
    (void)phys_last;
    (void)n;
}

PSX_COLD u32 psx_bus_read32_slow(PsxCpu *c, u32 a) {
    (void)c;
    (void)a;
    return 0xFFFFFFFFu;
}

PSX_COLD u16 psx_bus_read16_slow(PsxCpu *c, u32 a) {
    (void)c;
    (void)a;
    return 0xFFFFu;
}

PSX_COLD u8 psx_bus_read8_slow(PsxCpu *c, u32 a) {
    (void)c;
    (void)a;
    return 0xFFu;
}

PSX_COLD void psx_bus_write32_slow(PsxCpu *c, u32 a, u32 v) {
    (void)c;
    (void)a;
    (void)v;
}

PSX_COLD void psx_bus_write16_slow(PsxCpu *c, u32 a, u32 v) {
    (void)c;
    (void)a;
    (void)v;
}

PSX_COLD void psx_bus_write8_slow(PsxCpu *c, u32 a, u32 v) {
    (void)c;
    (void)a;
    (void)v;
}

u32 psx_bus_fetch32(PsxCpu *c, u32 pc) {
    (void)c;
    (void)pc;
    return 0u;
}
