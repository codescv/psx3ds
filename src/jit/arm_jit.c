#include "arm_emit.h"

#include <stddef.h>
#include <string.h>

#if PSX3DS_HAVE_JIT

/* ============================================================================
 * Cold-Stub Descriptor Queue (emitted out-of-line after the block terminator)
 * ============================================================================ */

#define JIT_MAX_COLD_STUBS  160u
#define JIT_EAGER_MAX_DEPTH 4u
#define JIT_EAGER_BUDGET    32u

typedef enum ColdStubKind {
    STUB_EXC = 0,
    STUB_MEM_LOAD,
    STUB_MEM_STORE
} ColdStubKind;

typedef struct ColdStub {
    u8  kind;
    u8  exc_code;
    u8  ce;
    u8  bd;
    u8  pend_reg;
    u8  dirty_mask;
    u8  width;
    u8  is_signed;
    u8  arm_reg;
    u8  badv_in_r1;
    u16 label_entry;
    u16 label_cont;
    u16 cyc_refund;
    u32 pc;
    u32 badv_const;
} ColdStub;

/* ============================================================================
 * Runtime C Leaf / Slow Helpers Invoked from Generated Code & jit_thunks.S
 * ============================================================================ */

static void jit_div_helper(PsxCpu *c, s32 num, s32 den) {
    jit_mips_div(num, den, &c->lo, &c->hi);
}

static void jit_divu_helper(PsxCpu *c, u32 num, u32 den) {
    jit_mips_divu(num, den, &c->lo, &c->hi);
}

static void jit_mtc0_sr_helper(PsxCpu *c, u32 val) {
    PsxSystem *sys = (PsxSystem *)c->sys.p;
    u32 old_sr = c->cop0[COP0_SR];
    jit_set_sr_wrap(sys, val);
    if (((old_sr ^ c->cop0[COP0_SR]) & PSX_SR_KUC) != 0u) {
        jit_purge_ibtc(&sys->jit);
    }
}

static void jit_mtc0_cause_helper(PsxCpu *c, u32 val) {
    PsxSystem *sys = (PsxSystem *)c->sys.p;
    jit_set_cause_wrap(sys, val);
}

static void jit_rfe_helper(PsxCpu *c) {
    PsxSystem *sys = (PsxSystem *)c->sys.p;
    u32 old_sr = c->cop0[COP0_SR];
    u32 next_sr = (old_sr & ~0x0Fu) | ((old_sr >> 2) & 0x0Fu);
    jit_set_sr_wrap(sys, next_sr);
    if (((old_sr ^ c->cop0[COP0_SR]) & PSX_SR_KUC) != 0u) {
        jit_purge_ibtc(&sys->jit);
    }
}

PSX_USED void jit_handle_exception_thunk(PsxCpu *cpu, u32 code, u32 pc, u32 flags) {
    PsxSystem *sys = (PsxSystem *)cpu->sys.p;
    bool bd = (flags & 1u) != 0u;
    u32 ce = (flags >> 8) & 3u;
    u32 branch_target = bd ? cpu->gte_scratch[0] : 0u;
    u32 badv = cpu->abort_badvaddr;
    cpu->jit_abort = 0u;
    jit_raise_exc_wrap(sys, code, pc, bd, branch_target, badv, ce);
}

PSX_USED void *jit_bios_vec_helper(PsxCpu *cpu, u32 pc) {
    PsxSystem *sys = (PsxSystem *)cpu->sys.p;
    cpu->pc = pc;
    cpu->next_pc = pc + 4u;
    cpu->in_jit = 0u;
    jit_step_until_clean_wrap(sys);
    cpu->in_jit = 1u;
    if (cpu->cycles_left <= 0 || sys->quit || sys->frame_done) {
        return (void *)jit_exit;
    }
    return (void *)jit_dispatch;
}

PSX_USED void *jit_psexe_helper(PsxCpu *cpu, u32 pc) {
    PsxSystem *sys = (PsxSystem *)cpu->sys.p;
    cpu->pc = pc;
    cpu->next_pc = pc + 4u;
    cpu->in_jit = 0u;
    jit_step_until_clean_wrap(sys);
    jit_refresh_pins(sys);
    cpu->in_jit = 1u;
    if (cpu->cycles_left <= 0 || sys->quit || sys->frame_done) {
        return (void *)jit_exit;
    }
    return (void *)jit_dispatch;
}

/* ============================================================================
 * Block Lookup Helpers for Direct Block Linking & Segment-Tag Verification
 * ============================================================================ */

static JitBlock *jit_find_live_block(const PsxSystem *sys, u32 pc) {
    const JitState *j = &sys->jit;
    if (!j->blocks || j->n_blocks == 0u) {
        return NULL;
    }
    if (jit_pc_is_ram(sys, pc)) {
        void *slot = j->tab_ram[BLOCKTAB_RAM_IDX(pc)];
        if (slot == (void *)jit_compile_stub ||
            slot == (void *)jit_bios_vec_stub ||
            slot == (void *)jit_psexe_stub) {
            return NULL;
        }
        u16 pg = (u16)((pc & PSX_RAM_MASK) >> 12);
        u16 cur = j->page_head[pg];
        while (cur != JIT_PAGE_NONE) {
            JitBlock *b = &j->blocks[cur];
            if (b->entry.p == slot && (b->flags & JB_DEAD) == 0u) {
                return b;
            }
            cur = b->next_in_page[(b->phys_page[0] == pg) ? 0 : 1];
        }
        return NULL;
    }
    if (jit_pc_is_bios(pc)) {
        void *slot = j->tab_bios[BLOCKTAB_BIOS_IDX(pc)];
        if (slot == (void *)jit_compile_stub) {
            return NULL;
        }
        for (u32 k = j->n_blocks; k > 0u; --k) {
            JitBlock *b = &j->blocks[k - 1u];
            if (b->entry.p == slot && (b->flags & JB_DEAD) == 0u) {
                return b;
            }
        }
    }
    return NULL;
}

static bool jit_pages_subset(const JitBlock *target, const u16 parent_pages[2]) {
    for (u32 k = 0u; k < 2u; ++k) {
        u16 tp = target->phys_page[k];
        if (tp == JIT_PAGE_NONE) {
            continue;
        }
        if (tp != parent_pages[0] && tp != parent_pages[1]) {
            return false;
        }
    }
    return true;
}

static inline void *jit_block_fast_entry(const JitBlock *b) {
    return (void *)((u8 *)b->entry.p + b->_pad);
}

static void jit_ibtc_record(PsxSystem *sys, u32 pc, const JitBlock *b) {
    if (!sys->cfg.jit_ibtc || !sys->jit.ibtc || !b) {
        return;
    }
    if (jit_is_pinned_pc(sys, pc)) {
        return;
    }
    if (((sys->cpu.cop0[COP0_SR] & PSX_SR_KUC) != 0u) && ((pc & 0x80000000u) != 0u)) {
        return;
    }
    u32 idx = (pc >> 2) & (JIT_IBTC_ENTRIES - 1u);
    sys->jit.ibtc[idx].tag = pc;
    sys->jit.ibtc[idx].entry = jit_block_fast_entry(b);
}

/* ============================================================================
 * Code-Gen Sub-Emitters (Cold Stubs, Pending Loads, Terminators, Fastmem)
 * ============================================================================ */

static u16 push_exc_stub(Emitter *e, ColdStub *stubs, u32 *n_stubs,
                         const JitRegAlloc *ra, u32 pend_reg,
                         const JitDecodedInsn *dec, u16 block_cycles,
                         u8 exc_code, u8 ce, bool badv_in_r1, u32 badv_const) {
    u16 lbl = arm_label_create(e);
    if (*n_stubs < JIT_MAX_COLD_STUBS) {
        ColdStub *s = &stubs[(*n_stubs)++];
        memset(s, 0, sizeof(*s));
        s->kind = (u8)STUB_EXC;
        s->exc_code = exc_code;
        s->ce = ce;
        s->bd = dec->in_delay_slot;
        s->pend_reg = (u8)pend_reg;
        s->dirty_mask = jit_ra_dirty_mask(ra);
        s->badv_in_r1 = badv_in_r1 ? 1u : 0u;
        s->label_entry = lbl;
        s->cyc_refund = (block_cycles > dec->cyc_before)
                            ? (u16)(block_cycles - dec->cyc_before)
                            : 0u;
        s->pc = dec->pc;
        s->badv_const = badv_const;
    } else {
        e->overflow = true;
    }
    return lbl;
}

static ColdStub *push_mem_stub(Emitter *e, ColdStub *stubs, u32 *n_stubs,
                               ColdStubKind kind, const JitRegAlloc *ra, u32 pend_reg,
                               const JitDecodedInsn *dec, u16 block_cycles,
                               u8 width, bool is_signed, ArmReg reg) {
    if (*n_stubs >= JIT_MAX_COLD_STUBS) {
        e->overflow = true;
        return NULL;
    }
    ColdStub *s = &stubs[(*n_stubs)++];
    memset(s, 0, sizeof(*s));
    s->kind = (u8)kind;
    s->bd = dec->in_delay_slot;
    s->pend_reg = (u8)pend_reg;
    s->dirty_mask = jit_ra_dirty_mask(ra);
    s->width = width;
    s->is_signed = is_signed ? 1u : 0u;
    s->arm_reg = (u8)reg;
    s->label_entry = arm_label_create(e);
    s->label_cont = arm_label_create(e);
    s->cyc_refund = (block_cycles > dec->cyc_before)
                        ? (u16)(block_cycles - dec->cyc_before)
                        : 0u;
    s->pc = dec->pc;
    return s;
}

static void emit_abort_tail(Emitter *e, const JitRegAlloc *ra, const ColdStub *s) {
    /* r2 holds non-zero jit_abort (code + 1); abort_badvaddr already saved by helper */
    emit_alu_imm_enc(e, ARM_CC_AL, ARM_OP_SUB, 0u, ARM_R1, ARM_R2, 1u);
    jit_ra_emit_mask_spill(e, ra, s->dirty_mask);
    if (s->pend_reg != 0u) {
        emit_ldr_imm(e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, PSX_OFF_GTE_SCRATCH + 8);
        emit_str_imm(e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, (s32)(PSX_OFF_GPR + (u32)s->pend_reg * 4u));
    }
    if (s->cyc_refund > 0u) {
        emit_add_u32(e, JIT_REG_CYC, JIT_REG_CYC, s->cyc_refund, ARM_R0);
    }
    emit_const(e, ARM_R2, s->pc);
    emit_const(e, ARM_R3, s->bd ? 1u : 0u);
    emit_branch_rx(e, ARM_CC_AL, (const void *)jit_raise_exception_exit);
}

