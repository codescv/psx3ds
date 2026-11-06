#ifndef PSX3DS_ARM_EMIT_H
#define PSX3DS_ARM_EMIT_H

#include "jit_internal.h"

/* ============================================================================
 * Strict ARM11v6K Code Emitter (ARCHITECTURE.md §5.1, §5.6, §5.11)
 * ZERO ARMv7 instructions (no MOVW/MOVT/UBFX/SBFX/BFI/SDIV/UDIV/DMB/Thumb-2).
 * ============================================================================ */

typedef enum ArmReg {
    ARM_R0  = 0,
    ARM_R1  = 1,
    ARM_R2  = 2,
    ARM_R3  = 3,
    ARM_R4  = 4,
    ARM_R5  = 5,
    ARM_R6  = 6,
    ARM_R7  = 7,
    ARM_R8  = 8,
    ARM_R9  = 9,   /* cpu->fastmem_r.p */
    ARM_R10 = 10,  /* cycles_left (s32) */
    ARM_R11 = 11,  /* PsxCpu * */
    ARM_R12 = 12,  /* cached MIPS slot 5 (saved around calls via PUSH {r12, lr}) */
    ARM_SP  = 13,
    ARM_LR  = 14,  /* cached MIPS slot 6 (saved around calls via PUSH {r12, lr}) */
    ARM_PC  = 15
} ArmReg;

#define JIT_REG_CPU ARM_R11
#define JIT_REG_FMR ARM_R9
#define JIT_REG_CYC ARM_R10

typedef enum ArmCond {
    ARM_CC_EQ = 0x0,
    ARM_CC_NE = 0x1,
    ARM_CC_CS = 0x2,
    ARM_CC_HS = 0x2,
    ARM_CC_CC = 0x3,
    ARM_CC_LO = 0x3,
    ARM_CC_MI = 0x4,
    ARM_CC_PL = 0x5,
    ARM_CC_VS = 0x6,
    ARM_CC_VC = 0x7,
    ARM_CC_HI = 0x8,
    ARM_CC_LS = 0x9,
    ARM_CC_GE = 0xA,
    ARM_CC_LT = 0xB,
    ARM_CC_GT = 0xC,
    ARM_CC_LE = 0xD,
    ARM_CC_AL = 0xE
} ArmCond;

typedef enum ArmShift {
    ARM_SH_LSL = 0,
    ARM_SH_LSR = 1,
    ARM_SH_ASR = 2,
    ARM_SH_ROR = 3
} ArmShift;

#define EMIT_MAX_LITS     384u
#define EMIT_MAX_FIXUPS   768u
#define EMIT_MAX_LABELS   512u
#define EMIT_LABEL_UNBOUND 0xFFFFu

typedef struct EmitLitFixup {
    u16 insn_idx;
    u16 lit_idx;
} EmitLitFixup;

typedef struct EmitBrFixup {
    u16 insn_idx;
    u16 label_id;
} EmitBrFixup;

typedef struct Emitter {
    u32         *cur;
    u32         *begin;
    u32         *end;
    ptrdiff_t    rw_to_rx;
    bool         overflow;

    u32          lit_vals[EMIT_MAX_LITS];
    u16          n_lits;
    EmitLitFixup lit_fixups[EMIT_MAX_FIXUPS];
    u16          n_lit_fixups;

    u16          label_pos[EMIT_MAX_LABELS];
    u16          n_labels;
    EmitBrFixup  br_fixups[EMIT_MAX_FIXUPS];
    u16          n_br_fixups;
} Emitter;

void arm_emit_reset(Emitter *e, void *rw_buf, u32 max_bytes);
void arm_emit_reset_dual(Emitter *e, void *rw_buf, ptrdiff_t rw_to_rx, u32 max_bytes);
bool arm_emit_finalize(Emitter *e, u32 *out_bytes);

static inline u32 arm_emit_offset_words(const Emitter *e) {
    return (u32)(e->cur - e->begin);
}

static inline uintptr_t arm_emit_cur_rx(const Emitter *e) {
    return (uintptr_t)((u8 *)e->cur + e->rw_to_rx);
}

static inline void emit_u32(Emitter *e, u32 word) {
    if (PSX_LIKELY(e->cur < e->end)) {
        *e->cur++ = word;
    } else {
        e->overflow = true;
    }
}

