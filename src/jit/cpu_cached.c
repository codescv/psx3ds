#include "jit_internal.h"

#include <string.h>

/* ============================================================================
 * Part 1: Standalone Core-Stub Compatibility Layer (active ONLY when Slice A's
 * src/core/ sources are pre-merge stubs, detected via gte_unr_table[0] == 0).
 * Post-merge, every wrapper forwards directly to Slice A's real functions.
 * ============================================================================ */

static const u8 k_jit_unr_fallback[257] = {
    0xFFu, 0xFDu, 0xFBu, 0xF9u, 0xF7u, 0xF5u, 0xF3u, 0xF1u,
    0xEFu, 0xEEu, 0xECu, 0xEAu, 0xE8u, 0xE6u, 0xE4u, 0xE3u,
    0xE1u, 0xDFu, 0xDDu, 0xDCu, 0xDAu, 0xD8u, 0xD6u, 0xD5u,
    0xD3u, 0xD1u, 0xD0u, 0xCEu, 0xCDu, 0xCBu, 0xC9u, 0xC8u,
    0xC6u, 0xC5u, 0xC3u, 0xC1u, 0xC0u, 0xBEu, 0xBDu, 0xBBu,
    0xBAu, 0xB8u, 0xB7u, 0xB5u, 0xB4u, 0xB2u, 0xB1u, 0xB0u,
    0xAEu, 0xADu, 0xABu, 0xAAu, 0xA9u, 0xA7u, 0xA6u, 0xA4u,
    0xA3u, 0xA2u, 0xA0u, 0x9Fu, 0x9Eu, 0x9Cu, 0x9Bu, 0x9Au,
    0x99u, 0x97u, 0x96u, 0x95u, 0x94u, 0x92u, 0x91u, 0x90u,
    0x8Fu, 0x8Du, 0x8Cu, 0x8Bu, 0x8Au, 0x89u, 0x87u, 0x86u,
    0x85u, 0x84u, 0x83u, 0x82u, 0x81u, 0x7Fu, 0x7Eu, 0x7Du,
    0x7Cu, 0x7Bu, 0x7Au, 0x79u, 0x78u, 0x77u, 0x75u, 0x74u,
    0x73u, 0x72u, 0x71u, 0x70u, 0x6Fu, 0x6Eu, 0x6Du, 0x6Cu,
    0x6Bu, 0x6Au, 0x69u, 0x68u, 0x67u, 0x66u, 0x65u, 0x64u,
    0x63u, 0x62u, 0x61u, 0x60u, 0x5Fu, 0x5Eu, 0x5Du, 0x5Du,
    0x5Cu, 0x5Bu, 0x5Au, 0x59u, 0x58u, 0x57u, 0x56u, 0x55u,
    0x54u, 0x53u, 0x53u, 0x52u, 0x51u, 0x50u, 0x4Fu, 0x4Eu,
    0x4Du, 0x4Du, 0x4Cu, 0x4Bu, 0x4Au, 0x49u, 0x48u, 0x48u,
    0x47u, 0x46u, 0x45u, 0x44u, 0x43u, 0x43u, 0x42u, 0x41u,
    0x40u, 0x3Fu, 0x3Fu, 0x3Eu, 0x3Du, 0x3Cu, 0x3Cu, 0x3Bu,
    0x3Au, 0x39u, 0x39u, 0x38u, 0x37u, 0x36u, 0x36u, 0x35u,
    0x34u, 0x33u, 0x33u, 0x32u, 0x31u, 0x31u, 0x30u, 0x2Fu,
    0x2Eu, 0x2Eu, 0x2Du, 0x2Cu, 0x2Cu, 0x2Bu, 0x2Au, 0x2Au,
    0x29u, 0x28u, 0x28u, 0x27u, 0x26u, 0x26u, 0x25u, 0x24u,
    0x24u, 0x23u, 0x22u, 0x22u, 0x21u, 0x20u, 0x20u, 0x1Fu,
    0x1Eu, 0x1Eu, 0x1Du, 0x1Du, 0x1Cu, 0x1Bu, 0x1Bu, 0x1Au,
    0x19u, 0x19u, 0x18u, 0x18u, 0x17u, 0x16u, 0x16u, 0x15u,
    0x15u, 0x14u, 0x14u, 0x13u, 0x12u, 0x12u, 0x11u, 0x11u,
    0x10u, 0x0Fu, 0x0Fu, 0x0Eu, 0x0Eu, 0x0Du, 0x0Du, 0x0Cu,
    0x0Cu, 0x0Bu, 0x0Au, 0x0Au, 0x09u, 0x09u, 0x08u, 0x08u,
    0x07u, 0x07u, 0x06u, 0x06u, 0x05u, 0x05u, 0x04u, 0x04u,
    0x03u, 0x03u, 0x02u, 0x02u, 0x01u, 0x01u, 0x00u, 0x00u,
    0x00u
};

const u8 *jit_gte_unr_table_ptr(void) {
    return jit_core_is_stub() ? k_jit_unr_fallback : gte_unr_table;
}

static void stub_set_ram_page_writable(PsxBus *b, u32 phys_page, bool writable) {
    if (!b || !b->fm_w || !b->ram || phys_page >= PSX_RAM_PAGES) {
        return;
    }
    const u32 segs[3] = { 0x00000000u, 0x80000000u, 0xA0000000u };
    u32 max_mirrors = b->strict_ram_size ? 1u : 4u;
    for (u32 s = 0u; s < 3u; ++s) {
        for (u32 m = 0u; m < max_mirrors; ++m) {
            u32 g = segs[s] | (m << 21) | (phys_page << 12);
            u32 vp = g >> PSX_FM_SHIFT;
            b->fm_w[vp] = writable
                ? ((uintptr_t)b->ram + ((uintptr_t)phys_page << 12) - (uintptr_t)g)
                : (uintptr_t)0u;
        }
    }
}

void jit_ref_code_page(PsxBus *b, u32 phys_page) {
    if (!b || phys_page >= PSX_RAM_PAGES) {
        return;
    }
    u16 before = b->code_page_refs[phys_page];
    bus_code_page_ref(b, phys_page);
    if (b->code_page_refs[phys_page] == before) {
        b->code_page_refs[phys_page] = (u16)(before + 1u);
        if (before == 0u) {
            stub_set_ram_page_writable(b, phys_page, false);
        }
    }
}

void jit_unref_code_page(PsxBus *b, u32 phys_page) {
    if (!b || phys_page >= PSX_RAM_PAGES) {
        return;
    }
    u16 before = b->code_page_refs[phys_page];
    if (before == 0u) {
        return;
    }
    bus_code_page_unref(b, phys_page);
    if (b->code_page_refs[phys_page] == before) {
        u16 after = (u16)(before - 1u);
        b->code_page_refs[phys_page] = after;
        if (after == 0u && !b->isolated) {
            stub_set_ram_page_writable(b, phys_page, true);
        }
    }
}

void jit_request_exit_wrap(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    if (!jit_core_is_stub()) {
        cpu_request_exit(sys);
        return;
    }
    if (sys->cpu.cycles_left > 0) {
        sys->cpu.slice_len -= sys->cpu.cycles_left;
        sys->cpu.cycles_left = 0;
    }
}

void jit_check_irq_wrap(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    if (!jit_core_is_stub()) {
        cpu_check_irq(sys);
        return;
    }
    PsxCpu *c = &sys->cpu;
    if ((sys->irq.stat & sys->irq.mask) != 0u) {
        c->cop0[COP0_CAUSE] |= PSX_CAUSE_IP2;
    } else {
        c->cop0[COP0_CAUSE] &= ~PSX_CAUSE_IP2;
    }
    u32 sr = c->cop0[COP0_SR];
    u32 cause = c->cop0[COP0_CAUSE];
    bool pending = ((sr & PSX_SR_IEC) != 0u) && ((sr & cause & PSX_CAUSE_IRQ_MASK) != 0u);
    c->irq_pending = pending ? 1u : 0u;
    if (pending) {
        jit_request_exit_wrap(sys);
    }
}

void jit_set_sr_wrap(PsxSystem *sys, u32 v) {
    if (!sys) {
        return;
    }
    if (!jit_core_is_stub()) {
        cpu_set_sr(sys, v);
        return;
    }
    PsxCpu *c = &sys->cpu;
    u32 old = c->cop0[COP0_SR];
    c->cop0[COP0_SR] = v;
    if (((old ^ v) & PSX_SR_ISC) != 0u && sys->bus.fm_w) {
        bool isc = (v & PSX_SR_ISC) != 0u;
        sys->bus.isolated = isc;
        const u32 segs[3] = { 0x00000u, 0x80000u, 0xA0000u };
        u32 max_pages = sys->bus.strict_ram_size ? 512u : 2048u;
        for (u32 s = 0u; s < 3u; ++s) {
            for (u32 p = 0u; p < max_pages; ++p) {
                u32 vp = segs[s] + p;
                u32 phys_p = p & 0x1FFu;
                sys->bus.fm_w[vp] = (isc || sys->bus.code_page_refs[phys_p] != 0u)
                    ? (uintptr_t)0u
                    : ((uintptr_t)sys->bus.ram + ((uintptr_t)phys_p << 12) - ((uintptr_t)vp << 12));
            }
        }
        if (sys->bus.scratch_page) {
            sys->bus.fm_w[0x1F800u] = isc ? 0u : ((uintptr_t)sys->bus.scratch_page - 0x1F800000u);
            sys->bus.fm_w[0x9F800u] = isc ? 0u : ((uintptr_t)sys->bus.scratch_page - 0x9F800000u);
        }
    }
    jit_check_irq_wrap(sys);
}

void jit_set_cause_wrap(PsxSystem *sys, u32 v) {
    if (!sys) {
        return;
    }
    if (!jit_core_is_stub()) {
        cpu_set_cause(sys, v);
        return;
    }
    PsxCpu *c = &sys->cpu;
    c->cop0[COP0_CAUSE] = (c->cop0[COP0_CAUSE] & ~PSX_CAUSE_SW_MASK) | (v & PSX_CAUSE_SW_MASK);
    jit_check_irq_wrap(sys);
}

void jit_raise_exc_wrap(PsxSystem *sys, u32 code, u32 pc, bool bd,
                        u32 branch_target, u32 badvaddr, u32 ce) {
    if (!sys) {
        return;
    }
    if (!jit_core_is_stub()) {
        u32 epc = bd ? (pc - 4u) : pc;
        cpu_raise_exception(sys, code, epc, bd, branch_target, badvaddr, ce);
        sys->cpu.exc_pending = 0u;
        return;
    }
    PsxCpu *c = &sys->cpu;
    if (c->load_delay_reg != 0u) {
        c->gpr[c->load_delay_reg] = c->load_delay_val;
        c->gpr[0] = 0u;
    }
    u32 sr = c->cop0[COP0_SR];
    u32 next_sr = (sr & ~0x3Fu) | ((sr & 0x0Fu) << 2);
    jit_set_sr_wrap(sys, next_sr);

    u32 cause = c->cop0[COP0_CAUSE] & 0x0000FF00u;
    cause |= (code & 0x1Fu) << PSX_CAUSE_EXCCODE_SHIFT;
    if (code == PSX_EXC_CPU) {
        cause |= (ce & 3u) << PSX_CAUSE_CE_SHIFT;
    }
    if (bd) {
        cause |= PSX_CAUSE_BD;
        c->cop0[COP0_EPC] = pc - 4u;
        c->cop0[COP0_JUMPDEST] = branch_target;
    } else {
        c->cop0[COP0_EPC] = pc;
    }
    if (code == PSX_EXC_ADEL || code == PSX_EXC_ADES) {
        c->cop0[COP0_BADVADDR] = badvaddr;
    }
    c->cop0[COP0_CAUSE] = cause;

    c->in_delay_slot = 0u;
    c->branch_taken = 0u;
    c->load_delay_reg = 0u;
    c->load_delay_val = 0u;
    c->pc = ((sr & PSX_SR_BEV) != 0u) ? PSX_EXC_VEC_ROM : PSX_EXC_VEC_RAM;
    c->next_pc = c->pc + 4u;
    sys->stats.exceptions++;
}

static void stub_bus_fault(PsxCpu *c, u32 code, u32 a) {
    PsxSystem *sys = (PsxSystem *)c->sys.p;
    if (c->in_jit != 0u) {
        c->jit_abort = code + 1u;
        c->abort_badvaddr = a;
        return;
    }
    jit_raise_exc_wrap(sys, code, c->pc, c->in_delay_slot != 0u, c->branch_target, a, 0u);
}

static u32 stub_mmio_read(PsxSystem *sys, u32 ph, u32 width) {
    u32 slot = (ph - PSX_MMIO_PHYS) >> 4;
    if (slot < PSX_MMIO_SLOTS && sys->bus.mmio_r[slot]) {
        return sys->bus.mmio_r[slot](sys, ph, width);
    }
    if (ph == PSX_REG_RAM_SIZE) {
        return sys->bus.ram_size_reg ? sys->bus.ram_size_reg : PSX_RAM_SIZE_DEFAULT;
    }
    if (ph == PSX_REG_I_STAT) {
        return sys->irq.stat;
    }
    if (ph == PSX_REG_I_MASK) {
        return sys->irq.mask;
    }
    if (ph == PSX_REG_GPUSTAT) {
        return sys->gpu.gpustat ? sys->gpu.gpustat : 0x1C002000u;
    }
    return 0u;
}

static void stub_mmio_write(PsxSystem *sys, u32 ph, u32 v, u32 width) {
    u32 slot = (ph - PSX_MMIO_PHYS) >> 4;
    if (slot < PSX_MMIO_SLOTS && sys->bus.mmio_w[slot]) {
        sys->bus.mmio_w[slot](sys, ph, v, width);
        return;
    }
    if (ph == PSX_REG_RAM_SIZE) {
        sys->bus.ram_size_reg = v;
    } else if (ph == PSX_REG_I_STAT) {
        sys->irq.stat &= v;
        jit_check_irq_wrap(sys);
    } else if (ph == PSX_REG_I_MASK) {
        sys->irq.mask = v & 0x7FFu;
        jit_check_irq_wrap(sys);
    } else if (ph >= PSX_REG_MEM_CTRL_BASE && ph < PSX_REG_MEM_CTRL_END) {
        sys->bus.mem_ctrl[(ph - PSX_REG_MEM_CTRL_BASE) >> 2] = v;
    }
}

static void stub_tty_putc(PsxSystem *sys, char ch) {
    if (!sys) {
        return;
    }
    if (sys->bios.tty_len + 1u < sizeof(sys->bios.tty_line)) {
        sys->bios.tty_line[sys->bios.tty_len++] = ch;
        sys->bios.tty_line[sys->bios.tty_len] = '\0';
        if (sys->bios.until_pattern[0] != '\0' &&
            strstr(sys->bios.tty_line, sys->bios.until_pattern) != NULL) {
            sys->bios.until_matched = true;
            sys->quit = true;
            jit_request_exit_wrap(sys);
        }
    }
    if (ch == '\n' || sys->bios.tty_len + 2u >= sizeof(sys->bios.tty_line)) {
        sys->bios.tty_len = 0u;
        sys->bios.tty_line[0] = '\0';
    }
}

u32 jit_read32_slow_wrap(PsxCpu *c, u32 a) {
    if (!jit_core_is_stub()) {
        return psx_bus_read32_slow(c, a);
    }
    PsxSystem *sys = (PsxSystem *)c->sys.p;
    sys->stats.slow_reads++;
    if (PSX_UNLIKELY(psx_addr_fault(c, a, 4u))) {
        stub_bus_fault(c, PSX_EXC_ADEL, a);
        return 0u;
    }
    bool seg_ok = (a < 0x20000000u) || (a >= 0x80000000u && a < 0xC0000000u);
    if (!seg_ok) {
        if (a == PSX_CACHE_CTRL_ADDR) {
            return c->cache_ctrl;
        }
        stub_bus_fault(c, PSX_EXC_DBE, a);
        return 0u;
    }
    u32 ph = a & 0x1FFFFFFFu;
    if (ph < 0x00800000u) {
        if (ph >= 0x00200000u && sys->cfg.strict_ram_size) {
            stub_bus_fault(c, PSX_EXC_DBE, a);
            return 0u;
        }
        u32 v = 0u;
        memcpy(&v, sys->bus.ram + (ph & PSX_RAM_MASK), 4u);
        return v;
    }
    if (ph < 0x1F000000u) {
        stub_bus_fault(c, PSX_EXC_DBE, a);
        return 0u;
    }
    if (ph < 0x1F800000u) {
        return 0xFFFFFFFFu;
    }
    if (ph < 0x1F800400u && a < 0xA0000000u && sys->bus.scratch_page) {
        u32 v = 0u;
        memcpy(&v, sys->bus.scratch_page + (ph & PSX_SCRATCH_MASK), 4u);
        return v;
    }
    if (ph < 0x1F801000u) {
        stub_bus_fault(c, PSX_EXC_DBE, a);
        return 0u;
    }
    if (ph < 0x1F802000u) {
        return stub_mmio_read(sys, ph, 4u);
    }
    if (ph < 0x1F803000u) {
        if (ph == PSX_REG_DUART_SRB) {
            return 0x04u;
        }
        return 0xFFFFFFFFu;
    }
    if (ph >= 0x1FA00000u && ph < 0x1FC00000u) {
        return 0xFFFFFFFFu;
    }
    if (ph >= 0x1FC00000u && ph < 0x1FC80000u && sys->bus.bios) {
        u32 v = 0u;
        memcpy(&v, sys->bus.bios + (ph & PSX_BIOS_MASK), 4u);
        return v;
    }
    stub_bus_fault(c, PSX_EXC_DBE, a);
    return 0u;
}

u16 jit_read16_slow_wrap(PsxCpu *c, u32 a) {
    if (!jit_core_is_stub()) {
        return psx_bus_read16_slow(c, a);
    }
    PsxSystem *sys = (PsxSystem *)c->sys.p;
    sys->stats.slow_reads++;
    if (PSX_UNLIKELY(psx_addr_fault(c, a, 2u))) {
        stub_bus_fault(c, PSX_EXC_ADEL, a);
        return 0u;
    }
    bool seg_ok = (a < 0x20000000u) || (a >= 0x80000000u && a < 0xC0000000u);
    if (!seg_ok) {
        stub_bus_fault(c, PSX_EXC_DBE, a);
        return 0u;
    }
    u32 ph = a & 0x1FFFFFFFu;
    if (ph < 0x00800000u) {
        if (ph >= 0x00200000u && sys->cfg.strict_ram_size) {
            stub_bus_fault(c, PSX_EXC_DBE, a);
            return 0u;
        }
        u16 v = 0u;
        memcpy(&v, sys->bus.ram + (ph & PSX_RAM_MASK), 2u);
        return v;
    }
    if (ph < 0x1F000000u) {
        stub_bus_fault(c, PSX_EXC_DBE, a);
        return 0u;
    }
    if (ph < 0x1F800000u) {
        return 0xFFFFu;
    }
    if (ph < 0x1F800400u && a < 0xA0000000u && sys->bus.scratch_page) {
        u16 v = 0u;
        memcpy(&v, sys->bus.scratch_page + (ph & PSX_SCRATCH_MASK), 2u);
        return v;
    }
    if (ph < 0x1F801000u) {
        stub_bus_fault(c, PSX_EXC_DBE, a);
        return 0u;
    }
    if (ph < 0x1F802000u) {
        return (u16)stub_mmio_read(sys, ph, 2u);
    }
    if (ph < 0x1F803000u) {
        return 0xFFFFu;
    }
    if (ph >= 0x1FA00000u && ph < 0x1FC00000u) {
        return 0xFFFFu;
    }
    if (ph >= 0x1FC00000u && ph < 0x1FC80000u && sys->bus.bios) {
        u16 v = 0u;
        memcpy(&v, sys->bus.bios + (ph & PSX_BIOS_MASK), 2u);
        return v;
    }
    stub_bus_fault(c, PSX_EXC_DBE, a);
    return 0u;
}