static void emit_all_cold_stubs(Emitter *e, const JitRegAlloc *ra,
                                const ColdStub *stubs, u32 n_stubs) {
    for (u32 i = 0u; i < n_stubs; ++i) {
        const ColdStub *s = &stubs[i];
        arm_label_bind(e, s->label_entry);

        if (s->kind == (u8)STUB_EXC) {
            if (s->badv_in_r1 != 0u) {
                emit_str_imm(e, ARM_CC_AL, ARM_R1, JIT_REG_CPU, PSX_OFF_ABORT_BADVADDR);
            } else if (s->exc_code == PSX_EXC_ADEL || s->exc_code == PSX_EXC_ADES) {
                emit_const(e, ARM_R0, s->badv_const);
                emit_str_imm(e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, PSX_OFF_ABORT_BADVADDR);
            }
            jit_ra_emit_mask_spill(e, ra, s->dirty_mask);
            if (s->pend_reg != 0u) {
                emit_ldr_imm(e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, PSX_OFF_GTE_SCRATCH + 8);
                emit_str_imm(e, ARM_CC_AL, ARM_R0, JIT_REG_CPU,
                             (s32)(PSX_OFF_GPR + (u32)s->pend_reg * 4u));
            }
            if (s->cyc_refund > 0u) {
                emit_add_u32(e, JIT_REG_CYC, JIT_REG_CYC, s->cyc_refund, ARM_R0);
            }
            emit_const(e, ARM_R1, s->exc_code);
            emit_const(e, ARM_R2, s->pc);
            emit_const(e, ARM_R3, (s->bd ? 1u : 0u) | (((u32)s->ce & 3u) << 8));
            emit_branch_rx(e, ARM_CC_AL, (const void *)jit_raise_exception_exit);
        } else if (s->kind == (u8)STUB_MEM_LOAD) {
            const void *fn = (s->width == 1u)
                                 ? (const void *)(uintptr_t)jit_read8_slow_wrap
                                 : ((s->width == 2u)
                                        ? (const void *)(uintptr_t)jit_read16_slow_wrap
                                        : (const void *)(uintptr_t)jit_read32_slow_wrap);
            emit_mov_reg(e, ARM_R0, JIT_REG_CPU);
            emit_call(e, fn, true);
            emit_ldr_imm(e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, PSX_OFF_JIT_ABORT);
            emit_cmp_imm_enc(e, ARM_CC_AL, ARM_R2, 0u);
            u16 lbl_abt = arm_label_create(e);
            emit_b_label(e, ARM_CC_NE, lbl_abt);

            ArmReg dst = (ArmReg)s->arm_reg;
            if (s->width == 1u) {
                if (s->is_signed != 0u) {
                    emit_sxtb(e, ARM_CC_AL, dst, ARM_R0);
                } else {
                    emit_uxtb(e, ARM_CC_AL, dst, ARM_R0);
                }
            } else if (s->width == 2u) {
                if (s->is_signed != 0u) {
                    emit_sxth(e, ARM_CC_AL, dst, ARM_R0, 0u);
                } else {
                    emit_uxth(e, ARM_CC_AL, dst, ARM_R0, 0u);
                }
            } else {
                emit_mov_reg(e, dst, ARM_R0);
            }
            emit_b_label(e, ARM_CC_AL, s->label_cont);

            arm_label_bind(e, lbl_abt);
            emit_abort_tail(e, ra, s);
        } else {
            const void *fn = (s->width == 1u)
                                 ? (const void *)(uintptr_t)jit_write8_slow_wrap
                                 : ((s->width == 2u)
                                        ? (const void *)(uintptr_t)jit_write16_slow_wrap
                                        : (const void *)(uintptr_t)jit_write32_slow_wrap);
            emit_mov_reg(e, ARM_R2, (ArmReg)s->arm_reg);
            emit_mov_reg(e, ARM_R0, JIT_REG_CPU);
            emit_call(e, fn, true);
            emit_ldr_imm(e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, PSX_OFF_JIT_ABORT);
            emit_cmp_imm_enc(e, ARM_CC_AL, ARM_R2, 0u);
            emit_b_label(e, ARM_CC_EQ, s->label_cont);
            emit_abort_tail(e, ra, s);
        }
    }
}

/* Commit or cancel in-flight 1-insn delayed load at end of non-load instruction */
static void commit_prev_load_after_alu(Emitter *e, JitRegAlloc *ra, u32 *pend_reg, u32 wreg) {
    u32 pr = *pend_reg;
    if (pr != 0u) {
        if (pr != wreg) {
            emit_ldr_imm(e, ARM_CC_AL, ARM_R3, JIT_REG_CPU, PSX_OFF_GTE_SCRATCH + 8);
            jit_ra_commit_write(e, ra, pr, ARM_R3);
        }
        *pend_reg = 0u;
    }
}

/* Commit or cancel prior delayed load and stage new delayed load value (`val_reg`) */
static void stage_new_delayed_load(Emitter *e, JitRegAlloc *ra, u32 *pend_reg,
                                   u32 new_rt, ArmReg val_reg) {
    u32 pr = *pend_reg;
    if (pr != 0u && pr != new_rt) {
        ArmReg tmp = (val_reg == ARM_R3) ? ARM_R2 : ARM_R3;
        emit_ldr_imm(e, ARM_CC_AL, tmp, JIT_REG_CPU, PSX_OFF_GTE_SCRATCH + 8);
        jit_ra_commit_write(e, ra, pr, tmp);
    }
    if (new_rt != 0u) {
        emit_str_imm(e, ARM_CC_AL, val_reg, JIT_REG_CPU, PSX_OFF_GTE_SCRATCH + 8);
        *pend_reg = new_rt;
    } else {
        *pend_reg = 0u;
    }
}

/* Emit static control transfer to `target_pc` (with dirty slots already flushed) */
static void emit_static_link(Emitter *e, const PsxSystem *sys,
                             u32 start_pc, const u16 parent_pages[2],
                             bool ends_unlinked, u16 lbl_entry_fast,
                             u16 lbl_exit_stub, bool idle_backedge,
                             u32 target_pc) {
    if (idle_backedge && target_pc == start_pc) {
        emit_const(e, ARM_R0, 1u);
        emit_str_imm(e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, PSX_OFF_IDLE_SKIP);
        emit_const(e, JIT_REG_CYC, 0u);
        emit_ldr_imm(e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, PSX_OFF_SYS);
        emit_ldr_imm(e, ARM_CC_AL, ARM_R3, ARM_R2, (s32)offsetof(PsxSystem, stats.idle_skips));
        emit_alu_imm_enc(e, ARM_CC_AL, ARM_OP_ADD, 0u, ARM_R3, ARM_R3, 1u);
        emit_str_imm(e, ARM_CC_AL, ARM_R3, ARM_R2, (s32)offsetof(PsxSystem, stats.idle_skips));
        emit_b_label(e, ARM_CC_AL, lbl_exit_stub);
        return;
    }

    if (ends_unlinked || ((target_pc & 3u) != 0u)) {
        emit_const(e, ARM_R0, target_pc);
        emit_branch_rx(e, ARM_CC_AL, (const void *)jit_dispatch);
        return;
    }

    if (sys->cfg.jit_link && !jit_is_pinned_pc(sys, target_pc)) {
        if (target_pc == start_pc) {
            emit_b_label(e, ARM_CC_AL, lbl_entry_fast);
            return;
        }
        JitBlock *tb = jit_find_live_block(sys, target_pc);
        if (tb && tb->pc == target_pc && jit_pages_subset(tb, parent_pages)) {
            emit_branch_rx(e, ARM_CC_AL, jit_block_fast_entry(tb));
            return;
        }
    }

    /* Indirect table-slot link (zero post-commit code patching) */
    u32 phys = target_pc & 0x1FFFFFFFu;
    bool kseg_ok = (target_pc < 0x20000000u) ||
                   (target_pc >= 0x80000000u && target_pc < 0xC0000000u);
    bool kuc_risk = (start_pc < 0x80000000u || sys->cfg.jit_check_kuc) &&
                    ((target_pc & 0x80000000u) != 0u);

    if (kseg_ok && !kuc_risk && phys < 0x00200000u && sys->jit.tab_ram) {
        void **slot_addr = &sys->jit.tab_ram[BLOCKTAB_RAM_IDX(target_pc)];
        emit_const(e, ARM_R0, target_pc);
        emit_const(e, ARM_R2, (u32)(uintptr_t)slot_addr);
        emit_ldr_imm(e, ARM_CC_AL, ARM_PC, ARM_R2, 0);
        return;
    }
    if (kseg_ok && !kuc_risk && phys >= PSX_BIOS_PHYS &&
        phys < (PSX_BIOS_PHYS + PSX_BIOS_SIZE) && sys->jit.tab_bios) {
        void **slot_addr = &sys->jit.tab_bios[BLOCKTAB_BIOS_IDX(target_pc)];
        emit_const(e, ARM_R0, target_pc);
        emit_const(e, ARM_R2, (u32)(uintptr_t)slot_addr);
        emit_ldr_imm(e, ARM_CC_AL, ARM_PC, ARM_R2, 0);
        return;
    }

    emit_const(e, ARM_R0, target_pc);
    emit_branch_rx(e, ARM_CC_AL, (const void *)jit_dispatch);
}

/* Compute dynamic effective address `gpr[rs] + simm` into ARM_R1 (clobbers ARM_R2) */
static void emit_calc_addr_r1(Emitter *e, JitRegAlloc *ra, const JitDecodedInsn *dec,
                              u32 rs, u32 simm) {
    if (dec->known_rs) {
        emit_const(e, ARM_R1, dec->kval_rs + simm);
        return;
    }
    ArmReg r_base = jit_ra_get_read(e, ra, rs, ARM_R1);
    if (simm == 0u) {
        emit_mov_reg(e, ARM_R1, r_base);
    } else if ((s32)simm > 0) {
        emit_add_u32(e, ARM_R1, r_base, simm, ARM_R2);
    } else {
        emit_sub_u32(e, ARM_R1, r_base, (u32)(0u - simm), ARM_R2);
    }
}

/* Emit alignment + optional KUc check on dynamic address in ARM_R1 */
static void emit_addr_guards_r1(Emitter *e, const PsxSystem *sys, u32 start_pc,
                                ColdStub *stubs, u32 *n_stubs,
                                const JitRegAlloc *ra, u32 pend_reg,
                                const JitDecodedInsn *dec, u16 block_cycles,
                                u8 width, u8 fault_exc) {
    bool check_align = sys->cfg.jit_check_align && (width > 1u);
    bool check_kuc = (start_pc < 0x80000000u) || sys->cfg.jit_check_kuc;
    if (!check_align && !check_kuc) {
        return;
    }
    u16 lbl_fault = push_exc_stub(e, stubs, n_stubs, ra, pend_reg, dec, block_cycles,
                                  fault_exc, 0u, true, 0u);
    if (check_align) {
        emit_tst_imm_enc(e, ARM_CC_AL, ARM_R1, width - 1u);
        emit_b_label(e, ARM_CC_NE, lbl_fault);
    }
    if (check_kuc) {
        u32 enc80 = 0u;
        (void)arm_encode_imm(0x80000000u, &enc80);
        emit_ldr_imm(e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, PSX_OFF_COP0_SR);
        emit_tst_imm_enc(e, ARM_CC_AL, ARM_R2, 2u);
        emit_tst_imm_enc(e, ARM_CC_NE, ARM_R1, enc80);
        emit_b_label(e, ARM_CC_NE, lbl_fault);
    }
}

