#include "arm_emit.h"

#include <string.h>

const ArmReg k_jit_slot_regs[JIT_NUM_SLOTS] = {
    ARM_R4, ARM_R5, ARM_R6, ARM_R7, ARM_R8, ARM_R12, ARM_LR
};

void jit_regalloc_reset(void) {
}

/* ============================================================================
 * Side-effect-free instruction peek & static classification helpers
 * ============================================================================ */

static bool scan_peek32(const PsxSystem *sys, u32 pc, u32 *out_insn) {
    if ((pc & 3u) != 0u) {
        return false;
    }
    bool seg_ok = (pc < 0x20000000u) || (pc >= 0x80000000u && pc < 0xC0000000u);
    if (!seg_ok) {
        return false;
    }
    u32 phys = pc & 0x1FFFFFFFu;
    if (phys < 0x00200000u || (phys < 0x00800000u && !sys->cfg.strict_ram_size)) {
        if (!sys->bus.ram) {
            return false;
        }
        memcpy(out_insn, sys->bus.ram + (phys & PSX_RAM_MASK), 4u);
        return true;
    }
    if (phys >= PSX_BIOS_PHYS && phys < (PSX_BIOS_PHYS + PSX_BIOS_SIZE)) {
        if (!sys->bus.bios) {
            return false;
        }
        memcpy(out_insn, sys->bus.bios + (phys & PSX_BIOS_MASK), 4u);
        return true;
    }
    return false;
}

static bool scan_try_add_ram_page(u16 pages[2], u16 phys_page) {
    if (pages[0] == phys_page || pages[1] == phys_page) {
        return true;
    }
    if (pages[0] == JIT_PAGE_NONE) {
        pages[0] = phys_page;
        return true;
    }
    if (pages[1] == JIT_PAGE_NONE) {
        pages[1] = phys_page;
        return true;
    }
    return false;
}

