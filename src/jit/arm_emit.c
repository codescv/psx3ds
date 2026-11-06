#include "arm_emit.h"

#include <string.h>

void arm_emit_reset_dual(Emitter *e, void *rw_buf, ptrdiff_t rw_to_rx, u32 max_bytes) {
    if (!e) {
        return;
    }
    e->begin = (u32 *)rw_buf;
    e->cur = (u32 *)rw_buf;
    e->end = (u32 *)((u8 *)rw_buf + max_bytes);
    e->rw_to_rx = rw_to_rx;
    e->overflow = false;
    e->n_lits = 0u;
    e->n_lit_fixups = 0u;
    e->n_labels = 0u;
    e->n_br_fixups = 0u;
}

void arm_emit_reset(Emitter *e, void *rw_buf, u32 max_bytes) {
    arm_emit_reset_dual(e, rw_buf, 0, max_bytes);
}

u16 arm_label_create(Emitter *e) {
    if (e->n_labels >= EMIT_MAX_LABELS) {
        e->overflow = true;
        return 0u;
    }
    u16 id = e->n_labels++;
    e->label_pos[id] = EMIT_LABEL_UNBOUND;
    return id;
}

void arm_label_bind(Emitter *e, u16 label_id) {
    if (label_id >= e->n_labels) {
        e->overflow = true;
        return;
    }
    u32 words = arm_emit_offset_words(e);
    if (words >= 0xFFFFu) {
        e->overflow = true;
        return;
    }
    e->label_pos[label_id] = (u16)words;
}

void emit_b_label(Emitter *e, ArmCond cc, u16 label_id) {
    if (label_id >= e->n_labels) {
        e->overflow = true;
        return;
    }
    u32 insn_idx = arm_emit_offset_words(e);
    if (e->label_pos[label_id] != EMIT_LABEL_UNBOUND) {
        s32 delta = (s32)e->label_pos[label_id] - ((s32)insn_idx + 2);
        emit_u32(e, (((u32)cc) << 28) | 0x0A000000u | ((u32)delta & 0x00FFFFFFu));
        return;
    }
    if (e->n_br_fixups >= EMIT_MAX_FIXUPS || insn_idx >= 0xFFFFu) {
        e->overflow = true;
        return;
    }
    e->br_fixups[e->n_br_fixups].insn_idx = (u16)insn_idx;
    e->br_fixups[e->n_br_fixups].label_id = label_id;
    e->n_br_fixups++;
    emit_u32(e, (((u32)cc) << 28) | 0x0A000000u);
}

bool arm_encode_imm(u32 val, u32 *out_imm12) {
    for (u32 rot = 0u; rot < 16u; ++rot) {
        u32 sh = rot * 2u;
        u32 rol = (sh == 0u) ? val : ((val << sh) | (val >> (32u - sh)));
        if ((rol & ~0xFFu) == 0u) {
            if (out_imm12) {
                *out_imm12 = (rot << 8) | rol;
            }
            return true;
        }
    }
    return false;
}

void emit_ldr_lit_cc(Emitter *e, ArmCond cc, ArmReg rd, u32 val) {
    u16 lit_idx = 0xFFFFu;
    for (u16 i = 0u; i < e->n_lits; ++i) {
        if (e->lit_vals[i] == val) {
            lit_idx = i;
            break;
        }
    }
    if (lit_idx == 0xFFFFu) {
        if (e->n_lits >= EMIT_MAX_LITS) {
            e->overflow = true;
            return;
        }
        lit_idx = e->n_lits++;
        e->lit_vals[lit_idx] = val;
    }
    u32 insn_idx = arm_emit_offset_words(e);
    if (e->n_lit_fixups >= EMIT_MAX_FIXUPS || insn_idx >= 0xFFFFu) {
        e->overflow = true;
        return;
    }
    e->lit_fixups[e->n_lit_fixups].insn_idx = (u16)insn_idx;
    e->lit_fixups[e->n_lit_fixups].lit_idx = lit_idx;
    e->n_lit_fixups++;
    emit_u32(e, (((u32)cc) << 28) | 0x059F0000u | (((u32)rd) << 12));
}