/* Emit byte/half/word load into `dst_reg` (typically ARM_R0) */
static void emit_mem_load_to_reg(Emitter *e, const PsxSystem *sys, u32 start_pc,
                                 ColdStub *stubs, u32 *n_stubs,
                                 JitRegAlloc *ra, u32 pend_reg,
                                 const JitDecodedInsn *dec, u16 block_cycles,
                                 u32 rs, u32 simm, u8 width, bool is_signed,
                                 ArmReg dst_reg) {
    JitAddrClass acls = (JitAddrClass)dec->addr_class;
    u32 caddr = dec->const_addr;
    u32 ph = caddr & 0x1FFFFFFFu;

    if (acls == ADDR_CONST_RAM || acls == ADDR_CONST_SCRATCH || acls == ADDR_CONST_BIOS) {
        const u8 *host_ptr = NULL;
        if (acls == ADDR_CONST_RAM) {
            host_ptr = sys->bus.ram + (ph & PSX_RAM_MASK);
        } else if (acls == ADDR_CONST_SCRATCH) {
            host_ptr = sys->bus.scratch_page + (ph & PSX_SCRATCH_MASK);
        } else {
            host_ptr = sys->bus.bios + (ph & PSX_BIOS_MASK);
        }
        emit_const(e, ARM_R2, (u32)(uintptr_t)host_ptr);
        if (width == 4u) {
            emit_ldr_imm(e, ARM_CC_AL, dst_reg, ARM_R2, 0);
        } else if (width == 2u) {
            if (is_signed) {
                emit_ldrsh_imm(e, ARM_CC_AL, dst_reg, ARM_R2, 0u);
            } else {
                emit_ldrh_imm(e, ARM_CC_AL, dst_reg, ARM_R2, 0u);
            }
        } else {
            if (is_signed) {
                emit_ldrsb_imm(e, ARM_CC_AL, dst_reg, ARM_R2, 0u);
            } else {
                emit_ldrb_imm(e, ARM_CC_AL, dst_reg, ARM_R2, 0u);
            }
        }
        return;
    }

    if (acls == ADDR_CONST_SLOW) {
        ColdStub *s = push_mem_stub(e, stubs, n_stubs, STUB_MEM_LOAD, ra, pend_reg,
                                    dec, block_cycles, width, is_signed, dst_reg);
        if (!s) {
            return;
        }
        emit_const(e, ARM_R1, caddr);
        emit_b_label(e, ARM_CC_AL, s->label_entry);
        arm_label_bind(e, s->label_cont);
        return;
    }

    /* Dynamic fastmem load (6 ARMv6K instructions on aligned KSEG0/KSEG1 fastpath) */
    emit_calc_addr_r1(e, ra, dec, rs, simm);
    emit_addr_guards_r1(e, sys, start_pc, stubs, n_stubs, ra, pend_reg,
                        dec, block_cycles, width, PSX_EXC_ADEL);

    ColdStub *s = push_mem_stub(e, stubs, n_stubs, STUB_MEM_LOAD, ra, pend_reg,
                                dec, block_cycles, width, is_signed, dst_reg);
    if (!s) {
        return;
    }

    emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R2, ARM_R1, ARM_SH_LSR, 12u);
    emit_ldr_reg_lsl(e, ARM_CC_AL, ARM_R2, JIT_REG_FMR, ARM_R2, 2u);
    emit_cmp_imm_enc(e, ARM_CC_AL, ARM_R2, 0u);
    if (width == 4u) {
        emit_ldr_reg_lsl(e, ARM_CC_NE, dst_reg, ARM_R2, ARM_R1, 0u);
    } else if (width == 2u) {
        if (is_signed) {
            emit_ldrsh_reg(e, ARM_CC_NE, dst_reg, ARM_R2, ARM_R1);
        } else {
            emit_ldrh_reg(e, ARM_CC_NE, dst_reg, ARM_R2, ARM_R1);
        }
    } else {
        if (is_signed) {
            emit_ldrsb_reg(e, ARM_CC_NE, dst_reg, ARM_R2, ARM_R1);
        } else {
            emit_ldrb_reg(e, ARM_CC_NE, dst_reg, ARM_R2, ARM_R1);
        }
    }
    emit_b_label(e, ARM_CC_EQ, s->label_entry);
    arm_label_bind(e, s->label_cont);
}

/* Emit byte/half/word store from `val_reg` (must NOT be ARM_R1, ARM_R2, ARM_R3) */
static void emit_mem_store_from_reg(Emitter *e, const PsxSystem *sys, u32 start_pc,
                                    ColdStub *stubs, u32 *n_stubs,
                                    JitRegAlloc *ra, u32 pend_reg,
                                    const JitDecodedInsn *dec, u16 block_cycles,
                                    u32 rs, u32 simm, u8 width, ArmReg val_reg) {
    JitAddrClass acls = (JitAddrClass)dec->addr_class;
    u32 caddr = dec->const_addr;

    if (acls == ADDR_CONST_RAM || acls == ADDR_CONST_SCRATCH) {
        ColdStub *s = push_mem_stub(e, stubs, n_stubs, STUB_MEM_STORE, ra, pend_reg,
                                    dec, block_cycles, width, false, val_reg);
        if (!s) {
            return;
        }
        u32 vp = caddr >> 12;
        emit_const(e, ARM_R1, caddr);
        emit_ldr_imm(e, ARM_CC_AL, ARM_R3, JIT_REG_CPU, PSX_OFF_FASTMEM_W);
        if ((vp * 4u) < 4096u) {
            emit_ldr_imm(e, ARM_CC_AL, ARM_R2, ARM_R3, (s32)(vp * 4u));
        } else {
            emit_const(e, ARM_R2, vp * 4u);
            emit_ldr_reg_lsl(e, ARM_CC_AL, ARM_R2, ARM_R3, ARM_R2, 0u);
        }
        emit_cmp_imm_enc(e, ARM_CC_AL, ARM_R2, 0u);
        if (width == 4u) {
            emit_str_reg_lsl(e, ARM_CC_NE, val_reg, ARM_R2, ARM_R1, 0u);
        } else if (width == 2u) {
            emit_strh_reg(e, ARM_CC_NE, val_reg, ARM_R2, ARM_R1);
        } else {
            emit_strb_reg(e, ARM_CC_NE, val_reg, ARM_R2, ARM_R1);
        }
        emit_b_label(e, ARM_CC_EQ, s->label_entry);
        arm_label_bind(e, s->label_cont);
        return;
    }

    if (acls == ADDR_CONST_SLOW || acls == ADDR_CONST_BIOS) {
        ColdStub *s = push_mem_stub(e, stubs, n_stubs, STUB_MEM_STORE, ra, pend_reg,
                                    dec, block_cycles, width, false, val_reg);
        if (!s) {
            return;
        }
        emit_const(e, ARM_R1, caddr);
        emit_b_label(e, ARM_CC_AL, s->label_entry);
        arm_label_bind(e, s->label_cont);
        return;
    }

    /* Dynamic fastmem store (checks fm_w for SMC code pages and SR.IsC) */
    emit_calc_addr_r1(e, ra, dec, rs, simm);
    emit_addr_guards_r1(e, sys, start_pc, stubs, n_stubs, ra, pend_reg,
                        dec, block_cycles, width, PSX_EXC_ADES);

    ColdStub *s = push_mem_stub(e, stubs, n_stubs, STUB_MEM_STORE, ra, pend_reg,
                                dec, block_cycles, width, false, val_reg);
    if (!s) {
        return;
    }

    emit_ldr_imm(e, ARM_CC_AL, ARM_R3, JIT_REG_CPU, PSX_OFF_FASTMEM_W);
    emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R2, ARM_R1, ARM_SH_LSR, 12u);
    emit_ldr_reg_lsl(e, ARM_CC_AL, ARM_R2, ARM_R3, ARM_R2, 2u);
    emit_cmp_imm_enc(e, ARM_CC_AL, ARM_R2, 0u);
    if (width == 4u) {
        emit_str_reg_lsl(e, ARM_CC_NE, val_reg, ARM_R2, ARM_R1, 0u);
    } else if (width == 2u) {
        emit_strh_reg(e, ARM_CC_NE, val_reg, ARM_R2, ARM_R1);
    } else {
        emit_strb_reg(e, ARM_CC_NE, val_reg, ARM_R2, ARM_R1);
    }
    emit_b_label(e, ARM_CC_EQ, s->label_entry);
    arm_label_bind(e, s->label_cont);
}

/* ============================================================================
 * Core Block Compiler + Same-Page Eager Linker
 * ============================================================================ */

static JitBlock *jit_compile_internal(PsxSystem *sys, u32 start_pc, u32 depth, u32 *budget);

static void jit_try_eager_target(PsxSystem *sys, u32 start_pc,
                                 const u16 parent_pages[2], u32 target_pc,
                                 u32 depth, u32 *budget) {
    if (!sys->cfg.jit_link || depth >= JIT_EAGER_MAX_DEPTH || *budget == 0u) {
        return;
    }
    if (target_pc == start_pc || (target_pc & 3u) != 0u || jit_is_pinned_pc(sys, target_pc)) {
        return;
    }
    if (jit_find_live_block(sys, target_pc) != NULL) {
        return;
    }
    if (jit_pc_is_ram(sys, target_pc)) {
        u16 tpg = (u16)((target_pc & PSX_RAM_MASK) >> 12);
        if (tpg != parent_pages[0] && tpg != parent_pages[1]) {
            return;
        }
    } else if (!jit_pc_is_bios(target_pc)) {
        return;
    }

    JitState *j = &sys->jit;
    if (j->n_blocks + 1u >= j->max_blocks ||
        (size_t)(j->code_end - j->code_ptr) < (2u * JIT_MAX_BLOCK_BYTES)) {
        return;
    }

    (*budget)--;
    (void)jit_compile_internal(sys, target_pc, depth + 1u, budget);
}

typedef enum BrTermKind {
    BR_NONE = 0,
    BR_STATIC_UNCOND,
    BR_STATIC_COND,
    BR_INDIRECT_REG
} BrTermKind;

