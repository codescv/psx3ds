#include "psx3ds/irq.h"
#include "psx3ds/system.h"

#define PSX_IRQ_VALID_MASK 0x000007FFu

PSX_INLINE u32 irq_width_mask(u32 width) {
    if (width == 1u) return 0xFFu;
    if (width == 2u) return 0xFFFFu;
    return 0xFFFFFFFFu;
}

static u32 irq_mmio_read(PsxSystem *sys, u32 addr, u32 width) {
    u32 word_addr = addr & ~3u;
    u32 shift = (addr & 3u) * 8u;
    u32 val = 0u;

    if (word_addr == PSX_REG_I_STAT) {
        val = sys->irq.stat;
    } else if (word_addr == PSX_REG_I_MASK) {
        val = sys->irq.mask;
    }
    return (val >> shift) & irq_width_mask(width);
}

static void irq_mmio_write(PsxSystem *sys, u32 addr, u32 v, u32 width) {
    u32 word_addr = addr & ~3u;
    u32 shift = (addr & 3u) * 8u;
    u32 lane_mask = irq_width_mask(width) << shift;

    if (word_addr == PSX_REG_I_STAT) {
        /* Write-0-to-clear, write-1-preserves; untouched byte lanes preserved */
        u32 ack_mask = ((v << shift) & lane_mask) | ~lane_mask;
        sys->irq.stat &= ack_mask & PSX_IRQ_VALID_MASK;
        cpu_check_irq(sys);
    } else if (word_addr == PSX_REG_I_MASK) {
        sys->irq.mask = ((sys->irq.mask & ~lane_mask) | ((v << shift) & lane_mask)) &
                        PSX_IRQ_VALID_MASK;
        cpu_check_irq(sys);
    }
}

void irq_init(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    sys->irq.stat = 0u;
    sys->irq.mask = 0u;
    bus_mmio_register(sys, PSX_REG_I_STAT, 0x10u, irq_mmio_read, irq_mmio_write);
}

void irq_reset(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    sys->irq.stat = 0u;
    sys->irq.mask = 0u;
    cpu_check_irq(sys);
}

void irq_state(PsxSystem *sys, StateIO *st) {
    if (!sys || !st) {
        return;
    }
    state_u32(st, &sys->irq.stat);
    state_u32(st, &sys->irq.mask);
    if (!st->writing) {
        sys->irq.stat &= PSX_IRQ_VALID_MASK;
        sys->irq.mask &= PSX_IRQ_VALID_MASK;
        cpu_check_irq(sys);
    }
}

void irq_shutdown(PsxSystem *sys) {
    (void)sys;
}

void irq_raise(PsxSystem *sys, u32 irq_bit) {
    if (!sys || irq_bit > 10u) {
        return;
    }
    sys->irq.stat |= (1u << irq_bit);
    cpu_check_irq(sys);
}