/* Labels & intra-block branches */
u16  arm_label_create(Emitter *e);
void arm_label_bind(Emitter *e, u16 label_id);
void emit_b_label(Emitter *e, ArmCond cc, u16 label_id);

/* Immediate encoding & constant synthesis (ARMv6K rotated imm8 + literal pool) */
bool arm_encode_imm(u32 val, u32 *out_imm12);
void emit_ldr_lit_cc(Emitter *e, ArmCond cc, ArmReg rd, u32 val);
void emit_const_cc(Emitter *e, ArmCond cc, ArmReg rd, u32 val);
static inline void emit_const(Emitter *e, ArmReg rd, u32 val) {
    emit_const_cc(e, ARM_CC_AL, rd, val);
}

/* Branch to absolute RX address (uses B<cc> if within +-32 MiB, else LDR<cc> pc, =target) */
void emit_branch_rx(Emitter *e, ArmCond cc, const void *target_rx);

/* Single sanctioned helper call primitive (PUSH {r12,lr}, cycle sync, BL/BLX, r9 reload, POP) */
void emit_call(Emitter *e, const void *fn, bool syncs_cycles);

/* Add/sub arbitrary 32-bit immediate (splits into rotated chunks or scratch r3) */
void emit_add_u32(Emitter *e, ArmReg rd, ArmReg rn, u32 imm, ArmReg scratch);
void emit_sub_u32(Emitter *e, ArmReg rd, ArmReg rn, u32 imm, ArmReg scratch);

/* ---- Core ARMv6K Data-Processing Instructions ---- */
#define ARM_DP_IMM(cc, op, s, rn, rd, imm12) \
    ((((u32)(cc)) << 28) | (1u << 25) | (((u32)(op)) << 21) | (((u32)(s)) << 20) | \
     (((u32)(rn)) << 16) | (((u32)(rd)) << 12) | ((u32)(imm12) & 0xFFFu))

#define ARM_DP_REG_IMM_SH(cc, op, s, rn, rd, sh_imm, sh_type, rm) \
    ((((u32)(cc)) << 28) | (((u32)(op)) << 21) | (((u32)(s)) << 20) | \
     (((u32)(rn)) << 16) | (((u32)(rd)) << 12) | (((u32)(sh_imm) & 31u) << 7) | \
     (((u32)(sh_type) & 3u) << 5) | ((u32)(rm) & 15u))

#define ARM_DP_REG_REG_SH(cc, op, s, rn, rd, rs, sh_type, rm) \
    ((((u32)(cc)) << 28) | (((u32)(op)) << 21) | (((u32)(s)) << 20) | \
     (((u32)(rn)) << 16) | (((u32)(rd)) << 12) | (((u32)(rs) & 15u) << 8) | \
     (((u32)(sh_type) & 3u) << 5) | (1u << 4) | ((u32)(rm) & 15u))

enum ArmDpOp {
    ARM_OP_AND = 0x0,
    ARM_OP_EOR = 0x1,
    ARM_OP_SUB = 0x2,
    ARM_OP_RSB = 0x3,
    ARM_OP_ADD = 0x4,
    ARM_OP_ADC = 0x5,
    ARM_OP_SBC = 0x6,
    ARM_OP_RSC = 0x7,
    ARM_OP_TST = 0x8,
    ARM_OP_TEQ = 0x9,
    ARM_OP_CMP = 0xA,
    ARM_OP_CMN = 0xB,
    ARM_OP_ORR = 0xC,
    ARM_OP_MOV = 0xD,
    ARM_OP_BIC = 0xE,
    ARM_OP_MVN = 0xF
};

