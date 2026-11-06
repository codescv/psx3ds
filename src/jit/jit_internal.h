#ifndef PSX3DS_JIT_INTERNAL_H
#define PSX3DS_JIT_INTERNAL_H

#include "psx3ds/arena.h"
#include "psx3ds/bus.h"
#include "psx3ds/config.h"
#include "psx3ds/cpu.h"
#include "psx3ds/gte.h"
#include "psx3ds/hw_regs.h"
#include "psx3ds/jit.h"
#include "psx3ds/jit_mem.h"
#include "psx3ds/state.h"
#include "psx3ds/stats.h"
#include "psx3ds/system.h"

/* Internal JitBlock flag distinguishing cached-interpreter CachedOp blocks from ARM JIT blocks */
#define JB_CACHED_OP (1u << 15)

/* Safe block pool cap: index 0xFFFF is reserved for JIT_PAGE_NONE */
#define JIT_SAFE_MAX_BLOCKS 0xFFFEu

/* Check whether a guest virtual PC is a pinned hook address */
static inline bool jit_is_pinned_pc(const PsxSystem *sys, u32 pc) {
    u32 phys = pc & 0x1FFFFFFFu;
    if (phys == 0xA0u || phys == 0xB0u || phys == 0xC0u) {
        return true;
    }
    if ((sys->cpu.psexe_pending != 0u || sys->bios.psexe_pending) && phys == 0x30000u) {
        return true;
    }
    return false;
}

/* Check whether a guest virtual PC maps to cacheable 2 MiB RAM (incl. mirrors unless strict) */
static inline bool jit_pc_is_ram(const PsxSystem *sys, u32 pc) {
    if ((pc & 3u) != 0u) {
        return false;
    }
    bool seg_ok = (pc < 0x20000000u) || (pc >= 0x80000000u && pc < 0xC0000000u);
    if (!seg_ok) {
        return false;
    }
    u32 phys = pc & 0x1FFFFFFFu;
    if (phys < 0x00200000u) {
        return true;
    }
    if (phys < 0x00800000u && !sys->cfg.strict_ram_size) {
        return true;
    }
    return false;
}

/* Check whether a guest virtual PC maps to 512 KiB BIOS ROM */
static inline bool jit_pc_is_bios(u32 pc) {
    if ((pc & 3u) != 0u) {
        return false;
    }
    bool seg_ok = (pc < 0x20000000u) || (pc >= 0x80000000u && pc < 0xC0000000u);
    if (!seg_ok) {
        return false;
    }
    u32 phys = pc & 0x1FFFFFFFu;
    return (phys >= PSX_BIOS_PHYS) && (phys < (PSX_BIOS_PHYS + PSX_BIOS_SIZE));
}

/* Cumulative cycles consumed after k_insns instructions and k_mem_ops loads/stores */
static inline u32 jit_cycles_after(const PsxConfig *cfg, u32 k_insns, u32 k_mem_ops) {
    u32 mult = cfg->cycle_mult ? (u32)cfg->cycle_mult : 256u;
    return ((k_insns * mult) >> 8) + k_mem_ops * (u32)cfg->cycles_per_mem;
}

/* Scale instruction + memory counts into block cycle cost (>= 1, <= 0xFFFF) */
static inline u16 jit_scale_cycles(const PsxConfig *cfg, u32 n_insns, u32 n_mem_ops) {
    u32 cyc = jit_cycles_after(cfg, n_insns, n_mem_ops);
    if (cyc == 0u) {
        cyc = 1u;
    }
    if (cyc > 0xFFFFu) {
        cyc = 0xFFFFu;
    }
    return (u16)cyc;
}

/* Block-pool & IBTC management helpers (jit_block.c) */
void jit_purge_ibtc(JitState *j);
void jit_refresh_pins(PsxSystem *sys);
bool jit_block_add_ram_page(PsxSystem *sys, u16 block_idx, u16 phys_page);
/* Register a freshly built block's guest code lines in the sub-page SMC filter */
void jit_block_mark_code(PsxSystem *sys, const JitBlock *b);

