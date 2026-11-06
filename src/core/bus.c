#include "psx3ds/bus.h"
#include "psx3ds/system.h"

#include <string.h>

static const u32 k_mem_ctrl_defaults[9] = {
    0x1F000000u, /* EXP1 base */
    0x1F802000u, /* EXP2 base */
    0x0013243Fu, /* EXP1 delay/size */
    0x00003022u, /* EXP3 delay/size */
    0x0013243Fu, /* BIOS ROM delay/size */
    0x200931E1u, /* SPU delay/size */
    0x00020843u, /* CDROM delay/size */
    0x00020943u, /* EXP2 delay/size */
    0x00031125u  /* COM_DELAY */
};

static u32 bus_ram_window_bytes(const PsxBus *b) {
    if (!b->strict_ram_size) {
        return PSX_RAM_MIRROR_END;
    }
    switch ((b->ram_size_reg >> 9) & 7u) {
        case 0u: return 0x00100000u;
        case 1u: return 0x00400000u;
        case 2u: return 0x00100000u;
        case 3u: return 0x00400000u;
        case 4u: return 0x00200000u;
        case 5u: return 0x00200000u;
        case 6u: return 0x00400000u;
        case 7u: return 0x00800000u;
        default: return 0x00200000u;
    }
}

PSX_INLINE u32 load_le16(const u8 *p) {
    u16 v;
    memcpy(&v, p, sizeof(v));
    return (u32)v;
}

PSX_INLINE u32 load_le32(const u8 *p) {
    u32 v;
    memcpy(&v, p, sizeof(v));
    return v;
}

PSX_INLINE void store_le16(u8 *p, u16 v) {
    memcpy(p, &v, sizeof(v));
}

PSX_INLINE void store_le32(u8 *p, u32 v) {
    memcpy(p, &v, sizeof(v));
}

PSX_INLINE u32 mask_for_width(u32 width) {
    if (width == 1u) {
        return 0xFFu;
    }
    if (width == 2u) {
        return 0xFFFFu;
    }
    return 0xFFFFFFFFu;
}

/* --------------------------------------------------------------------------
 * Built-in MMIO handlers (MemCtrl, SIO1 stub, RAM_SIZE, default GPU fallback)
 * -------------------------------------------------------------------------- */
static u32 mmio_memctrl_read(PsxSystem *sys, u32 addr, u32 width) {
    u32 off = addr - PSX_REG_MEM_CTRL_BASE;
    u32 idx = off >> 2;
    if (idx >= 9u) {
        return 0u;
    }
    u32 shift = (off & 3u) * 8u;
    return (sys->bus.mem_ctrl[idx] >> shift) & mask_for_width(width);
}

static void mmio_memctrl_write(PsxSystem *sys, u32 addr, u32 v, u32 width) {
    u32 off = addr - PSX_REG_MEM_CTRL_BASE;
    u32 idx = off >> 2;
    if (idx >= 9u) {
        return;
    }
    u32 shift = (off & 3u) * 8u;
    u32 m = mask_for_width(width) << shift;
    sys->bus.mem_ctrl[idx] = (sys->bus.mem_ctrl[idx] & ~m) | ((v << shift) & m);
}

static u32 mmio_sio1_read(PsxSystem *sys, u32 addr, u32 width) {
    (void)sys;
    u32 word_addr = addr & ~3u;
    u32 shift = (addr & 3u) * 8u;
    u32 val = (word_addr == 0x1F801054u) ? 0x00000005u : 0u; /* TX_RDY1 | TX_RDY2 */
    return (val >> shift) & mask_for_width(width);
}

static void mmio_sio1_write(PsxSystem *sys, u32 addr, u32 v, u32 width) {
    (void)sys;
    (void)addr;
    (void)v;
    (void)width;
}

static u32 mmio_ramsize_read(PsxSystem *sys, u32 addr, u32 width) {
    if ((addr & ~3u) != PSX_REG_RAM_SIZE) {
        return 0u;
    }
    u32 shift = (addr & 3u) * 8u;
    return (sys->bus.ram_size_reg >> shift) & mask_for_width(width);
}