static inline void emit_mov_reg_cc(Emitter *e, ArmCond cc, ArmReg rd, ArmReg rm) {
    if (cc == ARM_CC_AL && rd == rm) return;
    emit_u32(e, ARM_DP_REG_IMM_SH(cc, ARM_OP_MOV, 0u, 0u, rd, 0u, ARM_SH_LSL, rm));
}
static inline void emit_mov_reg(Emitter *e, ArmReg rd, ArmReg rm) {
    emit_mov_reg_cc(e, ARM_CC_AL, rd, rm);
}
static inline void emit_mvn_reg(Emitter *e, ArmReg rd, ArmReg rm) {
    emit_u32(e, ARM_DP_REG_IMM_SH(ARM_CC_AL, ARM_OP_MVN, 0u, 0u, rd, 0u, ARM_SH_LSL, rm));
}
static inline void emit_shift_imm(Emitter *e, ArmCond cc, u32 s, ArmReg rd, ArmReg rm, ArmShift sh, u32 amt) {
    if (amt == 0u) {
        emit_u32(e, ARM_DP_REG_IMM_SH(cc, ARM_OP_MOV, s, 0u, rd, 0u, ARM_SH_LSL, rm));
    } else {
        /* Note: LSR #32 / ASR #32 encode with sh_imm = 0 and sh_type = LSR/ASR */
        u32 enc_amt = (amt == 32u) ? 0u : (amt & 31u);
        emit_u32(e, ARM_DP_REG_IMM_SH(cc, ARM_OP_MOV, s, 0u, rd, enc_amt, sh, rm));
    }
}
static inline void emit_shift_reg(Emitter *e, ArmReg rd, ArmReg rm, ArmShift sh, ArmReg rs) {
    emit_u32(e, ARM_DP_REG_REG_SH(ARM_CC_AL, ARM_OP_MOV, 0u, 0u, rd, rs, sh, rm));
}

static inline void emit_alu_reg(Emitter *e, ArmCond cc, enum ArmDpOp op, u32 s,
                                ArmReg rd, ArmReg rn, ArmReg rm) {
    emit_u32(e, ARM_DP_REG_IMM_SH(cc, op, s, rn, rd, 0u, ARM_SH_LSL, rm));
}
static inline void emit_alu_reg_sh(Emitter *e, ArmCond cc, enum ArmDpOp op, u32 s,
                                   ArmReg rd, ArmReg rn, ArmReg rm, ArmShift sh, u32 amt) {
    u32 enc_amt = (amt == 32u) ? 0u : (amt & 31u);
    emit_u32(e, ARM_DP_REG_IMM_SH(cc, op, s, rn, rd, enc_amt, sh, rm));
}
static inline void emit_alu_imm_enc(Emitter *e, ArmCond cc, enum ArmDpOp op, u32 s,
                                    ArmReg rd, ArmReg rn, u32 imm12_enc) {
    emit_u32(e, ARM_DP_IMM(cc, op, s, rn, rd, imm12_enc));
}
static inline void emit_cmp_reg(Emitter *e, ArmCond cc, ArmReg rn, ArmReg rm) {
    emit_u32(e, ARM_DP_REG_IMM_SH(cc, ARM_OP_CMP, 1u, rn, 0u, 0u, ARM_SH_LSL, rm));
}
static inline void emit_cmn_imm_enc(Emitter *e, ArmCond cc, ArmReg rn, u32 imm12_enc) {
    emit_u32(e, ARM_DP_IMM(cc, ARM_OP_CMN, 1u, rn, 0u, imm12_enc));
}
static inline void emit_cmp_imm_enc(Emitter *e, ArmCond cc, ArmReg rn, u32 imm12_enc) {
    emit_u32(e, ARM_DP_IMM(cc, ARM_OP_CMP, 1u, rn, 0u, imm12_enc));
}
static inline void emit_tst_reg(Emitter *e, ArmCond cc, ArmReg rn, ArmReg rm) {
    emit_u32(e, ARM_DP_REG_IMM_SH(cc, ARM_OP_TST, 1u, rn, 0u, 0u, ARM_SH_LSL, rm));
}
static inline void emit_tst_imm_enc(Emitter *e, ArmCond cc, ArmReg rn, u32 imm12_enc) {
    emit_u32(e, ARM_DP_IMM(cc, ARM_OP_TST, 1u, rn, 0u, imm12_enc));
}