/*
 * Core-stub compatibility layer (implemented in cpu_cached.c, compiled on all targets):
 * When Slice A's src/core/ sources are real (gte_unr_table[0] == 0xFF), every wrapper
 * forwards directly to Slice A with zero behaviour change. When running standalone
 * unit tests in slice-b-jit before merge (gte_unr_table[0] == 0), they provide an
 * exact R3000A/COP0/Bus/GTE execution oracle so unit tests can exercise SMC, traps,
 * MMIO, branch-in-delay-slot, and inline vs reference GTE end-to-end.
 */
static inline bool jit_core_is_stub(void) {
    return gte_unr_table[0] == 0u;
}

const u8 *jit_gte_unr_table_ptr(void);

void jit_ref_code_page(PsxBus *b, u32 phys_page);
void jit_unref_code_page(PsxBus *b, u32 phys_page);
void jit_request_exit_wrap(PsxSystem *sys);

u32  jit_fetch32_wrap(PsxCpu *c, u32 pc);
u32  jit_read32_slow_wrap(PsxCpu *c, u32 a);
u16  jit_read16_slow_wrap(PsxCpu *c, u32 a);
u8   jit_read8_slow_wrap(PsxCpu *c, u32 a);
void jit_write32_slow_wrap(PsxCpu *c, u32 a, u32 v);
void jit_write16_slow_wrap(PsxCpu *c, u32 a, u32 v);
void jit_write8_slow_wrap(PsxCpu *c, u32 a, u32 v);

u32  jit_lwl_helper(PsxCpu *c, u32 addr, u32 cur_rt);
u32  jit_lwr_helper(PsxCpu *c, u32 addr, u32 cur_rt);
void jit_swl_helper(PsxCpu *c, u32 addr, u32 val_rt);
void jit_swr_helper(PsxCpu *c, u32 addr, u32 val_rt);

void jit_raise_exc_wrap(PsxSystem *sys, u32 code, u32 pc, bool bd,
                        u32 branch_target, u32 badvaddr, u32 ce);
void jit_set_sr_wrap(PsxSystem *sys, u32 v);
void jit_set_cause_wrap(PsxSystem *sys, u32 v);
void jit_check_irq_wrap(PsxSystem *sys);
void jit_step_one_wrap(PsxSystem *sys);
void jit_step_until_clean_wrap(PsxSystem *sys);

void jit_gte_exec_wrap(PsxCpu *cpu, u32 opcode);
u32  jit_gte_read_data_wrap(PsxCpu *cpu, u32 reg);
void jit_gte_write_data_wrap(PsxCpu *cpu, u32 reg, u32 val);
u32  jit_gte_read_ctrl_wrap(const PsxCpu *cpu, u32 reg);
void jit_gte_write_ctrl_wrap(PsxCpu *cpu, u32 reg, u32 val);
u32  jit_gte_div_unr_wrap(u32 h, u32 sz3, bool *overflow);

/* MIPS DIV / DIVU exact hardware semantics helper (shared by cached & arm_jit) */
static inline void jit_mips_div(s32 num, s32 den, u32 *out_lo, u32 *out_hi) {
    if (den == 0) {
        *out_lo = (num < 0) ? 1u : 0xFFFFFFFFu;
        *out_hi = (u32)num;
    } else if ((u32)num == 0x80000000u && den == -1) {
        *out_lo = 0x80000000u;
        *out_hi = 0u;
    } else {
        *out_lo = (u32)(num / den);
        *out_hi = (u32)(num % den);
    }
}

static inline void jit_mips_divu(u32 num, u32 den, u32 *out_lo, u32 *out_hi) {
    if (den == 0u) {
        *out_lo = 0xFFFFFFFFu;
        *out_hi = num;
    } else {
        *out_lo = num / den;
        *out_hi = num % den;
    }
}

#endif /* PSX3DS_JIT_INTERNAL_H */
