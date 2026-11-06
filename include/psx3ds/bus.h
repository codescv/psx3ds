#ifndef PSX3DS_BUS_H
#define PSX3DS_BUS_H

#include "psx3ds/cpu.h"
#include "psx3ds/hw_regs.h"
#include "psx3ds/types.h"

#define PSX_FM_SHIFT      12u
#define PSX_FM_ENTRIES    (1u << (32u - PSX_FM_SHIFT)) /* 1,048,576 4 KiB pages */
#define PSX_FM_BYTES      ((size_t)PSX_FM_ENTRIES * sizeof(uintptr_t))
#define PSX_RAM_PAGES     512u
#define PSX_MMIO_SLOTS    256u /* 4 KiB 0x1F801xxx / 16-byte slots */

typedef enum PsxCodePageMode {
    PSX_PAGE_TRAP   = 0,
    PSX_PAGE_VERIFY = 1
} PsxCodePageMode;

typedef u32  (*MmioRead)(PsxSystem *sys, u32 addr, u32 width);
typedef void (*MmioWrite)(PsxSystem *sys, u32 addr, u32 v, u32 width);

typedef struct PsxBus {
    u8        *ram;                            /* 2 MiB, 4 KiB-aligned (arena) */
    u8        *scratch_page;                   /* 4 KiB page; [0, 0x400) is real scratchpad */
    u8        *bios;                           /* 512 KiB */
    uintptr_t *fm_r;                           /* 1M-entry bias-adjusted read LUT */
    uintptr_t *fm_w;                           /* 1M-entry bias-adjusted write LUT */
    uintptr_t *fm_w_isc_null;                  /* optional all-NULL LUT when cfg.isc_swap_table */
    bool       isolated;                       /* mirror of SR.IsC applied to fm_w */
    u16        code_page_refs[PSX_RAM_PAGES];  /* live JIT block count per 4 KiB RAM page */
    u8         code_page_mode[PSX_RAM_PAGES];  /* PsxCodePageMode (TRAP / VERIFY) */
    u32        code_page_traps[PSX_RAM_PAGES]; /* trapped writes in current frame */
    u32        ram_size_reg;                   /* 0x1F801060 */
    bool       strict_ram_size;                /* copy of cfg.strict_ram_size */
    u32        mem_ctrl[9];                    /* 0x1F801000..0x1F801020 */
    u32        exp2_post;                      /* 0x1F802041 */
    MmioRead   mmio_r[PSX_MMIO_SLOTS];
    MmioWrite  mmio_w[PSX_MMIO_SLOTS];
} PsxBus;

/* Lifecycle */
void bus_init(PsxSystem *sys);
void bus_reset(PsxSystem *sys);
void bus_state(PsxSystem *sys, StateIO *st);
void bus_shutdown(PsxSystem *sys);

/* MMIO handler registration (base in 0x1F801000..0x1F801FFF, 16-byte aligned) */
void bus_mmio_register(PsxSystem *sys, u32 base, u32 size,
                       MmioRead read_fn, MmioWrite write_fn);

/* Fastmem table management & SMC code-page tracking */
void fastmem_build(PsxBus *b);
void fastmem_set_isolated(PsxBus *b, bool isc);
void bus_code_page_ref(PsxBus *b, u32 phys_page);
void bus_code_page_unref(PsxBus *b, u32 phys_page);

/* SMC-aware DMA RAM accessors (all DMA / loader writes into RAM go through here) */
void bus_dma_write_words(PsxSystem *sys, u32 phys, const u32 *src, u32 n);
void bus_dma_read_words(PsxSystem *sys, u32 phys, u32 *dst, u32 n);
void bus_dma_fill_desc(PsxSystem *sys, u32 phys_last, u32 n);

/* Slow-path & instruction-fetch handlers */
u32  psx_bus_read32_slow(PsxCpu *c, u32 a);
u16  psx_bus_read16_slow(PsxCpu *c, u32 a);
u8   psx_bus_read8_slow(PsxCpu *c, u32 a);