/* ---- Single Data Transfer (LDR / STR / LDRB / STRB) ---- */
static inline void emit_ldr_imm(Emitter *e, ArmCond cc, ArmReg rd, ArmReg rn, s32 off) {
    u32 u = (off >= 0) ? 1u : 0u;
    u32 mag = (u32)((off >= 0) ? off : -off) & 0xFFFu;
    emit_u32(e, (((u32)cc) << 28) | 0x05100000u | (u << 23) | (((u32)rn) << 16) | (((u32)rd) << 12) | mag);
}
static inline void emit_str_imm(Emitter *e, ArmCond cc, ArmReg rd, ArmReg rn, s32 off) {
    u32 u = (off >= 0) ? 1u : 0u;
    u32 mag = (u32)((off >= 0) ? off : -off) & 0xFFFu;
    emit_u32(e, (((u32)cc) << 28) | 0x05000000u | (u << 23) | (((u32)rn) << 16) | (((u32)rd) << 12) | mag);
}
static inline void emit_ldr_reg_lsl(Emitter *e, ArmCond cc, ArmReg rd, ArmReg rn, ArmReg rm, u32 lsl) {
    emit_u32(e, (((u32)cc) << 28) | 0x07900000u | (((u32)rn) << 16) | (((u32)rd) << 12) |
                ((lsl & 31u) << 7) | ((u32)rm & 15u));
}
static inline void emit_str_reg_lsl(Emitter *e, ArmCond cc, ArmReg rd, ArmReg rn, ArmReg rm, u32 lsl) {
    emit_u32(e, (((u32)cc) << 28) | 0x07800000u | (((u32)rn) << 16) | (((u32)rd) << 12) |
                ((lsl & 31u) << 7) | ((u32)rm & 15u));
}
static inline void emit_ldrb_reg(Emitter *e, ArmCond cc, ArmReg rd, ArmReg rn, ArmReg rm) {
    emit_u32(e, (((u32)cc) << 28) | 0x07D00000u | (((u32)rn) << 16) | (((u32)rd) << 12) | ((u32)rm & 15u));
}
static inline void emit_strb_reg(Emitter *e, ArmCond cc, ArmReg rd, ArmReg rn, ArmReg rm) {
    emit_u32(e, (((u32)cc) << 28) | 0x07C00000u | (((u32)rn) << 16) | (((u32)rd) << 12) | ((u32)rm & 15u));
}

/* ---- Halfword / Signed Byte Register-Offset Transfers ---- */
static inline void emit_ldrh_reg(Emitter *e, ArmCond cc, ArmReg rd, ArmReg rn, ArmReg rm) {
    emit_u32(e, (((u32)cc) << 28) | 0x019000B0u | (((u32)rn) << 16) | (((u32)rd) << 12) | ((u32)rm & 15u));
}
static inline void emit_ldrsb_reg(Emitter *e, ArmCond cc, ArmReg rd, ArmReg rn, ArmReg rm) {
    emit_u32(e, (((u32)cc) << 28) | 0x019000D0u | (((u32)rn) << 16) | (((u32)rd) << 12) | ((u32)rm & 15u));
}
static inline void emit_ldrsh_reg(Emitter *e, ArmCond cc, ArmReg rd, ArmReg rn, ArmReg rm) {
    emit_u32(e, (((u32)cc) << 28) | 0x019000F0u | (((u32)rn) << 16) | (((u32)rd) << 12) | ((u32)rm & 15u));
}
static inline void emit_strh_reg(Emitter *e, ArmCond cc, ArmReg rd, ArmReg rn, ArmReg rm) {
    emit_u32(e, (((u32)cc) << 28) | 0x018000B0u | (((u32)rn) << 16) | (((u32)rd) << 12) | ((u32)rm & 15u));
}
static inline void emit_ldrh_imm(Emitter *e, ArmCond cc, ArmReg rd, ArmReg rn, u32 off8) {
    u32 hi = (off8 >> 4) & 0xFu;
    u32 lo = off8 & 0xFu;
    emit_u32(e, (((u32)cc) << 28) | 0x01D000B0u | (((u32)rn) << 16) | (((u32)rd) << 12) | (hi << 8) | lo);
}
static inline void emit_ldrsh_imm(Emitter *e, ArmCond cc, ArmReg rd, ArmReg rn, u32 off8) {
    u32 hi = (off8 >> 4) & 0xFu;
    u32 lo = off8 & 0xFu;
    emit_u32(e, (((u32)cc) << 28) | 0x01D000F0u | (((u32)rn) << 16) | (((u32)rd) << 12) | (hi << 8) | lo);
}
static inline void emit_ldrsb_imm(Emitter *e, ArmCond cc, ArmReg rd, ArmReg rn, u32 off8) {
    u32 hi = (off8 >> 4) & 0xFu;
    u32 lo = off8 & 0xFu;
    emit_u32(e, (((u32)cc) << 28) | 0x01D000D0u | (((u32)rn) << 16) | (((u32)rd) << 12) | (hi << 8) | lo);
}
static inline void emit_ldrb_imm(Emitter *e, ArmCond cc, ArmReg rd, ArmReg rn, u32 off12) {
    emit_u32(e, (((u32)cc) << 28) | 0x05D00000u | (((u32)rn) << 16) | (((u32)rd) << 12) | (off12 & 0xFFFu));
}