static void mmio_ramsize_write(PsxSystem *sys, u32 addr, u32 v, u32 width) {
    if ((addr & ~3u) != PSX_REG_RAM_SIZE) {
        return;
    }
    u32 old = sys->bus.ram_size_reg;
    u32 shift = (addr & 3u) * 8u;
    u32 m = mask_for_width(width) << shift;
    u32 next = (old & ~m) | ((v << shift) & m);
    sys->bus.ram_size_reg = next;
    if (sys->bus.strict_ram_size && ((old ^ next) & 0x0E00u) != 0u) {
        fastmem_build(&sys->bus);
    }
}

static u32 mmio_gpu_default_read(PsxSystem *sys, u32 addr, u32 width) {
    u32 word_addr = addr & ~3u;
    u32 val = 0u;
    if (word_addr == PSX_REG_GPUREAD) {
        val = gpu_read(sys);
    } else if (word_addr == PSX_REG_GPUSTAT) {
        val = gpu_stat(sys);
    }
    u32 shift = (addr & 3u) * 8u;
    return (val >> shift) & mask_for_width(width);
}

static void mmio_gpu_default_write(PsxSystem *sys, u32 addr, u32 v, u32 width) {
    u32 word_addr = addr & ~3u;
    u32 shift = (addr & 3u) * 8u;
    u32 word = (v & mask_for_width(width)) << shift;
    if (word_addr == PSX_REG_GP0) {
        gpu_gp0_write(sys, word);
    } else if (word_addr == PSX_REG_GP1) {
        gpu_gp1_write(sys, word);
    }
}

/* --------------------------------------------------------------------------
 * Lifecycle & MMIO registration
 * -------------------------------------------------------------------------- */
void bus_mmio_register(PsxSystem *sys, u32 base, u32 size,
                       MmioRead read_fn, MmioWrite write_fn) {
    if (!sys || size == 0u || base < PSX_MMIO_PHYS || base >= (PSX_MMIO_PHYS + PSX_MMIO_SIZE)) {
        return;
    }
    u32 start_off = base - PSX_MMIO_PHYS;
    u32 end_off = start_off + size - 1u;
    if (end_off >= PSX_MMIO_SIZE) {
        end_off = PSX_MMIO_SIZE - 1u;
    }
    u32 start_slot = start_off >> 4;
    u32 end_slot = end_off >> 4;
    for (u32 slot = start_slot; slot <= end_slot; slot++) {
        sys->bus.mmio_r[slot] = read_fn;
        sys->bus.mmio_w[slot] = write_fn;
    }
}

void bus_init(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    PsxBus *b = &sys->bus;
    memset(b, 0, sizeof(*b));

    b->ram = (u8 *)arena_alloc(&sys->arena, PSX_RAM_SIZE, 4096u);
    b->scratch_page = (u8 *)arena_alloc(&sys->arena, PSX_SCRATCH_PAGE_SIZE, 4096u);
    b->bios = (u8 *)arena_alloc(&sys->arena, PSX_BIOS_SIZE, 4096u);
    b->fm_r = (uintptr_t *)arena_alloc(&sys->arena, (u32)PSX_FM_BYTES, 4096u);
    b->fm_w = (uintptr_t *)arena_alloc(&sys->arena, (u32)PSX_FM_BYTES, 4096u);
    if (sys->cfg.isc_swap_table) {
        b->fm_w_isc_null = (uintptr_t *)arena_alloc(&sys->arena, (u32)PSX_FM_BYTES, 4096u);
    }

    b->strict_ram_size = sys->cfg.strict_ram_size;
    b->ram_size_reg = PSX_RAM_SIZE_DEFAULT;
    memcpy(b->mem_ctrl, k_mem_ctrl_defaults, sizeof(k_mem_ctrl_defaults));

    /* Wire up PsxCpu pointer slots */
    sys->cpu.sys.p = sys;
    sys->cpu.fastmem_r.p = b->fm_r;
    sys->cpu.fastmem_w.p = b->fm_w;
    sys->cpu.ram_base.p = b->ram;
    sys->cpu.scratch_base.p = b->scratch_page;

    /* Default MMIO handlers (device *_init calls may override their own slots) */
    bus_mmio_register(sys, PSX_REG_MEM_CTRL_BASE, 0x30u, mmio_memctrl_read, mmio_memctrl_write);
    bus_mmio_register(sys, PSX_REG_SIO1_BASE, 0x10u, mmio_sio1_read, mmio_sio1_write);
    bus_mmio_register(sys, PSX_REG_RAM_SIZE, 0x10u, mmio_ramsize_read, mmio_ramsize_write);
    bus_mmio_register(sys, PSX_REG_GP0, 0x10u, mmio_gpu_default_read, mmio_gpu_default_write);

    fastmem_build(b);
}