void emit_const_cc(Emitter *e, ArmCond cc, ArmReg rd, u32 val) {
    u32 enc0 = 0u, enc1 = 0u;
    if (arm_encode_imm(val, &enc0)) {
        emit_alu_imm_enc(e, cc, ARM_OP_MOV, 0u, rd, 0u, enc0);
        return;
    }
    if (arm_encode_imm(~val, &enc0)) {
        emit_alu_imm_enc(e, cc, ARM_OP_MVN, 0u, rd, 0u, enc0);
        return;
    }
    /* Try 2-instruction MOV + ORR on ARMv6K (no MOVW/MOVT!) */
    if (val != 0u) {
        u32 bit = (u32)__builtin_ctz(val) & ~1u;
        u32 mask = 0xFFu << bit;
        u32 chunk0 = val & mask;
        u32 rest = val ^ chunk0;
        if (arm_encode_imm(chunk0, &enc0) && arm_encode_imm(rest, &enc1)) {
            emit_alu_imm_enc(e, cc, ARM_OP_MOV, 0u, rd, 0u, enc0);
            emit_alu_imm_enc(e, cc, ARM_OP_ORR, 0u, rd, rd, enc1);
            return;
        }
    }
    /* Try 2-instruction MVN + BIC */
    u32 inv = ~val;
    if (inv != 0u) {
        u32 bit = (u32)__builtin_ctz(inv) & ~1u;
        u32 mask = 0xFFu << bit;
        u32 chunk0 = inv & mask;
        u32 rest = inv ^ chunk0;
        if (arm_encode_imm(chunk0, &enc0) && arm_encode_imm(rest, &enc1)) {
            emit_alu_imm_enc(e, cc, ARM_OP_MVN, 0u, rd, 0u, enc0);
            emit_alu_imm_enc(e, cc, ARM_OP_BIC, 0u, rd, rd, enc1);
            return;
        }
    }
    /* Fallback: 1-instruction PC-relative load from block-end literal pool */
    emit_ldr_lit_cc(e, cc, rd, val);
}

void emit_branch_rx(Emitter *e, ArmCond cc, const void *target_rx) {
    uintptr_t cur_rx = arm_emit_cur_rx(e);
    intptr_t delta = (intptr_t)(uintptr_t)target_rx - (intptr_t)(cur_rx + 8u);
    if (delta >= -33554432 && delta <= 33554428 && ((delta & 3) == 0)) {
        u32 imm24 = ((u32)(delta >> 2)) & 0x00FFFFFFu;
        emit_u32(e, (((u32)cc) << 28) | 0x0A000000u | imm24);
    } else {
        /* Target >32 MiB away (e.g. .text thunk from mmap/FCRAM): 1-insn literal PC load */
        emit_ldr_lit_cc(e, cc, ARM_PC, (u32)(uintptr_t)target_rx);
    }
}

void emit_call(Emitter *e, const void *fn, bool syncs_cycles) {
    emit_push(e, (1u << ARM_R12) | (1u << ARM_LR));
    if (syncs_cycles) {
        emit_str_imm(e, ARM_CC_AL, JIT_REG_CYC, JIT_REG_CPU, PSX_OFF_CYCLES_LEFT);
    }

    uintptr_t cur_rx = arm_emit_cur_rx(e);
    intptr_t delta = (intptr_t)(uintptr_t)fn - (intptr_t)(cur_rx + 8u);
    if (delta >= -33554432 && delta <= 33554428 && ((delta & 3) == 0)) {
        u32 imm24 = ((u32)(delta >> 2)) & 0x00FFFFFFu;
        emit_u32(e, 0xEB000000u | imm24); /* BL fn */
    } else {
        /* r12 was just pushed above, so r0-r3 args stay intact! */
        emit_const(e, ARM_R12, (u32)(uintptr_t)fn);
        emit_blx_reg(e, ARM_CC_AL, ARM_R12);
    }

    if (syncs_cycles) {
        emit_ldr_imm(e, ARM_CC_AL, JIT_REG_CYC, JIT_REG_CPU, PSX_OFF_CYCLES_LEFT);
    }
    emit_ldr_imm(e, ARM_CC_AL, JIT_REG_FMR, JIT_REG_CPU, PSX_OFF_FASTMEM_R);
    emit_pop(e, (1u << ARM_R12) | (1u << ARM_LR));
}