/* ---- Block Transfers & Stack ---- */
static inline void emit_push(Emitter *e, u32 reg_mask) {
    emit_u32(e, 0xE92D0000u | (reg_mask & 0xFFFFu));
}
static inline void emit_pop(Emitter *e, u32 reg_mask) {
    emit_u32(e, 0xE8BD0000u | (reg_mask & 0xFFFFu));
}
static inline void emit_ldmia(Emitter *e, ArmCond cc, ArmReg rn, bool wb, u32 reg_mask) {
    emit_u32(e, (((u32)cc) << 28) | 0x08900000u | (wb ? (1u << 21) : 0u) |
                (((u32)rn) << 16) | (reg_mask & 0xFFFFu));
}

/* ---- Control Transfers ---- */
static inline void emit_bx(Emitter *e, ArmCond cc, ArmReg rm) {
    emit_u32(e, (((u32)cc) << 28) | 0x012FFF10u | ((u32)rm & 15u));
}
static inline void emit_blx_reg(Emitter *e, ArmCond cc, ArmReg rm) {
    emit_u32(e, (((u32)cc) << 28) | 0x012FFF30u | ((u32)rm & 15u));
}

/* ---- ARMv6K Multiply, 64-bit Dual-MAC, Saturation, Extend & CLZ ---- */
static inline void emit_mul(Emitter *e, ArmCond cc, ArmReg rd, ArmReg rm, ArmReg rs) {
    /* Respect Rd != Rm by swapping commutative operands if needed */
    if (rd == rm) {
        ArmReg tmp = rm; rm = rs; rs = tmp;
    }
    emit_u32(e, (((u32)cc) << 28) | 0x00000090u | (((u32)rd) << 16) |
                (((u32)rs) << 8) | ((u32)rm & 15u));
}
static inline void emit_smull(Emitter *e, ArmCond cc, ArmReg rdlo, ArmReg rdhi, ArmReg rm, ArmReg rs) {
    if (rdlo == rm || rdhi == rm) {
        ArmReg tmp = rm; rm = rs; rs = tmp;
    }
    emit_u32(e, (((u32)cc) << 28) | 0x00C00090u | (((u32)rdhi) << 16) |
                (((u32)rdlo) << 12) | (((u32)rs) << 8) | ((u32)rm & 15u));
}
static inline void emit_umull(Emitter *e, ArmCond cc, ArmReg rdlo, ArmReg rdhi, ArmReg rm, ArmReg rs) {
    if (rdlo == rm || rdhi == rm) {
        ArmReg tmp = rm; rm = rs; rs = tmp;
    }
    emit_u32(e, (((u32)cc) << 28) | 0x00800090u | (((u32)rdhi) << 16) |
                (((u32)rdlo) << 12) | (((u32)rs) << 8) | ((u32)rm & 15u));
}
static inline void emit_smlal(Emitter *e, ArmCond cc, ArmReg rdlo, ArmReg rdhi, ArmReg rm, ArmReg rs) {
    if (rdlo == rm || rdhi == rm) {
        ArmReg tmp = rm; rm = rs; rs = tmp;
    }
    emit_u32(e, (((u32)cc) << 28) | 0x00E00090u | (((u32)rdhi) << 16) |
                (((u32)rdlo) << 12) | (((u32)rs) << 8) | ((u32)rm & 15u));
}
static inline void emit_smlald(Emitter *e, ArmCond cc, ArmReg rdlo, ArmReg rdhi, ArmReg rn, ArmReg rm) {
    emit_u32(e, (((u32)cc) << 28) | 0x07400010u | (((u32)rdhi) << 16) |
                (((u32)rdlo) << 12) | (((u32)rm) << 8) | ((u32)rn & 15u));
}
static inline void emit_smlalxy(Emitter *e, ArmCond cc, ArmReg rdlo, ArmReg rdhi,
                                ArmReg rn, ArmReg rm, bool n_top, bool m_top) {
    u32 op = 0x01400080u | (m_top ? (1u << 6) : 0u) | (n_top ? (1u << 5) : 0u);
    emit_u32(e, (((u32)cc) << 28) | op | (((u32)rdhi) << 16) |
                (((u32)rdlo) << 12) | (((u32)rm) << 8) | ((u32)rn & 15u));
}
static inline void emit_clz(Emitter *e, ArmCond cc, ArmReg rd, ArmReg rm) {
    emit_u32(e, (((u32)cc) << 28) | 0x016F0F10u | (((u32)rd) << 12) | ((u32)rm & 15u));
}
static inline void emit_sxth(Emitter *e, ArmCond cc, ArmReg rd, ArmReg rm, u32 ror_bytes) {
    emit_u32(e, (((u32)cc) << 28) | 0x06BF0070u | (((u32)rd) << 12) |
                ((ror_bytes & 3u) << 10) | ((u32)rm & 15u));
}
static inline void emit_uxth(Emitter *e, ArmCond cc, ArmReg rd, ArmReg rm, u32 ror_bytes) {
    emit_u32(e, (((u32)cc) << 28) | 0x06FF0070u | (((u32)rd) << 12) |
                ((ror_bytes & 3u) << 10) | ((u32)rm & 15u));
}
static inline void emit_sxtb(Emitter *e, ArmCond cc, ArmReg rd, ArmReg rm) {
    emit_u32(e, (((u32)cc) << 28) | 0x06AF0070u | (((u32)rd) << 12) | ((u32)rm & 15u));
}
static inline void emit_uxtb(Emitter *e, ArmCond cc, ArmReg rd, ArmReg rm) {
    emit_u32(e, (((u32)cc) << 28) | 0x06EF0070u | (((u32)rd) << 12) | ((u32)rm & 15u));
}
static inline void emit_ssat(Emitter *e, ArmCond cc, ArmReg rd, u32 sat_bits /*1..32*/, ArmReg rm, u32 asr_sh) {
    u32 sh_enc = (asr_sh > 0u) ? (((asr_sh & 31u) << 7) | (1u << 6)) : 0u;
    emit_u32(e, (((u32)cc) << 28) | 0x06A00010u | (((sat_bits - 1u) & 31u) << 16) |
                (((u32)rd) << 12) | sh_enc | ((u32)rm & 15u));
}
static inline void emit_usat(Emitter *e, ArmCond cc, ArmReg rd, u32 sat_bits /*0..31*/, ArmReg rm, u32 asr_sh) {
    u32 sh_enc = (asr_sh > 0u) ? (((asr_sh & 31u) << 7) | (1u << 6)) : 0u;
    emit_u32(e, (((u32)cc) << 28) | 0x06E00010u | ((sat_bits & 31u) << 16) |
                (((u32)rd) << 12) | sh_enc | ((u32)rm & 15u));
}
static inline void emit_pkhbt(Emitter *e, ArmCond cc, ArmReg rd, ArmReg rn, ArmReg rm, u32 lsl_imm) {
    emit_u32(e, (((u32)cc) << 28) | 0x06800010u | (((u32)rn) << 16) |
                (((u32)rd) << 12) | ((lsl_imm & 31u) << 7) | ((u32)rm & 15u));
}