void bus_reset(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    PsxBus *b = &sys->bus;
    if (b->ram) {
        memset(b->ram, 0, PSX_RAM_SIZE);
    }
    if (b->scratch_page) {
        memset(b->scratch_page, 0, PSX_SCRATCH_PAGE_SIZE);
    }
    /* NOTE: b->bios and b->mmio_r/w are intentionally preserved across reset! */
    memset(b->code_page_refs, 0, sizeof(b->code_page_refs));
    memset(b->code_page_mode, 0, sizeof(b->code_page_mode));
    memset(b->code_page_traps, 0, sizeof(b->code_page_traps));

    b->isolated = false;
    b->strict_ram_size = sys->cfg.strict_ram_size;
    b->ram_size_reg = PSX_RAM_SIZE_DEFAULT;
    b->exp2_post = 0u;
    memcpy(b->mem_ctrl, k_mem_ctrl_defaults, sizeof(k_mem_ctrl_defaults));

    fastmem_build(b);
    sys->cpu.sys.p = sys;
    sys->cpu.fastmem_r.p = b->fm_r;
    sys->cpu.fastmem_w.p = b->fm_w;
    sys->cpu.ram_base.p = b->ram;
    sys->cpu.scratch_base.p = b->scratch_page;
}

void bus_state(PsxSystem *sys, StateIO *st) {
    if (!sys || !st) {
        return;
    }
    PsxBus *b = &sys->bus;
    state_bytes(st, b->ram, PSX_RAM_SIZE);
    state_bytes(st, b->scratch_page, PSX_SCRATCH_SIZE);
    state_u32(st, &b->ram_size_reg);
    state_bool(st, &b->strict_ram_size);
    state_bool(st, &b->isolated);
    for (u32 i = 0u; i < 9u; i++) {
        state_u32(st, &b->mem_ctrl[i]);
    }
    state_u32(st, &b->exp2_post);
    if (!st->writing) {
        memset(b->code_page_refs, 0, sizeof(b->code_page_refs));
        memset(b->code_page_mode, 0, sizeof(b->code_page_mode));
        memset(b->code_page_traps, 0, sizeof(b->code_page_traps));
        fastmem_build(b);
        if (sys->cfg.isc_swap_table && b->fm_w_isc_null) {
            sys->cpu.fastmem_w.p = b->isolated ? b->fm_w_isc_null : b->fm_w;
        } else {
            sys->cpu.fastmem_w.p = b->fm_w;
        }
    }
}

void bus_shutdown(PsxSystem *sys) {
    (void)sys;
}

/* --------------------------------------------------------------------------
 * SMC-aware DMA RAM helpers
 * -------------------------------------------------------------------------- */
void bus_dma_write_words(PsxSystem *sys, u32 phys, const u32 *src, u32 n) {
    if (!sys || !src || n == 0u) {
        return;
    }
    PsxBus *b = &sys->bus;
    u32 addr = phys & 0x1FFFFCu;

    if (addr + (n * 4u) <= PSX_RAM_SIZE) {
        memcpy(b->ram + addr, src, (size_t)n * 4u);
        u32 first_page = addr >> PSX_FM_SHIFT;
        u32 last_page = (addr + (n * 4u) - 1u) >> PSX_FM_SHIFT;
        for (u32 p = first_page; p <= last_page; p++) {
            if (b->code_page_refs[p] != 0u) {
                jit_invalidate_range(sys, addr, n * 4u);
                break;
            }
        }
        return;
    }

    for (u32 i = 0u; i < n; i++) {
        u32 off = (addr + (i * 4u)) & 0x1FFFFCu;
        store_le32(b->ram + off, src[i]);
        u32 page = off >> PSX_FM_SHIFT;
        if (b->code_page_refs[page] != 0u) {
            jit_invalidate_range(sys, off, 4u);
        }
    }
}