u8 jit_read8_slow_wrap(PsxCpu *c, u32 a) {
    if (!jit_core_is_stub()) {
        return psx_bus_read8_slow(c, a);
    }
    PsxSystem *sys = (PsxSystem *)c->sys.p;
    sys->stats.slow_reads++;
    if (PSX_UNLIKELY(psx_addr_fault(c, a, 1u))) {
        stub_bus_fault(c, PSX_EXC_ADEL, a);
        return 0u;
    }
    bool seg_ok = (a < 0x20000000u) || (a >= 0x80000000u && a < 0xC0000000u);
    if (!seg_ok) {
        stub_bus_fault(c, PSX_EXC_DBE, a);
        return 0u;
    }
    u32 ph = a & 0x1FFFFFFFu;
    if (ph < 0x00800000u) {
        if (ph >= 0x00200000u && sys->cfg.strict_ram_size) {
            stub_bus_fault(c, PSX_EXC_DBE, a);
            return 0u;
        }
        return sys->bus.ram[ph & PSX_RAM_MASK];
    }
    if (ph < 0x1F000000u) {
        stub_bus_fault(c, PSX_EXC_DBE, a);
        return 0u;
    }
    if (ph < 0x1F800000u) {
        return 0xFFu;
    }
    if (ph < 0x1F800400u && a < 0xA0000000u && sys->bus.scratch_page) {
        return sys->bus.scratch_page[ph & PSX_SCRATCH_MASK];
    }
    if (ph < 0x1F801000u) {
        stub_bus_fault(c, PSX_EXC_DBE, a);
        return 0u;
    }
    if (ph < 0x1F802000u) {
        return (u8)stub_mmio_read(sys, ph, 1u);
    }
    if (ph < 0x1F803000u) {
        if (ph == PSX_REG_DUART_SRB) {
            return 0x04u;
        }
        return 0xFFu;
    }
    if (ph >= 0x1FA00000u && ph < 0x1FC00000u) {
        return 0xFFu;
    }
    if (ph >= 0x1FC00000u && ph < 0x1FC80000u && sys->bus.bios) {
        return sys->bus.bios[ph & PSX_BIOS_MASK];
    }
    stub_bus_fault(c, PSX_EXC_DBE, a);
    return 0u;
}

void jit_write32_slow_wrap(PsxCpu *c, u32 a, u32 v) {
    if (!jit_core_is_stub()) {
        psx_bus_write32_slow(c, a, v);
        return;
    }
    PsxSystem *sys = (PsxSystem *)c->sys.p;
    sys->stats.slow_writes++;
    if (PSX_UNLIKELY(psx_addr_fault(c, a, 4u))) {
        stub_bus_fault(c, PSX_EXC_ADES, a);
        return;
    }
    if ((c->cop0[COP0_SR] & PSX_SR_ISC) != 0u) {
        return;
    }
    bool seg_ok = (a < 0x20000000u) || (a >= 0x80000000u && a < 0xC0000000u);
    if (!seg_ok) {
        if (a == PSX_CACHE_CTRL_ADDR) {
            c->cache_ctrl = v;
            return;
        }
        stub_bus_fault(c, PSX_EXC_DBE, a);
        return;
    }
    u32 ph = a & 0x1FFFFFFFu;
    if (ph < 0x00800000u) {
        if (ph >= 0x00200000u && sys->cfg.strict_ram_size) {
            stub_bus_fault(c, PSX_EXC_DBE, a);
            return;
        }
        u32 off = ph & PSX_RAM_MASK;
        sys->stats.smc_traps++;
        u32 cur = 0u;
        memcpy(&cur, sys->bus.ram + off, 4u);
        if (cur != v) {
            memcpy(sys->bus.ram + off, &v, 4u);
            jit_invalidate_page(sys, off >> 12);
        }
        return;
    }
    if (ph < 0x1F000000u) {
        stub_bus_fault(c, PSX_EXC_DBE, a);
        return;
    }
    if (ph < 0x1F800000u) {
        return;
    }
    if (ph < 0x1F800400u && a < 0xA0000000u && sys->bus.scratch_page) {
        memcpy(sys->bus.scratch_page + (ph & PSX_SCRATCH_MASK), &v, 4u);
        return;
    }
    if (ph < 0x1F801000u) {
        stub_bus_fault(c, PSX_EXC_DBE, a);
        return;
    }
    if (ph < 0x1F802000u) {
        stub_mmio_write(sys, ph, v, 4u);
        return;
    }
    if (ph < 0x1F803000u) {
        if (ph == PSX_REG_DUART_THR || ph == PSX_REG_PCSX_STDOUT) {
            stub_tty_putc(sys, (char)(v & 0xFFu));
        } else if (ph == PSX_REG_EXP2_POST) {
            sys->bus.exp2_post = v & 0xFFu;
        }
        return;
    }
    if (ph >= 0x1FA00000u && ph < 0x1FC00000u) {
        return;
    }
    if (ph >= 0x1FC00000u && ph < 0x1FC80000u) {
        return;
    }
    stub_bus_fault(c, PSX_EXC_DBE, a);
}

void jit_write16_slow_wrap(PsxCpu *c, u32 a, u32 v) {
    if (!jit_core_is_stub()) {
        psx_bus_write16_slow(c, a, v);
        return;
    }
    PsxSystem *sys = (PsxSystem *)c->sys.p;
    sys->stats.slow_writes++;
    if (PSX_UNLIKELY(psx_addr_fault(c, a, 2u))) {
        stub_bus_fault(c, PSX_EXC_ADES, a);
        return;
    }
    if ((c->cop0[COP0_SR] & PSX_SR_ISC) != 0u) {
        return;
    }
    bool seg_ok = (a < 0x20000000u) || (a >= 0x80000000u && a < 0xC0000000u);
    if (!seg_ok) {
        stub_bus_fault(c, PSX_EXC_DBE, a);
        return;
    }
    u32 ph = a & 0x1FFFFFFFu;
    u16 v16 = (u16)v;
    if (ph < 0x00800000u) {
        if (ph >= 0x00200000u && sys->cfg.strict_ram_size) {
            stub_bus_fault(c, PSX_EXC_DBE, a);
            return;
        }
        u32 off = ph & PSX_RAM_MASK;
        sys->stats.smc_traps++;
        u16 cur = 0u;
        memcpy(&cur, sys->bus.ram + off, 2u);
        if (cur != v16) {
            memcpy(sys->bus.ram + off, &v16, 2u);
            jit_invalidate_page(sys, off >> 12);
        }
        return;
    }
    if (ph < 0x1F000000u) {
        stub_bus_fault(c, PSX_EXC_DBE, a);
        return;
    }
    if (ph < 0x1F800000u) {
        return;
    }
    if (ph < 0x1F800400u && a < 0xA0000000u && sys->bus.scratch_page) {
        memcpy(sys->bus.scratch_page + (ph & PSX_SCRATCH_MASK), &v16, 2u);
        return;
    }
    if (ph < 0x1F801000u) {
        stub_bus_fault(c, PSX_EXC_DBE, a);
        return;
    }
    if (ph < 0x1F802000u) {
        stub_mmio_write(sys, ph, (u32)v16, 2u);
        return;
    }
    if (ph < 0x1F803000u || (ph >= 0x1FA00000u && ph < 0x1FC80000u)) {
        return;
    }
    stub_bus_fault(c, PSX_EXC_DBE, a);
}

void jit_write8_slow_wrap(PsxCpu *c, u32 a, u32 v) {
    if (!jit_core_is_stub()) {
        psx_bus_write8_slow(c, a, v);
        return;
    }
    PsxSystem *sys = (PsxSystem *)c->sys.p;
    sys->stats.slow_writes++;
    if (PSX_UNLIKELY(psx_addr_fault(c, a, 1u))) {
        stub_bus_fault(c, PSX_EXC_ADES, a);
        return;
    }
    if ((c->cop0[COP0_SR] & PSX_SR_ISC) != 0u) {
        return;
    }
    bool seg_ok = (a < 0x20000000u) || (a >= 0x80000000u && a < 0xC0000000u);
    if (!seg_ok) {
        stub_bus_fault(c, PSX_EXC_DBE, a);
        return;
    }
    u32 ph = a & 0x1FFFFFFFu;
    u8 v8 = (u8)v;
    if (ph < 0x00800000u) {
        if (ph >= 0x00200000u && sys->cfg.strict_ram_size) {
            stub_bus_fault(c, PSX_EXC_DBE, a);
            return;
        }
        u32 off = ph & PSX_RAM_MASK;
        sys->stats.smc_traps++;
        if (sys->bus.ram[off] != v8) {
            sys->bus.ram[off] = v8;
            jit_invalidate_page(sys, off >> 12);
        }
        return;
    }
    if (ph < 0x1F000000u) {
        stub_bus_fault(c, PSX_EXC_DBE, a);
        return;
    }
    if (ph < 0x1F800000u) {
        return;
    }
    if (ph < 0x1F800400u && a < 0xA0000000u && sys->bus.scratch_page) {
        sys->bus.scratch_page[ph & PSX_SCRATCH_MASK] = v8;
        return;
    }
    if (ph < 0x1F801000u) {
        stub_bus_fault(c, PSX_EXC_DBE, a);
        return;
    }
    if (ph < 0x1F802000u) {
        stub_mmio_write(sys, ph, (u32)v8, 1u);
        return;
    }
    if (ph < 0x1F803000u) {
        if (ph == PSX_REG_DUART_THR || ph == PSX_REG_PCSX_STDOUT) {
            stub_tty_putc(sys, (char)v8);
        } else if (ph == PSX_REG_EXP2_POST) {
            sys->bus.exp2_post = v8;
        }
        return;
    }
    if (ph >= 0x1FA00000u && ph < 0x1FC80000u) {
        return;
    }
    stub_bus_fault(c, PSX_EXC_DBE, a);
}

u32 jit_fetch32_wrap(PsxCpu *c, u32 pc) {
    if (!jit_core_is_stub()) {
        return psx_bus_fetch32(c, pc);
    }
    PsxSystem *sys = (PsxSystem *)c->sys.p;
    if ((pc & 3u) != 0u ||
        (((c->cop0[COP0_SR] & PSX_SR_KUC) != 0u) && ((pc & 0x80000000u) != 0u))) {
        jit_raise_exc_wrap(sys, PSX_EXC_ADEL, pc, c->in_delay_slot != 0u,
                           c->branch_target, pc, 0u);
        return 0u;
    }
    bool seg_ok = (pc < 0x20000000u) || (pc >= 0x80000000u && pc < 0xC0000000u);
    if (!seg_ok) {
        jit_raise_exc_wrap(sys, PSX_EXC_IBE, pc, c->in_delay_slot != 0u,
                           c->branch_target, pc, 0u);
        return 0u;
    }
    u32 ph = pc & 0x1FFFFFFFu;
    if (ph < 0x00800000u) {
        if (ph >= 0x00200000u && sys->cfg.strict_ram_size) {
            jit_raise_exc_wrap(sys, PSX_EXC_IBE, pc, c->in_delay_slot != 0u,
                               c->branch_target, pc, 0u);
            return 0u;
        }
        u32 insn = 0u;
        memcpy(&insn, sys->bus.ram + (ph & PSX_RAM_MASK), 4u);
        return insn;
    }
    if (ph >= 0x1FC00000u && ph < 0x1FC80000u && sys->bus.bios) {
        u32 insn = 0u;
        memcpy(&insn, sys->bus.bios + (ph & PSX_BIOS_MASK), 4u);
        return insn;
    }
    if (ph >= 0x1F801000u && ph < 0x1F802000u) {
        return stub_mmio_read(sys, ph, 4u);
    }
    if ((ph >= 0x1F000000u && ph < 0x1F800000u) ||
        (ph >= 0x1F802000u && ph < 0x1F803000u) ||
        (ph >= 0x1FA00000u && ph < 0x1FC00000u)) {
        return 0xFFFFFFFFu;
    }
    /* Scratchpad 0x1F800000..0x1F800FFF and unmapped ranges raise IBE on fetch */
    jit_raise_exc_wrap(sys, PSX_EXC_IBE, pc, c->in_delay_slot != 0u,
                       c->branch_target, pc, 0u);
    return 0u;
}

/* ---- Standalone Reference GTE (used when gte_unr_table[0] == 0) ---- */

static inline u32 stub_lzcr(u32 v) {
    if ((v & 0x80000000u) != 0u) {
        v = ~v;
    }
    if (v == 0u) {
        return 32u;
    }
    return (u32)__builtin_clz(v);
}

static inline u32 stub_pack_orgb(const PsxCpu *cpu) {
    s32 ir1 = (s16)cpu->gte_d[9] >> 7;
    s32 ir2 = (s16)cpu->gte_d[10] >> 7;
    s32 ir3 = (s16)cpu->gte_d[11] >> 7;
    if (ir1 < 0) ir1 = 0; else if (ir1 > 31) ir1 = 31;
    if (ir2 < 0) ir2 = 0; else if (ir2 > 31) ir2 = 31;
    if (ir3 < 0) ir3 = 0; else if (ir3 > 31) ir3 = 31;
    return (u32)ir1 | ((u32)ir2 << 5) | ((u32)ir3 << 10);
}

u32 jit_gte_read_data_wrap(PsxCpu *cpu, u32 reg) {
    if (!jit_core_is_stub()) {
        return gte_read_data(cpu, reg);
    }
    reg &= 31u;
    switch (reg) {
        case 1u: case 3u: case 5u:
        case 8u: case 9u: case 10u: case 11u:
            return (u32)(s32)(s16)cpu->gte_d[reg];
        case 7u:
        case 16u: case 17u: case 18u: case 19u:
            return cpu->gte_d[reg] & 0xFFFFu;
        case 15u:
            return cpu->gte_d[14];
        case 28u:
        case 29u:
            return stub_pack_orgb(cpu);
        case 31u:
            return stub_lzcr(cpu->gte_d[30]);
        default:
            return cpu->gte_d[reg];
    }
}

void jit_gte_write_data_wrap(PsxCpu *cpu, u32 reg, u32 val) {
    if (!jit_core_is_stub()) {
        gte_write_data(cpu, reg, val);
        return;
    }
    reg &= 31u;
    switch (reg) {
        case 1u: case 3u: case 5u:
        case 8u: case 9u: case 10u: case 11u:
            cpu->gte_d[reg] = (u32)(s32)(s16)val;
            break;
        case 7u:
        case 16u: case 17u: case 18u: case 19u:
            cpu->gte_d[reg] = val & 0xFFFFu;
            break;
        case 15u:
            cpu->gte_d[12] = cpu->gte_d[13];
            cpu->gte_d[13] = cpu->gte_d[14];
            cpu->gte_d[14] = val;
            cpu->gte_d[15] = val;
            break;
        case 28u:
            cpu->gte_d[28] = val & 0x7FFFu;
            cpu->gte_d[9]  = (u32)((val & 0x1Fu) << 7);
            cpu->gte_d[10] = (u32)(((val >> 5) & 0x1Fu) << 7);
            cpu->gte_d[11] = (u32)(((val >> 10) & 0x1Fu) << 7);
            break;
        case 29u:
        case 31u:
            /* ORGB and LZCR are read-only */
            break;
        case 30u:
            cpu->gte_d[30] = val;
            cpu->gte_d[31] = stub_lzcr(val);
            break;
        default:
            cpu->gte_d[reg] = val;
            break;
    }
}

u32 jit_gte_read_ctrl_wrap(const PsxCpu *cpu, u32 reg) {
    if (!jit_core_is_stub()) {
        return gte_read_ctrl(cpu, reg);
    }
    reg &= 31u;
    switch (reg) {
        case 4u: case 12u: case 20u:
        case 26u: case 27u: case 29u: case 30u:
            return (u32)(s32)(s16)cpu->gte_c[reg];
        case 31u: {
            u32 f = cpu->gte_c[31] & 0x7FFFF000u;
            if ((f & 0x7F87E000u) != 0u) {
                f |= 0x80000000u;
            }
            return f;
        }
        default:
            return cpu->gte_c[reg];
    }
}

void jit_gte_write_ctrl_wrap(PsxCpu *cpu, u32 reg, u32 val) {
    if (!jit_core_is_stub()) {
        gte_write_ctrl(cpu, reg, val);
        return;
    }
    reg &= 31u;
    switch (reg) {
        case 4u: case 12u: case 20u:
        case 26u: case 27u: case 29u: case 30u:
            cpu->gte_c[reg] = (u32)(s32)(s16)val;
            break;
        case 31u: {
            u32 f = val & 0x7FFFF000u;
            if ((f & 0x7F87E000u) != 0u) {
                f |= 0x80000000u;
            }
            cpu->gte_c[31] = f;
            break;
        }
        default:
            cpu->gte_c[reg] = val;
            break;
    }
}

u32 jit_gte_div_unr_wrap(u32 h, u32 sz3, bool *overflow) {
    if (!jit_core_is_stub()) {
        return gte_div_unr(h, sz3, overflow);
    }
    h &= 0xFFFFu;
    sz3 &= 0xFFFFu;
    if (h >= sz3 * 2u) {
        if (overflow) {
            *overflow = true;
        }
        return 0x1FFFFu;
    }
    if (overflow) {
        *overflow = false;
    }
    u32 z = (u32)(__builtin_clz(sz3) - 16);
    u32 n = h << z;
    u32 d = sz3 << z;
    u32 u = (u32)k_jit_unr_fallback[((d & 0x7FFFu) + 0x40u) >> 7] + 0x101u;
    u32 d1 = (0x2000080u - (d * u)) >> 8;
    u32 d2 = (0x0000080u + (d1 * u)) >> 8;
    u32 res = (u32)((((u64)n * (u64)d2) + 0x8000u) >> 16);
    return (res > 0x1FFFFu) ? 0x1FFFFu : res;
}

static inline s64 gte_shl12_s32(s32 val) {
    return (s64)((u64)(s64)val << 12);
}

static inline s64 gte_s44(u32 *flag, int idx /*1..3*/, s64 val) {
    const s64 max44 = ((s64)1 << 43) - 1;
    const s64 min44 = -((s64)1 << 43);
    if (val > max44) {
        *flag |= 1u << (31 - idx); /* 30, 29, 28 */
    } else if (val < min44) {
        *flag |= 1u << (28 - idx); /* 27, 26, 25 */
    }
    return ((s64)((u64)val << 20)) >> 20;
}

static inline s32 gte_set_mac0(PsxCpu *cpu, u32 *flag, s64 val) {
    if (val > 2147483647LL) {
        *flag |= 1u << 16;
    } else if (val < -2147483648LL) {
        *flag |= 1u << 15;
    }
    s32 res = (s32)(u32)(u64)val;
    cpu->gte_d[24] = (u32)res;
    return res;
}

static inline s32 gte_set_mac123(PsxCpu *cpu, u32 *flag, int idx, s64 val, u32 sf) {
    val = gte_s44(flag, idx, val);
    s32 mac = (s32)(u32)(u64)(sf ? (val >> 12) : val);
    cpu->gte_d[24 + idx] = (u32)mac;
    return mac;
}