/* ============================================================================
 * Block Scanner & 7-Slot Register Allocator (jit_regalloc.c)
 * ============================================================================ */

#define JIT_NUM_SLOTS 7u

extern const ArmReg k_jit_slot_regs[JIT_NUM_SLOTS];

typedef enum JitSlotState {
    SLOT_UNLOADED = 0,
    SLOT_CLEAN    = 1,
    SLOT_DIRTY    = 2
} JitSlotState;

typedef enum JitAddrClass {
    ADDR_DYNAMIC = 0,
    ADDR_CONST_RAM,
    ADDR_CONST_SCRATCH,
    ADDR_CONST_BIOS,
    ADDR_CONST_SLOW
} JitAddrClass;

typedef struct JitDecodedInsn {
    u32          raw;
    u32          pc;
    u16          cyc_before;
    u8           addr_class;   /* JitAddrClass for loads/stores */
    u8           known_rs;     /* 1 if rs was compile-time constant before this insn */
    u8           known_rt;     /* 1 if rt was compile-time constant before this insn */
    u8           in_delay_slot;
    u32          kval_rs;
    u32          kval_rt;
    u32          const_addr;   /* effective virtual address when known_rs */
} JitDecodedInsn;

typedef struct JitScanResult {
    JitDecodedInsn insns[JIT_MAX_BLOCK_INSNS];
    u32            n_insns;
    u32            n_mem_ops;
    u16            total_cycles;
    u16            block_flags;      /* JB_* */
    u16            ram_pages[2];     /* up to 2 physical RAM pages (block + peeked target) */
    bool           has_branch;
    bool           ends_step_one;    /* branch-in-delay-slot at index 0, or BC2F/BC2T at 0 */
    bool           ends_unlinked;    /* MTC0 SR/CAUSE or RFE */
    bool           counted_loop;     /* loop: addiu rX, rX, -1; bne rX, $0, loop; nop */
    u8             counted_reg;
    s8             slot_to_mips[JIT_NUM_SLOTS];
    s8             mips_to_slot[32];
} JitScanResult;

