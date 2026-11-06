#include "psx3ds/cpu.h"
#include "psx3ds/system.h"

void cpu_request_exit(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    PsxCpu *c = &sys->cpu;
    if (c->cycles_left > 0) {
        c->slice_len -= c->cycles_left;
        c->cycles_left = 0;
    }
}

void cpu_check_irq(PsxSystem *sys) {
    if (!sys) {
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
    u32 pending = (((sr & PSX_SR_IEC) != 0u) && ((sr & cause & PSX_CAUSE_IRQ_MASK) != 0u))
                      ? 1u
                      : 0u;
    c->irq_pending = pending;
    if (pending != 0u) {
        cpu_request_exit(sys);
    }
}

void cpu_set_sr(PsxSystem *sys, u32 v) {
    if (!sys) {
        return;
    }
    PsxCpu *c = &sys->cpu;
    u32 old = c->cop0[COP0_SR];
    c->cop0[COP0_SR] = v;

    if (((old ^ v) & PSX_SR_ISC) != 0u) {
        bool isc = (v & PSX_SR_ISC) != 0u;
        fastmem_set_isolated(&sys->bus, isc);
        if (sys->cfg.isc_swap_table && sys->bus.fm_w_isc_null) {
            c->fastmem_w.p = isc ? sys->bus.fm_w_isc_null : sys->bus.fm_w;
        }
    }

    cpu_check_irq(sys);
}

void cpu_set_cause(PsxSystem *sys, u32 v) {
    if (!sys) {
        return;
    }
    PsxCpu *c = &sys->cpu;
    c->cop0[COP0_CAUSE] = (c->cop0[COP0_CAUSE] & ~PSX_CAUSE_SW_MASK) | (v & PSX_CAUSE_SW_MASK);
    cpu_check_irq(sys);
}

void cpu_raise_exception(PsxSystem *sys, u32 code, u32 pc, bool bd,
                         u32 branch_target, u32 badvaddr, u32 ce) {
    if (!sys) {
        return;
    }
    PsxCpu *c = &sys->cpu;

    /* Commit any in-flight load delay: the prior load will not re-execute after RFE */
    if (c->load_delay_reg != 0u) {
        c->gpr[c->load_delay_reg] = c->load_delay_val;
        c->load_delay_reg = 0u;
        c->load_delay_val = 0u;
    }
    c->gpr[0] = 0u;

    c->cop0[COP0_EPC] = pc;

    /* Preserve pending interrupt lines (bits 15:8), rebuild ExcCode / CE / BD */
    u32 cause = c->cop0[COP0_CAUSE] & 0x0000FF00u;
    cause |= (code & 0x1Fu) << PSX_CAUSE_EXCCODE_SHIFT;
    if (code == PSX_EXC_CPU) {
        cause |= (ce & 3u) << PSX_CAUSE_CE_SHIFT;
    }
    if (bd) {
        cause |= PSX_CAUSE_BD;
        c->cop0[COP0_JUMPDEST] = branch_target;
    }
    c->cop0[COP0_CAUSE] = cause;

    /* BadVAddr is updated ONLY on AdEL and AdES */
    if (code == PSX_EXC_ADEL || code == PSX_EXC_ADES) {
        c->cop0[COP0_BADVADDR] = badvaddr;
    }

    /* Push 3-level KU/IE stack (bits 5:0 <- bits 3:0 << 2; new KUc=0, IEc=0) */
    u32 sr = c->cop0[COP0_SR];
    u32 next_sr = (sr & ~0x3Fu) | ((sr << 2) & 0x3Cu);
    cpu_set_sr(sys, next_sr);

    u32 vec = ((sr & PSX_SR_BEV) != 0u) ? PSX_EXC_VEC_ROM : PSX_EXC_VEC_RAM;
    c->pc = vec;
    c->next_pc = vec + 4u;
    c->in_delay_slot = 0u;
    c->branch_taken = 0u;
    c->branch_target = 0u;
    c->exc_pending = 1u;

    if (code == PSX_EXC_INT) {
        sys->stats.irqs++;
    } else {
        sys->stats.exceptions++;
    }
}

void cpu_dispatch_irq(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    PsxCpu *c = &sys->cpu;
    u32 epc = (c->in_delay_slot != 0u) ? (c->pc - 4u) : c->pc;
    cpu_raise_exception(sys, PSX_EXC_INT, epc, c->in_delay_slot != 0u,
                        c->branch_target, 0u, 0u);
    c->exc_pending = 0u;
}