static inline s16 gte_sat_ir(PsxCpu *cpu, u32 *flag, int idx /*1..3*/, s32 val, bool lm) {
    s32 lo = lm ? 0 : -0x8000;
    s32 hi = 0x7FFF;
    s32 sat = val;
    if (sat < lo) {
        sat = lo;
        *flag |= 1u << (25 - idx); /* 24, 23, 22 */
    } else if (sat > hi) {
        sat = hi;
        *flag |= 1u << (25 - idx);
    }
    cpu->gte_d[8 + idx] = (u32)(s32)(s16)sat;
    return (s16)sat;
}

static inline s32 gte_sat_ir0(PsxCpu *cpu, u32 *flag, s32 val) {
    s32 sat = val;
    if (sat < 0) {
        sat = 0;
        *flag |= 1u << 12;
    } else if (sat > 0x1000) {
        sat = 0x1000;
        *flag |= 1u << 12;
    }
    cpu->gte_d[8] = (u32)sat;
    return sat;
}

static inline u16 gte_sat_sz3_otz(u32 *flag, s64 val) {
    s32 sat;
    if (val < 0) {
        sat = 0;
        *flag |= 1u << 18;
    } else if (val > 0xFFFF) {
        sat = 0xFFFF;
        *flag |= 1u << 18;
    } else {
        sat = (s32)val;
    }
    return (u16)sat;
}

static inline s16 gte_sat_sxy(u32 *flag, int bit /*14=SX, 13=SY*/, s32 val) {
    if (val < -0x400) {
        *flag |= 1u << bit;
        return -0x400;
    }
    if (val > 0x3FF) {
        *flag |= 1u << bit;
        return 0x3FF;
    }
    return (s16)val;
}

static inline u8 gte_sat_fifo_rgb(u32 *flag, int bit /*21=R, 20=G, 19=B*/, s32 val) {
    if (val < 0) {
        *flag |= 1u << bit;
        return 0u;
    }
    if (val > 0xFF) {
        *flag |= 1u << bit;
        return 0xFFu;
    }
    return (u8)val;
}

static void gte_load_mtx(const PsxCpu *cpu, u32 mx, s16 m[3][3]) {
    if (mx < 3u) {
        u32 base = mx * 8u; /* 0: R, 8: L, 16: LC */
        u32 w0 = cpu->gte_c[base + 0u];
        u32 w1 = cpu->gte_c[base + 1u];
        u32 w2 = cpu->gte_c[base + 2u];
        u32 w3 = cpu->gte_c[base + 3u];
        u32 w4 = cpu->gte_c[base + 4u];
        m[0][0] = (s16)(w0 & 0xFFFFu);
        m[0][1] = (s16)(w0 >> 16);
        m[0][2] = (s16)(w1 & 0xFFFFu);
        m[1][0] = (s16)(w1 >> 16);
        m[1][1] = (s16)(w2 & 0xFFFFu);
        m[1][2] = (s16)(w2 >> 16);
        m[2][0] = (s16)(w3 & 0xFFFFu);
        m[2][1] = (s16)(w3 >> 16);
        m[2][2] = (s16)(w4 & 0xFFFFu);
    } else {
        /* Hardware mx==3 garbage matrix */
        s16 r4 = (s16)((u16)(cpu->gte_d[6] & 0xFFu) << 4);
        s16 r13 = (s16)(cpu->gte_c[1] & 0xFFFFu);
        s16 r22 = (s16)(cpu->gte_c[2] & 0xFFFFu);
        m[0][0] = (s16)(-r4);
        m[0][1] = r4;
        m[0][2] = (s16)cpu->gte_d[8];
        m[1][0] = r13; m[1][1] = r13; m[1][2] = r13;
        m[2][0] = r22; m[2][1] = r22; m[2][2] = r22;
    }
}

static void stub_rtps_vertex(PsxCpu *cpu, u32 *flag, int v_idx, u32 sf, bool lm, bool last) {
    s16 m[3][3];
    gte_load_mtx(cpu, 0u, m);
    u32 vxy = cpu->gte_d[v_idx * 2];
    s16 vx = (s16)(vxy & 0xFFFFu);
    s16 vy = (s16)(vxy >> 16);
    s16 vz = (s16)(cpu->gte_d[v_idx * 2 + 1] & 0xFFFFu);

    s64 raw[3];
    for (int k = 0; k < 3; ++k) {
        s64 acc = gte_shl12_s32((s32)cpu->gte_c[5 + k]);
        acc = gte_s44(flag, k + 1, acc + (s64)m[k][0] * (s64)vx);
        acc = gte_s44(flag, k + 1, acc + (s64)m[k][1] * (s64)vy);
        acc = gte_s44(flag, k + 1, acc + (s64)m[k][2] * (s64)vz);
        raw[k] = acc;
        s32 mac = (s32)(u32)(u64)(sf ? (acc >> 12) : acc);
        cpu->gte_d[25 + k] = (u32)mac;
    }

    (void)gte_sat_ir(cpu, flag, 1, (s32)cpu->gte_d[25], lm);
    (void)gte_sat_ir(cpu, flag, 2, (s32)cpu->gte_d[26], lm);

    /* RTPS/RTPT hardware quirk: IR3 flag checks (raw[2] >> 12) against -0x8000..0x7FFF */
    s64 ir3_chk = raw[2] >> 12;
    if (ir3_chk < -0x8000 || ir3_chk > 0x7FFF) {
        *flag |= 1u << 22;
    }
    s32 mac3 = (s32)cpu->gte_d[27];
    s32 ir3_lo = lm ? 0 : -0x8000;
    s32 ir3_val = (mac3 < ir3_lo) ? ir3_lo : ((mac3 > 0x7FFF) ? 0x7FFF : mac3);
    cpu->gte_d[11] = (u32)(s32)(s16)ir3_val;

    u16 sz3 = gte_sat_sz3_otz(flag, raw[2] >> 12);
    cpu->gte_d[16] = cpu->gte_d[17];
    cpu->gte_d[17] = cpu->gte_d[18];
    cpu->gte_d[18] = cpu->gte_d[19];
    cpu->gte_d[19] = (u32)sz3;

    bool div_ovf = false;
    u32 recip = jit_gte_div_unr_wrap(cpu->gte_c[26] & 0xFFFFu, sz3, &div_ovf);
    if (div_ovf) {
        *flag |= 1u << 17;
    }

    s64 mx_proj = (s64)recip * (s64)(s16)cpu->gte_d[9] + (s64)(s32)cpu->gte_c[24];
    (void)gte_set_mac0(cpu, flag, mx_proj);
    s16 sx2 = gte_sat_sxy(flag, 14, (s32)(mx_proj >> 16));

    s64 my_proj = (s64)recip * (s64)(s16)cpu->gte_d[10] + (s64)(s32)cpu->gte_c[25];
    (void)gte_set_mac0(cpu, flag, my_proj);
    s16 sy2 = gte_sat_sxy(flag, 13, (s32)(my_proj >> 16));

    u32 sxy_packed = ((u32)(u16)sx2) | ((u32)(u16)sy2 << 16);
    cpu->gte_d[12] = cpu->gte_d[13];
    cpu->gte_d[13] = cpu->gte_d[14];
    cpu->gte_d[14] = sxy_packed;
    cpu->gte_d[15] = sxy_packed;

    if (last) {
        s64 dq = (s64)recip * (s64)(s16)cpu->gte_c[27] + (s64)(s32)cpu->gte_c[28];
        (void)gte_set_mac0(cpu, flag, dq);
        (void)gte_sat_ir0(cpu, flag, (s32)(dq >> 12));
    }
}

static void stub_push_rgb_from_mac(PsxCpu *cpu, u32 *flag) {
    u8 r = gte_sat_fifo_rgb(flag, 21, (s32)cpu->gte_d[25] >> 4);
    u8 g = gte_sat_fifo_rgb(flag, 20, (s32)cpu->gte_d[26] >> 4);
    u8 b = gte_sat_fifo_rgb(flag, 19, (s32)cpu->gte_d[27] >> 4);
    u32 code = cpu->gte_d[6] & 0xFF000000u;
    cpu->gte_d[20] = cpu->gte_d[21];
    cpu->gte_d[21] = cpu->gte_d[22];
    cpu->gte_d[22] = code | (u32)r | ((u32)g << 8) | ((u32)b << 16);
}

static void stub_depth_cue_color(PsxCpu *cpu, u32 *flag, u32 rgb_word, u32 sf, bool lm, bool mult_ir) {
    s32 c[3];
    c[0] = (s32)(rgb_word & 0xFFu) << 4;
    c[1] = (s32)((rgb_word >> 8) & 0xFFu) << 4;
    c[2] = (s32)((rgb_word >> 16) & 0xFFu) << 4;
    s16 ir0 = (s16)cpu->gte_d[8];

    for (int k = 0; k < 3; ++k) {
        s64 base = mult_ir ? ((s64)c[k] * (s64)(s16)cpu->gte_d[9 + k]) : gte_shl12_s32(c[k]);
        s64 diff = gte_s44(flag, k + 1, gte_shl12_s32((s32)cpu->gte_c[21 + k]) - base);
        s32 tmp_ir = (s32)(u32)(u64)(sf ? (diff >> 12) : diff);
        /* Stage A saturates with lm=0 */
        u32 dummy_ir_val = (u32)(s32)gte_sat_ir(cpu, flag, k + 1, tmp_ir, false);
        s16 stage_ir = (s16)dummy_ir_val;
        s64 fin = gte_s44(flag, k + 1, base + (s64)ir0 * (s64)stage_ir);
        s32 mac = (s32)(u32)(u64)(sf ? (fin >> 12) : fin);
        cpu->gte_d[25 + k] = (u32)mac;
    }
    stub_push_rgb_from_mac(cpu, flag);
    for (int k = 0; k < 3; ++k) {
        (void)gte_sat_ir(cpu, flag, k + 1, (s32)cpu->gte_d[25 + k], lm);
    }
}

static void stub_nc_vertex(PsxCpu *cpu, u32 *flag, int v_idx, u32 sf, bool lm, bool use_rgb, bool depth_cue) {
    s16 lmtx[3][3], cmtx[3][3];
    gte_load_mtx(cpu, 1u, lmtx);
    gte_load_mtx(cpu, 2u, cmtx);
    u32 vxy = cpu->gte_d[v_idx * 2];
    s16 vx = (s16)(vxy & 0xFFFFu);
    s16 vy = (s16)(vxy >> 16);
    s16 vz = (s16)(cpu->gte_d[v_idx * 2 + 1] & 0xFFFFu);

    for (int k = 0; k < 3; ++k) {
        s64 acc = gte_s44(flag, k + 1, (s64)lmtx[k][0] * vx);
        acc = gte_s44(flag, k + 1, acc + (s64)lmtx[k][1] * vy);
        acc = gte_s44(flag, k + 1, acc + (s64)lmtx[k][2] * vz);
        s32 mac = (s32)(u32)(u64)(sf ? (acc >> 12) : acc);
        cpu->gte_d[25 + k] = (u32)mac;
        (void)gte_sat_ir(cpu, flag, k + 1, mac, lm);
    }

    s16 ir1 = (s16)cpu->gte_d[9];
    s16 ir2 = (s16)cpu->gte_d[10];
    s16 ir3 = (s16)cpu->gte_d[11];
    for (int k = 0; k < 3; ++k) {
        s64 acc = gte_shl12_s32((s32)cpu->gte_c[13 + k]);
        acc = gte_s44(flag, k + 1, acc + (s64)cmtx[k][0] * ir1);
        acc = gte_s44(flag, k + 1, acc + (s64)cmtx[k][1] * ir2);
        acc = gte_s44(flag, k + 1, acc + (s64)cmtx[k][2] * ir3);
        s32 mac = (s32)(u32)(u64)(sf ? (acc >> 12) : acc);
        cpu->gte_d[25 + k] = (u32)mac;
        (void)gte_sat_ir(cpu, flag, k + 1, mac, lm);
    }

    if (depth_cue) {
        stub_depth_cue_color(cpu, flag, cpu->gte_d[6], sf, lm, true);
        return;
    }
    if (use_rgb) {
        u32 rgb = cpu->gte_d[6];
        s32 c[3] = {
            (s32)(rgb & 0xFFu) << 4,
            (s32)((rgb >> 8) & 0xFFu) << 4,
            (s32)((rgb >> 16) & 0xFFu) << 4
        };
        for (int k = 0; k < 3; ++k) {
            s64 prod = gte_s44(flag, k + 1, (s64)c[k] * (s64)(s16)cpu->gte_d[9 + k]);
            s32 mac = (s32)(u32)(u64)(sf ? (prod >> 12) : prod);
            cpu->gte_d[25 + k] = (u32)mac;
        }
    }
    stub_push_rgb_from_mac(cpu, flag);
    if (use_rgb) {
        for (int k = 0; k < 3; ++k) {
            (void)gte_sat_ir(cpu, flag, k + 1, (s32)cpu->gte_d[25 + k], lm);
        }
    }
}