static JitBlock *jit_compile_internal(PsxSystem *sys, u32 start_pc, u32 depth, u32 *budget) {
    JitState *j = &sys->jit;
    if (!j->blocks || !j->mem.rw || !j->mem.rx || j->mem.mode == JIT_MEM_NONE) {
        return NULL;
    }
    if (jit_is_pinned_pc(sys, start_pc)) {
        return NULL;
    }
    bool is_ram = jit_pc_is_ram(sys, start_pc);
    bool is_bios = !is_ram && jit_pc_is_bios(start_pc);
    if (!is_ram && !is_bios) {
        return NULL;
    }
    if (j->n_blocks >= j->max_blocks ||
        (size_t)(j->code_end - j->code_ptr) < JIT_MAX_BLOCK_BYTES) {
        return NULL;
    }

    JitBlock *existing = jit_find_live_block(sys, start_pc);
    if (existing && existing->pc == start_pc) {
        return existing;
    }

    JitScanResult scan;
    jit_scan_block(sys, start_pc, &scan);
    if (scan.ends_step_one) {
        return NULL;
    }

    /* Eagerly compile intra-page static branch targets BEFORE emitting this block
     * so forward if/else diamonds and helper calls direct-link in 1 instruction! */
    if (scan.has_branch && !scan.ends_unlinked &&
        (scan.block_flags & JB_ENDS_LOAD_DS) == 0u && scan.n_insns >= 2u) {
        const JitDecodedInsn *br = &scan.insns[scan.n_insns - 2u];
        u32 br_op = br->raw >> 26;
        u32 simm = (u32)(s32)(s16)(br->raw & 0xFFFFu);
        if (br_op == 0x02u || br_op == 0x03u) {
            u32 tgt = ((br->pc + 4u) & 0xF0000000u) | ((br->raw & 0x03FFFFFFu) << 2);
            jit_try_eager_target(sys, start_pc, scan.ram_pages, tgt, depth, budget);
        } else if (br_op == 0x01u || (br_op >= 0x04u && br_op <= 0x07u)) {
            u32 tgt_t = (br->pc + 4u) + (simm << 2);
            u32 tgt_f = br->pc + 8u;
            jit_try_eager_target(sys, start_pc, scan.ram_pages, tgt_t, depth, budget);
            jit_try_eager_target(sys, start_pc, scan.ram_pages, tgt_f, depth, budget);
        }
    } else if (!scan.has_branch && !scan.ends_unlinked) {
        u32 last_raw = scan.insns[scan.n_insns - 1u].raw;
        bool is_trap = ((last_raw >> 26) == 0u) &&
                       (((last_raw & 63u) == 0x0Cu) || ((last_raw & 63u) == 0x0Du));
        if (!is_trap) {
            u32 ft = start_pc + scan.n_insns * 4u;
            jit_try_eager_target(sys, start_pc, scan.ram_pages, ft, depth, budget);
        }
    }

    /* Re-verify space after any eager child compilations */
    if (j->n_blocks >= j->max_blocks ||
        (size_t)(j->code_end - j->code_ptr) < JIT_MAX_BLOCK_BYTES) {
        return NULL;
    }
    existing = jit_find_live_block(sys, start_pc);
    if (existing && existing->pc == start_pc) {
        return existing;
    }

    u8 *rw_begin = j->code_ptr;
    ptrdiff_t rw_to_rx = j->mem.rx - j->mem.rw;

    Emitter e;
    arm_emit_reset_dual(&e, rw_begin, rw_to_rx, JIT_MAX_BLOCK_BYTES);

    JitRegAlloc ra;
    jit_ra_init(&ra, &scan);

    ColdStub stubs[JIT_MAX_COLD_STUBS];
    u32 n_stubs = 0u;

    u16 lbl_seg_mismatch = arm_label_create(&e);
    u16 lbl_exit_stub = arm_label_create(&e);

    /* 1. Segment-tag guard at table-dispatched entry (`entry_tab`) */
    u32 top3 = start_pc & 0xE0000000u;
    u32 enc_e0 = 0u;
    (void)arm_encode_imm(0xE0000000u, &enc_e0);
    if (top3 == 0u) {
        emit_tst_imm_enc(&e, ARM_CC_AL, ARM_R0, enc_e0);
        emit_b_label(&e, ARM_CC_NE, lbl_seg_mismatch);
    } else {
        u32 enc_top = 0u;
        (void)arm_encode_imm(top3, &enc_top);
        emit_alu_imm_enc(&e, ARM_CC_AL, ARM_OP_EOR, 0u, ARM_R2, ARM_R0, enc_top);
        emit_tst_imm_enc(&e, ARM_CC_AL, ARM_R2, enc_e0);
        emit_b_label(&e, ARM_CC_NE, lbl_seg_mismatch);
    }

    /* 2. Fast linked/IBTC entry (`entry_fast`): cycle check BEFORE subtracting! */
    u16 fast_off_bytes = (u16)(arm_emit_offset_words(&e) * 4u);
    u16 lbl_entry_fast = arm_label_create(&e);
    arm_label_bind(&e, lbl_entry_fast);

    emit_cmp_imm_enc(&e, ARM_CC_AL, JIT_REG_CYC, 0u);
    emit_b_label(&e, ARM_CC_LE, lbl_exit_stub);

    /* Optional arithmetic fold for `loop: addiu rX, rX, -1; bne rX, $0, loop; nop` */
    if (scan.counted_loop && sys->cfg.jit_idle_skip && scan.total_cycles > 0u) {
        u16 lbl_no_fold = arm_label_create(&e);
        u32 rX = scan.counted_reg;
        ArmReg r_cnt = jit_ra_get_read(&e, &ra, rX, ARM_R0);
        emit_cmp_imm_enc(&e, ARM_CC_AL, r_cnt, 0u);
        emit_b_label(&e, ARM_CC_EQ, lbl_no_fold);
        emit_shift_imm(&e, ARM_CC_AL, 1u /*S*/, ARM_R2, r_cnt, ARM_SH_LSR, 16u);
        emit_b_label(&e, ARM_CC_NE, lbl_no_fold);
        emit_const(&e, ARM_R2, scan.total_cycles);
        emit_mul(&e, ARM_CC_AL, ARM_R3, r_cnt, ARM_R2);
        emit_cmp_reg(&e, ARM_CC_AL, ARM_R3, JIT_REG_CYC);
        emit_b_label(&e, ARM_CC_HI, lbl_no_fold);

        /* Fold all `rX` iterations immediately -> rX = 0, exit to start_pc + 12 */
        emit_alu_reg(&e, ARM_CC_AL, ARM_OP_SUB, 0u, JIT_REG_CYC, JIT_REG_CYC, ARM_R3);
        emit_const(&e, ARM_R0, 0u);
        emit_str_imm(&e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, (s32)(PSX_OFF_GPR + rX * 4u));
        s8 cnt_slot = ra.mips_to_slot[rX];
        if (cnt_slot >= 0) {
            ra.state[(u32)cnt_slot] = SLOT_UNLOADED;
        }
        emit_ldr_imm(&e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, PSX_OFF_SYS);
        emit_ldr_imm(&e, ARM_CC_AL, ARM_R3, ARM_R2, (s32)offsetof(PsxSystem, stats.idle_skips));
        emit_alu_imm_enc(&e, ARM_CC_AL, ARM_OP_ADD, 0u, ARM_R3, ARM_R3, 1u);
        emit_str_imm(&e, ARM_CC_AL, ARM_R3, ARM_R2, (s32)offsetof(PsxSystem, stats.idle_skips));

        emit_static_link(&e, sys, start_pc, scan.ram_pages, false,
                         lbl_entry_fast, lbl_exit_stub, false, start_pc + 12u);
        arm_label_bind(&e, lbl_no_fold);
    }

    emit_sub_u32(&e, JIT_REG_CYC, JIT_REG_CYC, scan.total_cycles, ARM_R2);

    /* 3. Emit MIPS instruction stream */
    u32 pend_reg = 0u;
    bool cop2_checked = false;
    bool block_always_traps = false;

    BrTermKind br_kind = BR_NONE;
    u32 br_target_t = 0u;
    u32 br_target_f = 0u;

    for (u32 i = 0u; i < scan.n_insns; ++i) {
        const JitDecodedInsn *dec = &scan.insns[i];
        u32 insn = dec->raw;
        u32 op = insn >> 26;
        u32 rs = (insn >> 21) & 31u;
        u32 rt = (insn >> 16) & 31u;
        u32 rd = (insn >> 11) & 31u;
        u32 sh = (insn >> 6) & 31u;
        u32 fn = insn & 63u;
        u32 imm16 = insn & 0xFFFFu;
        u32 simm = (u32)(s32)(s16)imm16;

        switch (op) {
            case 0x00u: { /* SPECIAL */
                switch (fn) {
                    case 0x00u: /* SLL / NOP */
                        if (insn == 0u || rd == 0u) {
                            commit_prev_load_after_alu(&e, &ra, &pend_reg, 0u);
                        } else if (dec->known_rt) {
                            commit_prev_load_after_alu(&e, &ra, &pend_reg, rd);
                            ArmReg dst = jit_ra_get_write_buf(&ra, rd, ARM_R0);
                            emit_const(&e, dst, dec->kval_rt << sh);
                            jit_ra_commit_write(&e, &ra, rd, dst);
                        } else {
                            ArmReg r_rt = jit_ra_get_read(&e, &ra, rt, ARM_R1);
                            emit_shift_imm(&e, ARM_CC_AL, 0u, ARM_R0, r_rt, ARM_SH_LSL, sh);
                            commit_prev_load_after_alu(&e, &ra, &pend_reg, rd);
                            jit_ra_commit_write(&e, &ra, rd, ARM_R0);
                        }
                        break;

                    case 0x02u: /* SRL */
                    case 0x03u: { /* SRA */
                        ArmShift ash = (fn == 0x02u) ? ARM_SH_LSR : ARM_SH_ASR;
                        if (rd == 0u) {
                            commit_prev_load_after_alu(&e, &ra, &pend_reg, 0u);
                        } else if (dec->known_rt) {
                            u32 v = (fn == 0x02u) ? (dec->kval_rt >> sh)
                                                  : (u32)((s32)dec->kval_rt >> sh);
                            commit_prev_load_after_alu(&e, &ra, &pend_reg, rd);
                            ArmReg dst = jit_ra_get_write_buf(&ra, rd, ARM_R0);
                            emit_const(&e, dst, v);
                            jit_ra_commit_write(&e, &ra, rd, dst);
                        } else {
                            ArmReg r_rt = jit_ra_get_read(&e, &ra, rt, ARM_R1);
                            emit_shift_imm(&e, ARM_CC_AL, 0u, ARM_R0, r_rt, ash, sh);
                            commit_prev_load_after_alu(&e, &ra, &pend_reg, rd);
                            jit_ra_commit_write(&e, &ra, rd, ARM_R0);
                        }
                        break;
                    }

                    case 0x04u: /* SLLV */
                    case 0x06u: /* SRLV */
                    case 0x07u: { /* SRAV */
                        ArmShift ash = (fn == 0x04u) ? ARM_SH_LSL
                                                     : ((fn == 0x06u) ? ARM_SH_LSR : ARM_SH_ASR);
                        if (rd == 0u) {
                            commit_prev_load_after_alu(&e, &ra, &pend_reg, 0u);
                        } else if (dec->known_rt && dec->known_rs) {
                            u32 s5 = dec->kval_rs & 31u;
                            u32 v = (fn == 0x04u)
                                        ? (dec->kval_rt << s5)
                                        : ((fn == 0x06u) ? (dec->kval_rt >> s5)
                                                         : (u32)((s32)dec->kval_rt >> s5));
                            commit_prev_load_after_alu(&e, &ra, &pend_reg, rd);
                            ArmReg dst = jit_ra_get_write_buf(&ra, rd, ARM_R0);
                            emit_const(&e, dst, v);
                            jit_ra_commit_write(&e, &ra, rd, dst);
                        } else {
                            ArmReg r_rt = jit_ra_get_read(&e, &ra, rt, ARM_R1);
                            ArmReg r_rs = jit_ra_get_read(&e, &ra, rs, ARM_R2);
                            emit_alu_imm_enc(&e, ARM_CC_AL, ARM_OP_AND, 0u, ARM_R2, r_rs, 31u);
                            emit_shift_reg(&e, ARM_R0, r_rt, ash, ARM_R2);
                            commit_prev_load_after_alu(&e, &ra, &pend_reg, rd);
                            jit_ra_commit_write(&e, &ra, rd, ARM_R0);
                        }
                        break;
                    }

                    case 0x08u: /* JR */
                    case 0x09u: { /* JALR (read rs BEFORE writing rd = pc + 8!) */
                        ArmReg r_rs = jit_ra_get_read(&e, &ra, rs, ARM_R0);
                        emit_str_imm(&e, ARM_CC_AL, r_rs, JIT_REG_CPU, PSX_OFF_GTE_SCRATCH + 0);
                        if (fn == 0x09u) {
                            commit_prev_load_after_alu(&e, &ra, &pend_reg, rd);
                            if (rd != 0u) {
                                ArmReg dst = jit_ra_get_write_buf(&ra, rd, ARM_R0);
                                emit_const(&e, dst, dec->pc + 8u);
                                jit_ra_commit_write(&e, &ra, rd, dst);
                            }
                        } else {
                            commit_prev_load_after_alu(&e, &ra, &pend_reg, 0u);
                        }
                        br_kind = BR_INDIRECT_REG;
                        break;
                    }

                    case 0x0Cu: /* SYSCALL */
                    case 0x0Du: { /* BREAK */
                        u8 code = (fn == 0x0Cu) ? PSX_EXC_SYS : PSX_EXC_BP;
                        u16 lbl = push_exc_stub(&e, stubs, &n_stubs, &ra, pend_reg,
                                                dec, scan.total_cycles, code, 0u, false, 0u);
                        emit_b_label(&e, ARM_CC_AL, lbl);
                        block_always_traps = true;
                        break;
                    }

                    case 0x10u: /* MFHI */
                    case 0x12u: { /* MFLO */
                        s32 off = (fn == 0x10u) ? PSX_OFF_HI : PSX_OFF_LO;
                        commit_prev_load_after_alu(&e, &ra, &pend_reg, rd);
                        if (rd != 0u) {
                            ArmReg dst = jit_ra_get_write_buf(&ra, rd, ARM_R0);
                            emit_ldr_imm(&e, ARM_CC_AL, dst, JIT_REG_CPU, off);
                            jit_ra_commit_write(&e, &ra, rd, dst);
                        }
                        break;
                    }

                    case 0x11u: /* MTHI */
                    case 0x13u: { /* MTLO */
                        s32 off = (fn == 0x11u) ? PSX_OFF_HI : PSX_OFF_LO;
                        ArmReg r_rs = jit_ra_get_read(&e, &ra, rs, ARM_R0);
                        emit_str_imm(&e, ARM_CC_AL, r_rs, JIT_REG_CPU, off);
                        commit_prev_load_after_alu(&e, &ra, &pend_reg, 0u);
                        break;
                    }

                    case 0x18u: /* MULT */
                    case 0x19u: { /* MULTU */
                        ArmReg r_rs = jit_ra_get_read(&e, &ra, rs, ARM_R2);
                        ArmReg r_rt = jit_ra_get_read(&e, &ra, rt, ARM_R3);
                        /* RdLo=R0, RdHi=R1; neither equals R2..LR so Rd!=Rm always holds */
                        if (fn == 0x18u) {
                            emit_smull(&e, ARM_CC_AL, ARM_R0, ARM_R1, r_rs, r_rt);
                        } else {
                            emit_umull(&e, ARM_CC_AL, ARM_R0, ARM_R1, r_rs, r_rt);
                        }
                        emit_str_imm(&e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, PSX_OFF_LO);
                        emit_str_imm(&e, ARM_CC_AL, ARM_R1, JIT_REG_CPU, PSX_OFF_HI);
                        commit_prev_load_after_alu(&e, &ra, &pend_reg, 0u);
                        break;
                    }

                    case 0x1Au: /* DIV */
                    case 0x1Bu: { /* DIVU */
                        ArmReg r_rs = jit_ra_get_read(&e, &ra, rs, ARM_R1);
                        ArmReg r_rt = jit_ra_get_read(&e, &ra, rt, ARM_R2);
                        emit_mov_reg(&e, ARM_R1, r_rs);
                        emit_mov_reg(&e, ARM_R2, r_rt);
                        emit_mov_reg(&e, ARM_R0, JIT_REG_CPU);
                        emit_call(&e,
                                  (fn == 0x1Au) ? (const void *)(uintptr_t)jit_div_helper
                                                : (const void *)(uintptr_t)jit_divu_helper,
                                  false);
                        commit_prev_load_after_alu(&e, &ra, &pend_reg, 0u);
                        break;
                    }

                    case 0x20u: /* ADD (traps on signed overflow) */
                    case 0x22u: { /* SUB (traps on signed overflow) */
                        ArmReg r_rs = jit_ra_get_read(&e, &ra, rs, ARM_R1);
                        ArmReg r_rt = jit_ra_get_read(&e, &ra, rt, ARM_R2);
                        u16 lbl_ov = push_exc_stub(&e, stubs, &n_stubs, &ra, pend_reg,
                                                   dec, scan.total_cycles, PSX_EXC_OV,
                                                   0u, false, 0u);
                        emit_alu_reg(&e, ARM_CC_AL,
                                     (fn == 0x20u) ? ARM_OP_ADD : ARM_OP_SUB,
                                     1u /*S*/, ARM_R0, r_rs, r_rt);
                        emit_b_label(&e, ARM_CC_VS, lbl_ov);
                        commit_prev_load_after_alu(&e, &ra, &pend_reg, rd);
                        jit_ra_commit_write(&e, &ra, rd, ARM_R0);
                        break;
                    }

                    case 0x21u: /* ADDU */
                    case 0x23u: /* SUBU */
                    case 0x24u: /* AND */
                    case 0x25u: /* OR */
                    case 0x26u: /* XOR */
                    case 0x27u: { /* NOR */
                        if (rd == 0u) {
                            commit_prev_load_after_alu(&e, &ra, &pend_reg, 0u);
                            break;
                        }
                        if (dec->known_rs && dec->known_rt) {
                            u32 a = dec->kval_rs, b = dec->kval_rt, v = 0u;
                            switch (fn) {
                                case 0x21u: v = a + b; break;
                                case 0x23u: v = a - b; break;
                                case 0x24u: v = a & b; break;
                                case 0x25u: v = a | b; break;
                                case 0x26u: v = a ^ b; break;
                                default:    v = ~(a | b); break;
                            }
                            commit_prev_load_after_alu(&e, &ra, &pend_reg, rd);
                            ArmReg dst = jit_ra_get_write_buf(&ra, rd, ARM_R0);
                            emit_const(&e, dst, v);
                            jit_ra_commit_write(&e, &ra, rd, dst);
                            break;
                        }
                        ArmReg r_rs = jit_ra_get_read(&e, &ra, rs, ARM_R1);
                        ArmReg r_rt = jit_ra_get_read(&e, &ra, rt, ARM_R2);
                        enum ArmDpOp aop = ARM_OP_ADD;
                        switch (fn) {
                            case 0x21u: aop = ARM_OP_ADD; break;
                            case 0x23u: aop = ARM_OP_SUB; break;
                            case 0x24u: aop = ARM_OP_AND; break;
                            case 0x25u: aop = ARM_OP_ORR; break;
                            default:    aop = ARM_OP_EOR; break;
                        }
                        if (fn == 0x27u) {
                            emit_alu_reg(&e, ARM_CC_AL, ARM_OP_ORR, 0u, ARM_R0, r_rs, r_rt);
                            emit_mvn_reg(&e, ARM_R0, ARM_R0);
                        } else {
                            emit_alu_reg(&e, ARM_CC_AL, aop, 0u, ARM_R0, r_rs, r_rt);
                        }
                        commit_prev_load_after_alu(&e, &ra, &pend_reg, rd);
                        jit_ra_commit_write(&e, &ra, rd, ARM_R0);
                        break;
                    }

                    case 0x2Au: /* SLT */
                    case 0x2Bu: { /* SLTU */
                        if (rd == 0u) {
                            commit_prev_load_after_alu(&e, &ra, &pend_reg, 0u);
                            break;
                        }
                        ArmReg r_rs = jit_ra_get_read(&e, &ra, rs, ARM_R1);
                        ArmReg r_rt = jit_ra_get_read(&e, &ra, rt, ARM_R2);
                        emit_const(&e, ARM_R0, 0u);
                        emit_cmp_reg(&e, ARM_CC_AL, r_rs, r_rt);
                        emit_alu_imm_enc(&e, (fn == 0x2Au) ? ARM_CC_LT : ARM_CC_LO,
                                         ARM_OP_MOV, 0u, ARM_R0, 0u, 1u);
                        commit_prev_load_after_alu(&e, &ra, &pend_reg, rd);
                        jit_ra_commit_write(&e, &ra, rd, ARM_R0);
                        break;
                    }

                    default: { /* Reserved SPECIAL encoding -> RI */
                        u16 lbl = push_exc_stub(&e, stubs, &n_stubs, &ra, pend_reg,
                                                dec, scan.total_cycles, PSX_EXC_RI,
                                                0u, false, 0u);
                        emit_b_label(&e, ARM_CC_AL, lbl);
                        block_always_traps = true;
                        break;
                    }
                }
                break;
            }

            case 0x01u: { /* REGIMM: BLTZ / BGEZ / BLTZAL / BGEZAL */
                bool is_bgez = (rt & 1u) != 0u;
                bool link_ra = (rt & 0x1Eu) == 0x10u;
                u32 tgt_t = (dec->pc + 4u) + (simm << 2);
                u32 tgt_f = dec->pc + 8u;

                /* Evaluate condition on OLD rs BEFORE linking $ra (critical if rs == 31!) */
                ArmReg r_rs = jit_ra_get_read(&e, &ra, rs, ARM_R1);
                emit_cmp_imm_enc(&e, ARM_CC_AL, r_rs, 0u);
                ArmCond cc = is_bgez ? ARM_CC_GE : ARM_CC_LT;
                emit_const(&e, ARM_R0, 0u);
                emit_alu_imm_enc(&e, cc, ARM_OP_MOV, 0u, ARM_R0, 0u, 1u);
                emit_str_imm(&e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, PSX_OFF_GTE_SCRATCH + 4);

                emit_const(&e, ARM_R2, tgt_f);
                emit_const_cc(&e, cc, ARM_R2, tgt_t);
                emit_str_imm(&e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, PSX_OFF_GTE_SCRATCH + 0);

                if (link_ra) {
                    commit_prev_load_after_alu(&e, &ra, &pend_reg, 31u);
                    ArmReg dst = jit_ra_get_write_buf(&ra, 31u, ARM_R0);
                    emit_const(&e, dst, dec->pc + 8u);
                    jit_ra_commit_write(&e, &ra, 31u, dst);
                } else {
                    commit_prev_load_after_alu(&e, &ra, &pend_reg, 0u);
                }
                br_kind = BR_STATIC_COND;
                br_target_t = tgt_t;
                br_target_f = tgt_f;
                break;
            }

            case 0x02u: /* J */
            case 0x03u: { /* JAL */
                u32 tgt = ((dec->pc + 4u) & 0xF0000000u) | ((insn & 0x03FFFFFFu) << 2);
                emit_const(&e, ARM_R0, tgt);
                emit_str_imm(&e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, PSX_OFF_GTE_SCRATCH + 0);
                if (op == 0x03u) {
                    commit_prev_load_after_alu(&e, &ra, &pend_reg, 31u);
                    ArmReg dst = jit_ra_get_write_buf(&ra, 31u, ARM_R0);
                    emit_const(&e, dst, dec->pc + 8u);
                    jit_ra_commit_write(&e, &ra, 31u, dst);
                } else {
                    commit_prev_load_after_alu(&e, &ra, &pend_reg, 0u);
                }
                br_kind = BR_STATIC_UNCOND;
                br_target_t = tgt;
                break;
            }

            case 0x04u: /* BEQ */
            case 0x05u: /* BNE */
            case 0x06u: /* BLEZ */
            case 0x07u: { /* BGTZ */
                u32 tgt_t = (dec->pc + 4u) + (simm << 2);
                u32 tgt_f = dec->pc + 8u;

                if (op == 0x04u && rs == rt) {
                    /* Unconditional branch idiom `beq $r, $r, target` */
                    emit_const(&e, ARM_R0, tgt_t);
                    emit_str_imm(&e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, PSX_OFF_GTE_SCRATCH + 0);
                    commit_prev_load_after_alu(&e, &ra, &pend_reg, 0u);
                    br_kind = BR_STATIC_UNCOND;
                    br_target_t = tgt_t;
                    break;
                }

                ArmCond cc = ARM_CC_EQ;
                if (op == 0x04u || op == 0x05u) {
                    ArmReg r_rs = jit_ra_get_read(&e, &ra, rs, ARM_R1);
                    if (rt == 0u) {
                        emit_cmp_imm_enc(&e, ARM_CC_AL, r_rs, 0u);
                    } else {
                        ArmReg r_rt = jit_ra_get_read(&e, &ra, rt, ARM_R2);
                        emit_cmp_reg(&e, ARM_CC_AL, r_rs, r_rt);
                    }
                    cc = (op == 0x04u) ? ARM_CC_EQ : ARM_CC_NE;
                } else {
                    ArmReg r_rs = jit_ra_get_read(&e, &ra, rs, ARM_R1);
                    emit_cmp_imm_enc(&e, ARM_CC_AL, r_rs, 0u);
                    cc = (op == 0x06u) ? ARM_CC_LE : ARM_CC_GT;
                }

                emit_const(&e, ARM_R0, 0u);
                emit_alu_imm_enc(&e, cc, ARM_OP_MOV, 0u, ARM_R0, 0u, 1u);
                emit_str_imm(&e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, PSX_OFF_GTE_SCRATCH + 4);

                emit_const(&e, ARM_R2, tgt_f);
                emit_const_cc(&e, cc, ARM_R2, tgt_t);
                emit_str_imm(&e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, PSX_OFF_GTE_SCRATCH + 0);

                commit_prev_load_after_alu(&e, &ra, &pend_reg, 0u);
                br_kind = BR_STATIC_COND;
                br_target_t = tgt_t;
                br_target_f = tgt_f;
                break;
            }

            case 0x08u: { /* ADDI (traps on signed overflow, even when rt == 0!) */
                ArmReg r_rs = jit_ra_get_read(&e, &ra, rs, ARM_R1);
                u16 lbl_ov = push_exc_stub(&e, stubs, &n_stubs, &ra, pend_reg,
                                           dec, scan.total_cycles, PSX_EXC_OV,
                                           0u, false, 0u);
                u32 enc_imm = 0u;
                if (arm_encode_imm(simm, &enc_imm)) {
                    emit_alu_imm_enc(&e, ARM_CC_AL, ARM_OP_ADD, 1u /*S*/, ARM_R0, r_rs, enc_imm);
                } else if (simm != 0x80000000u && arm_encode_imm(0u - simm, &enc_imm)) {
                    /* Note: x + (-k) has identical signed overflow V flag to x - k for k != -2^31 */
                    emit_alu_imm_enc(&e, ARM_CC_AL, ARM_OP_SUB, 1u /*S*/, ARM_R0, r_rs, enc_imm);
                } else {
                    emit_const(&e, ARM_R2, simm);
                    emit_alu_reg(&e, ARM_CC_AL, ARM_OP_ADD, 1u /*S*/, ARM_R0, r_rs, ARM_R2);
                }
                emit_b_label(&e, ARM_CC_VS, lbl_ov);
                commit_prev_load_after_alu(&e, &ra, &pend_reg, rt);
                jit_ra_commit_write(&e, &ra, rt, ARM_R0);
                break;
            }

            case 0x09u: { /* ADDIU */
                if (rt == 0u) {
                    commit_prev_load_after_alu(&e, &ra, &pend_reg, 0u);
                } else if (dec->known_rs) {
                    commit_prev_load_after_alu(&e, &ra, &pend_reg, rt);
                    ArmReg dst = jit_ra_get_write_buf(&ra, rt, ARM_R0);
                    emit_const(&e, dst, dec->kval_rs + simm);
                    jit_ra_commit_write(&e, &ra, rt, dst);
                } else {
                    ArmReg r_rs = jit_ra_get_read(&e, &ra, rs, ARM_R1);
                    if ((s32)simm >= 0) {
                        emit_add_u32(&e, ARM_R0, r_rs, simm, ARM_R2);
                    } else {
                        emit_sub_u32(&e, ARM_R0, r_rs, (u32)(0u - simm), ARM_R2);
                    }
                    commit_prev_load_after_alu(&e, &ra, &pend_reg, rt);
                    jit_ra_commit_write(&e, &ra, rt, ARM_R0);
                }
                break;
            }

            case 0x0Au: /* SLTI */
            case 0x0Bu: { /* SLTIU */
                if (rt == 0u) {
                    commit_prev_load_after_alu(&e, &ra, &pend_reg, 0u);
                    break;
                }
                ArmReg r_rs = jit_ra_get_read(&e, &ra, rs, ARM_R1);
                emit_const(&e, ARM_R0, 0u);
                u32 enc_imm = 0u;
                if (arm_encode_imm(simm, &enc_imm)) {
                    emit_cmp_imm_enc(&e, ARM_CC_AL, r_rs, enc_imm);
                } else if (op == 0x0Bu && arm_encode_imm(0u - simm, &enc_imm)) {
                    /* Note: CMN sets C=1 iff rn + imm >= 2^32, NOT safe for signed LT if V flips */
                    emit_const(&e, ARM_R2, simm);
                    emit_cmp_reg(&e, ARM_CC_AL, r_rs, ARM_R2);
                } else {
                    emit_const(&e, ARM_R2, simm);
                    emit_cmp_reg(&e, ARM_CC_AL, r_rs, ARM_R2);
                }
                emit_alu_imm_enc(&e, (op == 0x0Au) ? ARM_CC_LT : ARM_CC_LO,
                                 ARM_OP_MOV, 0u, ARM_R0, 0u, 1u);
                commit_prev_load_after_alu(&e, &ra, &pend_reg, rt);
                jit_ra_commit_write(&e, &ra, rt, ARM_R0);
                break;
            }

            case 0x0Cu: /* ANDI */
            case 0x0Du: /* ORI */
            case 0x0Eu: { /* XORI */
                if (rt == 0u) {
                    commit_prev_load_after_alu(&e, &ra, &pend_reg, 0u);
                    break;
                }
                if (dec->known_rs) {
                    u32 v = (op == 0x0Cu) ? (dec->kval_rs & imm16)
                                          : ((op == 0x0Du) ? (dec->kval_rs | imm16)
                                                           : (dec->kval_rs ^ imm16));
                    commit_prev_load_after_alu(&e, &ra, &pend_reg, rt);
                    ArmReg dst = jit_ra_get_write_buf(&ra, rt, ARM_R0);
                    emit_const(&e, dst, v);
                    jit_ra_commit_write(&e, &ra, rt, dst);
                    break;
                }
                ArmReg r_rs = jit_ra_get_read(&e, &ra, rs, ARM_R1);
                enum ArmDpOp aop = (op == 0x0Cu) ? ARM_OP_AND
                                                 : ((op == 0x0Du) ? ARM_OP_ORR : ARM_OP_EOR);
                u32 enc_imm = 0u;
                if (arm_encode_imm(imm16, &enc_imm)) {
                    emit_alu_imm_enc(&e, ARM_CC_AL, aop, 0u, ARM_R0, r_rs, enc_imm);
                } else {
                    emit_const(&e, ARM_R2, imm16);
                    emit_alu_reg(&e, ARM_CC_AL, aop, 0u, ARM_R0, r_rs, ARM_R2);
                }
                commit_prev_load_after_alu(&e, &ra, &pend_reg, rt);
                jit_ra_commit_write(&e, &ra, rt, ARM_R0);
                break;
            }

            case 0x0Fu: { /* LUI */
                commit_prev_load_after_alu(&e, &ra, &pend_reg, rt);
                if (rt != 0u) {
                    ArmReg dst = jit_ra_get_write_buf(&ra, rt, ARM_R0);
                    emit_const(&e, dst, imm16 << 16);
                    jit_ra_commit_write(&e, &ra, rt, dst);
                }
                break;
            }

            case 0x10u: { /* COP0 */
                if (start_pc < 0x80000000u || sys->cfg.jit_check_kuc) {
                    /* COP0 usable iff (SR & CU0) || !(SR & KUc) */
                    u16 lbl_cpu0 = push_exc_stub(&e, stubs, &n_stubs, &ra, pend_reg,
                                                 dec, scan.total_cycles, PSX_EXC_CPU,
                                                 0u, false, 0u);
                    u16 lbl_ok = arm_label_create(&e);
                    u32 enc_cu0 = 0u;
                    (void)arm_encode_imm(PSX_SR_CU0, &enc_cu0);
                    emit_ldr_imm(&e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, PSX_OFF_COP0_SR);
                    emit_tst_imm_enc(&e, ARM_CC_AL, ARM_R2, enc_cu0);
                    emit_b_label(&e, ARM_CC_NE, lbl_ok);
                    emit_tst_imm_enc(&e, ARM_CC_AL, ARM_R2, PSX_SR_KUC);
                    emit_b_label(&e, ARM_CC_NE, lbl_cpu0);
                    arm_label_bind(&e, lbl_ok);
                }

                if ((rs & 0x10u) != 0u) {
                    if (fn == 0x10u) { /* RFE */
                        emit_mov_reg(&e, ARM_R0, JIT_REG_CPU);
                        emit_call(&e, (const void *)(uintptr_t)jit_rfe_helper, true);
                        commit_prev_load_after_alu(&e, &ra, &pend_reg, 0u);
                    } else {
                        /* Other CO=1 COP0 ops are NOPs on R3000A */
                        commit_prev_load_after_alu(&e, &ra, &pend_reg, 0u);
                    }
                } else if (rs == 0x00u) { /* MFC0 (load-delayed!) */
                    if (rd == COP0_PRID) {
                        emit_const(&e, ARM_R0, PSX_COP0_PRID_VALUE);
                    } else if (rd < 16u) {
                        emit_ldr_imm(&e, ARM_CC_AL, ARM_R0, JIT_REG_CPU,
                                     (s32)(PSX_OFF_COP0 + rd * 4u));
                    } else {
                        emit_const(&e, ARM_R0, 0u);
                    }
                    stage_new_delayed_load(&e, &ra, &pend_reg, rt, ARM_R0);
                } else if (rs == 0x04u) { /* MTC0 */
                    ArmReg r_rt = jit_ra_get_read(&e, &ra, rt, ARM_R1);
                    if (rd == COP0_SR) {
                        emit_mov_reg(&e, ARM_R1, r_rt);
                        emit_mov_reg(&e, ARM_R0, JIT_REG_CPU);
                        emit_call(&e, (const void *)(uintptr_t)jit_mtc0_sr_helper, true);
                    } else if (rd == COP0_CAUSE) {
                        emit_mov_reg(&e, ARM_R1, r_rt);
                        emit_mov_reg(&e, ARM_R0, JIT_REG_CPU);
                        emit_call(&e, (const void *)(uintptr_t)jit_mtc0_cause_helper, true);
                    } else if (rd == COP0_BPC || rd == COP0_BDA || rd == COP0_DCIC ||
                               rd == COP0_BDAM || rd == COP0_BPCM ||
                               (rd < 16u && rd != COP0_JUMPDEST && rd != COP0_BADVADDR &&
                                rd != COP0_PRID)) {
                        emit_str_imm(&e, ARM_CC_AL, r_rt, JIT_REG_CPU,
                                     (s32)(PSX_OFF_COP0 + rd * 4u));
                    }
                    commit_prev_load_after_alu(&e, &ra, &pend_reg, 0u);
                } else {
                    u16 lbl = push_exc_stub(&e, stubs, &n_stubs, &ra, pend_reg,
                                            dec, scan.total_cycles, PSX_EXC_RI,
                                            0u, false, 0u);
                    emit_b_label(&e, ARM_CC_AL, lbl);
                    block_always_traps = true;
                }
                break;
            }

            case 0x11u: /* COP1 */
            case 0x13u: { /* COP3 */
                u8 ce_n = (op == 0x11u) ? 1u : 3u;
                u32 cu_bit = (op == 0x11u) ? PSX_SR_CU1 : PSX_SR_CU3;
                u32 enc_cu = 0u;
                (void)arm_encode_imm(cu_bit, &enc_cu);
                u16 lbl = push_exc_stub(&e, stubs, &n_stubs, &ra, pend_reg,
                                        dec, scan.total_cycles, PSX_EXC_CPU,
                                        ce_n, false, 0u);
                emit_ldr_imm(&e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, PSX_OFF_COP0_SR);
                emit_tst_imm_enc(&e, ARM_CC_AL, ARM_R2, enc_cu);
                emit_b_label(&e, ARM_CC_EQ, lbl);
                commit_prev_load_after_alu(&e, &ra, &pend_reg, 0u);
                break;
            }

            case 0x12u: { /* COP2 (GTE) */
                if (!cop2_checked) {
                    u32 enc_cu2 = 0u;
                    (void)arm_encode_imm(PSX_SR_CU2, &enc_cu2);
                    u16 lbl_cpu2 = push_exc_stub(&e, stubs, &n_stubs, &ra, pend_reg,
                                                 dec, scan.total_cycles, PSX_EXC_CPU,
                                                 2u, false, 0u);
                    emit_ldr_imm(&e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, PSX_OFF_COP0_SR);
                    emit_tst_imm_enc(&e, ARM_CC_AL, ARM_R2, enc_cu2);
                    emit_b_label(&e, ARM_CC_EQ, lbl_cpu2);
                    cop2_checked = true;
                }

                if ((rs & 0x10u) != 0u) {
                    bool emitted_inline = false;
                    if (sys->cfg.jit_inline_gte) {
                        /* MANDATORY: spill DIRTY slots AND mark ALL 7 slots UNLOADED! */
                        jit_ra_spill_and_invalidate_all(&e, &ra);
                        emitted_inline = jit_emit_gte_inline(&e, insn);
                    }
                    if (!emitted_inline) {
                        emit_mov_reg(&e, ARM_R0, JIT_REG_CPU);
                        emit_const(&e, ARM_R1, insn);
                        emit_call(&e, (const void *)(uintptr_t)jit_gte_exec_wrap, false);
                    }
                    commit_prev_load_after_alu(&e, &ra, &pend_reg, 0u);
                } else if (rs == 0x00u) { /* MFC2 (load-delayed!) */
                    if (sys->cfg.jit_inline_gte) {
                        jit_emit_mfc2_inline(&e, rd, ARM_R0);
                    } else {
                        emit_mov_reg(&e, ARM_R0, JIT_REG_CPU);
                        emit_const(&e, ARM_R1, rd);
                        emit_call(&e, (const void *)(uintptr_t)jit_gte_read_data_wrap, false);
                    }
                    stage_new_delayed_load(&e, &ra, &pend_reg, rt, ARM_R0);
                } else if (rs == 0x02u) { /* CFC2 (load-delayed!) */
                    if (sys->cfg.jit_inline_gte) {
                        jit_emit_cfc2_inline(&e, rd, ARM_R0);
                    } else {
                        emit_mov_reg(&e, ARM_R0, JIT_REG_CPU);
                        emit_const(&e, ARM_R1, rd);
                        emit_call(&e, (const void *)(uintptr_t)jit_gte_read_ctrl_wrap, false);
                    }
                    stage_new_delayed_load(&e, &ra, &pend_reg, rt, ARM_R0);
                } else if (rs == 0x04u) { /* MTC2 */
                    ArmReg r_rt = jit_ra_get_read(&e, &ra, rt, ARM_R1);
                    if (sys->cfg.jit_inline_gte) {
                        jit_emit_mtc2_inline(&e, rd, r_rt);
                    } else {
                        emit_mov_reg(&e, ARM_R2, r_rt);
                        emit_const(&e, ARM_R1, rd);
                        emit_mov_reg(&e, ARM_R0, JIT_REG_CPU);
                        emit_call(&e, (const void *)(uintptr_t)jit_gte_write_data_wrap, false);
                    }
                    commit_prev_load_after_alu(&e, &ra, &pend_reg, 0u);
                } else if (rs == 0x06u) { /* CTC2 */
                    ArmReg r_rt = jit_ra_get_read(&e, &ra, rt, ARM_R1);
                    if (sys->cfg.jit_inline_gte) {
                        jit_emit_ctc2_inline(&e, rd, r_rt);
                    } else {
                        emit_mov_reg(&e, ARM_R2, r_rt);
                        emit_const(&e, ARM_R1, rd);
                        emit_mov_reg(&e, ARM_R0, JIT_REG_CPU);
                        emit_call(&e, (const void *)(uintptr_t)jit_gte_write_ctrl_wrap, false);
                    }
                    commit_prev_load_after_alu(&e, &ra, &pend_reg, 0u);
                } else {
                    u16 lbl = push_exc_stub(&e, stubs, &n_stubs, &ra, pend_reg,
                                            dec, scan.total_cycles, PSX_EXC_RI,
                                            0u, false, 0u);
                    emit_b_label(&e, ARM_CC_AL, lbl);
                    block_always_traps = true;
                }
                break;
            }

            case 0x20u: /* LB */
            case 0x21u: /* LH */
            case 0x23u: /* LW */
            case 0x24u: /* LBU */
            case 0x25u: { /* LHU */
                u8 width = (op == 0x23u) ? 4u : ((op == 0x21u || op == 0x25u) ? 2u : 1u);
                bool is_signed = (op == 0x20u || op == 0x21u);
                emit_mem_load_to_reg(&e, sys, start_pc, stubs, &n_stubs,
                                     &ra, pend_reg, dec, scan.total_cycles,
                                     rs, simm, width, is_signed, ARM_R0);
                stage_new_delayed_load(&e, &ra, &pend_reg, rt, ARM_R0);
                break;
            }

            case 0x22u: /* LWL */
            case 0x26u: { /* LWR */
                emit_calc_addr_r1(&e, &ra, dec, rs, simm);
                /* MEM->MEM bypass: if pend_reg == rt, merge against pending load! */
                if (pend_reg != 0u && pend_reg == rt) {
                    emit_ldr_imm(&e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, PSX_OFF_GTE_SCRATCH + 8);
                } else {
                    ArmReg r_rt = jit_ra_get_read(&e, &ra, rt, ARM_R2);
                    emit_mov_reg(&e, ARM_R2, r_rt);
                }
                emit_mov_reg(&e, ARM_R0, JIT_REG_CPU);
                emit_call(&e,
                          (op == 0x22u) ? (const void *)(uintptr_t)jit_lwl_helper
                                        : (const void *)(uintptr_t)jit_lwr_helper,
                          true);

                u16 lbl_ok = arm_label_create(&e);
                emit_ldr_imm(&e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, PSX_OFF_JIT_ABORT);
                emit_cmp_imm_enc(&e, ARM_CC_AL, ARM_R2, 0u);
                emit_b_label(&e, ARM_CC_EQ, lbl_ok);

                ColdStub abt_desc;
                memset(&abt_desc, 0, sizeof(abt_desc));
                abt_desc.bd = dec->in_delay_slot;
                abt_desc.pend_reg = (u8)pend_reg;
                abt_desc.dirty_mask = jit_ra_dirty_mask(&ra);
                abt_desc.cyc_refund = (scan.total_cycles > dec->cyc_before)
                                          ? (u16)(scan.total_cycles - dec->cyc_before)
                                          : 0u;
                abt_desc.pc = dec->pc;
                emit_abort_tail(&e, &ra, &abt_desc);

                arm_label_bind(&e, lbl_ok);
                stage_new_delayed_load(&e, &ra, &pend_reg, rt, ARM_R0);
                break;
            }

            case 0x28u: /* SB */
            case 0x29u: /* SH */
            case 0x2Bu: { /* SW */
                u8 width = (op == 0x2Bu) ? 4u : ((op == 0x29u) ? 2u : 1u);
                ArmReg r_val = jit_ra_get_read(&e, &ra, rt, ARM_R0);
                emit_mem_store_from_reg(&e, sys, start_pc, stubs, &n_stubs,
                                        &ra, pend_reg, dec, scan.total_cycles,
                                        rs, simm, width, r_val);
                commit_prev_load_after_alu(&e, &ra, &pend_reg, 0u);
                break;
            }

            case 0x2Au: /* SWL */
            case 0x2Eu: { /* SWR */
                ArmReg r_val = jit_ra_get_read(&e, &ra, rt, ARM_R0);
                if (r_val != ARM_R0) {
                    emit_mov_reg(&e, ARM_R0, r_val);
                }
                emit_calc_addr_r1(&e, &ra, dec, rs, simm);
                emit_mov_reg(&e, ARM_R2, ARM_R0);
                emit_mov_reg(&e, ARM_R0, JIT_REG_CPU);
                emit_call(&e,
                          (op == 0x2Au) ? (const void *)(uintptr_t)jit_swl_helper
                                        : (const void *)(uintptr_t)jit_swr_helper,
                          true);

                u16 lbl_ok = arm_label_create(&e);
                emit_ldr_imm(&e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, PSX_OFF_JIT_ABORT);
                emit_cmp_imm_enc(&e, ARM_CC_AL, ARM_R2, 0u);
                emit_b_label(&e, ARM_CC_EQ, lbl_ok);

                ColdStub abt_desc;
                memset(&abt_desc, 0, sizeof(abt_desc));
                abt_desc.bd = dec->in_delay_slot;
                abt_desc.pend_reg = (u8)pend_reg;
                abt_desc.dirty_mask = jit_ra_dirty_mask(&ra);
                abt_desc.cyc_refund = (scan.total_cycles > dec->cyc_before)
                                          ? (u16)(scan.total_cycles - dec->cyc_before)
                                          : 0u;
                abt_desc.pc = dec->pc;
                emit_abort_tail(&e, &ra, &abt_desc);

                arm_label_bind(&e, lbl_ok);
                commit_prev_load_after_alu(&e, &ra, &pend_reg, 0u);
                break;
            }

            case 0x32u: { /* LWC2 */
                if (!cop2_checked) {
                    u32 enc_cu2 = 0u;
                    (void)arm_encode_imm(PSX_SR_CU2, &enc_cu2);
                    u16 lbl_cpu2 = push_exc_stub(&e, stubs, &n_stubs, &ra, pend_reg,
                                                 dec, scan.total_cycles, PSX_EXC_CPU,
                                                 2u, false, 0u);
                    emit_ldr_imm(&e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, PSX_OFF_COP0_SR);
                    emit_tst_imm_enc(&e, ARM_CC_AL, ARM_R2, enc_cu2);
                    emit_b_label(&e, ARM_CC_EQ, lbl_cpu2);
                    cop2_checked = true;
                }
                emit_mem_load_to_reg(&e, sys, start_pc, stubs, &n_stubs,
                                     &ra, pend_reg, dec, scan.total_cycles,
                                     rs, simm, 4u, false, ARM_R0);
                if (sys->cfg.jit_inline_gte) {
                    jit_emit_mtc2_inline(&e, rt, ARM_R0);
                } else {
                    emit_mov_reg(&e, ARM_R2, ARM_R0);
                    emit_const(&e, ARM_R1, rt);
                    emit_mov_reg(&e, ARM_R0, JIT_REG_CPU);
                    emit_call(&e, (const void *)(uintptr_t)jit_gte_write_data_wrap, false);
                }
                commit_prev_load_after_alu(&e, &ra, &pend_reg, 0u);
                break;
            }

            case 0x3Au: { /* SWC2 */
                if (!cop2_checked) {
                    u32 enc_cu2 = 0u;
                    (void)arm_encode_imm(PSX_SR_CU2, &enc_cu2);
                    u16 lbl_cpu2 = push_exc_stub(&e, stubs, &n_stubs, &ra, pend_reg,
                                                 dec, scan.total_cycles, PSX_EXC_CPU,
                                                 2u, false, 0u);
                    emit_ldr_imm(&e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, PSX_OFF_COP0_SR);
                    emit_tst_imm_enc(&e, ARM_CC_AL, ARM_R2, enc_cu2);
                    emit_b_label(&e, ARM_CC_EQ, lbl_cpu2);
                    cop2_checked = true;
                }
                if (sys->cfg.jit_inline_gte) {
                    jit_emit_mfc2_inline(&e, rt, ARM_R0);
                } else {
                    emit_mov_reg(&e, ARM_R0, JIT_REG_CPU);
                    emit_const(&e, ARM_R1, rt);
                    emit_call(&e, (const void *)(uintptr_t)jit_gte_read_data_wrap, false);
                }
                emit_mem_store_from_reg(&e, sys, start_pc, stubs, &n_stubs,
                                        &ra, pend_reg, dec, scan.total_cycles,
                                        rs, simm, 4u, ARM_R0);
                commit_prev_load_after_alu(&e, &ra, &pend_reg, 0u);
                break;
            }

            case 0x30u: case 0x38u: /* LWC0 / SWC0 */
            case 0x31u: case 0x39u: /* LWC1 / SWC1 */
            case 0x33u: case 0x3Bu: { /* LWC3 / SWC3 */
                u8 ce_n = (u8)(op & 3u);
                u32 cu_bit = 1u << (28u + ce_n);
                u32 enc_cu = 0u;
                (void)arm_encode_imm(cu_bit, &enc_cu);
                u16 lbl_cpu = push_exc_stub(&e, stubs, &n_stubs, &ra, pend_reg,
                                            dec, scan.total_cycles, PSX_EXC_CPU,
                                            ce_n, false, 0u);
                emit_ldr_imm(&e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, PSX_OFF_COP0_SR);
                emit_tst_imm_enc(&e, ARM_CC_AL, ARM_R2, enc_cu);
                emit_b_label(&e, ARM_CC_EQ, lbl_cpu);

                u8 fault_exc = (op < 0x38u) ? PSX_EXC_ADEL : PSX_EXC_ADES;
                emit_calc_addr_r1(&e, &ra, dec, rs, simm);
                /* LWCn/SWCn ALWAYS check 4-byte alignment & KUc per cop.exe */
                u16 lbl_addr = push_exc_stub(&e, stubs, &n_stubs, &ra, pend_reg,
                                             dec, scan.total_cycles, fault_exc,
                                             0u, true, 0u);
                emit_tst_imm_enc(&e, ARM_CC_AL, ARM_R1, 3u);
                emit_b_label(&e, ARM_CC_NE, lbl_addr);
                if (start_pc < 0x80000000u || sys->cfg.jit_check_kuc) {
                    u32 enc80 = 0u;
                    (void)arm_encode_imm(0x80000000u, &enc80);
                    emit_ldr_imm(&e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, PSX_OFF_COP0_SR);
                    emit_tst_imm_enc(&e, ARM_CC_AL, ARM_R2, 2u);
                    emit_tst_imm_enc(&e, ARM_CC_NE, ARM_R1, enc80);
                    emit_b_label(&e, ARM_CC_NE, lbl_addr);
                }
                commit_prev_load_after_alu(&e, &ra, &pend_reg, 0u);
                break;
            }

            default: { /* Reserved primary opcode -> RI */
                u16 lbl = push_exc_stub(&e, stubs, &n_stubs, &ra, pend_reg,
                                        dec, scan.total_cycles, PSX_EXC_RI,
                                        0u, false, 0u);
                emit_b_label(&e, ARM_CC_AL, lbl);
                block_always_traps = true;
                break;
            }
        }
    }

    /* 4. Block epilogue: dirty GPR writeback, pending load resolution & terminator */
    if (!block_always_traps) {
        if (pend_reg != 0u) {
            if ((scan.block_flags & JB_PEEKED_TARGET) != 0u) {
                /* Target instruction proven not to read `pend_reg`: commit safely in-block! */
                emit_ldr_imm(&e, ARM_CC_AL, ARM_R3, JIT_REG_CPU, PSX_OFF_GTE_SCRATCH + 8);
                jit_ra_commit_write(&e, &ra, pend_reg, ARM_R3);
                pend_reg = 0u;
                jit_ra_flush_dirty(&e, &ra);
            } else {
                /* Preserve live load_delay_reg/val into PsxCpu and step 1 insn cleanly! */
                jit_ra_flush_dirty(&e, &ra);
                if (br_kind != BR_NONE) {
                    emit_ldr_imm(&e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, PSX_OFF_GTE_SCRATCH + 0);
                } else {
                    emit_const(&e, ARM_R0, start_pc + scan.n_insns * 4u);
                }
                emit_str_imm(&e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, PSX_OFF_PC);
                emit_alu_imm_enc(&e, ARM_CC_AL, ARM_OP_ADD, 0u, ARM_R1, ARM_R0, 4u);
                emit_str_imm(&e, ARM_CC_AL, ARM_R1, JIT_REG_CPU, PSX_OFF_NEXT_PC);
                emit_const(&e, ARM_R2, 0u);
                emit_str_imm(&e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, PSX_OFF_IN_DELAY_SLOT);
                emit_const(&e, ARM_R1, pend_reg);
                emit_str_imm(&e, ARM_CC_AL, ARM_R1, JIT_REG_CPU, PSX_OFF_LOAD_DELAY_REG);
                emit_ldr_imm(&e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, PSX_OFF_GTE_SCRATCH + 8);
                emit_str_imm(&e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, PSX_OFF_LOAD_DELAY_VAL);
                emit_branch_rx(&e, ARM_CC_AL, (const void *)jit_exit_step_one);
                goto emit_tails;
            }
        } else {
            jit_ra_flush_dirty(&e, &ra);
        }

        bool idle_loop = (scan.block_flags & JB_IDLE_LOOP) != 0u;

        if (br_kind == BR_NONE) {
            emit_static_link(&e, sys, start_pc, scan.ram_pages, scan.ends_unlinked,
                             lbl_entry_fast, lbl_exit_stub, false,
                             start_pc + scan.n_insns * 4u);
        } else if (br_kind == BR_STATIC_UNCOND) {
            emit_static_link(&e, sys, start_pc, scan.ram_pages, scan.ends_unlinked,
                             lbl_entry_fast, lbl_exit_stub, idle_loop, br_target_t);
        } else if (br_kind == BR_STATIC_COND) {
            u16 lbl_taken = arm_label_create(&e);
            emit_ldr_imm(&e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, PSX_OFF_GTE_SCRATCH + 4);
            emit_cmp_imm_enc(&e, ARM_CC_AL, ARM_R0, 0u);
            emit_b_label(&e, ARM_CC_NE, lbl_taken);

            emit_static_link(&e, sys, start_pc, scan.ram_pages, scan.ends_unlinked,
                             lbl_entry_fast, lbl_exit_stub, false, br_target_f);

            arm_label_bind(&e, lbl_taken);
            emit_static_link(&e, sys, start_pc, scan.ram_pages, scan.ends_unlinked,
                             lbl_entry_fast, lbl_exit_stub, idle_loop, br_target_t);
        } else {
            /* BR_INDIRECT_REG (`JR` / `JALR`) */
            emit_ldr_imm(&e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, PSX_OFF_GTE_SCRATCH + 0);
            if (!scan.ends_unlinked && sys->cfg.jit_ibtc && sys->jit.ibtc) {
                u32 enc_bic = 0u;
                (void)arm_encode_imm(0x00300000u, &enc_bic);
                emit_ldr_imm(&e, ARM_CC_AL, ARM_R3, JIT_REG_CPU, PSX_OFF_IBTC);
                emit_shift_imm(&e, ARM_CC_AL, 0u, ARM_R2, ARM_R0, ARM_SH_LSL, 20u);
                emit_alu_imm_enc(&e, ARM_CC_AL, ARM_OP_BIC, 0u, ARM_R2, ARM_R2, enc_bic);
                emit_alu_reg_sh(&e, ARM_CC_AL, ARM_OP_ADD, 0u, ARM_R2, ARM_R3, ARM_R2,
                                ARM_SH_LSR, 19u);
                emit_ldmia(&e, ARM_CC_AL, ARM_R2, false, (1u << ARM_R1) | (1u << ARM_R3));
                emit_cmp_reg(&e, ARM_CC_AL, ARM_R1, ARM_R0);
                emit_bx(&e, ARM_CC_EQ, ARM_R3);
                emit_branch_rx(&e, ARM_CC_AL, (const void *)jit_dispatch_slow);
            } else {
                emit_branch_rx(&e, ARM_CC_AL, (const void *)jit_dispatch);
            }
        }
    }

emit_tails:
    /* 5. Out-of-line exit_stub, segment-mismatch stub, cold exception/slow stubs, & .ltorg */
    arm_label_bind(&e, lbl_exit_stub);
    emit_const(&e, ARM_R0, start_pc);
    emit_str_imm(&e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, PSX_OFF_PC);
    emit_branch_rx(&e, ARM_CC_AL, (const void *)jit_exit);

    arm_label_bind(&e, lbl_seg_mismatch);
    emit_branch_rx(&e, ARM_CC_AL, (const void *)jit_dispatch_slow);

    emit_all_cold_stubs(&e, &ra, stubs, n_stubs);

    u32 emitted_bytes = 0u;
    if (!arm_emit_finalize(&e, &emitted_bytes)) {
        return NULL;
    }

    u8 *entry_rx = JIT_RW2RX(&j->mem, rw_begin);
    jit_mem_commit(&j->mem, rw_begin, emitted_bytes);

    u16 b_idx = (u16)j->n_blocks++;
    JitBlock *b = &j->blocks[b_idx];
    memset(b, 0, sizeof(*b));
    b->entry.p = entry_rx;
    b->pc = start_pc;
    b->code_bytes = emitted_bytes;
    b->n_insns = (u16)scan.n_insns;
    b->cycles = scan.total_cycles;
    b->phys_page[0] = JIT_PAGE_NONE;
    b->phys_page[1] = JIT_PAGE_NONE;
    b->next_in_page[0] = JIT_PAGE_NONE;
    b->next_in_page[1] = JIT_PAGE_NONE;
    b->flags = scan.block_flags;
    b->_pad = fast_off_bytes;

    j->code_ptr = rw_begin + emitted_bytes;

    for (u32 k = 0u; k < 2u; ++k) {
        if (scan.ram_pages[k] != JIT_PAGE_NONE) {
            (void)jit_block_add_ram_page(sys, b_idx, scan.ram_pages[k]);
        }
    }

    if (is_ram) {
        j->tab_ram[BLOCKTAB_RAM_IDX(start_pc)] = entry_rx;
    } else {
        j->tab_bios[BLOCKTAB_BIOS_IDX(start_pc)] = entry_rx;
    }

    sys->stats.jit_blocks_live++;
    sys->stats.jit_blocks_compiled_frame++;
    sys->stats.jit_code_bytes = (u32)(j->code_ptr - j->mem.rw);

    return b;
}