static bool scan_is_branch(u32 insn) {
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

static bool scan_is_mem_op(u32 insn) {
    u32 op = insn >> 26;
    return op >= 0x20u && op <= 0x3Bu;
}

static bool scan_is_delayed_load(u32 insn, u32 *out_rt) {
    u32 op = insn >> 26;
    u32 rs = (insn >> 21) & 31u;
    u32 rt = (insn >> 16) & 31u;
    if (op >= 0x20u && op <= 0x26u) {
        *out_rt = rt;
        return rt != 0u;
    }
    if (op == 0x10u && rs == 0x00u) { /* MFC0 */
        *out_rt = rt;
        return rt != 0u;
    }
    if (op == 0x12u && (rs == 0x00u || rs == 0x02u)) { /* MFC2 / CFC2 */
        *out_rt = rt;
        return rt != 0u;
    }
    *out_rt = 0u;
    return false;
}

static bool scan_is_unlinked_cop0(u32 insn) {
    u32 op = insn >> 26;
    if (op != 0x10u) {
        return false;
    }
    u32 rs = (insn >> 21) & 31u;
    u32 rd = (insn >> 11) & 31u;
    u32 fn = insn & 63u;
    if ((rs & 0x10u) != 0u && fn == 0x10u) {
        return true; /* RFE */
    }
    if (rs == 0x04u && (rd == COP0_SR || rd == COP0_CAUSE)) {
        return true; /* MTC0 SR / CAUSE */
    }
    return false;
}

static bool scan_terminates_immediately(u32 insn) {
    u32 op = insn >> 26;
    u32 fn = insn & 63u;
    if (op == 0u && (fn == 0x0Cu || fn == 0x0Du)) {
        return true; /* SYSCALL / BREAK */
    }
    return scan_is_unlinked_cop0(insn);
}

/* Conservatively true if `insn` could read MIPS GPR `reg` on either read port */
static bool scan_insn_reads_gpr(u32 insn, u32 reg) {
    if (reg == 0u) {
        return false;
    }
    u32 op = insn >> 26;
    if (op == 0x02u || op == 0x03u) { /* J / JAL */
        return false;
    }
    u32 rs = (insn >> 21) & 31u;
    u32 rt = (insn >> 16) & 31u;
    return (rs == reg) || (rt == reg);
}

static u32 scan_estimate_arm_words(const PsxSystem *sys, u32 start_pc, u32 insn) {
    u32 op = insn >> 26;
    u32 rs = (insn >> 21) & 31u;
    u32 fn = insn & 63u;
    if (op == 0x12u && (rs & 0x10u) != 0u) {
        return 240u; /* Inline RTPT/MVMVA worst-case + overflow stubs + literals */
    }
    if (op >= 0x20u && op <= 0x3Bu) {
        /* KUSEG / jit_check_kuc loads & stores emit fastmem + KUc/align STUB_EXC +
         * STUB_MEM_LOAD/STORE + abort_tail + literals (~52-60 ARM words worst-case) */
        if (start_pc < 0x80000000u || sys->cfg.jit_check_kuc) {
            return 64u;
        }
        return 48u;
    }
    if (op == 0x08u || (op == 0x00u && (fn == 0x20u || fn == 0x22u))) {
        return 26u; /* ADDI / ADD / SUB emit overflow STUB_EXC */
    }
    if ((op >= 0x10u && op <= 0x13u) ||
        (op == 0x00u && (fn == 0x1Au || fn == 0x1Bu))) {
        return 24u; /* COP0..COP3 moves/checks & DIV/DIVU helper calls */
    }
    return 10u;
}

static JitAddrClass scan_classify_const_addr(const PsxSystem *sys, u32 insn_pc, u32 insn, u32 addr) {
    u32 op = insn >> 26;
    if (op == 0x22u || op == 0x26u || op == 0x2Au || op == 0x2Eu) {
        /* LWL / LWR / SWL / SWR use dedicated helpers */
        return ADDR_DYNAMIC;
    }
    if ((sys->cfg.jit_check_kuc || insn_pc < 0x80000000u) && (addr & 0x80000000u) != 0u) {
        return ADDR_DYNAMIC;
    }
    u32 width = 1u;
    if (op == 0x21u || op == 0x25u || op == 0x29u) {
        width = 2u;
    } else if (op == 0x23u || op == 0x2Bu || op >= 0x30u) {
        width = 4u;
    }
    if ((addr & (width - 1u)) != 0u) {
        return ADDR_CONST_SLOW;
    }
    bool seg_ok = (addr < 0x20000000u) || (addr >= 0x80000000u && addr < 0xC0000000u);
    if (!seg_ok) {
        return ADDR_CONST_SLOW;
    }
    u32 ph = addr & 0x1FFFFFFFu;
    if (ph < 0x00200000u && !sys->cfg.strict_ram_size && sys->bus.ram) {
        return ADDR_CONST_RAM;
    }
    if (ph >= PSX_SCRATCH_PHYS && ph < (PSX_SCRATCH_PHYS + PSX_SCRATCH_SIZE) &&
        addr < 0xA0000000u && sys->bus.scratch_page) {
        return ADDR_CONST_SCRATCH;
    }
    bool is_load = (op < 0x28u) || (op >= 0x30u && op <= 0x33u);
    if (is_load && ph >= PSX_BIOS_PHYS && ph < (PSX_BIOS_PHYS + PSX_BIOS_SIZE) && sys->bus.bios) {
        return ADDR_CONST_BIOS;
    }
    return ADDR_CONST_SLOW;
}

/* ============================================================================
 * Pass 1: Block Scanner, Constant Propagator, DS Load Peeker & Loop Detector
 * ============================================================================ */

static void scan_acc_reg_usage(u32 insn, u32 reads[32], u32 writes[32]) {
    u32 op = insn >> 26;
    u32 rs = (insn >> 21) & 31u;
    u32 rt = (insn >> 16) & 31u;
    u32 rd = (insn >> 11) & 31u;
    u32 fn = insn & 63u;

    switch (op) {
        case 0x00u: /* SPECIAL */
            switch (fn) {
                case 0x00u: case 0x02u: case 0x03u:
                    if (insn != 0u) { reads[rt]++; writes[rd]++; }
                    break;
                case 0x04u: case 0x06u: case 0x07u:
                case 0x20u: case 0x21u: case 0x22u: case 0x23u:
                case 0x24u: case 0x25u: case 0x26u: case 0x27u:
                case 0x2Au: case 0x2Bu:
                    reads[rs]++; reads[rt]++; writes[rd]++;
                    break;
                case 0x08u: reads[rs]++; break;
                case 0x09u: reads[rs]++; writes[rd]++; break;
                case 0x10u: case 0x12u: writes[rd]++; break;
                case 0x11u: case 0x13u: reads[rs]++; break;
                case 0x18u: case 0x19u: case 0x1Au: case 0x1Bu:
                    reads[rs]++; reads[rt]++;
                    break;
                default:
                    break;
            }
            break;
        case 0x01u: /* REGIMM */
            reads[rs]++;
            if ((rt & 0x1Eu) == 0x10u) writes[31]++;
            break;
        case 0x02u: break;
        case 0x03u: writes[31]++; break;
        case 0x04u: case 0x05u: reads[rs]++; reads[rt]++; break;
        case 0x06u: case 0x07u: reads[rs]++; break;
        case 0x08u: case 0x09u: case 0x0Au: case 0x0Bu:
        case 0x0Cu: case 0x0Du: case 0x0Eu:
            reads[rs]++; writes[rt]++;
            break;
        case 0x0Fu: writes[rt]++; break;
        case 0x10u:
            if (rs == 0x00u) writes[rt]++;
            else if (rs == 0x04u) reads[rt]++;
            break;
        case 0x12u:
            if (rs == 0x00u || rs == 0x02u) writes[rt]++;
            else if (rs == 0x04u || rs == 0x06u) reads[rt]++;
            break;
        case 0x20u: case 0x21u: case 0x23u: case 0x24u: case 0x25u:
            reads[rs]++; writes[rt]++;
            break;
        case 0x22u: case 0x26u:
            reads[rs]++; reads[rt]++; writes[rt]++;
            break;
        case 0x28u: case 0x29u: case 0x2Au: case 0x2Bu: case 0x2Eu:
            reads[rs]++; reads[rt]++;
            break;
        case 0x30u: case 0x31u: case 0x32u: case 0x33u:
        case 0x38u: case 0x39u: case 0x3Au: case 0x3Bu:
            reads[rs]++;
            break;
        default:
            break;
    }
    reads[0] = 0u;
    writes[0] = 0u;
}

static void scan_propagate_insn(const PsxSystem *sys, JitDecodedInsn *dec,
                                u8 known[32], u32 kval[32], u32 *pend_ld_reg) {
    u32 insn = dec->raw;
    u32 op = insn >> 26;
    u32 rs = (insn >> 21) & 31u;
    u32 rt = (insn >> 16) & 31u;
    u32 rd = (insn >> 11) & 31u;
    u32 sh = (insn >> 6) & 31u;
    u32 fn = insn & 63u;
    u32 imm16 = insn & 0xFFFFu;
    u32 simm = (u32)(s32)(s16)imm16;

    known[0] = 1u;
    kval[0] = 0u;

    dec->known_rs = known[rs];
    dec->kval_rs = kval[rs];
    dec->known_rt = known[rt];
    dec->kval_rt = kval[rt];
    dec->addr_class = (u8)ADDR_DYNAMIC;
    dec->const_addr = 0u;

    if (scan_is_mem_op(insn) && dec->known_rs) {
        u32 addr = dec->kval_rs + simm;
        dec->const_addr = addr;
        dec->addr_class = (u8)scan_classify_const_addr(sys, dec->pc, insn, addr);
    }

    u32 wreg = 0u;
    u8  wknown = 0u;
    u32 wval = 0u;
    u32 lreg = 0u;
    bool is_load = scan_is_delayed_load(insn, &lreg);

    if (!is_load) {
        switch (op) {
            case 0x00u: /* SPECIAL */
                switch (fn) {
                    case 0x00u:
                        wreg = rd;
                        if (dec->known_rt) { wknown = 1u; wval = dec->kval_rt << sh; }
                        break;
                    case 0x02u:
                        wreg = rd;
                        if (dec->known_rt) { wknown = 1u; wval = dec->kval_rt >> sh; }
                        break;
                    case 0x03u:
                        wreg = rd;
                        if (dec->known_rt) { wknown = 1u; wval = (u32)((s32)dec->kval_rt >> sh); }
                        break;
                    case 0x04u:
                        wreg = rd;
                        if (dec->known_rt && dec->known_rs) {
                            wknown = 1u; wval = dec->kval_rt << (dec->kval_rs & 31u);
                        }
                        break;
                    case 0x06u:
                        wreg = rd;
                        if (dec->known_rt && dec->known_rs) {
                            wknown = 1u; wval = dec->kval_rt >> (dec->kval_rs & 31u);
                        }
                        break;
                    case 0x07u:
                        wreg = rd;
                        if (dec->known_rt && dec->known_rs) {
                            wknown = 1u; wval = (u32)((s32)dec->kval_rt >> (dec->kval_rs & 31u));
                        }
                        break;
                    case 0x09u: /* JALR */
                        wreg = rd; wknown = 1u; wval = dec->pc + 8u;
                        break;
                    case 0x10u: case 0x12u: /* MFHI / MFLO */
                        wreg = rd; wknown = 0u;
                        break;
                    case 0x20u: /* ADD */
                        wreg = rd;
                        if (dec->known_rs && dec->known_rt) {
                            u32 a = dec->kval_rs, b = dec->kval_rt, r = a + b;
                            if (((~(a ^ b) & (a ^ r)) & 0x80000000u) == 0u) {
                                wknown = 1u; wval = r;
                            }
                        }
                        break;
                    case 0x21u: /* ADDU */
                        wreg = rd;
                        if (dec->known_rs && dec->known_rt) { wknown = 1u; wval = dec->kval_rs + dec->kval_rt; }
                        break;
                    case 0x22u: /* SUB */
                        wreg = rd;
                        if (dec->known_rs && dec->known_rt) {
                            u32 a = dec->kval_rs, b = dec->kval_rt, r = a - b;
                            if ((((a ^ b) & (a ^ r)) & 0x80000000u) == 0u) {
                                wknown = 1u; wval = r;
                            }
                        }
                        break;
                    case 0x23u: /* SUBU */
                        wreg = rd;
                        if (dec->known_rs && dec->known_rt) { wknown = 1u; wval = dec->kval_rs - dec->kval_rt; }
                        break;
                    case 0x24u: /* AND */
                        wreg = rd;
                        if (dec->known_rs && dec->known_rt) { wknown = 1u; wval = dec->kval_rs & dec->kval_rt; }
                        break;
                    case 0x25u: /* OR */
                        wreg = rd;
                        if (dec->known_rs && dec->known_rt) { wknown = 1u; wval = dec->kval_rs | dec->kval_rt; }
                        break;
                    case 0x26u: /* XOR */
                        wreg = rd;
                        if (dec->known_rs && dec->known_rt) { wknown = 1u; wval = dec->kval_rs ^ dec->kval_rt; }
                        break;
                    case 0x27u: /* NOR */
                        wreg = rd;
                        if (dec->known_rs && dec->known_rt) { wknown = 1u; wval = ~(dec->kval_rs | dec->kval_rt); }
                        break;
                    case 0x2Au: /* SLT */
                        wreg = rd;
                        if (dec->known_rs && dec->known_rt) {
                            wknown = 1u; wval = ((s32)dec->kval_rs < (s32)dec->kval_rt) ? 1u : 0u;
                        }
                        break;
                    case 0x2Bu: /* SLTU */
                        wreg = rd;
                        if (dec->known_rs && dec->known_rt) {
                            wknown = 1u; wval = (dec->kval_rs < dec->kval_rt) ? 1u : 0u;
                        }
                        break;
                    default:
                        break;
                }
                break;
            case 0x01u: /* REGIMM */
                if ((rt & 0x1Eu) == 0x10u) {
                    wreg = 31u; wknown = 1u; wval = dec->pc + 8u;
                }
                break;
            case 0x03u: /* JAL */
                wreg = 31u; wknown = 1u; wval = dec->pc + 8u;
                break;
            case 0x08u: /* ADDI */
                wreg = rt;
                if (dec->known_rs) {
                    u32 a = dec->kval_rs, r = a + simm;
                    if (((~(a ^ simm) & (a ^ r)) & 0x80000000u) == 0u) {
                        wknown = 1u; wval = r;
                    }
                }
                break;
            case 0x09u: /* ADDIU */
                wreg = rt;
                if (dec->known_rs) { wknown = 1u; wval = dec->kval_rs + simm; }
                break;
            case 0x0Au: /* SLTI */
                wreg = rt;
                if (dec->known_rs) { wknown = 1u; wval = ((s32)dec->kval_rs < (s32)simm) ? 1u : 0u; }
                break;
            case 0x0Bu: /* SLTIU */
                wreg = rt;
                if (dec->known_rs) { wknown = 1u; wval = (dec->kval_rs < simm) ? 1u : 0u; }
                break;
            case 0x0Cu: /* ANDI */
                wreg = rt;
                if (dec->known_rs) { wknown = 1u; wval = dec->kval_rs & imm16; }
                break;
            case 0x0Du: /* ORI */
                wreg = rt;
                if (dec->known_rs) { wknown = 1u; wval = dec->kval_rs | imm16; }
                break;
            case 0x0Eu: /* XORI */
                wreg = rt;
                if (dec->known_rs) { wknown = 1u; wval = dec->kval_rs ^ imm16; }
                break;
            case 0x0Fu: /* LUI */
                wreg = rt; wknown = 1u; wval = imm16 << 16;
                break;
            default:
                break;
        }
    }

    /* Update 1-slot load-delay pipeline for constant propagation */
    u32 prev_ld = *pend_ld_reg;
    if (prev_ld != 0u) {
        if (prev_ld != wreg) {
            known[prev_ld] = 0u;
            kval[prev_ld] = 0u;
        }
        *pend_ld_reg = 0u;
    }
    if (wreg != 0u) {
        known[wreg] = wknown;
        kval[wreg] = wval;
    }
    if (is_load && lreg != 0u) {
        *pend_ld_reg = lreg;
    }
    known[0] = 1u;
    kval[0] = 0u;
}

/* Peek static branch or fallthrough targets when block ends with a pending load (ARCHITECTURE.md §5.7) */
static void scan_check_ds_load(const PsxSystem *sys, u32 start_pc, JitScanResult *out) {
    if (out->n_insns == 0u) {
        return;
    }

    u32 ld_rt = 0u;
    u32 targets[2];
    u32 n_targets = 0u;

    if (out->has_branch) {
        if (out->n_insns < 2u) {
            return;
        }
        const JitDecodedInsn *br = &out->insns[out->n_insns - 2u];
        const JitDecodedInsn *ds = &out->insns[out->n_insns - 1u];
        if (!scan_is_delayed_load(ds->raw, &ld_rt) || ld_rt == 0u) {
            return;
        }
        u32 op = br->raw >> 26;
        if (op == 0u || out->ends_unlinked) {
            /* JR / JALR or RFE/MTC0: indirect / unlinked target -> must preserve load_delay */
            out->block_flags |= JB_ENDS_LOAD_DS;
            return;
        }
        u32 simm = (u32)(s32)(s16)(br->raw & 0xFFFFu);
        if (op == 0x02u || op == 0x03u) { /* J / JAL */
            targets[0] = ((br->pc + 4u) & 0xF0000000u) | ((br->raw & 0x03FFFFFFu) << 2);
            n_targets = 1u;
        } else {
            targets[0] = (br->pc + 4u) + (simm << 2);
            targets[1] = br->pc + 8u;
            n_targets = 2u;
        }
    } else {
        const JitDecodedInsn *last = &out->insns[out->n_insns - 1u];
        if (!scan_is_delayed_load(last->raw, &ld_rt) || ld_rt == 0u) {
            return;
        }
        if (out->ends_unlinked) {
            out->block_flags |= JB_ENDS_LOAD_DS;
            return;
        }
        targets[0] = start_pc + out->n_insns * 4u;
        n_targets = 1u;
    }

    u16 trial_pages[2] = { out->ram_pages[0], out->ram_pages[1] };
    for (u32 t = 0u; t < n_targets; ++t) {
        u32 t_pc = targets[t];
        if (jit_is_pinned_pc(sys, t_pc)) {
            out->block_flags |= JB_ENDS_LOAD_DS;
            return;
        }
        u32 t_insn = 0u;
        if (!scan_peek32(sys, t_pc, &t_insn)) {
            out->block_flags |= JB_ENDS_LOAD_DS;
            return;
        }
        if (scan_insn_reads_gpr(t_insn, ld_rt)) {
            out->block_flags |= JB_ENDS_LOAD_DS;
            return;
        }
        if (jit_pc_is_ram(sys, t_pc)) {
            u16 pg = (u16)((t_pc & PSX_RAM_MASK) >> 12);
            if (!scan_try_add_ram_page(trial_pages, pg)) {
                out->block_flags |= JB_ENDS_LOAD_DS;
                return;
            }
        }
    }

    /* Safe to commit DS load directly into gpr[ld_rt] before branch! */
    out->ram_pages[0] = trial_pages[0];
    out->ram_pages[1] = trial_pages[1];
    out->block_flags |= JB_PEEKED_TARGET;
}

static void scan_detect_loops(const PsxSystem *sys, u32 start_pc, JitScanResult *out) {
    if (!sys->cfg.jit_idle_skip || !out->has_branch || out->ends_step_one ||
        out->ends_unlinked || (out->block_flags & JB_ENDS_LOAD_DS) != 0u ||
        out->n_insns < 2u || out->n_insns > 8u) {
        return;
    }

    const JitDecodedInsn *br = &out->insns[out->n_insns - 2u];
    const JitDecodedInsn *ds = &out->insns[out->n_insns - 1u];
    u32 br_op = br->raw >> 26;
    u32 br_rs = (br->raw >> 21) & 31u;
    u32 br_rt = (br->raw >> 16) & 31u;
    u32 simm = (u32)(s32)(s16)(br->raw & 0xFFFFu);

    u32 target = 0xFFFFFFFFu;
    if (br_op == 0x02u) { /* J (not JAL, which writes $ra) */
        target = ((br->pc + 4u) & 0xF0000000u) | ((br->raw & 0x03FFFFFFu) << 2);
    } else if (br_op == 0x01u && (br_rt & 0x1Eu) != 0x10u) {
        target = (br->pc + 4u) + (simm << 2);
    } else if (br_op >= 0x04u && br_op <= 0x07u) {
        target = (br->pc + 4u) + (simm << 2);
    }
    if (target != start_pc) {
        return;
    }

    /* 1. Counted delay-loop: loop: addiu rX, rX, -1; bne rX, $0, loop; nop */
    if (out->n_insns == 3u) {
        u32 i0 = out->insns[0].raw;
        u32 op0 = i0 >> 26;
        u32 rs0 = (i0 >> 21) & 31u;
        u32 rt0 = (i0 >> 16) & 31u;
        u32 imm0 = i0 & 0xFFFFu;
        if (op0 == 0x09u && rs0 == rt0 && rt0 != 0u && imm0 == 0xFFFFu &&
            br_op == 0x05u &&
            ((br_rs == rt0 && br_rt == 0u) || (br_rt == rt0 && br_rs == 0u)) &&
            ds->raw == 0u) {
            out->counted_loop = true;
            out->counted_reg = (u8)rt0;
            return;
        }
    }

    /* 2. Tight polling idle loop (ARCHITECTURE.md §5.10) */
    u32 written_mask = 0u;
    u32 load_count = 0u;
    u32 load_base_reg = 0u;

    for (u32 i = 0u; i < out->n_insns; ++i) {
        u32 insn = out->insns[i].raw;
        u32 op = insn >> 26;
        u32 rs = (insn >> 21) & 31u;
        u32 rt = (insn >> 16) & 31u;
        u32 rd = (insn >> 11) & 31u;
        u32 fn = insn & 63u;

        /* Reject stores, COP0/1/2/3, link branches, SYSCALL/BREAK, MULT/DIV, MTHI/MTLO */
        if (op >= 0x28u || (op >= 0x10u && op <= 0x13u) || op == 0x03u) {
            return;
        }
        if (op == 0x01u && (rt & 0x1Eu) == 0x10u) {
            return;
        }
        if (op == 0x00u && (fn == 0x08u || fn == 0x09u || fn == 0x0Cu || fn == 0x0Du ||
                            (fn >= 0x10u && fn <= 0x1Bu) || fn == 0x20u || fn == 0x22u)) {
            return;
        }
        if (op == 0x08u || op == 0x22u || op == 0x26u) {
            return;
        }

        if (op >= 0x20u && op <= 0x25u) {
            load_count++;
            if (load_count > 1u) {
                return;
            }
            load_base_reg = rs;
            if (rt != 0u) {
                written_mask |= (1u << rt);
            }
        } else if (op == 0x00u && insn != 0u) {
            if ((fn >= 0x04u && fn <= 0x07u) || (fn >= 0x21u && fn <= 0x2Bu)) {
                if (rs == rd && ((1u << rs) & ~written_mask) == 0u) {
                    /* ok */
                } else if (rs == rd) {
                    /* Accumulating counter -> not pure polling loop */
                    return;
                }
            }
            if (rd != 0u) {
                written_mask |= (1u << rd);
            }
        } else if (op >= 0x09u && op <= 0x0Eu) {
            if (rs == rt && ((written_mask & (1u << rt)) == 0u)) {
                /* Accumulating counter (e.g. addiu rX, rX, 1) -> not static polling */
                return;
            }
            if (rt != 0u) {
                written_mask |= (1u << rt);
            }
        } else if (op == 0x0Fu) {
            if (rt != 0u) {
                written_mask |= (1u << rt);
            }
        }
    }

    if (load_count == 1u && (written_mask & (1u << load_base_reg)) != 0u) {
        /* Unless load_base_reg was established via LUI (+ ORI) constant inside the loop */
        const JitDecodedInsn *ld_dec = NULL;
        for (u32 i = 0u; i < out->n_insns; ++i) {
            u32 op = out->insns[i].raw >> 26;
            if (op >= 0x20u && op <= 0x25u) {
                ld_dec = &out->insns[i];
                break;
            }
        }
        if (!ld_dec || !ld_dec->known_rs) {
            return;
        }
    }

    out->block_flags |= JB_IDLE_LOOP;
}

void jit_scan_block_limit(const PsxSystem *sys, u32 start_pc, u32 max_insns, JitScanResult *out) {
    if (max_insns == 0u) {
        max_insns = 1u;
    } else if (max_insns > JIT_MAX_BLOCK_INSNS) {
        max_insns = JIT_MAX_BLOCK_INSNS;
    }
    memset(out, 0, sizeof(*out));
    out->ram_pages[0] = JIT_PAGE_NONE;
    out->ram_pages[1] = JIT_PAGE_NONE;
    for (u32 s = 0u; s < JIT_NUM_SLOTS; ++s) {
        out->slot_to_mips[s] = -1;
    }
    for (u32 r = 0u; r < 32u; ++r) {
        out->mips_to_slot[r] = -1;
    }

    bool start_is_ram = jit_pc_is_ram(sys, start_pc);
    bool start_is_bios = !start_is_ram && jit_pc_is_bios(start_pc);
    if (!start_is_ram && !start_is_bios) {
        out->ends_step_one = true;
        out->n_insns = 1u;
        return;
    }

    u32 est_words = 32u;
    for (u32 i = 0u; i < max_insns; ++i) {
        u32 cur_pc = start_pc + i * 4u;
        if (i > 0u && jit_is_pinned_pc(sys, cur_pc)) {
            break;
        }
        if (start_is_ram && !jit_pc_is_ram(sys, cur_pc)) {
            break;
        }
        if (start_is_bios && !jit_pc_is_bios(cur_pc)) {
            break;
        }

        u32 insn = 0u;
        if (!scan_peek32(sys, cur_pc, &insn)) {
            if (i == 0u) {
                out->ends_step_one = true;
                out->n_insns = 1u;
            }
            break;
        }

        u32 cost = scan_estimate_arm_words(sys, start_pc, insn);
        if (i > 0u && (est_words + cost > 700u)) {
            break;
        }

        if (scan_is_branch(insn)) {
            /* BC2F / BC2T: delegate rare COP2 branch cleanly */
            if ((insn >> 26) == 0x12u && ((insn >> 21) & 31u) == 0x08u) {
                if (i == 0u) {
                    out->ends_step_one = true;
                    out->n_insns = 1u;
                }
                break;
            }

            u32 ds_pc = cur_pc + 4u;
            bool ds_region_ok = start_is_ram
                ? (jit_pc_is_ram(sys, ds_pc) && !jit_is_pinned_pc(sys, ds_pc))
                : jit_pc_is_bios(ds_pc);
            bool ds_limit_ok = (i == 0u) ? (i + 1u < JIT_MAX_BLOCK_INSNS) : (i + 1u < max_insns);
            u32 ds_insn = 0u;
            if (!ds_region_ok || !ds_limit_ok || !scan_peek32(sys, ds_pc, &ds_insn)) {
                if (i == 0u) {
                    out->ends_step_one = true;
                    out->n_insns = 1u;
                }
                break;
            }

            /* Branch in delay slot: refuse block before first branch, or step-one at index 0 */
            if (scan_is_branch(ds_insn)) {
                if (i == 0u) {
                    out->ends_step_one = true;
                    out->n_insns = 1u;
                }
                break;
            }

            u32 ds_cost = scan_estimate_arm_words(sys, start_pc, ds_insn);
            if (i > 0u && (est_words + cost + ds_cost > 820u)) {
                break;
            }

            if (start_is_ram) {
                u16 p0 = (u16)((cur_pc & PSX_RAM_MASK) >> 12);
                u16 p1 = (u16)((ds_pc & PSX_RAM_MASK) >> 12);
                u16 tmp[2] = { out->ram_pages[0], out->ram_pages[1] };
                if (!scan_try_add_ram_page(tmp, p0) || !scan_try_add_ram_page(tmp, p1)) {
                    if (i == 0u) {
                        out->ends_step_one = true;
                        out->n_insns = 1u;
                    }
                    break;
                }
                out->ram_pages[0] = tmp[0];
                out->ram_pages[1] = tmp[1];
            }

            out->insns[out->n_insns].raw = insn;
            out->insns[out->n_insns].pc = cur_pc;
            out->insns[out->n_insns].in_delay_slot = 0u;
            out->n_insns++;

            out->insns[out->n_insns].raw = ds_insn;
            out->insns[out->n_insns].pc = ds_pc;
            out->insns[out->n_insns].in_delay_slot = 1u;
            out->n_insns++;

            out->has_branch = true;
            if (scan_is_unlinked_cop0(ds_insn)) {
                out->ends_unlinked = true;
            }
            break;
        }

        if (start_is_ram) {
            u16 pg = (u16)((cur_pc & PSX_RAM_MASK) >> 12);
            if (!scan_try_add_ram_page(out->ram_pages, pg)) {
                break;
            }
        }

        out->insns[out->n_insns].raw = insn;
        out->insns[out->n_insns].pc = cur_pc;
        out->insns[out->n_insns].in_delay_slot = 0u;
        out->n_insns++;
        est_words += cost;

        if (scan_terminates_immediately(insn)) {
            if (scan_is_unlinked_cop0(insn)) {
                out->ends_unlinked = true;
            }
            break;
        }
    }

    if (out->ends_step_one || out->n_insns == 0u) {
        out->ends_step_one = true;
        out->n_insns = 1u;
        out->total_cycles = 0u;
        return;
    }

    /* Pass 1b: Cycle accounting, GTE detection, constant propagation, & GPR scoring */
    u32 reads[32] = { 0 };
    u32 writes[32] = { 0 };
    u8  known[32] = { 0 };
    u32 kval[32] = { 0 };
    u32 pend_ld_reg = 0u;
    u32 mem_ops = 0u;

    known[0] = 1u;
    kval[0] = 0u;

    for (u32 i = 0u; i < out->n_insns; ++i) {
        JitDecodedInsn *dec = &out->insns[i];
        u32 insn = dec->raw;
        u32 op = insn >> 26;

        if (scan_is_mem_op(insn)) {
            mem_ops++;
        }
        /* Cumulative cycles consumed THROUGH instruction i (matches cpu_step_one on fault) */
        dec->cyc_before = (u16)jit_cycles_after(&sys->cfg, i + 1u, mem_ops);

        if (op == 0x12u || op == 0x32u || op == 0x3Au) {
            out->block_flags |= JB_HAS_GTE;
        }

        scan_acc_reg_usage(insn, reads, writes);
        scan_propagate_insn(sys, dec, known, kval, &pend_ld_reg);
    }

    out->n_mem_ops = mem_ops;
    out->total_cycles = jit_scale_cycles(&sys->cfg, out->n_insns, mem_ops);

    /* Check delay-slot / fallthrough load target safety & loop skipping */
    scan_check_ds_load(sys, start_pc, out);
    scan_detect_loops(sys, start_pc, out);

    /* Pass 2: Rank top-7 MIPS GPRs (1..31) by score = reads + 2 * writes */
    u32 scores[32] = { 0 };
    for (u32 r = 1u; r < 32u; ++r) {
        scores[r] = reads[r] + 2u * writes[r];
    }

    for (u32 s = 0u; s < JIT_NUM_SLOTS; ++s) {
        u32 best_reg = 0u;
        u32 best_score = 0u;
        for (u32 r = 1u; r < 32u; ++r) {
            if (out->mips_to_slot[r] < 0 && scores[r] > best_score) {
                best_score = scores[r];
                best_reg = r;
            }
        }
        if (best_reg == 0u || best_score == 0u) {
            break;
        }
        out->slot_to_mips[s] = (s8)best_reg;
        out->mips_to_slot[best_reg] = (s8)s;
    }
}

void jit_scan_block(const PsxSystem *sys, u32 start_pc, JitScanResult *out) {
    jit_scan_block_limit(sys, start_pc, JIT_MAX_BLOCK_INSNS, out);
}

/* ============================================================================
 * 7-Slot Block Register Allocator Primitives (r4-r8, r12, lr)
 * ============================================================================ */

void jit_ra_init(JitRegAlloc *ra, const JitScanResult *scan) {
    for (u32 s = 0u; s < JIT_NUM_SLOTS; ++s) {
        ra->slot_to_mips[s] = scan->slot_to_mips[s];
        ra->state[s] = SLOT_UNLOADED;
    }
    for (u32 r = 0u; r < 32u; ++r) {
        ra->mips_to_slot[r] = scan->mips_to_slot[r];
    }
}

ArmReg jit_ra_get_read(Emitter *e, JitRegAlloc *ra, u32 mips_reg, ArmReg fallback_scratch) {
    if (mips_reg == 0u) {
        emit_const(e, fallback_scratch, 0u);
        return fallback_scratch;
    }
    s8 slot = ra->mips_to_slot[mips_reg];
    if (slot >= 0) {
        ArmReg hw = k_jit_slot_regs[(u32)slot];
        if (ra->state[(u32)slot] == SLOT_UNLOADED) {
            emit_ldr_imm(e, ARM_CC_AL, hw, JIT_REG_CPU, (s32)(PSX_OFF_GPR + mips_reg * 4u));
            ra->state[(u32)slot] = SLOT_CLEAN;
        }
        return hw;
    }
    emit_ldr_imm(e, ARM_CC_AL, fallback_scratch, JIT_REG_CPU, (s32)(PSX_OFF_GPR + mips_reg * 4u));
    return fallback_scratch;
}

ArmReg jit_ra_get_write_buf(JitRegAlloc *ra, u32 mips_reg, ArmReg fallback_scratch) {
    if (mips_reg == 0u) {
        return fallback_scratch;
    }
    s8 slot = ra->mips_to_slot[mips_reg];
    if (slot >= 0) {
        ra->state[(u32)slot] = SLOT_DIRTY;
        return k_jit_slot_regs[(u32)slot];
    }
    return fallback_scratch;
}

void jit_ra_commit_write(Emitter *e, JitRegAlloc *ra, u32 mips_reg, ArmReg reg_written) {
    if (mips_reg == 0u) {
        return; /* Drops writes to $zero */
    }
    s8 slot = ra->mips_to_slot[mips_reg];
    if (slot >= 0) {
        ArmReg hw = k_jit_slot_regs[(u32)slot];
        if (reg_written != hw) {
            emit_mov_reg(e, hw, reg_written);
        }
        ra->state[(u32)slot] = SLOT_DIRTY;
        return;
    }
    emit_str_imm(e, ARM_CC_AL, reg_written, JIT_REG_CPU, (s32)(PSX_OFF_GPR + mips_reg * 4u));
}

u8 jit_ra_dirty_mask(const JitRegAlloc *ra) {
    u8 mask = 0u;
    for (u32 s = 0u; s < JIT_NUM_SLOTS; ++s) {
        if (ra->slot_to_mips[s] > 0 && ra->state[s] == SLOT_DIRTY) {
            mask |= (u8)(1u << s);
        }
    }
    return mask;
}

void jit_ra_emit_mask_spill(Emitter *e, const JitRegAlloc *ra, u8 dirty_mask) {
    for (u32 s = 0u; s < JIT_NUM_SLOTS; ++s) {
        if ((dirty_mask & (1u << s)) != 0u && ra->slot_to_mips[s] > 0) {
            u32 mips_reg = (u32)ra->slot_to_mips[s];
            emit_str_imm(e, ARM_CC_AL, k_jit_slot_regs[s], JIT_REG_CPU,
                         (s32)(PSX_OFF_GPR + mips_reg * 4u));
        }
    }
}

void jit_ra_flush_dirty(Emitter *e, JitRegAlloc *ra) {
    for (u32 s = 0u; s < JIT_NUM_SLOTS; ++s) {
        if (ra->slot_to_mips[s] > 0 && ra->state[s] == SLOT_DIRTY) {
            u32 mips_reg = (u32)ra->slot_to_mips[s];
            emit_str_imm(e, ARM_CC_AL, k_jit_slot_regs[s], JIT_REG_CPU,
                         (s32)(PSX_OFF_GPR + mips_reg * 4u));
            ra->state[s] = SLOT_CLEAN;
        }
    }
}

void jit_ra_spill_and_invalidate_all(Emitter *e, JitRegAlloc *ra) {
    for (u32 s = 0u; s < JIT_NUM_SLOTS; ++s) {
        if (ra->slot_to_mips[s] > 0 && ra->state[s] == SLOT_DIRTY) {
            u32 mips_reg = (u32)ra->slot_to_mips[s];
            emit_str_imm(e, ARM_CC_AL, k_jit_slot_regs[s], JIT_REG_CPU,
                         (s32)(PSX_OFF_GPR + mips_reg * 4u));
        }
        ra->state[s] = SLOT_UNLOADED;
    }
}