void jit_gte_exec_wrap(PsxCpu *cpu, u32 opcode) {
    PsxSystem *sys = (PsxSystem *)cpu->sys.p;
    if (sys) {
        sys->stats.gte_helper++;
    }
    if (!jit_core_is_stub()) {
        gte_exec_c(cpu, opcode);
        return;
    }

    u32 sf = (opcode >> 19) & 1u;
    bool lm = ((opcode >> 10) & 1u) != 0u;
    u32 cmd = opcode & 0x3Fu;
    u32 flag = 0u;

    switch (cmd) {
        case 0x01u: /* RTPS */
            stub_rtps_vertex(cpu, &flag, 0, sf, lm, true);
            break;
        case 0x06u: { /* NCLIP */
            s16 sx0 = (s16)(cpu->gte_d[12] & 0xFFFFu);
            s16 sy0 = (s16)(cpu->gte_d[12] >> 16);
            s16 sx1 = (s16)(cpu->gte_d[13] & 0xFFFFu);
            s16 sy1 = (s16)(cpu->gte_d[13] >> 16);
            s16 sx2 = (s16)(cpu->gte_d[14] & 0xFFFFu);
            s16 sy2 = (s16)(cpu->gte_d[14] >> 16);
            s64 area = (s64)sx0 * ((s64)sy1 - (s64)sy2)
                     + (s64)sx1 * ((s64)sy2 - (s64)sy0)
                     + (s64)sx2 * ((s64)sy0 - (s64)sy1);
            (void)gte_set_mac0(cpu, &flag, area);
            break;
        }
        case 0x0Cu: { /* OP */
            s16 d1 = (s16)(cpu->gte_c[0] & 0xFFFFu); /* R11 */
            s16 d2 = (s16)(cpu->gte_c[2] & 0xFFFFu); /* R22 */
            s16 d3 = (s16)(cpu->gte_c[4] & 0xFFFFu); /* R33 */
            s16 ir1 = (s16)cpu->gte_d[9];
            s16 ir2 = (s16)cpu->gte_d[10];
            s16 ir3 = (s16)cpu->gte_d[11];
            s32 m1 = gte_set_mac123(cpu, &flag, 1, (s64)ir3 * d2 - (s64)ir2 * d3, sf);
            s32 m2 = gte_set_mac123(cpu, &flag, 2, (s64)ir1 * d3 - (s64)ir3 * d1, sf);
            s32 m3 = gte_set_mac123(cpu, &flag, 3, (s64)ir2 * d1 - (s64)ir1 * d2, sf);
            (void)gte_sat_ir(cpu, &flag, 1, m1, lm);
            (void)gte_sat_ir(cpu, &flag, 2, m2, lm);
            (void)gte_sat_ir(cpu, &flag, 3, m3, lm);
            break;
        }
        case 0x10u: /* DPCS */
            stub_depth_cue_color(cpu, &flag, cpu->gte_d[6], sf, lm, false);
            break;
        case 0x11u: { /* INTPL */
            s16 ir0 = (s16)cpu->gte_d[8];
            s32 orig[3] = {
                (s32)(s16)cpu->gte_d[9],
                (s32)(s16)cpu->gte_d[10],
                (s32)(s16)cpu->gte_d[11]
            };
            for (int k = 0; k < 3; ++k) {
                s64 base = gte_shl12_s32(orig[k]);
                s64 diff = gte_s44(&flag, k + 1, gte_shl12_s32((s32)cpu->gte_c[21 + k]) - base);
                s32 tmp = (s32)(u32)(u64)(sf ? (diff >> 12) : diff);
                s16 stage_ir = gte_sat_ir(cpu, &flag, k + 1, tmp, false);
                s64 fin = gte_s44(&flag, k + 1, base + (s64)ir0 * (s64)stage_ir);
                cpu->gte_d[25 + k] = (u32)(s32)(u64)(sf ? (fin >> 12) : fin);
            }
            stub_push_rgb_from_mac(cpu, &flag);
            for (int k = 0; k < 3; ++k) {
                (void)gte_sat_ir(cpu, &flag, k + 1, (s32)cpu->gte_d[25 + k], lm);
            }
            break;
        }
        case 0x12u: { /* MVMVA */
            u32 mx = (opcode >> 17) & 3u;
            u32 vx_sel = (opcode >> 15) & 3u;
            u32 cv = (opcode >> 13) & 3u;
            s16 m[3][3];
            gte_load_mtx(cpu, mx, m);
            s16 vx, vy, vz;
            if (vx_sel < 3u) {
                u32 vxy = cpu->gte_d[vx_sel * 2u];
                vx = (s16)(vxy & 0xFFFFu);
                vy = (s16)(vxy >> 16);
                vz = (s16)(cpu->gte_d[vx_sel * 2u + 1u] & 0xFFFFu);
            } else {
                vx = (s16)cpu->gte_d[9];
                vy = (s16)cpu->gte_d[10];
                vz = (s16)cpu->gte_d[11];
            }
            for (int k = 0; k < 3; ++k) {
                if (cv == 2u) {
                    /* Far Color hardware bug */
                    s64 bug = gte_s44(&flag, k + 1,
                        gte_shl12_s32((s32)cpu->gte_c[21 + k]) + (s64)m[k][0] * vx);
                    s32 bug_mac = (s32)(u32)(u64)(sf ? (bug >> 12) : bug);
                    (void)gte_sat_ir(cpu, &flag, k + 1, bug_mac, false);
                    s64 acc = gte_s44(&flag, k + 1, (s64)m[k][1] * vy);
                    acc = gte_s44(&flag, k + 1, acc + (s64)m[k][2] * vz);
                    s32 mac = (s32)(u32)(u64)(sf ? (acc >> 12) : acc);
                    cpu->gte_d[25 + k] = (u32)mac;
                    (void)gte_sat_ir(cpu, &flag, k + 1, mac, lm);
                } else {
                    s64 tr = 0;
                    if (cv == 0u) tr = gte_shl12_s32((s32)cpu->gte_c[5 + k]);
                    else if (cv == 1u) tr = gte_shl12_s32((s32)cpu->gte_c[13 + k]);
                    s64 acc = gte_s44(&flag, k + 1, tr + (s64)m[k][0] * vx);
                    acc = gte_s44(&flag, k + 1, acc + (s64)m[k][1] * vy);
                    acc = gte_s44(&flag, k + 1, acc + (s64)m[k][2] * vz);
                    s32 mac = (s32)(u32)(u64)(sf ? (acc >> 12) : acc);
                    cpu->gte_d[25 + k] = (u32)mac;
                    (void)gte_sat_ir(cpu, &flag, k + 1, mac, lm);
                }
            }
            break;
        }
        case 0x13u: /* NCDS */
            stub_nc_vertex(cpu, &flag, 0, sf, lm, true, true);
            break;
        case 0x14u: { /* CDP */
            s16 cmtx[3][3];
            gte_load_mtx(cpu, 2u, cmtx);
            s16 ir1 = (s16)cpu->gte_d[9];
            s16 ir2 = (s16)cpu->gte_d[10];
            s16 ir3 = (s16)cpu->gte_d[11];
            for (int k = 0; k < 3; ++k) {
                s64 acc = gte_shl12_s32((s32)cpu->gte_c[13 + k]);
                acc = gte_s44(&flag, k + 1, acc + (s64)cmtx[k][0] * ir1);
                acc = gte_s44(&flag, k + 1, acc + (s64)cmtx[k][1] * ir2);
                acc = gte_s44(&flag, k + 1, acc + (s64)cmtx[k][2] * ir3);
                s32 mac = (s32)(u32)(u64)(sf ? (acc >> 12) : acc);
                cpu->gte_d[25 + k] = (u32)mac;
                (void)gte_sat_ir(cpu, &flag, k + 1, mac, lm);
            }
            stub_depth_cue_color(cpu, &flag, cpu->gte_d[6], sf, lm, true);
            break;
        }
        case 0x16u: /* NCDT */
            stub_nc_vertex(cpu, &flag, 0, sf, lm, true, true);
            stub_nc_vertex(cpu, &flag, 1, sf, lm, true, true);
            stub_nc_vertex(cpu, &flag, 2, sf, lm, true, true);
            break;
        case 0x1Bu: /* NCCS */
            stub_nc_vertex(cpu, &flag, 0, sf, lm, true, false);
            break;
        case 0x1Cu: { /* CC */
            s16 cmtx[3][3];
            gte_load_mtx(cpu, 2u, cmtx);
            s16 ir1 = (s16)cpu->gte_d[9];
            s16 ir2 = (s16)cpu->gte_d[10];
            s16 ir3 = (s16)cpu->gte_d[11];
            for (int k = 0; k < 3; ++k) {
                s64 acc = gte_shl12_s32((s32)cpu->gte_c[13 + k]);
                acc = gte_s44(&flag, k + 1, acc + (s64)cmtx[k][0] * ir1);
                acc = gte_s44(&flag, k + 1, acc + (s64)cmtx[k][1] * ir2);
                acc = gte_s44(&flag, k + 1, acc + (s64)cmtx[k][2] * ir3);
                s32 mac = (s32)(u32)(u64)(sf ? (acc >> 12) : acc);
                cpu->gte_d[25 + k] = (u32)mac;
                (void)gte_sat_ir(cpu, &flag, k + 1, mac, lm);
            }
            u32 rgb = cpu->gte_d[6];
            s32 c[3] = {
                (s32)(rgb & 0xFFu) << 4,
                (s32)((rgb >> 8) & 0xFFu) << 4,
                (s32)((rgb >> 16) & 0xFFu) << 4
            };
            for (int k = 0; k < 3; ++k) {
                s64 prod = gte_s44(&flag, k + 1, (s64)c[k] * (s64)(s16)cpu->gte_d[9 + k]);
                cpu->gte_d[25 + k] = (u32)(s32)(u64)(sf ? (prod >> 12) : prod);
            }
            stub_push_rgb_from_mac(cpu, &flag);
            for (int k = 0; k < 3; ++k) {
                (void)gte_sat_ir(cpu, &flag, k + 1, (s32)cpu->gte_d[25 + k], lm);
            }
            break;
        }
        case 0x1Eu: /* NCS */
            stub_nc_vertex(cpu, &flag, 0, sf, lm, false, false);
            break;
        case 0x20u: /* NCT */
            stub_nc_vertex(cpu, &flag, 0, sf, lm, false, false);
            stub_nc_vertex(cpu, &flag, 1, sf, lm, false, false);
            stub_nc_vertex(cpu, &flag, 2, sf, lm, false, false);
            break;
        case 0x28u: { /* SQR */
            for (int k = 0; k < 3; ++k) {
                s32 ir = (s16)cpu->gte_d[9 + k];
                s32 sq = (ir * ir) >> (sf ? 12 : 0);
                cpu->gte_d[25 + k] = (u32)sq;
                (void)gte_sat_ir(cpu, &flag, k + 1, sq, lm);
            }
            break;
        }
        case 0x29u: /* DCPL */
            stub_depth_cue_color(cpu, &flag, cpu->gte_d[6], sf, lm, true);
            break;
        case 0x2Au: { /* DPCT */
            for (int pass = 0; pass < 3; ++pass) {
                stub_depth_cue_color(cpu, &flag, cpu->gte_d[20], sf, lm, false);
            }
            break;
        }
        case 0x2Du: { /* AVSZ3 */
            s64 sum = (s64)(u16)cpu->gte_d[17] + (s64)(u16)cpu->gte_d[18] + (s64)(u16)cpu->gte_d[19];
            s64 prod = (s64)(s16)cpu->gte_c[29] * sum;
            (void)gte_set_mac0(cpu, &flag, prod);
            cpu->gte_d[7] = (u32)gte_sat_sz3_otz(&flag, prod >> 12);
            break;
        }
        case 0x2Eu: { /* AVSZ4 */
            s64 sum = (s64)(u16)cpu->gte_d[16] + (s64)(u16)cpu->gte_d[17]
                    + (s64)(u16)cpu->gte_d[18] + (s64)(u16)cpu->gte_d[19];
            s64 prod = (s64)(s16)cpu->gte_c[30] * sum;
            (void)gte_set_mac0(cpu, &flag, prod);
            cpu->gte_d[7] = (u32)gte_sat_sz3_otz(&flag, prod >> 12);
            break;
        }
        case 0x30u: /* RTPT */
            stub_rtps_vertex(cpu, &flag, 0, sf, lm, false);
            stub_rtps_vertex(cpu, &flag, 1, sf, lm, false);
            stub_rtps_vertex(cpu, &flag, 2, sf, lm, true);
            break;
        case 0x3Du: { /* GPF */
            s16 ir0 = (s16)cpu->gte_d[8];
            for (int k = 0; k < 3; ++k) {
                s64 prod = (s64)ir0 * (s64)(s16)cpu->gte_d[9 + k];
                s32 mac = gte_set_mac123(cpu, &flag, k + 1, prod, sf);
                (void)gte_sat_ir(cpu, &flag, k + 1, mac, lm);
            }
            stub_push_rgb_from_mac(cpu, &flag);
            break;
        }
        case 0x3Eu: { /* GPL */
            s16 ir0 = (s16)cpu->gte_d[8];
            for (int k = 0; k < 3; ++k) {
                s64 base = sf ? gte_shl12_s32((s32)cpu->gte_d[25 + k]) : (s64)(s32)cpu->gte_d[25 + k];
                s64 acc = base + (s64)ir0 * (s64)(s16)cpu->gte_d[9 + k];
                s32 mac = gte_set_mac123(cpu, &flag, k + 1, acc, sf);
                (void)gte_sat_ir(cpu, &flag, k + 1, mac, lm);
            }
            stub_push_rgb_from_mac(cpu, &flag);
            break;
        }
        case 0x3Fu: /* NCCT */
            stub_nc_vertex(cpu, &flag, 0, sf, lm, true, false);
            stub_nc_vertex(cpu, &flag, 1, sf, lm, true, false);
            stub_nc_vertex(cpu, &flag, 2, sf, lm, true, false);
            break;
        default:
            /* Unused COP2 command: hardware clears FLAG without raising RI */
            break;
    }

    flag &= 0x7FFFF000u;
    if ((flag & 0x7F87E000u) != 0u) {
        flag |= 0x80000000u;
    }
    cpu->gte_c[31] = flag;
}

/* ---- Fastmem inline wrappers routing slow path through jit_*_wrap ---- */

static inline u32 cached_read32(PsxCpu *c, u32 a) {
    if (PSX_UNLIKELY(psx_addr_fault(c, a, 4u)) || !c->fastmem_r.p) {
        return jit_read32_slow_wrap(c, a);
    }
    uintptr_t e = ((const uintptr_t *)c->fastmem_r.p)[a >> PSX_FM_SHIFT];
    if (PSX_LIKELY(e != 0u)) {
        u32 v;
        memcpy(&v, (const void *)(e + (uintptr_t)a), 4u);
        return v;
    }
    return jit_read32_slow_wrap(c, a);
}

static inline u16 cached_read16(PsxCpu *c, u32 a) {
    if (PSX_UNLIKELY(psx_addr_fault(c, a, 2u)) || !c->fastmem_r.p) {
        return jit_read16_slow_wrap(c, a);
    }
    uintptr_t e = ((const uintptr_t *)c->fastmem_r.p)[a >> PSX_FM_SHIFT];
    if (PSX_LIKELY(e != 0u)) {
        u16 v;
        memcpy(&v, (const void *)(e + (uintptr_t)a), 2u);
        return v;
    }
    return jit_read16_slow_wrap(c, a);
}

static inline u8 cached_read8(PsxCpu *c, u32 a) {
    if (PSX_UNLIKELY(psx_addr_fault(c, a, 1u)) || !c->fastmem_r.p) {
        return jit_read8_slow_wrap(c, a);
    }
    uintptr_t e = ((const uintptr_t *)c->fastmem_r.p)[a >> PSX_FM_SHIFT];
    return PSX_LIKELY(e != 0u) ? *(const u8 *)(e + (uintptr_t)a) : jit_read8_slow_wrap(c, a);
}

static inline void cached_write32(PsxCpu *c, u32 a, u32 v) {
    if (PSX_UNLIKELY(psx_addr_fault(c, a, 4u)) || !c->fastmem_w.p) {
        jit_write32_slow_wrap(c, a, v);
        return;
    }
    uintptr_t e = ((const uintptr_t *)c->fastmem_w.p)[a >> PSX_FM_SHIFT];
    if (PSX_LIKELY(e != 0u)) {
        memcpy((void *)(e + (uintptr_t)a), &v, 4u);
    } else {
        jit_write32_slow_wrap(c, a, v);
    }
}

static inline void cached_write16(PsxCpu *c, u32 a, u16 v) {
    if (PSX_UNLIKELY(psx_addr_fault(c, a, 2u)) || !c->fastmem_w.p) {
        jit_write16_slow_wrap(c, a, (u32)v);
        return;
    }
    uintptr_t e = ((const uintptr_t *)c->fastmem_w.p)[a >> PSX_FM_SHIFT];
    if (PSX_LIKELY(e != 0u)) {
        memcpy((void *)(e + (uintptr_t)a), &v, 2u);
    } else {
        jit_write16_slow_wrap(c, a, (u32)v);
    }
}

static inline void cached_write8(PsxCpu *c, u32 a, u8 v) {
    if (PSX_UNLIKELY(psx_addr_fault(c, a, 1u)) || !c->fastmem_w.p) {
        jit_write8_slow_wrap(c, a, (u32)v);
        return;
    }
    uintptr_t e = ((const uintptr_t *)c->fastmem_w.p)[a >> PSX_FM_SHIFT];
    if (PSX_LIKELY(e != 0u)) {
        *(u8 *)(e + (uintptr_t)a) = v;
    } else {
        jit_write8_slow_wrap(c, a, (u32)v);
    }
}

u32 jit_lwl_helper(PsxCpu *c, u32 addr, u32 cur_rt) {
    if (PSX_UNLIKELY(((c->cop0[COP0_SR] & PSX_SR_KUC) != 0u) && ((addr & 0x80000000u) != 0u))) {
        c->abort_badvaddr = addr;
        c->jit_abort = PSX_EXC_ADEL + 1u;
        return 0u;
    }
    u32 mem = cached_read32(c, addr & ~3u);
    if (PSX_UNLIKELY(c->jit_abort != 0u)) {
        return 0u;
    }
    u32 sh8 = (addr & 3u) * 8u;
    return (cur_rt & (0x00FFFFFFu >> sh8)) | (mem << (24u - sh8));
}

u32 jit_lwr_helper(PsxCpu *c, u32 addr, u32 cur_rt) {
    if (PSX_UNLIKELY(((c->cop0[COP0_SR] & PSX_SR_KUC) != 0u) && ((addr & 0x80000000u) != 0u))) {
        c->abort_badvaddr = addr;
        c->jit_abort = PSX_EXC_ADEL + 1u;
        return 0u;
    }
    u32 mem = cached_read32(c, addr & ~3u);
    if (PSX_UNLIKELY(c->jit_abort != 0u)) {
        return 0u;
    }
    u32 s = addr & 3u;
    u32 mask = (s == 0u) ? 0u : (0xFFFFFF00u << ((3u - s) * 8u));
    return (cur_rt & mask) | (mem >> (s * 8u));
}

void jit_swl_helper(PsxCpu *c, u32 addr, u32 val_rt) {
    if (PSX_UNLIKELY(((c->cop0[COP0_SR] & PSX_SR_KUC) != 0u) && ((addr & 0x80000000u) != 0u))) {
        c->abort_badvaddr = addr;
        c->jit_abort = PSX_EXC_ADES + 1u;
        return;
    }
    if (PSX_UNLIKELY((c->cop0[COP0_SR] & PSX_SR_ISC) != 0u)) {
        c->icache_tag_dummy[(addr >> 4) & 0xFFu] = val_rt;
        return;
    }
    u32 mem = cached_read32(c, addr & ~3u);
    if (PSX_UNLIKELY(c->jit_abort != 0u)) {
        return;
    }
    u32 s = addr & 3u;
    u32 mask = (s == 3u) ? 0u : (0xFFFFFF00u << (s * 8u));
    cached_write32(c, addr & ~3u, (mem & mask) | (val_rt >> ((3u - s) * 8u)));
}

void jit_swr_helper(PsxCpu *c, u32 addr, u32 val_rt) {
    if (PSX_UNLIKELY(((c->cop0[COP0_SR] & PSX_SR_KUC) != 0u) && ((addr & 0x80000000u) != 0u))) {
        c->abort_badvaddr = addr;
        c->jit_abort = PSX_EXC_ADES + 1u;
        return;
    }
    if (PSX_UNLIKELY((c->cop0[COP0_SR] & PSX_SR_ISC) != 0u)) {
        c->icache_tag_dummy[(addr >> 4) & 0xFFu] = val_rt;
        return;
    }
    u32 mem = cached_read32(c, addr & ~3u);
    if (PSX_UNLIKELY(c->jit_abort != 0u)) {
        return;
    }
    u32 s = addr & 3u;
    u32 mask = (s == 0u) ? 0u : (0x00FFFFFFu >> ((3u - s) * 8u));
    cached_write32(c, addr & ~3u, (mem & mask) | (val_rt << (s * 8u)));
}

static inline bool jit_bios_hook_wrap(PsxSystem *sys, u32 pc) {
    if (bios_pc_hook(sys, pc)) {
        return true;
    }
    if (jit_core_is_stub()) {
        u32 ph = pc & 0x1FFFFFFFu;
        if ((ph == 0xA0u && sys->cpu.gpr[9] == 0x3Cu) ||
            (ph == 0xB0u && sys->cpu.gpr[9] == 0x3Du)) {
            stub_tty_putc(sys, (char)(sys->cpu.gpr[4] & 0xFFu));
        }
    }
    return false;
}

static inline u32 cop0_read_reg(const PsxCpu *c, u32 rd) {
    if (rd == COP0_PRID) {
        return PSX_COP0_PRID_VALUE;
    }
    if (rd < 16u) {
        return c->cop0[rd];
    }
    return 0u;
}

static inline void cop0_write_non_sr_cause(PsxCpu *c, u32 rd, u32 val) {
    switch (rd) {
        case COP0_BPC:
        case COP0_BDA:
        case COP0_DCIC:
        case COP0_BDAM:
        case COP0_BPCM:
            c->cop0[rd] = val;
            break;
        default:
            break;
    }
}