void psx_bus_write32_slow(PsxCpu *c, u32 a, u32 v);
void psx_bus_write16_slow(PsxCpu *c, u32 a, u32 v);
void psx_bus_write8_slow(PsxCpu *c, u32 a, u32 v);

u32  psx_bus_fetch32(PsxCpu *c, u32 pc);

/* Inline C fast-path accessors (alignment + KUc checked before LUT dereference) */
static inline bool psx_addr_fault(const PsxCpu *c, u32 a, u32 size) {
    return ((a & (size - 1u)) != 0u) ||
           (((c->cop0[COP0_SR] & PSX_SR_KUC) != 0u) && ((a & 0x80000000u) != 0u));
}

static inline u32 psx_read32(PsxCpu *c, u32 a) {
    if (PSX_UNLIKELY(psx_addr_fault(c, a, 4u))) {
        return psx_bus_read32_slow(c, a);
    }
    uintptr_t e = ((const uintptr_t *)c->fastmem_r.p)[a >> PSX_FM_SHIFT];
    return PSX_LIKELY(e != 0u) ? *(const u32 *)(e + (uintptr_t)a) : psx_bus_read32_slow(c, a);
}

static inline u16 psx_read16(PsxCpu *c, u32 a) {
    if (PSX_UNLIKELY(psx_addr_fault(c, a, 2u))) {
        return psx_bus_read16_slow(c, a);
    }
    uintptr_t e = ((const uintptr_t *)c->fastmem_r.p)[a >> PSX_FM_SHIFT];
    return PSX_LIKELY(e != 0u) ? *(const u16 *)(e + (uintptr_t)a) : psx_bus_read16_slow(c, a);
}

static inline u8 psx_read8(PsxCpu *c, u32 a) {
    if (PSX_UNLIKELY(psx_addr_fault(c, a, 1u))) {
        return psx_bus_read8_slow(c, a);
    }
    uintptr_t e = ((const uintptr_t *)c->fastmem_r.p)[a >> PSX_FM_SHIFT];
    return PSX_LIKELY(e != 0u) ? *(const u8 *)(e + (uintptr_t)a) : psx_bus_read8_slow(c, a);
}

static inline void psx_write32(PsxCpu *c, u32 a, u32 v) {
    if (PSX_UNLIKELY(psx_addr_fault(c, a, 4u))) {
        psx_bus_write32_slow(c, a, v);
        return;
    }
    uintptr_t e = ((const uintptr_t *)c->fastmem_w.p)[a >> PSX_FM_SHIFT];
    if (PSX_LIKELY(e != 0u)) {
        *(u32 *)(e + (uintptr_t)a) = v;
    } else {
        psx_bus_write32_slow(c, a, v);
    }
}

static inline void psx_write16(PsxCpu *c, u32 a, u16 v) {
    if (PSX_UNLIKELY(psx_addr_fault(c, a, 2u))) {
        psx_bus_write16_slow(c, a, (u32)v);
        return;
    }
    uintptr_t e = ((const uintptr_t *)c->fastmem_w.p)[a >> PSX_FM_SHIFT];
    if (PSX_LIKELY(e != 0u)) {
        *(u16 *)(e + (uintptr_t)a) = v;
    } else {
        psx_bus_write16_slow(c, a, (u32)v);
    }
}

static inline void psx_write8(PsxCpu *c, u32 a, u8 v) {
    if (PSX_UNLIKELY(psx_addr_fault(c, a, 1u))) {
        psx_bus_write8_slow(c, a, (u32)v);
        return;
    }
    uintptr_t e = ((const uintptr_t *)c->fastmem_w.p)[a >> PSX_FM_SHIFT];
    if (PSX_LIKELY(e != 0u)) {
        *(u8 *)(e + (uintptr_t)a) = v;
    } else {
        psx_bus_write8_slow(c, a, (u32)v);
    }
}

#endif /* PSX3DS_BUS_H */