void bus_dma_read_words(PsxSystem *sys, u32 phys, u32 *dst, u32 n) {
    if (!sys || !dst || n == 0u) {
        return;
    }
    const PsxBus *b = &sys->bus;
    u32 addr = phys & 0x1FFFFCu;

    if (addr + (n * 4u) <= PSX_RAM_SIZE) {
        memcpy(dst, b->ram + addr, (size_t)n * 4u);
        return;
    }

    for (u32 i = 0u; i < n; i++) {
        u32 off = (addr + (i * 4u)) & 0x1FFFFCu;
        dst[i] = load_le32(b->ram + off);
    }
}

void bus_dma_fill_desc(PsxSystem *sys, u32 phys_last, u32 n) {
    if (!sys || n == 0u) {
        return;
    }
    PsxBus *b = &sys->bus;
    u32 cur = phys_last & 0x1FFFFCu;

    for (u32 i = 0u; i < n - 1u; i++) {
        u32 next = (cur - 4u) & 0x1FFFFCu;
        store_le32(b->ram + cur, next);
        if (b->code_page_refs[cur >> PSX_FM_SHIFT] != 0u) {
            jit_invalidate_range(sys, cur, 4u);
        }
        cur = next;
    }
    store_le32(b->ram + cur, 0x00FFFFFFu);
    if (b->code_page_refs[cur >> PSX_FM_SHIFT] != 0u) {
        jit_invalidate_range(sys, cur, 4u);
    }
}

/* --------------------------------------------------------------------------
 * Slow-path memory & Expansion 2 / MMIO / SMC trap handlers
 * -------------------------------------------------------------------------- */
static void bus_fault(PsxCpu *c, u32 code, u32 badvaddr) {
    PsxSystem *sys = (PsxSystem *)c->sys.p;
    if (c->in_jit) {
        c->jit_abort = code + 1u;
        c->abort_badvaddr = badvaddr;
        return;
    }
    u32 fault_pc = c->in_delay_slot ? (c->pc - 4u) : c->pc;
    cpu_raise_exception(sys, code, fault_pc, c->in_delay_slot != 0u,
                        c->branch_target, badvaddr, 0u);
}

static u32 exp2_read_byte(const PsxSystem *sys, u32 ph) {
    (void)sys;
    if (ph == PSX_REG_DUART_SRB) {
        return 0x04u; /* TXRDY so OpenBIOS / Redux UART polls succeed */
    }
    return 0xFFu;
}

static u32 exp2_read(const PsxSystem *sys, u32 ph, u32 width) {
    u32 v = 0u;
    for (u32 i = 0u; i < width; i++) {
        v |= exp2_read_byte(sys, ph + i) << (i * 8u);
    }
    return v;
}

static void exp2_write(PsxSystem *sys, u32 ph, u32 v, u32 width) {
    (void)width;
    if (ph == PSX_REG_EXP2_POST) {
        sys->bus.exp2_post = v & 0xFFu;
        return;
    }
    if (ph == PSX_REG_DUART_THR || ph == PSX_REG_PCSX_STDOUT) {
        char ch = (char)(v & 0xFFu);
        plat_log("%c", ch);
        return;
    }
    if (ph == PSX_REG_PCSX_EXIT) {
        sys->quit = true;
        cpu_request_exit(sys);
        return;
    }
}

static u32 mmio_dispatch_read(PsxSystem *sys, u32 ph, u32 width) {
    u32 slot = (ph - PSX_MMIO_PHYS) >> 4;
    MmioRead fn = sys->bus.mmio_r[slot];
    if (fn) {
        return fn(sys, ph, width) & mask_for_width(width);
    }
    return 0u;
}

static void mmio_dispatch_write(PsxSystem *sys, u32 ph, u32 v, u32 width) {
    u32 slot = (ph - PSX_MMIO_PHYS) >> 4;
    MmioWrite fn = sys->bus.mmio_w[slot];
    if (fn) {
        fn(sys, ph, v & mask_for_width(width), width);
    }
}