void emit_add_u32(Emitter *e, ArmReg rd, ArmReg rn, u32 imm, ArmReg scratch) {
    if (imm == 0u) {
        emit_mov_reg(e, rd, rn);
        return;
    }
    u32 enc0 = 0u, enc1 = 0u;
    if (arm_encode_imm(imm, &enc0)) {
        emit_alu_imm_enc(e, ARM_CC_AL, ARM_OP_ADD, 0u, rd, rn, enc0);
        return;
    }
    if (arm_encode_imm(0u - imm, &enc0)) {
        emit_alu_imm_enc(e, ARM_CC_AL, ARM_OP_SUB, 0u, rd, rn, enc0);
        return;
    }
    u32 bit = (u32)__builtin_ctz(imm) & ~1u;
    u32 c0 = imm & (0xFFu << bit);
    u32 c1 = imm ^ c0;
    if (arm_encode_imm(c0, &enc0) && arm_encode_imm(c1, &enc1)) {
        emit_alu_imm_enc(e, ARM_CC_AL, ARM_OP_ADD, 0u, rd, rn, enc0);
        emit_alu_imm_enc(e, ARM_CC_AL, ARM_OP_ADD, 0u, rd, rd, enc1);
        return;
    }
    u32 neg = 0u - imm;
    if (neg != 0u) {
        u32 nbit = (u32)__builtin_ctz(neg) & ~1u;
        u32 nc0 = neg & (0xFFu << nbit);
        u32 nc1 = neg ^ nc0;
        if (arm_encode_imm(nc0, &enc0) && arm_encode_imm(nc1, &enc1)) {
            emit_alu_imm_enc(e, ARM_CC_AL, ARM_OP_SUB, 0u, rd, rn, enc0);
            emit_alu_imm_enc(e, ARM_CC_AL, ARM_OP_SUB, 0u, rd, rd, enc1);
            return;
        }
    }
    emit_const(e, scratch, imm);
    emit_alu_reg(e, ARM_CC_AL, ARM_OP_ADD, 0u, rd, rn, scratch);
}

void emit_sub_u32(Emitter *e, ArmReg rd, ArmReg rn, u32 imm, ArmReg scratch) {
    emit_add_u32(e, rd, rn, 0u - imm, scratch);
}

bool arm_emit_finalize(Emitter *e, u32 *out_bytes) {
    if (!e || e->overflow) {
        return false;
    }

    for (u16 i = 0u; i < e->n_br_fixups; ++i) {
        u16 insn_idx = e->br_fixups[i].insn_idx;
        u16 lbl = e->br_fixups[i].label_id;
        if (lbl >= e->n_labels || e->label_pos[lbl] == EMIT_LABEL_UNBOUND) {
            e->overflow = true;
            return false;
        }
        s32 delta = (s32)e->label_pos[lbl] - ((s32)insn_idx + 2);
        e->begin[insn_idx] = (e->begin[insn_idx] & 0xFF000000u) | ((u32)delta & 0x00FFFFFFu);
    }

    u32 pool_base_word = arm_emit_offset_words(e);
    for (u16 i = 0u; i < e->n_lits; ++i) {
        emit_u32(e, e->lit_vals[i]);
    }
    if (e->overflow) {
        return false;
    }

    for (u16 i = 0u; i < e->n_lit_fixups; ++i) {
        u32 insn_idx = e->lit_fixups[i].insn_idx;
        u32 lit_word = pool_base_word + (u32)e->lit_fixups[i].lit_idx;
        s32 byte_off = ((s32)lit_word - ((s32)insn_idx + 2)) * 4;
        s32 mag = (byte_off >= 0) ? byte_off : -byte_off;
        if (mag > 4095) {
            e->overflow = true;
            return false;
        }
        u32 u_bit = (byte_off >= 0) ? (1u << 23) : 0u;
        e->begin[insn_idx] = (e->begin[insn_idx] & 0xFF7FF000u) | u_bit | ((u32)mag & 0xFFFu);
    }

    if (out_bytes) {
        *out_bytes = arm_emit_offset_words(e) * 4u;
    }
    return true;
}