/* ============================================================================
 * Public Dispatcher / Compiler Entry Points (`jit.h`)
 * ============================================================================ */

PSX_USED void *jit_compile_at_pc(PsxCpu *cpu) {
    PsxSystem *sys = (PsxSystem *)cpu->sys.p;
    JitState *j = &sys->jit;
    u32 pc = cpu->pc;

    if (cpu->cycles_left <= 0 || sys->quit || sys->frame_done) {
        return (void *)jit_exit;
    }

    /* Top-level headroom check (>= 33 blocks & >= 33 * 4096 B code space) */
    if ((j->max_blocks - j->n_blocks) < (JIT_EAGER_BUDGET + 1u) ||
        (size_t)(j->code_end - j->code_ptr) < ((size_t)(JIT_EAGER_BUDGET + 1u) * JIT_MAX_BLOCK_BYTES)) {
        jit_flush_all(sys);
    }

    u32 budget = JIT_EAGER_BUDGET;
    JitBlock *b = jit_compile_internal(sys, pc, 0u, &budget);
    if (!b) {
        return (void *)jit_exit_step_one;
    }
    jit_ibtc_record(sys, pc, b);
    return jit_block_fast_entry(b);
}

PSX_USED void *jit_lookup_slow(PsxCpu *cpu, u32 pc) {
    PsxSystem *sys = (PsxSystem *)cpu->sys.p;
    JitState *j = &sys->jit;
    cpu->pc = pc;
    sys->stats.dispatch_slow++;

    if (cpu->cycles_left <= 0 || sys->quit || sys->frame_done) {
        return (void *)jit_exit;
    }

    if (((cpu->cop0[COP0_SR] & PSX_SR_KUC) != 0u) && ((pc & 0x80000000u) != 0u)) {
        return (void *)jit_exit_step_one;
    }

    bool is_ram = jit_pc_is_ram(sys, pc);
    bool is_bios = !is_ram && jit_pc_is_bios(pc);
    if (!is_ram && !is_bios) {
        return (void *)jit_exit_step_one;
    }

    u32 phys = pc & 0x1FFFFFFFu;
    if (phys == 0xA0u || phys == 0xB0u || phys == 0xC0u) {
        return (void *)jit_bios_vec_stub;
    }
    if ((cpu->psexe_pending != 0u || sys->bios.psexe_pending) && phys == 0x30000u) {
        return (void *)jit_psexe_stub;
    }

    JitBlock *b = jit_find_live_block(sys, pc);
    if (b && b->pc != pc) {
        /* Virtual segment alias mismatch (e.g. KSEG1 0xA0 vs KSEG0 0x80 or KUSEG 0x00) */
        if (is_ram) {
            s32 saved_left = cpu->cycles_left;
            s32 saved_slice = cpu->slice_len;
            jit_invalidate_page(sys, (pc & PSX_RAM_MASK) >> 12);
            cpu->cycles_left = saved_left;
            cpu->slice_len = saved_slice;
        } else {
            j->tab_bios[BLOCKTAB_BIOS_IDX(pc)] = (void *)jit_compile_stub;
            b->flags |= JB_DEAD;
        }
        b = NULL;
    }

    if (!b) {
        sys->stats.ibtc_misses++;
        return jit_compile_at_pc(cpu);
    }

    sys->stats.ibtc_misses++;
    jit_ibtc_record(sys, pc, b);
    return jit_block_fast_entry(b);
}