static u32 bus_read_slow_common(PsxCpu *c, u32 a, u32 width) {
    PsxSystem *sys = (PsxSystem *)c->sys.p;
    PsxBus *b = &sys->bus;
    sys->stats.slow_reads++;

    if (PSX_UNLIKELY(psx_addr_fault(c, a, width))) {
        bus_fault(c, PSX_EXC_ADEL, a);
        return 0u;
    }

    bool seg_ok = (a < 0x20000000u) || (a >= 0x80000000u && a < 0xC0000000u);
    if (!seg_ok) {
        if ((a & ~3u) == PSX_CACHE_CTRL_ADDR) {
            u32 shift = (a & 3u) * 8u;
            return (c->cache_ctrl >> shift) & mask_for_width(width);
        }
        bus_fault(c, PSX_EXC_DBE, a);
        return 0u;
    }

    u32 ph = a & 0x1FFFFFFFu;
    if (ph < PSX_RAM_MIRROR_END) {
        if (ph >= bus_ram_window_bytes(b)) {
            bus_fault(c, PSX_EXC_DBE, a);
            return 0u;
        }
        const u8 *p = b->ram + (ph & PSX_RAM_MASK);
        if (width == 4u) return load_le32(p);
        if (width == 2u) return load_le16(p);
        return p[0];
    }

    if (ph >= PSX_EXP1_PHYS && ph < (PSX_EXP1_PHYS + PSX_EXP1_SIZE)) {
        return mask_for_width(width);
    }

    if (ph >= PSX_SCRATCH_PHYS && ph < (PSX_SCRATCH_PHYS + PSX_SCRATCH_SIZE) && a < 0xA0000000u) {
        const u8 *p = b->scratch_page + (ph & PSX_SCRATCH_MASK);
        if (width == 4u) return load_le32(p);
        if (width == 2u) return load_le16(p);
        return p[0];
    }

    if (ph >= PSX_MMIO_PHYS && ph < (PSX_MMIO_PHYS + PSX_MMIO_SIZE)) {
        return mmio_dispatch_read(sys, ph, width);
    }

    if (ph >= PSX_EXP2_PHYS && ph < (PSX_EXP2_PHYS + PSX_EXP2_SIZE)) {
        return exp2_read(sys, ph, width);
    }

    if (ph >= PSX_EXP3_PHYS && ph < (PSX_EXP3_PHYS + PSX_EXP3_SIZE)) {
        return mask_for_width(width);
    }

    if (ph >= PSX_BIOS_PHYS && ph < (PSX_BIOS_PHYS + PSX_BIOS_SIZE)) {
        const u8 *p = b->bios + (ph - PSX_BIOS_PHYS);
        if (width == 4u) return load_le32(p);
        if (width == 2u) return load_le16(p);
        return p[0];
    }

    bus_fault(c, PSX_EXC_DBE, a);
    return 0u;
}

static void bus_smc_ram_write(PsxSystem *sys, u32 off, u32 v, u32 width) {
    PsxBus *b = &sys->bus;
    u32 page = off >> PSX_FM_SHIFT;
    u8 *p = b->ram + off;

    u32 old_val;
    u32 new_val;
    if (width == 4u) {
        old_val = load_le32(p);
        new_val = v;
    } else if (width == 2u) {
        old_val = load_le16(p);
        new_val = v & 0xFFFFu;
    } else {
        old_val = p[0];
        new_val = v & 0xFFu;
    }

    bool trapped_code = (b->code_page_refs[page] != 0u);
    if (trapped_code) {
        sys->stats.smc_traps++;
    }

    if (old_val != new_val) {
        if (width == 4u) {
            store_le32(p, new_val);
        } else if (width == 2u) {
            store_le16(p, (u16)new_val);
        } else {
            p[0] = (u8)new_val;
        }
        if (trapped_code) {
            b->code_page_traps[page] = 0u;
            /* Sub-page precise: only wipes the page if a live block's code overlaps */
            jit_invalidate_range(sys, off, width);
        }
    } else if (trapped_code) {
        /*
         * Identical rewrite: nothing to invalidate. The page MUST stay in
         * PSX_PAGE_TRAP while any block references it: promoting it to a
         * writable fastmem mapping would let later real code mutations bypass
         * SMC detection entirely (stale translated code).
         */
        b->code_page_traps[page]++;
    }
}

