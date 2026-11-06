#include "psx3ds/cpu.h"
#include "psx3ds/system.h"

#include <string.h>

void cpu_init(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    sys->cpu.sys.p = sys;
    cpu_reset(sys);
}

void cpu_reset(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    PsxCpu *c = &sys->cpu;

    /* Preserve arena/system pointer slots initialized by bus_init / jit_init */
    PsxPtrSlot saved_fm_r = c->fastmem_r;
    PsxPtrSlot saved_fm_w = c->fastmem_w;
    PsxPtrSlot saved_ibtc = c->ibtc;
    PsxPtrSlot saved_tab_ram = c->blocktab_ram;
    PsxPtrSlot saved_tab_bios = c->blocktab_bios;
    PsxPtrSlot saved_ram = c->ram_base;
    PsxPtrSlot saved_scratch = c->scratch_base;
    u32 saved_psexe = c->psexe_pending;

    memset(c, 0, sizeof(*c));

    c->fastmem_r = saved_fm_r;
    c->fastmem_w = saved_fm_w;
    c->ibtc = saved_ibtc;
    c->blocktab_ram = saved_tab_ram;
    c->blocktab_bios = saved_tab_bios;
    c->ram_base = saved_ram;
    c->scratch_base = saved_scratch;
    c->sys.p = sys;
    c->psexe_pending = saved_psexe;

    c->pc = PSX_RESET_PC;
    c->next_pc = PSX_RESET_PC + 4u;
    c->cop0[COP0_PRID] = PSX_COP0_PRID_VALUE;
    cpu_set_sr(sys, PSX_SR_BEV);
}

void cpu_state(PsxSystem *sys, StateIO *st) {
    if (!sys || !st) {
        return;
    }
    PsxCpu *c = &sys->cpu;
    for (u32 i = 0u; i < 32u; i++) {
        state_u32(st, &c->gpr[i]);
    }
    state_u32(st, &c->pc);
    state_u32(st, &c->next_pc);
    state_u32(st, &c->hi);
    state_u32(st, &c->lo);
    state_u32(st, (u32 *)&c->cycles_left);
    state_u32(st, (u32 *)&c->cycle_deficit);
    state_u32(st, &c->irq_pending);
    state_u32(st, &c->in_delay_slot);
    state_u32(st, &c->load_delay_reg);
    state_u32(st, &c->load_delay_val);
    state_u32(st, &c->branch_target);
    state_u32(st, &c->branch_taken);
    state_u32(st, (u32 *)&c->slice_len);
    for (u32 i = 0u; i < 16u; i++) {
        state_u32(st, &c->cop0[i]);
    }
    for (u32 i = 0u; i < 32u; i++) {
        state_u32(st, &c->gte_d[i]);
    }
    for (u32 i = 0u; i < 32u; i++) {
        state_u32(st, &c->gte_c[i]);
    }
    state_u64(st, &c->total_cycles);
    state_u32(st, &c->cache_ctrl);

    if (!st->writing) {
        c->gpr[0] = 0u;
        c->cop0[COP0_PRID] = PSX_COP0_PRID_VALUE;
        c->jit_abort = 0u;
        c->in_jit = 0u;
        c->exc_pending = 0u;
        fastmem_set_isolated(&sys->bus, (c->cop0[COP0_SR] & PSX_SR_ISC) != 0u);
        if (sys->cfg.isc_swap_table && sys->bus.fm_w_isc_null) {
            c->fastmem_w.p = ((c->cop0[COP0_SR] & PSX_SR_ISC) != 0u)
                                 ? sys->bus.fm_w_isc_null
                                 : sys->bus.fm_w;
        } else {
            c->fastmem_w.p = sys->bus.fm_w;
        }
        cpu_check_irq(sys);
    }
}

void cpu_shutdown(PsxSystem *sys) {
    (void)sys;
}