/* Single-instruction reference stepper (used in standalone tests when cpu_step_one is stubbed) */
static void stub_step_one_impl(PsxSystem *sys) {
    PsxCpu *c = &sys->cpu;
    u32 pc = c->pc;
    u32 mult = sys->cfg.cycle_mult ? (u32)sys->cfg.cycle_mult : 256u;
    u32 k = (u32)(c->total_cycles & 0xFFu);
    s32 step_cyc = (s32)((((k + 1u) * mult) >> 8) - ((k * mult) >> 8));
    if (step_cyc <= 0) {
        step_cyc = 1;
    }

    if (c->in_delay_slot == 0u && jit_is_pinned_pc(sys, pc)) {
        if (jit_bios_hook_wrap(sys, pc)) {
            c->cycles_left -= step_cyc;
            c->total_cycles++;
            sys->stats.cycles_total += (u64)step_cyc;
            sys->stats.insns_total++;
            return;
        }
    }

    u32 prev_ld_reg = c->load_delay_reg;
    u32 prev_ld_val = c->load_delay_val;
    u32 cur_ds = c->in_delay_slot;
    u32 cur_bt = c->branch_target;
    u32 cur_btaken = c->branch_taken;

    u32 op_pc = pc;
    u32 next_pc = cur_ds ? (cur_btaken ? cur_bt : c->next_pc) : c->next_pc;

    u32 insn = jit_fetch32_wrap(c, op_pc);
    if (c->pc != op_pc) {
        /* Fetch raised AdEL or IBE */
        c->cycles_left -= step_cyc;
        c->total_cycles++;
        sys->stats.cycles_total += (u64)step_cyc;
        return;
    }

    c->cycles_left -= step_cyc;
    c->total_cycles++;
    sys->stats.cycles_total += (u64)step_cyc;
    sys->stats.insns_total++;

    /* Advance delay-slot state before instruction potentially sets a new branch */
    c->in_delay_slot = 0u;
    c->branch_taken = 0u;

    u32 new_ld_reg = 0u;
    u32 new_ld_val = 0u;
    u32 written_reg = 0u;
    u32 written_val = 0u;

    u32 op = insn >> 26;
    u32 rs = (insn >> 21) & 31u;
    u32 rt = (insn >> 16) & 31u;
    u32 rd = (insn >> 11) & 31u;
    u32 sh = (insn >> 6) & 31u;
    u32 fn = insn & 63u;
    u32 imm16 = insn & 0xFFFFu;
    u32 simm = (u32)(s32)(s16)imm16;

    u32 v_rs = c->gpr[rs];
    u32 v_rt = c->gpr[rt];

    bool raised = false;
#define STUB_RAISE(exc_code, badv, ce_idx)                                      \
    do {                                                                        \
        if (prev_ld_reg != 0u) {                                                \
            c->gpr[prev_ld_reg] = prev_ld_val;                                  \
            c->gpr[0] = 0u;                                                     \
        }                                                                       \
        jit_raise_exc_wrap(sys, (exc_code), op_pc, cur_ds != 0u, next_pc,       \
                           (badv), (ce_idx));                                   \
        raised = true;                                                          \
    } while (0)

    c->in_jit = 1u; /* Route slow-path faults through jit_abort for exact commit */
    c->jit_abort = 0u;

    switch (op) {
        case 0x00u: /* SPECIAL */
            switch (fn) {
                case 0x00u: written_reg = rd; written_val = v_rt << sh; break;
                case 0x02u: written_reg = rd; written_val = v_rt >> sh; break;
                case 0x03u: written_reg = rd; written_val = (u32)((s32)v_rt >> sh); break;
                case 0x04u: written_reg = rd; written_val = v_rt << (v_rs & 31u); break;
                case 0x06u: written_reg = rd; written_val = v_rt >> (v_rs & 31u); break;
                case 0x07u: written_reg = rd; written_val = (u32)((s32)v_rt >> (v_rs & 31u)); break;
                case 0x08u: /* JR */
                    c->in_delay_slot = 1u;
                    c->branch_taken = 1u;
                    c->branch_target = v_rs;
                    c->next_pc = next_pc + 4u;
                    break;
                case 0x09u: /* JALR */
                    written_reg = rd;
                    written_val = next_pc + 4u;
                    c->in_delay_slot = 1u;
                    c->branch_taken = 1u;
                    c->branch_target = v_rs;
                    c->next_pc = next_pc + 4u;
                    break;
                case 0x0Cu: STUB_RAISE(PSX_EXC_SYS, 0u, 0u); break;
                case 0x0Du: STUB_RAISE(PSX_EXC_BP, 0u, 0u); break;
                case 0x10u: written_reg = rd; written_val = c->hi; break;
                case 0x11u: c->hi = v_rs; break;
                case 0x12u: written_reg = rd; written_val = c->lo; break;
                case 0x13u: c->lo = v_rs; break;
                case 0x18u: {
                    s64 p = (s64)(s32)v_rs * (s64)(s32)v_rt;
                    c->lo = (u32)(u64)p;
                    c->hi = (u32)((u64)p >> 32);
                    break;
                }
                case 0x19u: {
                    u64 p = (u64)v_rs * (u64)v_rt;
                    c->lo = (u32)p;
                    c->hi = (u32)(p >> 32);
                    break;
                }
                case 0x1Au: jit_mips_div((s32)v_rs, (s32)v_rt, &c->lo, &c->hi); break;
                case 0x1Bu: jit_mips_divu(v_rs, v_rt, &c->lo, &c->hi); break;
                case 0x20u: {
                    u32 r = v_rs + v_rt;
                    if (((~(v_rs ^ v_rt) & (v_rs ^ r)) & 0x80000000u) != 0u) {
                        STUB_RAISE(PSX_EXC_OV, 0u, 0u);
                    } else {
                        written_reg = rd;
                        written_val = r;
                    }
                    break;
                }
                case 0x21u: written_reg = rd; written_val = v_rs + v_rt; break;
                case 0x22u: {
                    u32 r = v_rs - v_rt;
                    if ((((v_rs ^ v_rt) & (v_rs ^ r)) & 0x80000000u) != 0u) {
                        STUB_RAISE(PSX_EXC_OV, 0u, 0u);
                    } else {
                        written_reg = rd;
                        written_val = r;
                    }
                    break;
                }
                case 0x23u: written_reg = rd; written_val = v_rs - v_rt; break;
                case 0x24u: written_reg = rd; written_val = v_rs & v_rt; break;
                case 0x25u: written_reg = rd; written_val = v_rs | v_rt; break;
                case 0x26u: written_reg = rd; written_val = v_rs ^ v_rt; break;
                case 0x27u: written_reg = rd; written_val = ~(v_rs | v_rt); break;
                case 0x2Au: written_reg = rd; written_val = ((s32)v_rs < (s32)v_rt) ? 1u : 0u; break;
                case 0x2Bu: written_reg = rd; written_val = (v_rs < v_rt) ? 1u : 0u; break;
                default: STUB_RAISE(PSX_EXC_RI, 0u, 0u); break;
            }
            break;

        case 0x01u: { /* REGIMM */
            bool cond = ((rt & 1u) != 0u) ? ((s32)v_rs >= 0) : ((s32)v_rs < 0);
            if ((rt & 0x1Eu) == 0x10u) {
                written_reg = 31u;
                written_val = next_pc + 4u;
            }
            c->in_delay_slot = 1u;
            c->branch_taken = cond ? 1u : 0u;
            c->branch_target = next_pc + (simm << 2);
            c->next_pc = next_pc + 4u;
            break;
        }
        case 0x02u: /* J */
            c->in_delay_slot = 1u;
            c->branch_taken = 1u;
            c->branch_target = (next_pc & 0xF0000000u) | ((insn & 0x03FFFFFFu) << 2);
            c->next_pc = next_pc + 4u;
            break;
        case 0x03u: /* JAL */
            written_reg = 31u;
            written_val = next_pc + 4u;
            c->in_delay_slot = 1u;
            c->branch_taken = 1u;
            c->branch_target = (next_pc & 0xF0000000u) | ((insn & 0x03FFFFFFu) << 2);
            c->next_pc = next_pc + 4u;
            break;
        case 0x04u: /* BEQ */
            c->in_delay_slot = 1u;
            c->branch_taken = (v_rs == v_rt) ? 1u : 0u;
            c->branch_target = next_pc + (simm << 2);
            c->next_pc = next_pc + 4u;
            break;
        case 0x05u: /* BNE */
            c->in_delay_slot = 1u;
            c->branch_taken = (v_rs != v_rt) ? 1u : 0u;
            c->branch_target = next_pc + (simm << 2);
            c->next_pc = next_pc + 4u;
            break;
        case 0x06u: /* BLEZ */
            c->in_delay_slot = 1u;
            c->branch_taken = ((s32)v_rs <= 0) ? 1u : 0u;
            c->branch_target = next_pc + (simm << 2);
            c->next_pc = next_pc + 4u;
            break;
        case 0x07u: /* BGTZ */
            c->in_delay_slot = 1u;
            c->branch_taken = ((s32)v_rs > 0) ? 1u : 0u;
            c->branch_target = next_pc + (simm << 2);
            c->next_pc = next_pc + 4u;
            break;
        case 0x08u: { /* ADDI */
            u32 r = v_rs + simm;
            if (((~(v_rs ^ simm) & (v_rs ^ r)) & 0x80000000u) != 0u) {
                STUB_RAISE(PSX_EXC_OV, 0u, 0u);
            } else {
                written_reg = rt;
                written_val = r;
            }
            break;
        }
        case 0x09u: written_reg = rt; written_val = v_rs + simm; break;
        case 0x0Au: written_reg = rt; written_val = ((s32)v_rs < (s32)simm) ? 1u : 0u; break;
        case 0x0Bu: written_reg = rt; written_val = (v_rs < simm) ? 1u : 0u; break;
        case 0x0Cu: written_reg = rt; written_val = v_rs & imm16; break;
        case 0x0Du: written_reg = rt; written_val = v_rs | imm16; break;
        case 0x0Eu: written_reg = rt; written_val = v_rs ^ imm16; break;
        case 0x0Fu: written_reg = rt; written_val = imm16 << 16; break;

        case 0x10u: { /* COP0 */
            u32 sr = c->cop0[COP0_SR];
            if (((sr & PSX_SR_CU0) == 0u) && ((sr & PSX_SR_KUC) != 0u)) {
                STUB_RAISE(PSX_EXC_CPU, 0u, 0u);
                break;
            }
            if ((rs & 0x10u) != 0u) {
                if (fn == 0x10u) { /* RFE */
                    c->in_jit = 0u;
                    jit_set_sr_wrap(sys, (sr & ~0x0Fu) | ((sr >> 2) & 0x0Fu));
                }
            } else if (rs == 0x00u) { /* MFC0 (load-delayed) */
                new_ld_reg = rt;
                new_ld_val = cop0_read_reg(c, rd);
            } else if (rs == 0x04u) { /* MTC0 */
                c->in_jit = 0u;
                if (rd == COP0_SR) {
                    jit_set_sr_wrap(sys, v_rt);
                } else if (rd == COP0_CAUSE) {
                    jit_set_cause_wrap(sys, v_rt);
                } else {
                    cop0_write_non_sr_cause(c, rd, v_rt);
                }
            } else {
                STUB_RAISE(PSX_EXC_RI, 0u, 0u);
            }
            break;
        }
        case 0x11u: /* COP1 */
            if ((c->cop0[COP0_SR] & PSX_SR_CU1) == 0u) {
                STUB_RAISE(PSX_EXC_CPU, 0u, 1u);
            }
            break;
        case 0x12u: { /* COP2 (GTE) */
            if ((c->cop0[COP0_SR] & PSX_SR_CU2) == 0u) {
                STUB_RAISE(PSX_EXC_CPU, 0u, 2u);
                break;
            }
            if ((rs & 0x10u) != 0u) {
                jit_gte_exec_wrap(c, insn);
            } else if (rs == 0x00u) {
                new_ld_reg = rt;
                new_ld_val = jit_gte_read_data_wrap(c, rd);
            } else if (rs == 0x02u) {
                new_ld_reg = rt;
                new_ld_val = jit_gte_read_ctrl_wrap(c, rd);
            } else if (rs == 0x04u) {
                jit_gte_write_data_wrap(c, rd, v_rt);
            } else if (rs == 0x06u) {
                jit_gte_write_ctrl_wrap(c, rd, v_rt);
            } else if (rs == 0x08u) { /* BC2F / BC2T */
                c->in_delay_slot = 1u;
                c->branch_taken = ((rt & 1u) == 0u) ? 1u : 0u;
                c->branch_target = next_pc + (simm << 2);
                c->next_pc = next_pc + 4u;
            } else {
                STUB_RAISE(PSX_EXC_RI, 0u, 0u);
            }
            break;
        }
        case 0x13u: /* COP3 */
            if ((c->cop0[COP0_SR] & PSX_SR_CU3) == 0u) {
                STUB_RAISE(PSX_EXC_CPU, 0u, 3u);
            }
            break;

        case 0x20u: { /* LB */
            u8 v = cached_read8(c, v_rs + simm);
            if (c->jit_abort == 0u) { new_ld_reg = rt; new_ld_val = (u32)(s32)(s8)v; }
            break;
        }
        case 0x21u: { /* LH */
            u16 v = cached_read16(c, v_rs + simm);
            if (c->jit_abort == 0u) { new_ld_reg = rt; new_ld_val = (u32)(s32)(s16)v; }
            break;
        }
        case 0x22u: { /* LWL */
            u32 base_rt = (prev_ld_reg == rt && rt != 0u) ? prev_ld_val : v_rt;
            u32 v = jit_lwl_helper(c, v_rs + simm, base_rt);
            if (c->jit_abort == 0u) { new_ld_reg = rt; new_ld_val = v; }
            break;
        }
        case 0x23u: { /* LW */
            u32 v = cached_read32(c, v_rs + simm);
            if (c->jit_abort == 0u) { new_ld_reg = rt; new_ld_val = v; }
            break;
        }
        case 0x24u: { /* LBU */
            u8 v = cached_read8(c, v_rs + simm);
            if (c->jit_abort == 0u) { new_ld_reg = rt; new_ld_val = (u32)v; }
            break;
        }
        case 0x25u: { /* LHU */
            u16 v = cached_read16(c, v_rs + simm);
            if (c->jit_abort == 0u) { new_ld_reg = rt; new_ld_val = (u32)v; }
            break;
        }
        case 0x26u: { /* LWR */
            u32 base_rt = (prev_ld_reg == rt && rt != 0u) ? prev_ld_val : v_rt;
            u32 v = jit_lwr_helper(c, v_rs + simm, base_rt);
            if (c->jit_abort == 0u) { new_ld_reg = rt; new_ld_val = v; }
            break;
        }
        case 0x28u: cached_write8(c, v_rs + simm, (u8)v_rt); break;
        case 0x29u: cached_write16(c, v_rs + simm, (u16)v_rt); break;
        case 0x2Au: jit_swl_helper(c, v_rs + simm, v_rt); break;
        case 0x2Bu: cached_write32(c, v_rs + simm, v_rt); break;
        case 0x2Eu: jit_swr_helper(c, v_rs + simm, v_rt); break;
        case 0x30u: case 0x38u: { /* LWC0 / SWC0 */
            if ((c->cop0[COP0_SR] & PSX_SR_CU0) == 0u) {
                STUB_RAISE(PSX_EXC_CPU, 0u, 0u);
            } else {
                u32 addr = v_rs + simm;
                if (psx_addr_fault(c, addr, 4u)) {
                    STUB_RAISE((op == 0x30u) ? PSX_EXC_ADEL : PSX_EXC_ADES, addr, 0u);
                }
            }
            break;
        }
        case 0x31u: case 0x39u: { /* LWC1 / SWC1 */
            if ((c->cop0[COP0_SR] & PSX_SR_CU1) == 0u) {
                STUB_RAISE(PSX_EXC_CPU, 0u, 1u);
            } else {
                u32 addr = v_rs + simm;
                if (psx_addr_fault(c, addr, 4u)) {
                    STUB_RAISE((op == 0x31u) ? PSX_EXC_ADEL : PSX_EXC_ADES, addr, 0u);
                }
            }
            break;
        }
        case 0x32u: { /* LWC2 */
            if ((c->cop0[COP0_SR] & PSX_SR_CU2) == 0u) {
                STUB_RAISE(PSX_EXC_CPU, 0u, 2u);
                break;
            }
            u32 v = cached_read32(c, v_rs + simm);
            if (c->jit_abort == 0u) {
                jit_gte_write_data_wrap(c, rt, v);
            }
            break;
        }
        case 0x3Au: { /* SWC2 */
            if ((c->cop0[COP0_SR] & PSX_SR_CU2) == 0u) {
                STUB_RAISE(PSX_EXC_CPU, 0u, 2u);
                break;
            }
            u32 v = jit_gte_read_data_wrap(c, rt);
            cached_write32(c, v_rs + simm, v);
            break;
        }
        case 0x33u: case 0x3Bu: { /* LWC3 / SWC3 */
            if ((c->cop0[COP0_SR] & PSX_SR_CU3) == 0u) {
                STUB_RAISE(PSX_EXC_CPU, 0u, 3u);
            } else {
                u32 addr = v_rs + simm;
                if (psx_addr_fault(c, addr, 4u)) {
                    STUB_RAISE((op == 0x33u) ? PSX_EXC_ADEL : PSX_EXC_ADES, addr, 0u);
                }
            }
            break;
        }
        default:
            STUB_RAISE(PSX_EXC_RI, 0u, 0u);
            break;
    }

    c->in_jit = 0u;

    if (!raised && c->jit_abort != 0u) {
        u32 abort_code = c->jit_abort - 1u;
        c->jit_abort = 0u;
        STUB_RAISE(abort_code, c->abort_badvaddr, 0u);
    }
#undef STUB_RAISE

    if (raised) {
        return;
    }

    if (prev_ld_reg != 0u && prev_ld_reg != written_reg && prev_ld_reg != new_ld_reg) {
        c->gpr[prev_ld_reg] = prev_ld_val;
    }
    if (written_reg != 0u) {
        c->gpr[written_reg] = written_val;
    }
    c->gpr[0] = 0u;
    c->load_delay_reg = (new_ld_reg != 0u) ? new_ld_reg : 0u;
    c->load_delay_val = (new_ld_reg != 0u) ? new_ld_val : 0u;
    c->pc = next_pc;
    if (c->in_delay_slot == 0u) {
        c->next_pc = next_pc + 4u;
    }
}

void jit_step_one_wrap(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    if (sys->cpu.in_delay_slot == 0u) {
        sys->cpu.next_pc = sys->cpu.pc + 4u;
    }
    sys->stats.step_one_fallbacks++;
    if (!jit_core_is_stub()) {
        cpu_step_one(sys);
        return;
    }
    stub_step_one_impl(sys);
}

void jit_step_until_clean_wrap(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    if (sys->cpu.in_delay_slot == 0u) {
        sys->cpu.next_pc = sys->cpu.pc + 4u;
    }
    if (!jit_core_is_stub()) {
        sys->stats.step_one_fallbacks++;
        cpu_step_until_clean(sys);
        return;
    }
    do {
        jit_step_one_wrap(sys);
    } while ((sys->cpu.in_delay_slot != 0u || sys->cpu.load_delay_reg != 0u) &&
             sys->cpu.cycles_left > 0 && !sys->quit);
}

/* ============================================================================
 * Part 2: Cached Block Interpreter (ARCHITECTURE.md §6)
 * Pre-decodes blocks into CachedOp streams inside jit.mem.rw and dispatches
 * via computed-goto threaded code, sharing JitState, tab_ram/tab_bios, and SMC.
 * ============================================================================ */

typedef enum CachedOpcode {
    COP_NOP = 0,
    COP_SLL, COP_SRL, COP_SRA,
    COP_SLLV, COP_SRLV, COP_SRAV,
    COP_ADD, COP_ADDU, COP_SUB, COP_SUBU,
    COP_AND, COP_OR, COP_XOR, COP_NOR,
    COP_SLT, COP_SLTU,
    COP_ADDI, COP_ADDIU, COP_SLTI, COP_SLTIU,
    COP_ANDI, COP_ORI, COP_XORI, COP_LUI,
    COP_MULT, COP_MULTU, COP_DIV, COP_DIVU,
    COP_MFHI, COP_MTHI, COP_MFLO, COP_MTLO,
    COP_LB, COP_LH, COP_LW, COP_LBU, COP_LHU, COP_LWL, COP_LWR,
    COP_SB, COP_SH, COP_SW, COP_SWL, COP_SWR,
    COP_J, COP_JAL, COP_JR, COP_JALR,
    COP_BEQ, COP_BNE, COP_BLEZ, COP_BGTZ,
    COP_BLTZ, COP_BGEZ, COP_BLTZAL, COP_BGEZAL,
    COP_SYSCALL, COP_BREAK, COP_RI,
    COP_MFC0, COP_MTC0, COP_RFE, COP_COP0_NOP,
    COP_COP1_NOP, COP_COP3_NOP,
    COP_MFC2, COP_CFC2, COP_MTC2, COP_CTC2, COP_GTE_EXEC,
    COP_LWC2, COP_SWC2,
    COP_LWC0_NOP, COP_SWC0_NOP,
    COP_LWC1_NOP, COP_SWC1_NOP,
    COP_LWC3_NOP, COP_SWC3_NOP,
    COP_STEP_FALLBACK,
    COP_END,
    COP_COUNT
} CachedOpcode;

enum CachedOpFlags {
    CF_IN_DS          = (1u << 0),
    CF_COUNTED_DEC    = (1u << 1)
};

typedef struct CachedOp {
    u8  op;
    u8  rs;
    u8  rt;
    u8  rd;
    u32 imm;
    u32 pc;
    u16 cyc_before;
    u16 flags;
} CachedOp;

_Static_assert(sizeof(CachedOp) == 16, "CachedOp must be 16 bytes");

static bool mips_is_branch(u32 insn) {
    u32 op = insn >> 26;
    if (op >= 1u && op <= 7u) {
        return true;
    }
    if (op == 0u) {
        u32 fn = insn & 63u;
        return fn == 0x08u || fn == 0x09u;
    }
    if (op == 0x12u && ((insn >> 21) & 31u) == 0x08u) {
        return true; /* BC2F / BC2T */
    }
    return false;
}

static bool mips_is_mem_op(u32 insn) {
    u32 op = insn >> 26;
    return op >= 0x20u && op <= 0x3Bu;
}

static bool mips_terminates_after_insn(u32 insn) {
    u32 op = insn >> 26;
    u32 rs = (insn >> 21) & 31u;
    u32 rd = (insn >> 11) & 31u;
    u32 fn = insn & 63u;
    if (op == 0u && (fn == 0x0Cu || fn == 0x0Du)) {
        return true; /* SYSCALL / BREAK */
    }
    if (op == 0x10u) {
        if ((rs & 0x10u) != 0u && fn == 0x10u) {
            return true; /* RFE */
        }
        if (rs == 0x04u && (rd == COP0_SR || rd == COP0_CAUSE)) {
            return true; /* MTC0 SR / CAUSE */
        }
    }
    return false;
}