typedef struct JitRegAlloc {
    s8           slot_to_mips[JIT_NUM_SLOTS];
    s8           mips_to_slot[32];
    JitSlotState state[JIT_NUM_SLOTS];
} JitRegAlloc;

void jit_regalloc_reset(void);
void jit_scan_block(const PsxSystem *sys, u32 start_pc, JitScanResult *out);
void jit_scan_block_limit(const PsxSystem *sys, u32 start_pc, u32 max_insns, JitScanResult *out);
void jit_ra_init(JitRegAlloc *ra, const JitScanResult *scan);

/* Read MIPS GPR into an ArmReg (returns cached slot reg, or loads into fallback_scratch) */
ArmReg jit_ra_get_read(Emitter *e, JitRegAlloc *ra, u32 mips_reg, ArmReg fallback_scratch);
/* Prepare MIPS GPR for full overwrite (returns cached slot reg marked DIRTY, or fallback_scratch) */
ArmReg jit_ra_get_write_buf(JitRegAlloc *ra, u32 mips_reg, ArmReg fallback_scratch);
/* Commit write if uncached (no-op if cached slot, STR if fallback_scratch; drops $zero) */
void   jit_ra_commit_write(Emitter *e, JitRegAlloc *ra, u32 mips_reg, ArmReg reg_written);
/* Snapshot 7-bit mask of currently DIRTY slots (for cold exc_stub / abort_stub writebacks) */
u8     jit_ra_dirty_mask(const JitRegAlloc *ra);
/* Emit STRs for a 7-bit dirty mask without altering current allocator state */
void   jit_ra_emit_mask_spill(Emitter *e, const JitRegAlloc *ra, u8 dirty_mask);
/* Spill all DIRTY slots to PsxCpu and mark CLEAN */
void   jit_ra_flush_dirty(Emitter *e, JitRegAlloc *ra);
/* Spill all DIRTY slots AND mark ALL 7 slots UNLOADED (mandatory before inline GTE!) */
void   jit_ra_spill_and_invalidate_all(Emitter *e, JitRegAlloc *ra);
/* Spill DIRTY r4-r8 slots (0..4) and mark them UNLOADED; r12 / lr slots stay live (inline GTE) */
void   jit_ra_spill_and_invalidate_r4_r8(Emitter *e, JitRegAlloc *ra);

/* ============================================================================
 * Inline ARM11v6K GTE Emitter & Assembly Thunks (jit_gte.c, jit_thunks.S)
 * ============================================================================ */

bool jit_emit_gte_inline(Emitter *e, u32 opcode);
/* True iff jit_emit_gte_inline() would emit an inline kernel for this COP2 command */
bool jit_can_inline_gte(u32 opcode);
/* Worst-case ARM words emitted for a COP2 command (inline kernel or C helper call) */
u32  jit_gte_inline_words(u32 opcode);
void jit_emit_mfc2_inline(Emitter *e, u32 gte_reg, ArmReg dst);
void jit_emit_cfc2_inline(Emitter *e, u32 gte_reg, ArmReg dst);
void jit_emit_mtc2_inline(Emitter *e, u32 gte_reg, ArmReg src);
void jit_emit_ctc2_inline(Emitter *e, u32 gte_reg, ArmReg src);

/* Extra assembly trampolines & leaf helpers (jit_thunks.S) */
extern char jit_exit_step_one[];
extern char jit_raise_exception_exit[];
extern char jit_resume_trampoline[];
void jit_gte_div_unr_asm(void);

#endif /* PSX3DS_ARM_EMIT_H */