void jit_run(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    PsxCpu *c = &sys->cpu;
    JitState *j = &sys->jit;

    /* Fallback seamlessly to cached interpreter if host/OS denied executable JIT memory */
    if (!j->mem.rx || j->mem.mode == JIT_MEM_NONE || !j->tab_ram || !j->tab_bios) {
        cpu_cached_run(sys);
        return;
    }

    /* Flush block pool if previously populated by cpu_cached_run */
    if (j->n_blocks > 0u && (j->blocks[0].flags & JB_CACHED_OP) != 0u) {
        jit_flush_all(sys);
    }

    if (c->icache_dirty != 0u) {
        jit_mem_icache_flush_all(&j->mem);
        c->icache_dirty = 0u;
    }
    jit_refresh_pins(sys);

    while (c->cycles_left > 0 && !sys->quit && !sys->frame_done) {
        if (c->in_delay_slot != 0u || c->load_delay_reg != 0u) {
            jit_step_until_clean_wrap(sys);
            if (c->cycles_left <= 0 || sys->quit || sys->frame_done) {
                break;
            }
        }
        if (c->icache_dirty != 0u) {
            jit_mem_icache_flush_all(&j->mem);
            c->icache_dirty = 0u;
        }

        int rc = jit_enter(c);
        c->in_jit = 0u;

        if (rc == PSX_EXIT_STEP_ONE) {
            if (c->cycles_left > 0 && !sys->quit && !sys->frame_done) {
                jit_step_until_clean_wrap(sys);
            }
            continue;
        }
        if (rc == PSX_EXIT_EVENTS || rc == PSX_EXIT_HALT) {
            break;
        }
    }
}

#endif /* PSX3DS_HAVE_JIT */