static void decode_cached_op(u32 insn, u32 pc, u16 cyc_before, u16 flags, CachedOp *out) {
    out->pc = pc;
    out->cyc_before = cyc_before;
    out->flags = flags;

    if (insn == 0u) {
        out->op = COP_NOP;
        out->rs = out->rt = out->rd = 0u;
        out->imm = 0u;
        return;
    }

    u32 op = insn >> 26;
    u32 rs = (insn >> 21) & 31u;
    u32 rt = (insn >> 16) & 31u;
    u32 rd = (insn >> 11) & 31u;
    u32 sh = (insn >> 6) & 31u;
    u32 fn = insn & 63u;
    u32 imm16 = insn & 0xFFFFu;
    u32 simm = (u32)(s32)(s16)imm16;

    out->rs = (u8)rs;
    out->rt = (u8)rt;
    out->rd = (u8)rd;
    out->imm = simm;

    switch (op) {
        case 0x00u:
            switch (fn) {
                case 0x00u: out->op = (rd == 0u) ? COP_NOP : COP_SLL; out->imm = sh; return;
                case 0x02u: out->op = (rd == 0u) ? COP_NOP : COP_SRL; out->imm = sh; return;
                case 0x03u: out->op = (rd == 0u) ? COP_NOP : COP_SRA; out->imm = sh; return;
                case 0x04u: out->op = (rd == 0u) ? COP_NOP : COP_SLLV; return;
                case 0x06u: out->op = (rd == 0u) ? COP_NOP : COP_SRLV; return;
                case 0x07u: out->op = (rd == 0u) ? COP_NOP : COP_SRAV; return;
                case 0x08u: out->op = COP_JR; return;
                case 0x09u: out->op = COP_JALR; return;
                case 0x0Cu: out->op = COP_SYSCALL; return;
                case 0x0Du: out->op = COP_BREAK; return;
                case 0x10u: out->op = (rd == 0u) ? COP_NOP : COP_MFHI; return;
                case 0x11u: out->op = COP_MTHI; return;
                case 0x12u: out->op = (rd == 0u) ? COP_NOP : COP_MFLO; return;
                case 0x13u: out->op = COP_MTLO; return;
                case 0x18u: out->op = COP_MULT; return;
                case 0x19u: out->op = COP_MULTU; return;
                case 0x1Au: out->op = COP_DIV; return;
                case 0x1Bu: out->op = COP_DIVU; return;
                case 0x20u: out->op = COP_ADD; return;
                case 0x21u: out->op = (rd == 0u) ? COP_NOP : COP_ADDU; return;
                case 0x22u: out->op = COP_SUB; return;
                case 0x23u: out->op = (rd == 0u) ? COP_NOP : COP_SUBU; return;
                case 0x24u: out->op = (rd == 0u) ? COP_NOP : COP_AND; return;
                case 0x25u: out->op = (rd == 0u) ? COP_NOP : COP_OR; return;
                case 0x26u: out->op = (rd == 0u) ? COP_NOP : COP_XOR; return;
                case 0x27u: out->op = (rd == 0u) ? COP_NOP : COP_NOR; return;
                case 0x2Au: out->op = (rd == 0u) ? COP_NOP : COP_SLT; return;
                case 0x2Bu: out->op = (rd == 0u) ? COP_NOP : COP_SLTU; return;
                default: out->op = COP_RI; return;
            }
        case 0x01u: {
            bool ge = (rt & 1u) != 0u;
            bool link = (rt & 0x1Eu) == 0x10u;
            out->imm = (pc + 4u) + (simm << 2);
            out->op = link ? (ge ? COP_BGEZAL : COP_BLTZAL)
                           : (ge ? COP_BGEZ   : COP_BLTZ);
            return;
        }
        case 0x02u:
            out->op = COP_J;
            out->imm = ((pc + 4u) & 0xF0000000u) | ((insn & 0x03FFFFFFu) << 2);
            return;
        case 0x03u:
            out->op = COP_JAL;
            out->imm = ((pc + 4u) & 0xF0000000u) | ((insn & 0x03FFFFFFu) << 2);
            return;
        case 0x04u: out->op = COP_BEQ;  out->imm = (pc + 4u) + (simm << 2); return;
        case 0x05u: out->op = COP_BNE;  out->imm = (pc + 4u) + (simm << 2); return;
        case 0x06u: out->op = COP_BLEZ; out->imm = (pc + 4u) + (simm << 2); return;
        case 0x07u: out->op = COP_BGTZ; out->imm = (pc + 4u) + (simm << 2); return;
        case 0x08u: out->op = COP_ADDI; return;
        case 0x09u: out->op = (rt == 0u) ? COP_NOP : COP_ADDIU; return;
        case 0x0Au: out->op = (rt == 0u) ? COP_NOP : COP_SLTI; return;
        case 0x0Bu: out->op = (rt == 0u) ? COP_NOP : COP_SLTIU; return;
        case 0x0Cu: out->op = (rt == 0u) ? COP_NOP : COP_ANDI; out->imm = imm16; return;
        case 0x0Du: out->op = (rt == 0u) ? COP_NOP : COP_ORI;  out->imm = imm16; return;
        case 0x0Eu: out->op = (rt == 0u) ? COP_NOP : COP_XORI; out->imm = imm16; return;
        case 0x0Fu: out->op = (rt == 0u) ? COP_NOP : COP_LUI;  out->imm = imm16 << 16; return;
        case 0x10u:
            if ((rs & 0x10u) != 0u) {
                out->op = (fn == 0x10u) ? COP_RFE : COP_COP0_NOP;
            } else if (rs == 0x00u) {
                out->op = COP_MFC0;
            } else if (rs == 0x04u) {
                out->op = COP_MTC0;
            } else {
                out->op = COP_RI;
            }
            return;
        case 0x11u: out->op = COP_COP1_NOP; return;
        case 0x12u:
            out->imm = insn;
            if ((rs & 0x10u) != 0u) {
                out->op = COP_GTE_EXEC;
            } else if (rs == 0x00u) {
                out->op = COP_MFC2;
            } else if (rs == 0x02u) {
                out->op = COP_CFC2;
            } else if (rs == 0x04u) {
                out->op = COP_MTC2;
            } else if (rs == 0x06u) {
                out->op = COP_CTC2;
            } else {
                out->op = COP_RI;
            }
            return;
        case 0x13u: out->op = COP_COP3_NOP; return;
        case 0x20u: out->op = COP_LB; return;
        case 0x21u: out->op = COP_LH; return;
        case 0x22u: out->op = COP_LWL; return;
        case 0x23u: out->op = COP_LW; return;
        case 0x24u: out->op = COP_LBU; return;
        case 0x25u: out->op = COP_LHU; return;
        case 0x26u: out->op = COP_LWR; return;
        case 0x28u: out->op = COP_SB; return;
        case 0x29u: out->op = COP_SH; return;
        case 0x2Au: out->op = COP_SWL; return;
        case 0x2Bu: out->op = COP_SW; return;
        case 0x2Eu: out->op = COP_SWR; return;
        case 0x30u: out->op = COP_LWC0_NOP; return;
        case 0x31u: out->op = COP_LWC1_NOP; return;
        case 0x32u: out->op = COP_LWC2; return;
        case 0x33u: out->op = COP_LWC3_NOP; return;
        case 0x38u: out->op = COP_SWC0_NOP; return;
        case 0x39u: out->op = COP_SWC1_NOP; return;
        case 0x3Au: out->op = COP_SWC2; return;
        case 0x3Bu: out->op = COP_SWC3_NOP; return;
        default: out->op = COP_RI; return;
    }
}

/* Detect idle spin-loops & counted loops per ARCHITECTURE.md §5.10 */
static void analyze_cached_loop(const PsxSystem *sys, JitBlock *b, CachedOp *ops,
                                const u32 *raw, u32 n) {
    if (!sys->cfg.jit_idle_skip || n < 2u || n > 8u) {
        return;
    }
    CachedOp *br = &ops[n - 2u];
    CachedOp *ds = &ops[n - 1u];

    bool is_self_target = false;
    if (br->op == COP_J || br->op == COP_BEQ || br->op == COP_BNE ||
        br->op == COP_BLEZ || br->op == COP_BGTZ ||
        br->op == COP_BLTZ || br->op == COP_BGEZ) {
        is_self_target = (br->imm == b->pc);
    }
    if (!is_self_target) {
        return;
    }

    /* Exact counted loop: loop: addiu rX, rX, -1; bne rX, $zero, loop; nop */
    if (n == 3u && ops[0].op == COP_ADDIU && ops[0].rs == ops[0].rt &&
        ops[0].rt != 0u && ops[0].imm == 0xFFFFFFFFu &&
        ((br->op == COP_BNE && ((br->rs == ops[0].rt && br->rt == 0u) ||
                                (br->rt == ops[0].rt && br->rs == 0u)))) &&
        ds->op == COP_NOP) {
        ops[0].flags |= CF_COUNTED_DEC;
        return;
    }

    /* Idle wait loop: no stores/GTE/COP0-writes/SYSCALL/DIV/MULT/links, <=1 load from static reg */
    u32 written_mask = 0u;
    u32 load_count = 0u;
    u32 load_base_reg = 0u;

    for (u32 i = 0u; i < n; ++i) {
        u32 insn = raw[i];
        u32 op = insn >> 26;
        u32 fn = insn & 63u;
        CachedOpcode cop = (CachedOpcode)ops[i].op;
        switch (cop) {
            case COP_NOP:
            case COP_J: case COP_BEQ: case COP_BNE:
            case COP_BLEZ: case COP_BGTZ: case COP_BLTZ: case COP_BGEZ:
                break;
            case COP_SLL: case COP_SRL: case COP_SRA:
            case COP_SLLV: case COP_SRLV: case COP_SRAV:
            case COP_ADDU: case COP_SUBU: case COP_AND:
            case COP_OR: case COP_XOR: case COP_NOR:
            case COP_SLT: case COP_SLTU:
                if (ops[i].rd != 0u) written_mask |= (1u << ops[i].rd);
                break;
            case COP_ADDIU: case COP_SLTI: case COP_SLTIU:
            case COP_ANDI: case COP_ORI: case COP_XORI: case COP_LUI:
                if (ops[i].rt != 0u) written_mask |= (1u << ops[i].rt);
                break;
            case COP_LB: case COP_LH: case COP_LW: case COP_LBU: case COP_LHU:
                load_count++;
                load_base_reg = ops[i].rs;
                if (ops[i].rt != 0u) written_mask |= (1u << ops[i].rt);
                break;
            default:
                (void)op; (void)fn;
                return;
        }
    }

    if (load_count > 1u) {
        return;
    }
    if (load_count == 1u && ((written_mask & (1u << load_base_reg)) != 0u)) {
        /* Base address mutates across iterations (pointer walk, not polling) */
        return;
    }
    /* A pure register-mutate loop without a load (e.g. addiu rX, rX, 1; bne) is only idle if written_mask == 0 */
    if (load_count == 0u && written_mask != 0u) {
        return;
    }

    b->flags |= JB_IDLE_LOOP;
}

static JitBlock *cached_compile_block(PsxSystem *sys, u32 start_pc) {
    JitState *j = &sys->jit;
    bool is_ram = jit_pc_is_ram(sys, start_pc);
    bool is_bios = !is_ram && jit_pc_is_bios(start_pc);
    if ((!is_ram && !is_bios) || !j->mem.rw || !j->blocks) {
        return NULL;
    }

    u32 max_ops_bytes = (JIT_MAX_BLOCK_INSNS + 2u) * (u32)sizeof(CachedOp);
    uintptr_t aligned_ptr = ((uintptr_t)j->code_ptr + 7u) & ~(uintptr_t)7u;
    if (j->n_blocks >= j->max_blocks ||
        aligned_ptr + max_ops_bytes > (uintptr_t)j->code_end) {
        jit_flush_all(sys);
        aligned_ptr = ((uintptr_t)j->code_ptr + 7u) & ~(uintptr_t)7u;
    }

    const u8 *backing = is_ram ? sys->bus.ram : sys->bus.bios;
    if (!backing) {
        return NULL;
    }

    u32 raw[JIT_MAX_BLOCK_INSNS];
    u32 n_insns = 0u;
    u32 n_mem = 0u;
    bool emit_step_fallback = false;

    for (u32 i = 0u; i < JIT_MAX_BLOCK_INSNS; ++i) {
        u32 cur_pc = start_pc + i * 4u;
        if (i > 0u && jit_is_pinned_pc(sys, cur_pc)) {
            break;
        }
        if (is_ram && ((cur_pc & 0x1FFFFFFFu) >= 0x00800000u)) {
            break;
        }
        if (!is_ram && !jit_pc_is_bios(cur_pc)) {
            break;
        }

        u32 insn = 0u;
        if (is_ram) {
            memcpy(&insn, backing + (cur_pc & PSX_RAM_MASK), 4u);
        } else {
            memcpy(&insn, backing + (cur_pc & PSX_BIOS_MASK), 4u);
        }

        if (mips_is_branch(insn)) {
            if ((insn >> 26) == 0x12u && ((insn >> 21) & 31u) == 0x08u) {
                /* BC2F / BC2T: delegate rare COP2 branch to step-until-clean */
                if (i == 0u) {
                    emit_step_fallback = true;
                }
                break;
            }
            /* Peek delay slot */
            u32 ds_pc = cur_pc + 4u;
            bool ds_ok = is_ram
                ? (((ds_pc & 0x1FFFFFFFu) < (sys->cfg.strict_ram_size ? 0x00200000u : 0x00800000u)) &&
                   !jit_is_pinned_pc(sys, ds_pc))
                : jit_pc_is_bios(ds_pc);
            if (!ds_ok || i + 1u >= JIT_MAX_BLOCK_INSNS) {
                if (i == 0u) {
                    emit_step_fallback = true;
                }
                break;
            }
            u32 ds_insn = 0u;
            if (is_ram) {
                memcpy(&ds_insn, backing + (ds_pc & PSX_RAM_MASK), 4u);
            } else {
                memcpy(&ds_insn, backing + (ds_pc & PSX_BIOS_MASK), 4u);
            }
            if (mips_is_branch(ds_insn)) {
                /* Branch in delay slot: end before branch (or step-fallback if at block start) */
                if (i == 0u) {
                    emit_step_fallback = true;
                }
                break;
            }
            raw[n_insns++] = insn;
            if (mips_is_mem_op(insn)) n_mem++;
            raw[n_insns++] = ds_insn;
            if (mips_is_mem_op(ds_insn)) n_mem++;
            break;
        }

        raw[n_insns++] = insn;
        if (mips_is_mem_op(insn)) {
            n_mem++;
        }
        if (mips_terminates_after_insn(insn)) {
            break;
        }
    }

    u16 b_idx = (u16)j->n_blocks++;
    JitBlock *b = &j->blocks[b_idx];
    CachedOp *ops = (CachedOp *)aligned_ptr;

    b->entry.p = ops;
    b->pc = start_pc;
    b->phys_page[0] = JIT_PAGE_NONE;
    b->phys_page[1] = JIT_PAGE_NONE;
    b->next_in_page[0] = JIT_PAGE_NONE;
    b->next_in_page[1] = JIT_PAGE_NONE;
    b->flags = JB_CACHED_OP;
    b->_pad = 0u;

    if (emit_step_fallback || n_insns == 0u) {
        ops[0].op = COP_STEP_FALLBACK;
        ops[0].rs = ops[0].rt = ops[0].rd = 0u;
        ops[0].imm = 0u;
        ops[0].pc = start_pc;
        ops[0].cyc_before = 0u;
        ops[0].flags = 0u;
        ops[1].op = COP_END;
        ops[1].rs = ops[1].rt = ops[1].rd = 0u;
        ops[1].imm = start_pc;
        ops[1].pc = start_pc;
        ops[1].cyc_before = 0u;
        ops[1].flags = 0u;

        b->n_insns = 1u;
        b->cycles = 0u;
        b->code_bytes = 2u * (u32)sizeof(CachedOp);
    } else {
        u32 mem_so_far = 0u;
        bool has_branch = (n_insns >= 2u) && mips_is_branch(raw[n_insns - 2u]);
        for (u32 i = 0u; i < n_insns; ++i) {
            if (mips_is_mem_op(raw[i])) {
                mem_so_far++;
            }
            u16 cyc_after = (u16)jit_cycles_after(&sys->cfg, i + 1u, mem_so_far);
            u16 fl = (has_branch && i == n_insns - 1u) ? CF_IN_DS : 0u;
            decode_cached_op(raw[i], start_pc + i * 4u, cyc_after, fl, &ops[i]);
            if (ops[i].op == COP_GTE_EXEC || ops[i].op == COP_MFC2 ||
                ops[i].op == COP_CFC2 || ops[i].op == COP_MTC2 ||
                ops[i].op == COP_CTC2 || ops[i].op == COP_LWC2 ||
                ops[i].op == COP_SWC2) {
                b->flags |= JB_HAS_GTE;
            }
        }
        ops[n_insns].op = COP_END;
        ops[n_insns].rs = ops[n_insns].rt = ops[n_insns].rd = 0u;
        ops[n_insns].imm = start_pc + n_insns * 4u; /* fallthrough PC */
        ops[n_insns].pc = start_pc + n_insns * 4u;
        ops[n_insns].cyc_before = jit_scale_cycles(&sys->cfg, n_insns, n_mem);
        ops[n_insns].flags = 0u;

        b->n_insns = (u16)n_insns;
        b->cycles = jit_scale_cycles(&sys->cfg, n_insns, n_mem);
        b->code_bytes = (n_insns + 1u) * (u32)sizeof(CachedOp);

        analyze_cached_loop(sys, b, ops, raw, n_insns);
    }

    j->code_ptr = (u8 *)ops + b->code_bytes;

    if (is_ram) {
        u32 byte_span = (b->n_insns > 0u ? (u32)b->n_insns : 1u) * 4u;
        u16 p0 = (u16)((start_pc & PSX_RAM_MASK) >> 12);
        u16 p1 = (u16)(((start_pc + byte_span - 1u) & PSX_RAM_MASK) >> 12);
        (void)jit_block_add_ram_page(sys, b_idx, p0);
        if (p1 != p0) {
            (void)jit_block_add_ram_page(sys, b_idx, p1);
        }
        j->tab_ram[BLOCKTAB_RAM_IDX(start_pc)] = b;
    } else {
        j->tab_bios[BLOCKTAB_BIOS_IDX(start_pc)] = b;
    }

    sys->stats.jit_blocks_live++;
    sys->stats.jit_blocks_compiled_frame++;
    sys->stats.jit_code_bytes = (u32)(j->code_ptr - j->mem.rw);
    return b;
}