PSX_INLINE void interp_raise(PsxSystem *sys, u32 code, u32 insn_pc, u32 was_in_ds,
                             u32 ds_branch_target, u32 badvaddr, u32 ce) {
    u32 epc = (was_in_ds != 0u) ? (insn_pc - 4u) : insn_pc;
    cpu_raise_exception(sys, code, epc, was_in_ds != 0u, ds_branch_target, badvaddr, ce);
}

void cpu_step_one(PsxSystem *sys) {
    PsxCpu *c = &sys->cpu;
    c->exc_pending = 0u;

    /* BIOS A0/B0/C0 call vector & PS-EXE 0x80030000 shell injection hook */
    u32 phys_pc = c->pc & 0x1FFFFFFFu;
    if (PSX_UNLIKELY(phys_pc == 0xA0u || phys_pc == 0xB0u || phys_pc == 0xC0u ||
                     (c->psexe_pending != 0u && phys_pc == 0x30000u))) {
        if ((sys->bios.hle || (c->psexe_pending != 0u && phys_pc == 0x30000u)) &&
            c->load_delay_reg != 0u) {
            c->gpr[c->load_delay_reg] = c->load_delay_val;
            c->load_delay_reg = 0u;
            c->load_delay_val = 0u;
            c->gpr[0] = 0u;
        }
        if (bios_pc_hook(sys, c->pc)) {
            u64 k = sys->stats.insns_total++;
            u32 mult = (sys->cfg.cycle_mult != 0u) ? (u32)sys->cfg.cycle_mult : 256u;
            s32 step_cyc = (s32)(((k + 1u) * (u64)mult) >> 8) - (s32)((k * (u64)mult) >> 8);
            c->cycles_left -= step_cyc;
            c->next_pc = c->pc + 4u;
            c->in_delay_slot = 0u;
            c->branch_taken = 0u;
            return;
        }
    }

    u32 insn_pc = c->pc;
    u32 was_in_ds = c->in_delay_slot;
    u32 ds_branch_target = c->branch_target;

    /* Charge instruction cycles (exact 8.8 fixed-point step) */
    u64 k = sys->stats.insns_total++;
    u32 mult = (sys->cfg.cycle_mult != 0u) ? (u32)sys->cfg.cycle_mult : 256u;
    s32 step_cyc = (s32)(((k + 1u) * (u64)mult) >> 8) - (s32)((k * (u64)mult) >> 8);
    c->cycles_left -= step_cyc;

    u32 insn = psx_bus_fetch32(c, insn_pc);
    if (PSX_UNLIKELY(c->exc_pending != 0u)) {
        return;
    }

    u32 prev_ld_reg = c->load_delay_reg;
    u32 prev_ld_val = c->load_delay_val;

    /* Default pipeline advance (naturally handles branch-in-delay-slot too!) */
    u32 fallthrough_pc = c->next_pc;
    u32 new_next_pc = fallthrough_pc + 4u;
    u32 new_in_ds = 0u;
    u32 new_br_target = 0u;
    u32 new_br_taken = 0u;

    u32 direct_reg = 0u;
    u32 direct_val = 0u;
    u32 new_ld_reg = 0u;
    u32 new_ld_val = 0u;

    u32 op = insn >> 26;
    u32 rs = (insn >> 21) & 0x1Fu;
    u32 rt = (insn >> 16) & 0x1Fu;
    u32 rd = (insn >> 11) & 0x1Fu;
    u32 sa = (insn >> 6) & 0x1Fu;
    u32 imm = insn & 0xFFFFu;
    u32 simm = (u32)(s32)(s16)imm;

    switch (op) {
        case 0x00u: { /* SPECIAL */
            u32 fn = insn & 0x3Fu;
            switch (fn) {
                case 0x00u: /* SLL */
                    direct_reg = rd;
                    direct_val = c->gpr[rt] << sa;
                    break;
                case 0x02u: /* SRL */
                    direct_reg = rd;
                    direct_val = c->gpr[rt] >> sa;
                    break;
                case 0x03u: /* SRA */
                    direct_reg = rd;
                    direct_val = (u32)((s32)c->gpr[rt] >> sa);
                    break;
                case 0x04u: /* SLLV */
                    direct_reg = rd;
                    direct_val = c->gpr[rt] << (c->gpr[rs] & 31u);
                    break;
                case 0x06u: /* SRLV */
                    direct_reg = rd;
                    direct_val = c->gpr[rt] >> (c->gpr[rs] & 31u);
                    break;
                case 0x07u: /* SRAV */
                    direct_reg = rd;
                    direct_val = (u32)((s32)c->gpr[rt] >> (c->gpr[rs] & 31u));
                    break;
                case 0x08u: { /* JR */
                    u32 tgt = c->gpr[rs];
                    new_in_ds = 1u;
                    new_br_taken = 1u;
                    new_br_target = tgt;
                    new_next_pc = tgt;
                    break;
                }
                case 0x09u: { /* JALR (read rs BEFORE writing rd: safe when rd == rs) */
                    u32 tgt = c->gpr[rs];
                    direct_reg = rd;
                    direct_val = fallthrough_pc + 4u;
                    new_in_ds = 1u;
                    new_br_taken = 1u;
                    new_br_target = tgt;
                    new_next_pc = tgt;
                    break;
                }
                case 0x0Cu: /* SYSCALL */
                    interp_raise(sys, PSX_EXC_SYS, insn_pc, was_in_ds, ds_branch_target, 0u, 0u);
                    return;
                case 0x0Du: /* BREAK */
                    interp_raise(sys, PSX_EXC_BP, insn_pc, was_in_ds, ds_branch_target, 0u, 0u);
                    return;
                case 0x10u: /* MFHI */
                    direct_reg = rd;
                    direct_val = c->hi;
                    break;
                case 0x11u: /* MTHI */
                    c->hi = c->gpr[rs];
                    break;
                case 0x12u: /* MFLO */
                    direct_reg = rd;
                    direct_val = c->lo;
                    break;
                case 0x13u: /* MTLO */
                    c->lo = c->gpr[rs];
                    break;
                case 0x18u: { /* MULT */
                    s64 prod = (s64)(s32)c->gpr[rs] * (s64)(s32)c->gpr[rt];
                    c->lo = (u32)(u64)prod;
                    c->hi = (u32)((u64)prod >> 32);
                    break;
                }
                case 0x19u: { /* MULTU */
                    u64 prod = (u64)c->gpr[rs] * (u64)c->gpr[rt];
                    c->lo = (u32)prod;
                    c->hi = (u32)(prod >> 32);
                    break;
                }
                case 0x1Au: { /* DIV (exact R3000A hardware truth table) */
                    u32 a = c->gpr[rs];
                    u32 b = c->gpr[rt];
                    if (b == 0u) {
                        c->lo = ((s32)a < 0) ? 1u : 0xFFFFFFFFu;
                        c->hi = a;
                    } else if (a == 0x80000000u && b == 0xFFFFFFFFu) {
                        c->lo = 0x80000000u;
                        c->hi = 0u;
                    } else {
                        c->lo = (u32)((s32)a / (s32)b);
                        c->hi = (u32)((s32)a % (s32)b);
                    }
                    break;
                }
                case 0x1Bu: { /* DIVU */
                    u32 a = c->gpr[rs];
                    u32 b = c->gpr[rt];
                    if (b == 0u) {
                        c->lo = 0xFFFFFFFFu;
                        c->hi = a;
                    } else {
                        c->lo = a / b;
                        c->hi = a % b;
                    }
                    break;
                }
                case 0x20u: { /* ADD */
                    u32 a = c->gpr[rs];
                    u32 b = c->gpr[rt];
                    u32 res = a + b;
                    if (((~(a ^ b) & (a ^ res)) & 0x80000000u) != 0u) {
                        interp_raise(sys, PSX_EXC_OV, insn_pc, was_in_ds, ds_branch_target, 0u, 0u);
                        return;
                    }
                    direct_reg = rd;
                    direct_val = res;
                    break;
                }
                case 0x21u: /* ADDU */
                    direct_reg = rd;
                    direct_val = c->gpr[rs] + c->gpr[rt];
                    break;
                case 0x22u: { /* SUB */
                    u32 a = c->gpr[rs];
                    u32 b = c->gpr[rt];
                    u32 res = a - b;
                    if ((((a ^ b) & (a ^ res)) & 0x80000000u) != 0u) {
                        interp_raise(sys, PSX_EXC_OV, insn_pc, was_in_ds, ds_branch_target, 0u, 0u);
                        return;
                    }
                    direct_reg = rd;
                    direct_val = res;
                    break;
                }
                case 0x23u: /* SUBU */
                    direct_reg = rd;
                    direct_val = c->gpr[rs] - c->gpr[rt];
                    break;
                case 0x24u: /* AND */
                    direct_reg = rd;
                    direct_val = c->gpr[rs] & c->gpr[rt];
                    break;
                case 0x25u: /* OR */
                    direct_reg = rd;
                    direct_val = c->gpr[rs] | c->gpr[rt];
                    break;
                case 0x26u: /* XOR */
                    direct_reg = rd;
                    direct_val = c->gpr[rs] ^ c->gpr[rt];
                    break;
                case 0x27u: /* NOR */
                    direct_reg = rd;
                    direct_val = ~(c->gpr[rs] | c->gpr[rt]);
                    break;
                case 0x2Au: /* SLT */
                    direct_reg = rd;
                    direct_val = ((s32)c->gpr[rs] < (s32)c->gpr[rt]) ? 1u : 0u;
                    break;
                case 0x2Bu: /* SLTU */
                    direct_reg = rd;
                    direct_val = (c->gpr[rs] < c->gpr[rt]) ? 1u : 0u;
                    break;
                default:
                    interp_raise(sys, PSX_EXC_RI, insn_pc, was_in_ds, ds_branch_target, 0u, 0u);
                    return;
            }
            break;
        }

        case 0x01u: { /* REGIMM: BLTZ/BGEZ/BLTZAL/BGEZAL + undocumented aliases 0x00..0x1F */
            s32 sv = (s32)c->gpr[rs];
            bool taken = ((rt & 1u) != 0u) ? (sv >= 0) : (sv < 0);
            if ((rt & 0x1Eu) == 0x10u) {
                /* BLTZAL (0x10) / BGEZAL (0x11) write $ra unconditionally, AFTER reading rs */
                direct_reg = 31u;
                direct_val = fallthrough_pc + 4u;
            }
            u32 tgt = fallthrough_pc + (simm << 2);
            new_in_ds = 1u;
            new_br_taken = taken ? 1u : 0u;
            new_br_target = taken ? tgt : (fallthrough_pc + 4u);
            new_next_pc = new_br_target;
            break;
        }

        case 0x02u: { /* J */
            u32 tgt = (fallthrough_pc & 0xF0000000u) | ((insn & 0x03FFFFFFu) << 2);
            new_in_ds = 1u;
            new_br_taken = 1u;
            new_br_target = tgt;
            new_next_pc = tgt;
            break;
        }

        case 0x03u: { /* JAL */
            u32 tgt = (fallthrough_pc & 0xF0000000u) | ((insn & 0x03FFFFFFu) << 2);
            direct_reg = 31u;
            direct_val = fallthrough_pc + 4u;
            new_in_ds = 1u;
            new_br_taken = 1u;
            new_br_target = tgt;
            new_next_pc = tgt;
            break;
        }

        case 0x04u: /* BEQ */
        case 0x05u: /* BNE */
        case 0x06u: /* BLEZ */
        case 0x07u: { /* BGTZ */
            bool taken;
            if (op == 0x04u) taken = (c->gpr[rs] == c->gpr[rt]);
            else if (op == 0x05u) taken = (c->gpr[rs] != c->gpr[rt]);
            else if (op == 0x06u) taken = ((s32)c->gpr[rs] <= 0);
            else taken = ((s32)c->gpr[rs] > 0);

            u32 tgt = fallthrough_pc + (simm << 2);
            new_in_ds = 1u;
            new_br_taken = taken ? 1u : 0u;
            new_br_target = taken ? tgt : (fallthrough_pc + 4u);
            new_next_pc = new_br_target;
            break;
        }

        case 0x08u: { /* ADDI */
            u32 a = c->gpr[rs];
            u32 res = a + simm;
            if (((~(a ^ simm) & (a ^ res)) & 0x80000000u) != 0u) {
                interp_raise(sys, PSX_EXC_OV, insn_pc, was_in_ds, ds_branch_target, 0u, 0u);
                return;
            }
            direct_reg = rt;
            direct_val = res;
            break;
        }
        case 0x09u: /* ADDIU */
            direct_reg = rt;
            direct_val = c->gpr[rs] + simm;
            break;
        case 0x0Au: /* SLTI */
            direct_reg = rt;
            direct_val = ((s32)c->gpr[rs] < (s32)simm) ? 1u : 0u;
            break;
        case 0x0Bu: /* SLTIU */
            direct_reg = rt;
            direct_val = (c->gpr[rs] < simm) ? 1u : 0u;
            break;
        case 0x0Cu: /* ANDI */
            direct_reg = rt;
            direct_val = c->gpr[rs] & imm;
            break;
        case 0x0Du: /* ORI */
            direct_reg = rt;
            direct_val = c->gpr[rs] | imm;
            break;
        case 0x0Eu: /* XORI */
            direct_reg = rt;
            direct_val = c->gpr[rs] ^ imm;
            break;
        case 0x0Fu: /* LUI */
            direct_reg = rt;
            direct_val = imm << 16;
            break;

        case 0x10u: { /* COP0 */
            u32 sr = c->cop0[COP0_SR];
            if (((sr & PSX_SR_CU0) == 0u) && ((sr & PSX_SR_KUC) != 0u)) {
                interp_raise(sys, PSX_EXC_CPU, insn_pc, was_in_ds, ds_branch_target, 0u, 0u);
                return;
            }
            if ((rs & 0x10u) != 0u) {
                if ((insn & 0x3Fu) == 0x10u) { /* RFE */
                    cpu_set_sr(sys, (sr & ~0x0Fu) | ((sr >> 2) & 0x0Fu));
                }
                /* Other COP0 cofun encodings (e.g. 0x43E00000 in cop.exe) are NOPs */
            } else {
                switch (rs) {
                    case 0x00u: /* MFC0 */
                        new_ld_reg = rt;
                        new_ld_val = (rd < 16u) ? c->cop0[rd] : 0u;
                        break;
                    case 0x04u: { /* MTC0 */
                        u32 v = c->gpr[rt];
                        if (rd == COP0_SR) {
                            cpu_set_sr(sys, v);
                        } else if (rd == COP0_CAUSE) {
                            cpu_set_cause(sys, v);
                        } else if (rd < 16u && rd != COP0_JUMPDEST &&
                                   rd != COP0_BADVADDR && rd != COP0_PRID) {
                            c->cop0[rd] = v;
                        }
                        break;
                    }
                    default:
                        interp_raise(sys, PSX_EXC_RI, insn_pc, was_in_ds, ds_branch_target, 0u, 0u);
                        return;
                }
            }
            break;
        }

        case 0x11u: /* COP1 (disabled -> CpU ce=1; enabled -> NOP per cop.exe) */
            if ((c->cop0[COP0_SR] & PSX_SR_CU1) == 0u) {
                interp_raise(sys, PSX_EXC_CPU, insn_pc, was_in_ds, ds_branch_target, 0u, 1u);
                return;
            }
            break;

        case 0x12u: { /* COP2 (GTE) */
            if ((c->cop0[COP0_SR] & PSX_SR_CU2) == 0u) {
                interp_raise(sys, PSX_EXC_CPU, insn_pc, was_in_ds, ds_branch_target, 0u, 2u);
                return;
            }
            if ((rs & 0x10u) != 0u) {
                sys->stats.gte_helper++;
                gte_exec_c(c, insn);
            } else {
                switch (rs) {
                    case 0x00u: /* MFC2 */
                        new_ld_reg = rt;
                        new_ld_val = gte_read_data(c, rd);
                        break;
                    case 0x02u: /* CFC2 */
                        new_ld_reg = rt;
                        new_ld_val = gte_read_ctrl(c, rd);
                        break;
                    case 0x04u: /* MTC2 */
                        gte_write_data(c, rd, c->gpr[rt]);
                        break;
                    case 0x06u: /* CTC2 */
                        gte_write_ctrl(c, rd, c->gpr[rt]);
                        break;
                    case 0x08u: { /* BC2F / BC2T (Cop2 condition line is constant false on PSX) */
                        bool taken = ((rt & 1u) == 0u);
                        u32 tgt = fallthrough_pc + (simm << 2);
                        new_in_ds = 1u;
                        new_br_taken = taken ? 1u : 0u;
                        new_br_target = taken ? tgt : (fallthrough_pc + 4u);
                        new_next_pc = new_br_target;
                        break;
                    }
                    default:
                        interp_raise(sys, PSX_EXC_RI, insn_pc, was_in_ds, ds_branch_target, 0u, 0u);
                        return;
                }
            }
            break;
        }

        case 0x13u: /* COP3 (disabled -> CpU ce=3; enabled -> NOP per cop.exe) */
            if ((c->cop0[COP0_SR] & PSX_SR_CU3) == 0u) {
                interp_raise(sys, PSX_EXC_CPU, insn_pc, was_in_ds, ds_branch_target, 0u, 3u);
                return;
            }
            break;

        case 0x20u: { /* LB */
            c->cycles_left -= (s32)sys->cfg.cycles_per_mem;
            u32 addr = c->gpr[rs] + simm;
            u8 v = psx_read8(c, addr);
            if (PSX_UNLIKELY(c->exc_pending != 0u)) return;
            new_ld_reg = rt;
            new_ld_val = (u32)(s32)(s8)v;
            break;
        }
        case 0x21u: { /* LH */
            c->cycles_left -= (s32)sys->cfg.cycles_per_mem;
            u32 addr = c->gpr[rs] + simm;
            u16 v = psx_read16(c, addr);
            if (PSX_UNLIKELY(c->exc_pending != 0u)) return;
            new_ld_reg = rt;
            new_ld_val = (u32)(s32)(s16)v;
            break;
        }
        case 0x22u: /* LWL */
        case 0x26u: { /* LWR */
            c->cycles_left -= (s32)sys->cfg.cycles_per_mem;
            u32 addr = c->gpr[rs] + simm;
            if (PSX_UNLIKELY(((c->cop0[COP0_SR] & PSX_SR_KUC) != 0u) &&
                             ((addr & 0x80000000u) != 0u))) {
                interp_raise(sys, PSX_EXC_ADEL, insn_pc, was_in_ds, ds_branch_target, addr, 0u);
                return;
            }
            u32 mem = psx_read32(c, addr & ~3u);
            if (PSX_UNLIKELY(c->exc_pending != 0u)) return;

            /* MEM->MEM bypass: merge against pending load when targeting same $rt */
            u32 cur = (prev_ld_reg == rt && rt != 0u) ? prev_ld_val : c->gpr[rt];
            u32 shift = addr & 3u;
            u32 merged;
            if (op == 0x22u) { /* LWL (little-endian) */
                switch (shift) {
                    case 0u: merged = (cur & 0x00FFFFFFu) | (mem << 24); break;
                    case 1u: merged = (cur & 0x0000FFFFu) | (mem << 16); break;
                    case 2u: merged = (cur & 0x000000FFu) | (mem << 8);  break;
                    default: merged = mem;                               break;
                }
            } else { /* LWR (little-endian) */
                switch (shift) {
                    case 0u: merged = mem;                               break;
                    case 1u: merged = (cur & 0xFF000000u) | (mem >> 8);  break;
                    case 2u: merged = (cur & 0xFFFF0000u) | (mem >> 16); break;
                    default: merged = (cur & 0xFFFFFF00u) | (mem >> 24); break;
                }
            }
            new_ld_reg = rt;
            new_ld_val = merged;
            break;
        }
        case 0x23u: { /* LW */
            c->cycles_left -= (s32)sys->cfg.cycles_per_mem;
            u32 addr = c->gpr[rs] + simm;
            u32 v = psx_read32(c, addr);
            if (PSX_UNLIKELY(c->exc_pending != 0u)) return;
            new_ld_reg = rt;
            new_ld_val = v;
            break;
        }
        case 0x24u: { /* LBU */
            c->cycles_left -= (s32)sys->cfg.cycles_per_mem;
            u32 addr = c->gpr[rs] + simm;
            u8 v = psx_read8(c, addr);
            if (PSX_UNLIKELY(c->exc_pending != 0u)) return;
            new_ld_reg = rt;
            new_ld_val = (u32)v;
            break;
        }
        case 0x25u: { /* LHU */
            c->cycles_left -= (s32)sys->cfg.cycles_per_mem;
            u32 addr = c->gpr[rs] + simm;
            u16 v = psx_read16(c, addr);
            if (PSX_UNLIKELY(c->exc_pending != 0u)) return;
            new_ld_reg = rt;
            new_ld_val = (u32)v;
            break;
        }

        case 0x28u: { /* SB */
            c->cycles_left -= (s32)sys->cfg.cycles_per_mem;
            u32 addr = c->gpr[rs] + simm;
            psx_write8(c, addr, (u8)c->gpr[rt]);
            if (PSX_UNLIKELY(c->exc_pending != 0u)) return;
            break;
        }
        case 0x29u: { /* SH */
            c->cycles_left -= (s32)sys->cfg.cycles_per_mem;
            u32 addr = c->gpr[rs] + simm;
            psx_write16(c, addr, (u16)c->gpr[rt]);
            if (PSX_UNLIKELY(c->exc_pending != 0u)) return;
            break;
        }
        case 0x2Au: /* SWL */
        case 0x2Eu: { /* SWR */
            c->cycles_left -= (s32)sys->cfg.cycles_per_mem;
            u32 addr = c->gpr[rs] + simm;
            if (PSX_UNLIKELY(((c->cop0[COP0_SR] & PSX_SR_KUC) != 0u) &&
                             ((addr & 0x80000000u) != 0u))) {
                interp_raise(sys, PSX_EXC_ADES, insn_pc, was_in_ds, ds_branch_target, addr, 0u);
                return;
            }
            if (PSX_UNLIKELY((c->cop0[COP0_SR] & PSX_SR_ISC) != 0u)) {
                c->icache_tag_dummy[(addr >> 4) & 0xFFu] = c->gpr[rt];
                break;
            }
            u32 aligned = addr & ~3u;
            u32 mem = psx_read32(c, aligned);
            if (PSX_UNLIKELY(c->exc_pending != 0u)) return;

            u32 val = c->gpr[rt];
            u32 shift = addr & 3u;
            u32 merged;
            if (op == 0x2Au) { /* SWL (little-endian) */
                switch (shift) {
                    case 0u: merged = (mem & 0xFFFFFF00u) | (val >> 24); break;
                    case 1u: merged = (mem & 0xFFFF0000u) | (val >> 16); break;
                    case 2u: merged = (mem & 0xFF000000u) | (val >> 8);  break;
                    default: merged = val;                               break;
                }
            } else { /* SWR (little-endian) */
                switch (shift) {
                    case 0u: merged = val;                               break;
                    case 1u: merged = (mem & 0x000000FFu) | (val << 8);  break;
                    case 2u: merged = (mem & 0x0000FFFFu) | (val << 16); break;
                    default: merged = (mem & 0x00FFFFFFu) | (val << 24); break;
                }
            }
            psx_write32(c, aligned, merged);
            if (PSX_UNLIKELY(c->exc_pending != 0u)) return;
            break;
        }
        case 0x2Bu: { /* SW */
            c->cycles_left -= (s32)sys->cfg.cycles_per_mem;
            u32 addr = c->gpr[rs] + simm;
            psx_write32(c, addr, c->gpr[rt]);
            if (PSX_UNLIKELY(c->exc_pending != 0u)) return;
            break;
        }

        case 0x30u: /* LWC0 */
        case 0x31u: /* LWC1 */
        case 0x32u: /* LWC2 */
        case 0x33u: { /* LWC3 */
            c->cycles_left -= (s32)sys->cfg.cycles_per_mem;
            u32 copn = op & 3u;
            if ((c->cop0[COP0_SR] & (1u << (28u + copn))) == 0u) {
                interp_raise(sys, PSX_EXC_CPU, insn_pc, was_in_ds, ds_branch_target, 0u, copn);
                return;
            }
            u32 addr = c->gpr[rs] + simm;
            if (PSX_UNLIKELY(psx_addr_fault(c, addr, 4u))) {
                interp_raise(sys, PSX_EXC_ADEL, insn_pc, was_in_ds, ds_branch_target, addr, 0u);
                return;
            }
            if (copn == 2u) {
                u32 v = psx_read32(c, addr);
                if (PSX_UNLIKELY(c->exc_pending != 0u)) return;
                gte_write_data(c, rt, v);
            }
            break;
        }

        case 0x38u: /* SWC0 */
        case 0x39u: /* SWC1 */
        case 0x3Au: /* SWC2 */
        case 0x3Bu: { /* SWC3 */
            c->cycles_left -= (s32)sys->cfg.cycles_per_mem;
            u32 copn = op & 3u;
            if ((c->cop0[COP0_SR] & (1u << (28u + copn))) == 0u) {
                interp_raise(sys, PSX_EXC_CPU, insn_pc, was_in_ds, ds_branch_target, 0u, copn);
                return;
            }
            u32 addr = c->gpr[rs] + simm;
            if (PSX_UNLIKELY(psx_addr_fault(c, addr, 4u))) {
                interp_raise(sys, PSX_EXC_ADES, insn_pc, was_in_ds, ds_branch_target, addr, 0u);
                return;
            }
            if (copn == 2u) {
                u32 v = gte_read_data(c, rt);
                psx_write32(c, addr, v);
                if (PSX_UNLIKELY(c->exc_pending != 0u)) return;
            }
            break;
        }

        default:
            interp_raise(sys, PSX_EXC_RI, insn_pc, was_in_ds, ds_branch_target, 0u, 0u);
            return;
    }

    /* Commit prior delayed load unless overridden by a direct write or consecutive load to same GPR */
    if (prev_ld_reg != 0u && prev_ld_reg != direct_reg && prev_ld_reg != new_ld_reg) {
        c->gpr[prev_ld_reg] = prev_ld_val;
    }
    if (direct_reg != 0u) {
        c->gpr[direct_reg] = direct_val;
    }
    if (new_ld_reg != 0u) {
        c->load_delay_reg = new_ld_reg;
        c->load_delay_val = new_ld_val;
    } else {
        c->load_delay_reg = 0u;
        c->load_delay_val = 0u;
    }
    c->gpr[0] = 0u;

    c->pc = fallthrough_pc;
    c->next_pc = new_next_pc;
    c->in_delay_slot = new_in_ds;
    c->branch_target = new_br_target;
    c->branch_taken = new_br_taken;
}

void cpu_step_until_clean(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    PsxCpu *c = &sys->cpu;
    do {
        sys->stats.step_one_fallbacks++;
        cpu_step_one(sys);
    } while ((c->in_delay_slot != 0u || c->load_delay_reg != 0u) && !sys->quit);
}

void cpu_run_interp(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    PsxCpu *c = &sys->cpu;
    while (c->cycles_left > 0 && !sys->quit && !sys->frame_done) {
        cpu_step_one(sys);
    }
}