static void bus_write_slow_common(PsxCpu *c, u32 a, u32 v, u32 width) {
    PsxSystem *sys = (PsxSystem *)c->sys.p;
    PsxBus *b = &sys->bus;
    sys->stats.slow_writes++;

    /* Alignment & user-mode segment checks MUST happen before SR.IsC! */
    if (PSX_UNLIKELY(psx_addr_fault(c, a, width))) {
        bus_fault(c, PSX_EXC_ADES, a);
        return;
    }

    /* Cache isolated: drop data writes so BIOS cache flush never wipes RAM */
    if ((c->cop0[COP0_SR] & PSX_SR_ISC) != 0u) {
        c->icache_tag_dummy[(a >> 4) & 0xFFu] = v;
        return;
    }

    bool seg_ok = (a < 0x20000000u) || (a >= 0x80000000u && a < 0xC0000000u);
    if (!seg_ok) {
        if ((a & ~3u) == PSX_CACHE_CTRL_ADDR) {
            u32 shift = (a & 3u) * 8u;
            u32 m = mask_for_width(width) << shift;
            c->cache_ctrl = (c->cache_ctrl & ~m) | ((v << shift) & m);
            return;
        }
        bus_fault(c, PSX_EXC_DBE, a);
        return;
    }

    u32 ph = a & 0x1FFFFFFFu;
    if (ph < PSX_RAM_MIRROR_END) {
        if (ph >= bus_ram_window_bytes(b)) {
            bus_fault(c, PSX_EXC_DBE, a);
            return;
        }
        bus_smc_ram_write(sys, ph & PSX_RAM_MASK, v, width);
        return;
    }

    if (ph >= PSX_EXP1_PHYS && ph < (PSX_EXP1_PHYS + PSX_EXP1_SIZE)) {
        return; /* EXP1 writes ignored */
    }

    if (ph >= PSX_SCRATCH_PHYS && ph < (PSX_SCRATCH_PHYS + PSX_SCRATCH_SIZE) && a < 0xA0000000u) {
        u8 *p = b->scratch_page + (ph & PSX_SCRATCH_MASK);
        if (width == 4u) store_le32(p, v);
        else if (width == 2u) store_le16(p, (u16)v);
        else p[0] = (u8)v;
        return;
    }

    if (ph >= PSX_MMIO_PHYS && ph < (PSX_MMIO_PHYS + PSX_MMIO_SIZE)) {
        mmio_dispatch_write(sys, ph, v, width);
        return;
    }

    if (ph >= PSX_EXP2_PHYS && ph < (PSX_EXP2_PHYS + PSX_EXP2_SIZE)) {
        exp2_write(sys, ph, v, width);
        return;
    }

    if (ph >= PSX_EXP3_PHYS && ph < (PSX_EXP3_PHYS + PSX_EXP3_SIZE)) {
        return; /* EXP3 writes ignored without fault (io-access-bitwidth.exe) */
    }

    if (ph >= PSX_BIOS_PHYS && ph < (PSX_BIOS_PHYS + PSX_BIOS_SIZE)) {
        return; /* BIOS ROM writes ignored */
    }

    bus_fault(c, PSX_EXC_DBE, a);
}

PSX_COLD u32 psx_bus_read32_slow(PsxCpu *c, u32 a) {
    return bus_read_slow_common(c, a, 4u);
}

PSX_COLD u16 psx_bus_read16_slow(PsxCpu *c, u32 a) {
    return (u16)bus_read_slow_common(c, a, 2u);
}

PSX_COLD u8 psx_bus_read8_slow(PsxCpu *c, u32 a) {
    return (u8)bus_read_slow_common(c, a, 1u);
}

PSX_COLD void psx_bus_write32_slow(PsxCpu *c, u32 a, u32 v) {
    bus_write_slow_common(c, a, v, 4u);
}

PSX_COLD void psx_bus_write16_slow(PsxCpu *c, u32 a, u32 v) {
    bus_write_slow_common(c, a, v, 2u);
}