void cpu_cached_run(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    PsxCpu *c = &sys->cpu;
    JitState *j = &sys->jit;

    if (!j->tab_ram || !j->tab_bios || !j->mem.rw) {
        while (c->cycles_left > 0 && !sys->quit) {
            jit_step_one_wrap(sys);
        }
        c->exit_reason = PSX_EXIT_EVENTS;
        return;
    }

    /* Flush if live blocks belong to ARM JIT engine */
    if (j->n_blocks > 0u && (j->blocks[0].flags & JB_CACHED_OP) == 0u) {
        jit_flush_all(sys);
    }
    jit_refresh_pins(sys);

    if (c->in_delay_slot != 0u || c->load_delay_reg != 0u) {
        jit_step_until_clean_wrap(sys);
    }
    if (c->cycles_left <= 0 || sys->quit) {
        c->exit_reason = PSX_EXIT_EVENTS;
        return;
    }

#if defined(__GNUC__)
    static const void *const k_dispatch[COP_COUNT] = {
        [COP_NOP]           = &&L_COP_NOP,
        [COP_SLL]           = &&L_COP_SLL,
        [COP_SRL]           = &&L_COP_SRL,
        [COP_SRA]           = &&L_COP_SRA,
        [COP_SLLV]          = &&L_COP_SLLV,
        [COP_SRLV]          = &&L_COP_SRLV,
        [COP_SRAV]          = &&L_COP_SRAV,
        [COP_ADD]           = &&L_COP_ADD,
        [COP_ADDU]          = &&L_COP_ADDU,
        [COP_SUB]           = &&L_COP_SUB,
        [COP_SUBU]          = &&L_COP_SUBU,
        [COP_AND]           = &&L_COP_AND,
        [COP_OR]            = &&L_COP_OR,
        [COP_XOR]           = &&L_COP_XOR,
        [COP_NOR]           = &&L_COP_NOR,
        [COP_SLT]           = &&L_COP_SLT,
        [COP_SLTU]          = &&L_COP_SLTU,
        [COP_ADDI]          = &&L_COP_ADDI,
        [COP_ADDIU]         = &&L_COP_ADDIU,
        [COP_SLTI]          = &&L_COP_SLTI,
        [COP_SLTIU]         = &&L_COP_SLTIU,
        [COP_ANDI]          = &&L_COP_ANDI,
        [COP_ORI]           = &&L_COP_ORI,
        [COP_XORI]          = &&L_COP_XORI,
        [COP_LUI]           = &&L_COP_LUI,
        [COP_MULT]          = &&L_COP_MULT,
        [COP_MULTU]         = &&L_COP_MULTU,
        [COP_DIV]           = &&L_COP_DIV,
        [COP_DIVU]          = &&L_COP_DIVU,
        [COP_MFHI]          = &&L_COP_MFHI,
        [COP_MTHI]          = &&L_COP_MTHI,
        [COP_MFLO]          = &&L_COP_MFLO,
        [COP_MTLO]          = &&L_COP_MTLO,
        [COP_LB]            = &&L_COP_LB,
        [COP_LH]            = &&L_COP_LH,
        [COP_LW]            = &&L_COP_LW,
        [COP_LBU]           = &&L_COP_LBU,
        [COP_LHU]           = &&L_COP_LHU,
        [COP_LWL]           = &&L_COP_LWL,
        [COP_LWR]           = &&L_COP_LWR,
        [COP_SB]            = &&L_COP_SB,
        [COP_SH]            = &&L_COP_SH,
        [COP_SW]            = &&L_COP_SW,
        [COP_SWL]           = &&L_COP_SWL,
        [COP_SWR]           = &&L_COP_SWR,
        [COP_J]             = &&L_COP_J,
        [COP_JAL]           = &&L_COP_JAL,
        [COP_JR]            = &&L_COP_JR,
        [COP_JALR]          = &&L_COP_JALR,
        [COP_BEQ]           = &&L_COP_BEQ,
        [COP_BNE]           = &&L_COP_BNE,
        [COP_BLEZ]          = &&L_COP_BLEZ,
        [COP_BGTZ]          = &&L_COP_BGTZ,
        [COP_BLTZ]          = &&L_COP_BLTZ,
        [COP_BGEZ]          = &&L_COP_BGEZ,
        [COP_BLTZAL]        = &&L_COP_BLTZAL,
        [COP_BGEZAL]        = &&L_COP_BGEZAL,
        [COP_SYSCALL]       = &&L_COP_SYSCALL,
        [COP_BREAK]         = &&L_COP_BREAK,
        [COP_RI]            = &&L_COP_RI,
        [COP_MFC0]          = &&L_COP_MFC0,
        [COP_MTC0]          = &&L_COP_MTC0,
        [COP_RFE]           = &&L_COP_RFE,
        [COP_COP0_NOP]      = &&L_COP_COP0_NOP,
        [COP_COP1_NOP]      = &&L_COP_COP1_NOP,
        [COP_COP3_NOP]      = &&L_COP_COP3_NOP,
        [COP_MFC2]          = &&L_COP_MFC2,
        [COP_CFC2]          = &&L_COP_CFC2,
        [COP_MTC2]          = &&L_COP_MTC2,
        [COP_CTC2]          = &&L_COP_CTC2,
        [COP_GTE_EXEC]      = &&L_COP_GTE_EXEC,
        [COP_LWC2]          = &&L_COP_LWC2,
        [COP_SWC2]          = &&L_COP_SWC2,
        [COP_LWC0_NOP]      = &&L_COP_LWC0_NOP,
        [COP_SWC0_NOP]      = &&L_COP_SWC0_NOP,
        [COP_LWC1_NOP]      = &&L_COP_LWC1_NOP,
        [COP_SWC1_NOP]      = &&L_COP_SWC1_NOP,
        [COP_LWC3_NOP]      = &&L_COP_LWC3_NOP,
        [COP_SWC3_NOP]      = &&L_COP_SWC3_NOP,
        [COP_STEP_FALLBACK] = &&L_COP_STEP_FALLBACK,
        [COP_END]           = &&L_COP_END
    };
#  define CACHED_DISPATCH() goto *k_dispatch[op->op]
#else
#  define CACHED_DISPATCH() goto L_SWITCH
#endif

    while (c->cycles_left > 0 && !sys->quit) {
        if (c->in_delay_slot != 0u || c->load_delay_reg != 0u) {
            jit_step_until_clean_wrap(sys);
            if (c->cycles_left <= 0 || sys->quit) {
                break;
            }
        }

        u32 pc = c->pc;
        if (PSX_UNLIKELY((pc & 3u) != 0u ||
                         (((c->cop0[COP0_SR] & PSX_SR_KUC) != 0u) && ((pc & 0x80000000u) != 0u)))) {
            jit_step_until_clean_wrap(sys);
            continue;
        }

        void *slot = NULL;
        if (jit_pc_is_ram(sys, pc)) {
            slot = j->tab_ram[BLOCKTAB_RAM_IDX(pc)];
        } else if (jit_pc_is_bios(pc)) {
            slot = j->tab_bios[BLOCKTAB_BIOS_IDX(pc)];
        } else {
            sys->stats.dispatch_slow++;
            jit_step_until_clean_wrap(sys);
            continue;
        }

        if (slot == (void *)jit_bios_vec_stub || slot == (void *)jit_psexe_stub) {
            bool was_psexe = (slot == (void *)jit_psexe_stub);
            c->next_pc = pc + 4u;
            jit_step_until_clean_wrap(sys);
            if (was_psexe) {
                jit_refresh_pins(sys);
            }
            continue;
        }

        JitBlock *b;
        if (slot == (void *)jit_compile_stub) {
            b = cached_compile_block(sys, pc);
            if (!b) {
                jit_step_until_clean_wrap(sys);
                continue;
            }
        } else {
            b = (JitBlock *)slot;
            if (PSX_UNLIKELY(b->pc != pc)) {
                /* Different virtual segment mirror of same RAM word (e.g. 0xA0 vs 0x80): recompile for exact PC */
                if (jit_pc_is_ram(sys, pc)) {
                    jit_invalidate_page(sys, (pc & PSX_RAM_MASK) >> 12);
                    /* Undo cpu_request_exit from mirror recompile */
                    if (c->cycles_left == 0 && c->slice_len > 0) {
                        c->cycles_left = 1;
                    }
                }
                b = cached_compile_block(sys, pc);
                if (!b) {
                    jit_step_until_clean_wrap(sys);
                    continue;
                }
            }
        }

        const CachedOp *op = (const CachedOp *)b->entry.p;
        if (PSX_UNLIKELY(op->op == COP_STEP_FALLBACK)) {
        L_COP_STEP_FALLBACK:
            jit_step_until_clean_wrap(sys);
            continue;
        }

        if (c->cycles_left <= 0) {
            break;
        }

        /* Counted delay-loop folding: loop: addiu rX, rX, -1; bne rX, $0, loop; nop */
        if (PSX_UNLIKELY((op->flags & CF_COUNTED_DEC) != 0u)) {
            u32 rX = c->gpr[op->rt];
            if (rX > 0u && rX <= 0xFFFFu && b->cycles > 0u) {
                u32 total_cyc = rX * (u32)b->cycles;
                if (total_cyc <= (u32)c->cycles_left) {
                    c->cycles_left -= (s32)total_cyc;
                    c->total_cycles += (u64)total_cyc;
                    sys->stats.cycles_total += (u64)total_cyc;
                    sys->stats.insns_total += (u64)rX * 3u;
                    sys->stats.idle_skips++;
                    c->gpr[op->rt] = 0u;
                    c->pc = b->pc + 12u;
                    c->next_pc = c->pc + 4u;
                    continue;
                }
            }
        }

        c->cycles_left -= (s32)b->cycles;

        u32 pend_reg = 0u;
        u32 pend_val = 0u;
        bool has_branch = false;
        bool branch_taken = false;
        u32 branch_target = 0u;
        bool cop2_checked = false;
        u32 exc_code = 0u;
        u32 exc_badv = 0u;
        u32 exc_ce = 0u;

        c->in_jit = 1u;
        c->jit_abort = 0u;

#define COMMIT_PREV_LOAD_UNLESS(wreg)            \
    do {                                         \
        if (pend_reg != 0u) {                    \
            if (pend_reg != (wreg)) {            \
                c->gpr[pend_reg] = pend_val;     \
            }                                    \
            pend_reg = 0u;                       \
        }                                        \
    } while (0)

#define COMMIT_PREV_FOR_NEW_LOAD(nreg, nval)     \
    do {                                         \
        if (pend_reg != 0u && pend_reg != (nreg)) { \
            c->gpr[pend_reg] = pend_val;         \
        }                                        \
        pend_reg = (nreg);                       \
        pend_val = (nval);                       \
    } while (0)

#define WRITE_GPR_IMM(r, val)                    \
    do {                                         \
        u32 _v = (val);                          \
        COMMIT_PREV_LOAD_UNLESS(r);              \
        if ((r) != 0u) c->gpr[(r)] = _v;         \
    } while (0)

#define NEXT_OP()                                \
    do {                                         \
        ++op;                                    \
        CACHED_DISPATCH();                       \
    } while (0)

#define CACHED_FAULT(code_, badv_, ce_)          \
    do {                                         \
        exc_code = (code_);                      \
        exc_badv = (badv_);                      \
        exc_ce   = (ce_);                        \
        goto L_BLOCK_EXC;                        \
    } while (0)

        CACHED_DISPATCH();

#if !defined(__GNUC__)
    L_SWITCH:
        switch (op->op) {
            case COP_NOP:           goto L_COP_NOP;
            case COP_SLL:           goto L_COP_SLL;
            case COP_SRL:           goto L_COP_SRL;
            case COP_SRA:           goto L_COP_SRA;
            case COP_SLLV:          goto L_COP_SLLV;
            case COP_SRLV:          goto L_COP_SRLV;
            case COP_SRAV:          goto L_COP_SRAV;
            case COP_ADD:           goto L_COP_ADD;
            case COP_ADDU:          goto L_COP_ADDU;
            case COP_SUB:           goto L_COP_SUB;
            case COP_SUBU:          goto L_COP_SUBU;
            case COP_AND:           goto L_COP_AND;
            case COP_OR:            goto L_COP_OR;
            case COP_XOR:           goto L_COP_XOR;
            case COP_NOR:           goto L_COP_NOR;
            case COP_SLT:           goto L_COP_SLT;
            case COP_SLTU:          goto L_COP_SLTU;
            case COP_ADDI:          goto L_COP_ADDI;
            case COP_ADDIU:         goto L_COP_ADDIU;
            case COP_SLTI:          goto L_COP_SLTI;
            case COP_SLTIU:         goto L_COP_SLTIU;
            case COP_ANDI:          goto L_COP_ANDI;
            case COP_ORI:           goto L_COP_ORI;
            case COP_XORI:          goto L_COP_XORI;
            case COP_LUI:           goto L_COP_LUI;
            case COP_MULT:          goto L_COP_MULT;
            case COP_MULTU:         goto L_COP_MULTU;
            case COP_DIV:           goto L_COP_DIV;
            case COP_DIVU:          goto L_COP_DIVU;
            case COP_MFHI:          goto L_COP_MFHI;
            case COP_MTHI:          goto L_COP_MTHI;
            case COP_MFLO:          goto L_COP_MFLO;
            case COP_MTLO:          goto L_COP_MTLO;
            case COP_LB:            goto L_COP_LB;
            case COP_LH:            goto L_COP_LH;
            case COP_LW:            goto L_COP_LW;
            case COP_LBU:           goto L_COP_LBU;
            case COP_LHU:           goto L_COP_LHU;
            case COP_LWL:           goto L_COP_LWL;
            case COP_LWR:           goto L_COP_LWR;
            case COP_SB:            goto L_COP_SB;
            case COP_SH:            goto L_COP_SH;
            case COP_SW:            goto L_COP_SW;
            case COP_SWL:           goto L_COP_SWL;
            case COP_SWR:           goto L_COP_SWR;
            case COP_J:             goto L_COP_J;
            case COP_JAL:           goto L_COP_JAL;
            case COP_JR:            goto L_COP_JR;
            case COP_JALR:          goto L_COP_JALR;
            case COP_BEQ:           goto L_COP_BEQ;
            case COP_BNE:           goto L_COP_BNE;
            case COP_BLEZ:          goto L_COP_BLEZ;
            case COP_BGTZ:          goto L_COP_BGTZ;
            case COP_BLTZ:          goto L_COP_BLTZ;
            case COP_BGEZ:          goto L_COP_BGEZ;
            case COP_BLTZAL:        goto L_COP_BLTZAL;
            case COP_BGEZAL:        goto L_COP_BGEZAL;
            case COP_SYSCALL:       goto L_COP_SYSCALL;
            case COP_BREAK:         goto L_COP_BREAK;
            case COP_RI:            goto L_COP_RI;
            case COP_MFC0:          goto L_COP_MFC0;
            case COP_MTC0:          goto L_COP_MTC0;
            case COP_RFE:           goto L_COP_RFE;
            case COP_COP0_NOP:      goto L_COP_COP0_NOP;
            case COP_COP1_NOP:      goto L_COP_COP1_NOP;
            case COP_COP3_NOP:      goto L_COP_COP3_NOP;
            case COP_MFC2:          goto L_COP_MFC2;
            case COP_CFC2:          goto L_COP_CFC2;
            case COP_MTC2:          goto L_COP_MTC2;
            case COP_CTC2:          goto L_COP_CTC2;
            case COP_GTE_EXEC:      goto L_COP_GTE_EXEC;
            case COP_LWC2:          goto L_COP_LWC2;
            case COP_SWC2:          goto L_COP_SWC2;
            case COP_LWC0_NOP:      goto L_COP_LWC0_NOP;
            case COP_SWC0_NOP:      goto L_COP_SWC0_NOP;
            case COP_LWC1_NOP:      goto L_COP_LWC1_NOP;
            case COP_SWC1_NOP:      goto L_COP_SWC1_NOP;
            case COP_LWC3_NOP:      goto L_COP_LWC3_NOP;
            case COP_SWC3_NOP:      goto L_COP_SWC3_NOP;
            case COP_STEP_FALLBACK: goto L_COP_STEP_FALLBACK;
            default:                goto L_COP_END;
        }
#endif

    L_COP_NOP:
        COMMIT_PREV_LOAD_UNLESS(0u);
        NEXT_OP();

    L_COP_SLL:
        WRITE_GPR_IMM(op->rd, c->gpr[op->rt] << op->imm);
        NEXT_OP();
    L_COP_SRL:
        WRITE_GPR_IMM(op->rd, c->gpr[op->rt] >> op->imm);
        NEXT_OP();
    L_COP_SRA:
        WRITE_GPR_IMM(op->rd, (u32)((s32)c->gpr[op->rt] >> op->imm));
        NEXT_OP();
    L_COP_SLLV:
        WRITE_GPR_IMM(op->rd, c->gpr[op->rt] << (c->gpr[op->rs] & 31u));
        NEXT_OP();
    L_COP_SRLV:
        WRITE_GPR_IMM(op->rd, c->gpr[op->rt] >> (c->gpr[op->rs] & 31u));
        NEXT_OP();
    L_COP_SRAV:
        WRITE_GPR_IMM(op->rd, (u32)((s32)c->gpr[op->rt] >> (c->gpr[op->rs] & 31u)));
        NEXT_OP();

    L_COP_ADD: {
        u32 a = c->gpr[op->rs], bv = c->gpr[op->rt], r = a + bv;
        if (PSX_UNLIKELY(((~(a ^ bv) & (a ^ r)) & 0x80000000u) != 0u)) {
            CACHED_FAULT(PSX_EXC_OV, 0u, 0u);
        }
        WRITE_GPR_IMM(op->rd, r);
        NEXT_OP();
    }
    L_COP_ADDU:
        WRITE_GPR_IMM(op->rd, c->gpr[op->rs] + c->gpr[op->rt]);
        NEXT_OP();
    L_COP_SUB: {
        u32 a = c->gpr[op->rs], bv = c->gpr[op->rt], r = a - bv;
        if (PSX_UNLIKELY((((a ^ bv) & (a ^ r)) & 0x80000000u) != 0u)) {
            CACHED_FAULT(PSX_EXC_OV, 0u, 0u);
        }
        WRITE_GPR_IMM(op->rd, r);
        NEXT_OP();
    }
    L_COP_SUBU:
        WRITE_GPR_IMM(op->rd, c->gpr[op->rs] - c->gpr[op->rt]);
        NEXT_OP();
    L_COP_AND:
        WRITE_GPR_IMM(op->rd, c->gpr[op->rs] & c->gpr[op->rt]);
        NEXT_OP();
    L_COP_OR:
        WRITE_GPR_IMM(op->rd, c->gpr[op->rs] | c->gpr[op->rt]);
        NEXT_OP();
    L_COP_XOR:
        WRITE_GPR_IMM(op->rd, c->gpr[op->rs] ^ c->gpr[op->rt]);
        NEXT_OP();
    L_COP_NOR:
        WRITE_GPR_IMM(op->rd, ~(c->gpr[op->rs] | c->gpr[op->rt]));
        NEXT_OP();
    L_COP_SLT:
        WRITE_GPR_IMM(op->rd, ((s32)c->gpr[op->rs] < (s32)c->gpr[op->rt]) ? 1u : 0u);
        NEXT_OP();
    L_COP_SLTU:
        WRITE_GPR_IMM(op->rd, (c->gpr[op->rs] < c->gpr[op->rt]) ? 1u : 0u);
        NEXT_OP();

    L_COP_ADDI: {
        u32 a = c->gpr[op->rs], bv = op->imm, r = a + bv;
        if (PSX_UNLIKELY(((~(a ^ bv) & (a ^ r)) & 0x80000000u) != 0u)) {
            CACHED_FAULT(PSX_EXC_OV, 0u, 0u);
        }
        WRITE_GPR_IMM(op->rt, r);
        NEXT_OP();
    }
    L_COP_ADDIU:
        WRITE_GPR_IMM(op->rt, c->gpr[op->rs] + op->imm);
        NEXT_OP();
    L_COP_SLTI:
        WRITE_GPR_IMM(op->rt, ((s32)c->gpr[op->rs] < (s32)op->imm) ? 1u : 0u);
        NEXT_OP();
    L_COP_SLTIU:
        WRITE_GPR_IMM(op->rt, (c->gpr[op->rs] < op->imm) ? 1u : 0u);
        NEXT_OP();
    L_COP_ANDI:
        WRITE_GPR_IMM(op->rt, c->gpr[op->rs] & op->imm);
        NEXT_OP();
    L_COP_ORI:
        WRITE_GPR_IMM(op->rt, c->gpr[op->rs] | op->imm);
        NEXT_OP();
    L_COP_XORI:
        WRITE_GPR_IMM(op->rt, c->gpr[op->rs] ^ op->imm);
        NEXT_OP();
    L_COP_LUI:
        WRITE_GPR_IMM(op->rt, op->imm);
        NEXT_OP();

    L_COP_MULT: {
        s64 p = (s64)(s32)c->gpr[op->rs] * (s64)(s32)c->gpr[op->rt];
        c->lo = (u32)(u64)p;
        c->hi = (u32)((u64)p >> 32);
        COMMIT_PREV_LOAD_UNLESS(0u);
        NEXT_OP();
    }
    L_COP_MULTU: {
        u64 p = (u64)c->gpr[op->rs] * (u64)c->gpr[op->rt];
        c->lo = (u32)p;
        c->hi = (u32)(p >> 32);
        COMMIT_PREV_LOAD_UNLESS(0u);
        NEXT_OP();
    }
    L_COP_DIV:
        jit_mips_div((s32)c->gpr[op->rs], (s32)c->gpr[op->rt], &c->lo, &c->hi);
        COMMIT_PREV_LOAD_UNLESS(0u);
        NEXT_OP();
    L_COP_DIVU:
        jit_mips_divu(c->gpr[op->rs], c->gpr[op->rt], &c->lo, &c->hi);
        COMMIT_PREV_LOAD_UNLESS(0u);
        NEXT_OP();

    L_COP_MFHI:
        WRITE_GPR_IMM(op->rd, c->hi);
        NEXT_OP();
    L_COP_MTHI:
        c->hi = c->gpr[op->rs];
        COMMIT_PREV_LOAD_UNLESS(0u);
        NEXT_OP();
    L_COP_MFLO:
        WRITE_GPR_IMM(op->rd, c->lo);
        NEXT_OP();
    L_COP_MTLO:
        c->lo = c->gpr[op->rs];
        COMMIT_PREV_LOAD_UNLESS(0u);
        NEXT_OP();

    L_COP_LB: {
        u8 v = cached_read8(c, c->gpr[op->rs] + op->imm);
        if (PSX_UNLIKELY(c->jit_abort != 0u)) goto L_MEM_ABORT;
        COMMIT_PREV_FOR_NEW_LOAD(op->rt, (u32)(s32)(s8)v);
        NEXT_OP();
    }
    L_COP_LH: {
        u16 v = cached_read16(c, c->gpr[op->rs] + op->imm);
        if (PSX_UNLIKELY(c->jit_abort != 0u)) goto L_MEM_ABORT;
        COMMIT_PREV_FOR_NEW_LOAD(op->rt, (u32)(s32)(s16)v);
        NEXT_OP();
    }
    L_COP_LW: {
        u32 v = cached_read32(c, c->gpr[op->rs] + op->imm);
        if (PSX_UNLIKELY(c->jit_abort != 0u)) goto L_MEM_ABORT;
        COMMIT_PREV_FOR_NEW_LOAD(op->rt, v);
        NEXT_OP();
    }
    L_COP_LBU: {
        u8 v = cached_read8(c, c->gpr[op->rs] + op->imm);
        if (PSX_UNLIKELY(c->jit_abort != 0u)) goto L_MEM_ABORT;
        COMMIT_PREV_FOR_NEW_LOAD(op->rt, (u32)v);
        NEXT_OP();
    }
    L_COP_LHU: {
        u16 v = cached_read16(c, c->gpr[op->rs] + op->imm);
        if (PSX_UNLIKELY(c->jit_abort != 0u)) goto L_MEM_ABORT;
        COMMIT_PREV_FOR_NEW_LOAD(op->rt, (u32)v);
        NEXT_OP();
    }
    L_COP_LWL: {
        u32 base_rt = (pend_reg == op->rt && op->rt != 0u) ? pend_val : c->gpr[op->rt];
        u32 v = jit_lwl_helper(c, c->gpr[op->rs] + op->imm, base_rt);
        if (PSX_UNLIKELY(c->jit_abort != 0u)) goto L_MEM_ABORT;
        COMMIT_PREV_FOR_NEW_LOAD(op->rt, v);
        NEXT_OP();
    }
    L_COP_LWR: {
        u32 base_rt = (pend_reg == op->rt && op->rt != 0u) ? pend_val : c->gpr[op->rt];
        u32 v = jit_lwr_helper(c, c->gpr[op->rs] + op->imm, base_rt);
        if (PSX_UNLIKELY(c->jit_abort != 0u)) goto L_MEM_ABORT;
        COMMIT_PREV_FOR_NEW_LOAD(op->rt, v);
        NEXT_OP();
    }

    L_COP_SB:
        cached_write8(c, c->gpr[op->rs] + op->imm, (u8)c->gpr[op->rt]);
        if (PSX_UNLIKELY(c->jit_abort != 0u)) goto L_MEM_ABORT;
        COMMIT_PREV_LOAD_UNLESS(0u);
        NEXT_OP();
    L_COP_SH:
        cached_write16(c, c->gpr[op->rs] + op->imm, (u16)c->gpr[op->rt]);
        if (PSX_UNLIKELY(c->jit_abort != 0u)) goto L_MEM_ABORT;
        COMMIT_PREV_LOAD_UNLESS(0u);
        NEXT_OP();
    L_COP_SW:
        cached_write32(c, c->gpr[op->rs] + op->imm, c->gpr[op->rt]);
        if (PSX_UNLIKELY(c->jit_abort != 0u)) goto L_MEM_ABORT;
        COMMIT_PREV_LOAD_UNLESS(0u);
        NEXT_OP();
    L_COP_SWL:
        jit_swl_helper(c, c->gpr[op->rs] + op->imm, c->gpr[op->rt]);
        if (PSX_UNLIKELY(c->jit_abort != 0u)) goto L_MEM_ABORT;
        COMMIT_PREV_LOAD_UNLESS(0u);
        NEXT_OP();
    L_COP_SWR:
        jit_swr_helper(c, c->gpr[op->rs] + op->imm, c->gpr[op->rt]);
        if (PSX_UNLIKELY(c->jit_abort != 0u)) goto L_MEM_ABORT;
        COMMIT_PREV_LOAD_UNLESS(0u);
        NEXT_OP();

    L_COP_J:
        has_branch = true;
        branch_taken = true;
        branch_target = op->imm;
        COMMIT_PREV_LOAD_UNLESS(0u);
        NEXT_OP();
    L_COP_JAL:
        has_branch = true;
        branch_taken = true;
        branch_target = op->imm;
        WRITE_GPR_IMM(31u, op->pc + 8u);
        NEXT_OP();
    L_COP_JR:
        has_branch = true;
        branch_taken = true;
        branch_target = c->gpr[op->rs];
        COMMIT_PREV_LOAD_UNLESS(0u);
        NEXT_OP();
    L_COP_JALR: {
        u32 tgt = c->gpr[op->rs];
        has_branch = true;
        branch_taken = true;
        branch_target = tgt;
        WRITE_GPR_IMM(op->rd, op->pc + 8u);
        NEXT_OP();
    }
    L_COP_BEQ:
        has_branch = true;
        branch_taken = (c->gpr[op->rs] == c->gpr[op->rt]);
        branch_target = op->imm;
        COMMIT_PREV_LOAD_UNLESS(0u);
        NEXT_OP();
    L_COP_BNE:
        has_branch = true;
        branch_taken = (c->gpr[op->rs] != c->gpr[op->rt]);
        branch_target = op->imm;
        COMMIT_PREV_LOAD_UNLESS(0u);
        NEXT_OP();
    L_COP_BLEZ:
        has_branch = true;
        branch_taken = ((s32)c->gpr[op->rs] <= 0);
        branch_target = op->imm;
        COMMIT_PREV_LOAD_UNLESS(0u);
        NEXT_OP();
    L_COP_BGTZ:
        has_branch = true;
        branch_taken = ((s32)c->gpr[op->rs] > 0);
        branch_target = op->imm;
        COMMIT_PREV_LOAD_UNLESS(0u);
        NEXT_OP();
    L_COP_BLTZ:
        has_branch = true;
        branch_taken = ((s32)c->gpr[op->rs] < 0);
        branch_target = op->imm;
        COMMIT_PREV_LOAD_UNLESS(0u);
        NEXT_OP();
    L_COP_BGEZ:
        has_branch = true;
        branch_taken = ((s32)c->gpr[op->rs] >= 0);
        branch_target = op->imm;
        COMMIT_PREV_LOAD_UNLESS(0u);
        NEXT_OP();
    L_COP_BLTZAL: {
        bool cond = ((s32)c->gpr[op->rs] < 0);
        has_branch = true;
        branch_taken = cond;
        branch_target = op->imm;
        WRITE_GPR_IMM(31u, op->pc + 8u);
        NEXT_OP();
    }
    L_COP_BGEZAL: {
        bool cond = ((s32)c->gpr[op->rs] >= 0);
        has_branch = true;
        branch_taken = cond;
        branch_target = op->imm;
        WRITE_GPR_IMM(31u, op->pc + 8u);
        NEXT_OP();
    }

    L_COP_SYSCALL:
        CACHED_FAULT(PSX_EXC_SYS, 0u, 0u);
    L_COP_BREAK:
        CACHED_FAULT(PSX_EXC_BP, 0u, 0u);
    L_COP_RI:
        CACHED_FAULT(PSX_EXC_RI, 0u, 0u);

    L_COP_MFC0: {
        u32 sr = c->cop0[COP0_SR];
        if (PSX_UNLIKELY(((sr & PSX_SR_CU0) == 0u) && ((sr & PSX_SR_KUC) != 0u))) {
            CACHED_FAULT(PSX_EXC_CPU, 0u, 0u);
        }
        COMMIT_PREV_FOR_NEW_LOAD(op->rt, cop0_read_reg(c, op->rd));
        NEXT_OP();
    }
    L_COP_MTC0: {
        u32 sr = c->cop0[COP0_SR];
        if (PSX_UNLIKELY(((sr & PSX_SR_CU0) == 0u) && ((sr & PSX_SR_KUC) != 0u))) {
            CACHED_FAULT(PSX_EXC_CPU, 0u, 0u);
        }
        u32 v = c->gpr[op->rt];
        if (op->rd == COP0_SR) {
            jit_set_sr_wrap(sys, v);
        } else if (op->rd == COP0_CAUSE) {
            jit_set_cause_wrap(sys, v);
        } else {
            cop0_write_non_sr_cause(c, op->rd, v);
        }
        COMMIT_PREV_LOAD_UNLESS(0u);
        NEXT_OP();
    }
    L_COP_RFE: {
        u32 sr = c->cop0[COP0_SR];
        if (PSX_UNLIKELY(((sr & PSX_SR_CU0) == 0u) && ((sr & PSX_SR_KUC) != 0u))) {
            CACHED_FAULT(PSX_EXC_CPU, 0u, 0u);
        }
        jit_set_sr_wrap(sys, (sr & ~0x0Fu) | ((sr >> 2) & 0x0Fu));
        COMMIT_PREV_LOAD_UNLESS(0u);
        NEXT_OP();
    }
    L_COP_COP0_NOP: {
        u32 sr = c->cop0[COP0_SR];
        if (PSX_UNLIKELY(((sr & PSX_SR_CU0) == 0u) && ((sr & PSX_SR_KUC) != 0u))) {
            CACHED_FAULT(PSX_EXC_CPU, 0u, 0u);
        }
        COMMIT_PREV_LOAD_UNLESS(0u);
        NEXT_OP();
    }
    L_COP_COP1_NOP:
        if (PSX_UNLIKELY((c->cop0[COP0_SR] & PSX_SR_CU1) == 0u)) {
            CACHED_FAULT(PSX_EXC_CPU, 0u, 1u);
        }
        COMMIT_PREV_LOAD_UNLESS(0u);
        NEXT_OP();
    L_COP_COP3_NOP:
        if (PSX_UNLIKELY((c->cop0[COP0_SR] & PSX_SR_CU3) == 0u)) {
            CACHED_FAULT(PSX_EXC_CPU, 0u, 3u);
        }
        COMMIT_PREV_LOAD_UNLESS(0u);
        NEXT_OP();

    L_COP_MFC2:
        if (!cop2_checked) {
            if (PSX_UNLIKELY((c->cop0[COP0_SR] & PSX_SR_CU2) == 0u)) {
                CACHED_FAULT(PSX_EXC_CPU, 0u, 2u);
            }
            cop2_checked = true;
        }
        COMMIT_PREV_FOR_NEW_LOAD(op->rt, jit_gte_read_data_wrap(c, op->rd));
        NEXT_OP();
    L_COP_CFC2:
        if (!cop2_checked) {
            if (PSX_UNLIKELY((c->cop0[COP0_SR] & PSX_SR_CU2) == 0u)) {
                CACHED_FAULT(PSX_EXC_CPU, 0u, 2u);
            }
            cop2_checked = true;
        }
        COMMIT_PREV_FOR_NEW_LOAD(op->rt, jit_gte_read_ctrl_wrap(c, op->rd));
        NEXT_OP();
    L_COP_MTC2:
        if (!cop2_checked) {
            if (PSX_UNLIKELY((c->cop0[COP0_SR] & PSX_SR_CU2) == 0u)) {
                CACHED_FAULT(PSX_EXC_CPU, 0u, 2u);
            }
            cop2_checked = true;
        }
        jit_gte_write_data_wrap(c, op->rd, c->gpr[op->rt]);
        COMMIT_PREV_LOAD_UNLESS(0u);
        NEXT_OP();
    L_COP_CTC2:
        if (!cop2_checked) {
            if (PSX_UNLIKELY((c->cop0[COP0_SR] & PSX_SR_CU2) == 0u)) {
                CACHED_FAULT(PSX_EXC_CPU, 0u, 2u);
            }
            cop2_checked = true;
        }
        jit_gte_write_ctrl_wrap(c, op->rd, c->gpr[op->rt]);
        COMMIT_PREV_LOAD_UNLESS(0u);
        NEXT_OP();
    L_COP_GTE_EXEC:
        if (!cop2_checked) {
            if (PSX_UNLIKELY((c->cop0[COP0_SR] & PSX_SR_CU2) == 0u)) {
                CACHED_FAULT(PSX_EXC_CPU, 0u, 2u);
            }
            cop2_checked = true;
        }
        jit_gte_exec_wrap(c, op->imm);
        COMMIT_PREV_LOAD_UNLESS(0u);
        NEXT_OP();
    L_COP_LWC2: {
        if (!cop2_checked) {
            if (PSX_UNLIKELY((c->cop0[COP0_SR] & PSX_SR_CU2) == 0u)) {
                CACHED_FAULT(PSX_EXC_CPU, 0u, 2u);
            }
            cop2_checked = true;
        }
        u32 v = cached_read32(c, c->gpr[op->rs] + op->imm);
        if (PSX_UNLIKELY(c->jit_abort != 0u)) goto L_MEM_ABORT;
        jit_gte_write_data_wrap(c, op->rt, v);
        COMMIT_PREV_LOAD_UNLESS(0u);
        NEXT_OP();
    }
    L_COP_SWC2: {
        if (!cop2_checked) {
            if (PSX_UNLIKELY((c->cop0[COP0_SR] & PSX_SR_CU2) == 0u)) {
                CACHED_FAULT(PSX_EXC_CPU, 0u, 2u);
            }
            cop2_checked = true;
        }
        u32 v = jit_gte_read_data_wrap(c, op->rt);
        cached_write32(c, c->gpr[op->rs] + op->imm, v);
        if (PSX_UNLIKELY(c->jit_abort != 0u)) goto L_MEM_ABORT;
        COMMIT_PREV_LOAD_UNLESS(0u);
        NEXT_OP();
    }
    L_COP_LWC0_NOP:
    L_COP_SWC0_NOP: {
        if (PSX_UNLIKELY((c->cop0[COP0_SR] & PSX_SR_CU0) == 0u)) {
            CACHED_FAULT(PSX_EXC_CPU, 0u, 0u);
        }
        u32 addr = c->gpr[op->rs] + op->imm;
        if (PSX_UNLIKELY(psx_addr_fault(c, addr, 4u))) {
            CACHED_FAULT((op->op == COP_LWC0_NOP) ? PSX_EXC_ADEL : PSX_EXC_ADES, addr, 0u);
        }
        COMMIT_PREV_LOAD_UNLESS(0u);
        NEXT_OP();
    }
    L_COP_LWC1_NOP:
    L_COP_SWC1_NOP: {
        if (PSX_UNLIKELY((c->cop0[COP0_SR] & PSX_SR_CU1) == 0u)) {
            CACHED_FAULT(PSX_EXC_CPU, 0u, 1u);
        }
        u32 addr = c->gpr[op->rs] + op->imm;
        if (PSX_UNLIKELY(psx_addr_fault(c, addr, 4u))) {
            CACHED_FAULT((op->op == COP_LWC1_NOP) ? PSX_EXC_ADEL : PSX_EXC_ADES, addr, 0u);
        }
        COMMIT_PREV_LOAD_UNLESS(0u);
        NEXT_OP();
    }
    L_COP_LWC3_NOP:
    L_COP_SWC3_NOP: {
        if (PSX_UNLIKELY((c->cop0[COP0_SR] & PSX_SR_CU3) == 0u)) {
            CACHED_FAULT(PSX_EXC_CPU, 0u, 3u);
        }
        u32 addr = c->gpr[op->rs] + op->imm;
        if (PSX_UNLIKELY(psx_addr_fault(c, addr, 4u))) {
            CACHED_FAULT((op->op == COP_LWC3_NOP) ? PSX_EXC_ADEL : PSX_EXC_ADES, addr, 0u);
        }
        COMMIT_PREV_LOAD_UNLESS(0u);
        NEXT_OP();
    }

    L_MEM_ABORT:
        exc_code = c->jit_abort - 1u;
        exc_badv = c->abort_badvaddr;
        exc_ce = 0u;
        c->jit_abort = 0u;
        /* Fallthrough to L_BLOCK_EXC */

    L_BLOCK_EXC: {
        c->in_jit = 0u;
        if (pend_reg != 0u) {
            c->gpr[pend_reg] = pend_val;
            c->gpr[0] = 0u;
            pend_reg = 0u;
        }
        s32 refund = (s32)b->cycles - (s32)op->cyc_before;
        if (refund > 0) {
            c->cycles_left += refund;
        }
        u32 executed_insns = ((op->pc - b->pc) >> 2) + 1u;
        u32 executed_cyc = (u32)op->cyc_before;
        c->total_cycles += (u64)executed_cyc;
        sys->stats.cycles_total += (u64)executed_cyc;
        sys->stats.insns_total += (u64)executed_insns;

        u32 jumpdest = branch_taken ? branch_target : (op->pc + 4u);
        jit_raise_exc_wrap(sys, exc_code, op->pc, (op->flags & CF_IN_DS) != 0u,
                           jumpdest, exc_badv, exc_ce);
        continue;
    }

    L_COP_END: {
        c->in_jit = 0u;
        c->gpr[0] = 0u;
        u32 next_pc = (has_branch && branch_taken) ? branch_target : op->imm;
        c->pc = next_pc;
        c->next_pc = next_pc + 4u;
        if (pend_reg != 0u) {
            c->load_delay_reg = pend_reg;
            c->load_delay_val = pend_val;
        } else {
            c->load_delay_reg = 0u;
        }

        c->total_cycles += (u64)b->cycles;
        sys->stats.cycles_total += (u64)b->cycles;
        sys->stats.insns_total += (u64)b->n_insns;

        if ((b->flags & JB_IDLE_LOOP) != 0u && next_pc == b->pc && pend_reg == 0u) {
            c->idle_skip = 1u;
            c->cycles_left = 0;
            sys->stats.idle_skips++;
            break;
        }
    }
    } /* while (cycles_left > 0) */

#undef COMMIT_PREV_LOAD_UNLESS
#undef COMMIT_PREV_FOR_NEW_LOAD
#undef WRITE_GPR_IMM
#undef NEXT_OP
#undef CACHED_FAULT
#undef CACHED_DISPATCH

    if (c->in_delay_slot == 0u) {
        c->next_pc = c->pc + 4u;
    }
    c->exit_reason = PSX_EXIT_EVENTS;
}