PSX_COLD void psx_bus_write8_slow(PsxCpu *c, u32 a, u32 v) {
    bus_write_slow_common(c, a, v, 1u);
}

/*
 * Instruction fetch (verified against tests/roms/cpu/code-in-io.exe):
 *  - Misaligned / KUc kernel-segment violation -> AdEL (BadVAddr = pc)
 *  - RAM (within mapped window) & BIOS -> direct fetch
 *  - Scratchpad (0x1F800000..0x1F800FFF) -> IBE (D-cache is NOT on I-bus!)
 *  - MMIO (0x1F801000..0x1F801FFF) -> succeeds via mmio_dispatch_read(4)
 *  - EXP1 / EXP2 / EXP3 -> returns bus read value (e.g. 0xFFFFFFFF -> RI)
 *  - Everything else -> IBE
 */
u32 psx_bus_fetch32(PsxCpu *c, u32 pc) {
    PsxSystem *sys = (PsxSystem *)c->sys.p;
    const PsxBus *b = &sys->bus;

    if (PSX_UNLIKELY(psx_addr_fault(c, pc, 4u))) {
        bus_fault(c, PSX_EXC_ADEL, pc);
        return 0u;
    }

    bool seg_ok = (pc < 0x20000000u) || (pc >= 0x80000000u && pc < 0xC0000000u);
    if (PSX_UNLIKELY(!seg_ok)) {
        bus_fault(c, PSX_EXC_IBE, pc);
        return 0u;
    }

    u32 ph = pc & 0x1FFFFFFFu;
    if (PSX_LIKELY(ph < PSX_RAM_MIRROR_END)) {
        if (PSX_UNLIKELY(ph >= bus_ram_window_bytes(b))) {
            bus_fault(c, PSX_EXC_IBE, pc);
            return 0u;
        }
        return load_le32(b->ram + (ph & PSX_RAM_MASK));
    }

    if (PSX_LIKELY(ph >= PSX_BIOS_PHYS && ph < (PSX_BIOS_PHYS + PSX_BIOS_SIZE))) {
        return load_le32(b->bios + (ph - PSX_BIOS_PHYS));
    }

    /* Scratchpad 0x1F800000..0x1F800FFF MUST raise IBE */
    if (ph >= PSX_SCRATCH_PHYS && ph < PSX_MMIO_PHYS) {
        bus_fault(c, PSX_EXC_IBE, pc);
        return 0u;
    }

    if (ph >= PSX_MMIO_PHYS && ph < (PSX_MMIO_PHYS + PSX_MMIO_SIZE)) {
        /*
         * Per real CXD8530CQ hardware (code-in-io.exe):
         *  - I_STAT/I_MASK (0x1F801070..107F), MDEC (0x1F801820..182F), and
         *    unmapped MMIO holes raise IBE (6) on instruction fetch.
         *  - Mapped DMA (0x1F801080..10FF) and SPU (0x1F801C00..1FFF) registers
         *    can be fetched and executed as instructions!
         */
        u32 slot = (ph - PSX_MMIO_PHYS) >> 4;
        if (!b->mmio_r[slot] ||
            (ph >= PSX_REG_I_STAT && ph < PSX_REG_DMA_BASE) ||
            (ph >= PSX_REG_MDEC_BASE && ph < (PSX_REG_MDEC_BASE + 0x10u))) {
            bus_fault(c, PSX_EXC_IBE, pc);
            return 0u;
        }
        return mmio_dispatch_read(sys, ph, 4u);
    }

    if (ph >= PSX_EXP1_PHYS && ph < (PSX_EXP1_PHYS + PSX_EXP1_SIZE)) {
        return 0xFFFFFFFFu;
    }

    if (ph >= PSX_EXP2_PHYS && ph < (PSX_EXP2_PHYS + PSX_EXP2_SIZE)) {
        return exp2_read(sys, ph, 4u);
    }

    if (ph >= PSX_EXP3_PHYS && ph < (PSX_EXP3_PHYS + PSX_EXP3_SIZE)) {
        return 0xFFFFFFFFu;
    }

    bus_fault(c, PSX_EXC_IBE, pc);
    return 0u;
}
