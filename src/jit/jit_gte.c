#include "arm_emit.h"

#include <stddef.h>

#define GTE_FLAG_ERR_MASK 0x7F87E000u
#define GTE_FLAG_RW_MASK  0x7FFFF000u

#define OFF_D(n) ((s32)(PSX_OFF_GTE_D + ((u32)(n) * 4u)))
#define OFF_C(n) ((s32)(PSX_OFF_GTE_C + ((u32)(n) * 4u)))

/* ============================================================================
 * Inline COP2 Register Transfers (MFC2 / CFC2 / MTC2 / CTC2)
 * Uses only ARM_R0..ARM_R3 scratch + dst/src; preserves all 7 cached GPR slots.
 * ============================================================================ */

static ArmReg pick_scratch_not(ArmReg avoid1, ArmReg avoid2) {
    const ArmReg cands[4] = { ARM_R2, ARM_R3, ARM_R1, ARM_R0 };
    for (u32 i = 0u; i < 4u; ++i) {
        if (cands[i] != avoid1 && cands[i] != avoid2) {
            return cands[i];
        }
    }
    return ARM_R2;
}

void jit_emit_mfc2_inline(Emitter *e, u32 gte_reg, ArmReg dst) {
    u32 reg = gte_reg & 31u;
    switch (reg) {
        case 1u:  /* VZ0 */
        case 3u:  /* VZ1 */
        case 5u:  /* VZ2 */
        case 8u:  /* IR0 */
        case 9u:  /* IR1 */
        case 10u: /* IR2 */
        case 11u: /* IR3 */
            emit_ldr_imm(e, ARM_CC_AL, dst, JIT_REG_CPU, OFF_D(reg));
            emit_sxth(e, ARM_CC_AL, dst, dst, 0u);
            break;

        case 7u:  /* OTZ */
        case 16u: /* SZ0 */
        case 17u: /* SZ1 */
        case 18u: /* SZ2 */
        case 19u: /* SZ3 */
            emit_ldr_imm(e, ARM_CC_AL, dst, JIT_REG_CPU, OFF_D(reg));
            emit_uxth(e, ARM_CC_AL, dst, dst, 0u);
            break;

        case 15u: /* SXYP mirrors SXY2 (d14) */
            emit_ldr_imm(e, ARM_CC_AL, dst, JIT_REG_CPU, OFF_D(14u));
            break;

        case 28u: /* IRGB */
        case 29u: { /* ORGB */
            ArmReg t = pick_scratch_not(dst, dst);
            emit_ldr_imm(e, ARM_CC_AL, t, JIT_REG_CPU, OFF_D(9u));
            emit_sxth(e, ARM_CC_AL, t, t, 0u);
            emit_usat(e, ARM_CC_AL, dst, 5u, t, 7u);

            emit_ldr_imm(e, ARM_CC_AL, t, JIT_REG_CPU, OFF_D(10u));
            emit_sxth(e, ARM_CC_AL, t, t, 0u);
            emit_usat(e, ARM_CC_AL, t, 5u, t, 7u);
            emit_alu_reg_sh(e, ARM_CC_AL, ARM_OP_ORR, 0u, dst, dst, t, ARM_SH_LSL, 5u);

            emit_ldr_imm(e, ARM_CC_AL, t, JIT_REG_CPU, OFF_D(11u));
            emit_sxth(e, ARM_CC_AL, t, t, 0u);
            emit_usat(e, ARM_CC_AL, t, 5u, t, 7u);
            emit_alu_reg_sh(e, ARM_CC_AL, ARM_OP_ORR, 0u, dst, dst, t, ARM_SH_LSL, 10u);
            break;
        }

        default:
            emit_ldr_imm(e, ARM_CC_AL, dst, JIT_REG_CPU, OFF_D(reg));
            break;
    }
}

void jit_emit_cfc2_inline(Emitter *e, u32 gte_reg, ArmReg dst) {
    u32 reg = gte_reg & 31u;
    switch (reg) {
        case 4u:  /* R33 */
        case 12u: /* L33 */
        case 20u: /* LB3 */
        case 26u: /* H */
        case 27u: /* DQA */
        case 29u: /* ZSF3 */
        case 30u: /* ZSF4 */
            emit_ldr_imm(e, ARM_CC_AL, dst, JIT_REG_CPU, OFF_C(reg));
            emit_sxth(e, ARM_CC_AL, dst, dst, 0u);
            break;

        case 31u: { /* FLAG */
            ArmReg t = pick_scratch_not(dst, dst);
            emit_ldr_imm(e, ARM_CC_AL, dst, JIT_REG_CPU, OFF_C(31u));
            emit_const(e, t, GTE_FLAG_RW_MASK);
            emit_alu_reg(e, ARM_CC_AL, ARM_OP_AND, 0u, dst, dst, t);
            emit_const(e, t, GTE_FLAG_ERR_MASK);
            emit_tst_reg(e, ARM_CC_AL, dst, t);
            u32 enc80 = 0u;
            (void)arm_encode_imm(0x80000000u, &enc80);
            emit_alu_imm_enc(e, ARM_CC_NE, ARM_OP_ORR, 0u, dst, dst, enc80);
            break;
        }

        default:
            emit_ldr_imm(e, ARM_CC_AL, dst, JIT_REG_CPU, OFF_C(reg));
            break;
    }
}

void jit_emit_mtc2_inline(Emitter *e, u32 gte_reg, ArmReg src) {
    u32 reg = gte_reg & 31u;
    ArmReg t1 = pick_scratch_not(src, src);
    ArmReg t2 = pick_scratch_not(src, t1);

    switch (reg) {
        case 1u:
        case 3u:
        case 5u:
        case 8u:
        case 9u:
        case 10u:
        case 11u:
            emit_sxth(e, ARM_CC_AL, t1, src, 0u);
            emit_str_imm(e, ARM_CC_AL, t1, JIT_REG_CPU, OFF_D(reg));
            break;

        case 7u:
        case 16u:
        case 17u:
        case 18u:
        case 19u:
            emit_uxth(e, ARM_CC_AL, t1, src, 0u);
            emit_str_imm(e, ARM_CC_AL, t1, JIT_REG_CPU, OFF_D(reg));
            break;

        case 15u: /* SXYP FIFO push: SXY0<-SXY1, SXY1<-SXY2, SXY2=SXYP=val */
            emit_ldr_imm(e, ARM_CC_AL, t1, JIT_REG_CPU, OFF_D(13u));
            emit_ldr_imm(e, ARM_CC_AL, t2, JIT_REG_CPU, OFF_D(14u));
            emit_str_imm(e, ARM_CC_AL, t1, JIT_REG_CPU, OFF_D(12u));
            emit_str_imm(e, ARM_CC_AL, t2, JIT_REG_CPU, OFF_D(13u));
            emit_str_imm(e, ARM_CC_AL, src, JIT_REG_CPU, OFF_D(14u));
            emit_str_imm(e, ARM_CC_AL, src, JIT_REG_CPU, OFF_D(15u));
            break;

        case 28u: { /* IRGB -> IR1..IR3 = 5-bit lane * 0x80 */
            u32 enc1f = 0x1Fu;
            emit_shift_imm(e, ARM_CC_AL, 0u, t1, src, ARM_SH_LSL, 17u);
            emit_shift_imm(e, ARM_CC_AL, 0u, t1, t1, ARM_SH_LSR, 17u);
            emit_str_imm(e, ARM_CC_AL, t1, JIT_REG_CPU, OFF_D(28u));

            emit_alu_imm_enc(e, ARM_CC_AL, ARM_OP_AND, 0u, t1, src, enc1f);
            emit_shift_imm(e, ARM_CC_AL, 0u, t1, t1, ARM_SH_LSL, 7u);
            emit_str_imm(e, ARM_CC_AL, t1, JIT_REG_CPU, OFF_D(9u));

            emit_shift_imm(e, ARM_CC_AL, 0u, t1, src, ARM_SH_LSR, 5u);
            emit_alu_imm_enc(e, ARM_CC_AL, ARM_OP_AND, 0u, t1, t1, enc1f);
            emit_shift_imm(e, ARM_CC_AL, 0u, t1, t1, ARM_SH_LSL, 7u);
            emit_str_imm(e, ARM_CC_AL, t1, JIT_REG_CPU, OFF_D(10u));

            emit_shift_imm(e, ARM_CC_AL, 0u, t1, src, ARM_SH_LSR, 10u);
            emit_alu_imm_enc(e, ARM_CC_AL, ARM_OP_AND, 0u, t1, t1, enc1f);
            emit_shift_imm(e, ARM_CC_AL, 0u, t1, t1, ARM_SH_LSL, 7u);
            emit_str_imm(e, ARM_CC_AL, t1, JIT_REG_CPU, OFF_D(11u));
            break;
        }

        case 29u: /* ORGB read-only */
        case 31u: /* LZCR read-only */
            break;

        case 30u: /* LZCS -> LZCR via ARMv6 CLZ */
            emit_str_imm(e, ARM_CC_AL, src, JIT_REG_CPU, OFF_D(30u));
            emit_shift_imm(e, ARM_CC_AL, 1u /*S*/, t1, src, ARM_SH_LSL, 0u);
            emit_u32(e, ARM_DP_REG_IMM_SH(ARM_CC_MI, ARM_OP_MVN, 0u, 0u, t1, 0u, ARM_SH_LSL, t1));
            emit_clz(e, ARM_CC_AL, t1, t1);
            emit_str_imm(e, ARM_CC_AL, t1, JIT_REG_CPU, OFF_D(31u));
            break;

        default:
            emit_str_imm(e, ARM_CC_AL, src, JIT_REG_CPU, OFF_D(reg));
            break;
    }
}

void jit_emit_ctc2_inline(Emitter *e, u32 gte_reg, ArmReg src) {
    u32 reg = gte_reg & 31u;
    ArmReg t1 = pick_scratch_not(src, src);
    ArmReg t2 = pick_scratch_not(src, t1);

    switch (reg) {
        case 4u:
        case 12u:
        case 20u:
        case 26u:
        case 27u:
        case 29u:
        case 30u:
            emit_sxth(e, ARM_CC_AL, t1, src, 0u);
            emit_str_imm(e, ARM_CC_AL, t1, JIT_REG_CPU, OFF_C(reg));
            break;

        case 31u: {
            emit_const(e, t1, GTE_FLAG_RW_MASK);
            emit_alu_reg(e, ARM_CC_AL, ARM_OP_AND, 0u, t1, src, t1);
            emit_const(e, t2, GTE_FLAG_ERR_MASK);
            emit_tst_reg(e, ARM_CC_AL, t1, t2);
            u32 enc80 = 0u;
            (void)arm_encode_imm(0x80000000u, &enc80);
            emit_alu_imm_enc(e, ARM_CC_NE, ARM_OP_ORR, 0u, t1, t1, enc80);
            emit_str_imm(e, ARM_CC_AL, t1, JIT_REG_CPU, OFF_C(31u));
            break;
        }

        default:
            emit_str_imm(e, ARM_CC_AL, src, JIT_REG_CPU, OFF_C(reg));
            break;
    }
}

/* ============================================================================
 * Inline ARM11v6K GTE Command Kernels (ARCHITECTURE.md §5.11)
 *
 * Precondition: caller invoked jit_ra_spill_and_invalidate_all(), so r0-r8,
 * r12, lr are free scratch. rFLAG lives in ARM_R8 across the kernel.
 * ============================================================================ */

#define GTE_REG_FLAG ARM_R8

static void gte_emit_flag_bit_cc(Emitter *e, ArmCond cc, u32 bit_idx) {
    u32 enc = 0u;
    (void)arm_encode_imm(1u << bit_idx, &enc);
    emit_alu_imm_enc(e, cc, ARM_OP_ORR, 0u, GTE_REG_FLAG, GTE_REG_FLAG, enc);
}

/* Check 32-bit signed MAC0 overflow on 64-bit rHi:rLo (sets bit 16 pos / bit 15 neg) */
static void gte_emit_check_mac0(Emitter *e, ArmReg rLo, ArmReg rHi) {
    /* cmp rHi, rLo, asr #31 */
    emit_u32(e, ARM_DP_REG_IMM_SH(ARM_CC_AL, ARM_OP_CMP, 1u, rHi, 0u, 31u, ARM_SH_ASR, rLo));
    gte_emit_flag_bit_cc(e, ARM_CC_GT, 16u);
    gte_emit_flag_bit_cc(e, ARM_CC_LT, 15u);
}

/* Shift 64-bit rHi:rLo right by sf (0 or 12) into 32-bit dst */
static void gte_emit_shift_sf64(Emitter *e, ArmReg dst, ArmReg rLo, ArmReg rHi, bool sf12) {
    if (!sf12) {
        emit_mov_reg(e, dst, rLo);
    } else {
        emit_shift_imm(e, ARM_CC_AL, 0u, dst, rLo, ARM_SH_LSR, 12u);
        emit_alu_reg_sh(e, ARM_CC_AL, ARM_OP_ORR, 0u, dst, dst, rHi, ARM_SH_LSL, 20u);
    }
}

/* Saturate 32-bit mac_reg to IR[idx] (idx=1..3 -> bits 24,23,22), result in dst_ir */
static void gte_emit_sat_ir(Emitter *e, ArmReg dst_ir, ArmReg mac_reg, u32 idx, bool lm) {
    if (lm) {
        emit_usat(e, ARM_CC_AL, dst_ir, 15u, mac_reg, 0u);
    } else {
        emit_ssat(e, ARM_CC_AL, dst_ir, 16u, mac_reg, 0u);
    }
    emit_cmp_reg(e, ARM_CC_AL, dst_ir, mac_reg);
    gte_emit_flag_bit_cc(e, ARM_CC_NE, 25u - idx);
}

/* Push RGB FIFO from MAC1..MAC3 (already stored in gte_d[25..27]) + RGBC[31:24] */
static void gte_emit_push_rgb_fifo(Emitter *e) {
    /* Clobbers r0..r4 */
    emit_ldr_imm(e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, OFF_D(25u));
    emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R1, ARM_R0, ARM_SH_ASR, 4u);
    emit_usat(e, ARM_CC_AL, ARM_R2, 8u, ARM_R1, 0u);
    emit_cmp_reg(e, ARM_CC_AL, ARM_R2, ARM_R1);
    gte_emit_flag_bit_cc(e, ARM_CC_NE, 21u);

    emit_ldr_imm(e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, OFF_D(26u));
    emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R1, ARM_R0, ARM_SH_ASR, 4u);
    emit_usat(e, ARM_CC_AL, ARM_R3, 8u, ARM_R1, 0u);
    emit_cmp_reg(e, ARM_CC_AL, ARM_R3, ARM_R1);
    gte_emit_flag_bit_cc(e, ARM_CC_NE, 20u);
    emit_alu_reg_sh(e, ARM_CC_AL, ARM_OP_ORR, 0u, ARM_R2, ARM_R2, ARM_R3, ARM_SH_LSL, 8u);

    emit_ldr_imm(e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, OFF_D(27u));
    emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R1, ARM_R0, ARM_SH_ASR, 4u);
    emit_usat(e, ARM_CC_AL, ARM_R3, 8u, ARM_R1, 0u);
    emit_cmp_reg(e, ARM_CC_AL, ARM_R3, ARM_R1);
    gte_emit_flag_bit_cc(e, ARM_CC_NE, 19u);
    emit_alu_reg_sh(e, ARM_CC_AL, ARM_OP_ORR, 0u, ARM_R2, ARM_R2, ARM_R3, ARM_SH_LSL, 16u);

    /* Merge CODE byte from RGBC[31:24] */
    emit_ldr_imm(e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, OFF_D(6u));
    emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R0, ARM_R0, ARM_SH_LSR, 24u);
    emit_alu_reg_sh(e, ARM_CC_AL, ARM_OP_ORR, 0u, ARM_R2, ARM_R2, ARM_R0, ARM_SH_LSL, 24u);

    emit_ldr_imm(e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, OFF_D(21u));
    emit_ldr_imm(e, ARM_CC_AL, ARM_R1, JIT_REG_CPU, OFF_D(22u));
    emit_str_imm(e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, OFF_D(20u));
    emit_str_imm(e, ARM_CC_AL, ARM_R1, JIT_REG_CPU, OFF_D(21u));
    emit_str_imm(e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, OFF_D(22u));
}

/* Finalize FLAG bit 31 from GTE_REG_FLAG, store gte_c[31], bump stats.gte_inline */
static void gte_emit_epilogue(Emitter *e, bool has_bounce, u16 lbl_bounce, u32 opcode) {
    emit_const(e, ARM_R2, GTE_FLAG_ERR_MASK);
    emit_tst_reg(e, ARM_CC_AL, GTE_REG_FLAG, ARM_R2);
    u32 enc80 = 0u;
    (void)arm_encode_imm(0x80000000u, &enc80);
    emit_alu_imm_enc(e, ARM_CC_NE, ARM_OP_ORR, 0u, GTE_REG_FLAG, GTE_REG_FLAG, enc80);
    emit_str_imm(e, ARM_CC_AL, GTE_REG_FLAG, JIT_REG_CPU, OFF_C(31u));

    /* stats.gte_inline++ */
    emit_ldr_imm(e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, (s32)PSX_OFF_SYS);
    emit_ldr_imm(e, ARM_CC_AL, ARM_R3, ARM_R2, (s32)offsetof(PsxSystem, stats.gte_inline));
    emit_alu_imm_enc(e, ARM_CC_AL, ARM_OP_ADD, 0u, ARM_R3, ARM_R3, 1u);
    emit_str_imm(e, ARM_CC_AL, ARM_R3, ARM_R2, (s32)offsetof(PsxSystem, stats.gte_inline));

    if (has_bounce) {
        u16 lbl_done = arm_label_create(e);
        emit_b_label(e, ARM_CC_AL, lbl_done);

        arm_label_bind(e, lbl_bounce);
        emit_mov_reg(e, ARM_R0, JIT_REG_CPU);
        emit_const(e, ARM_R1, opcode);
        emit_call(e, (const void *)(uintptr_t)jit_gte_exec_wrap, false);

        arm_label_bind(e, lbl_done);
    }
}

/* Guard |val_reg| >= 2^30 -> branch to lbl_bounce (exact iff for [-2^30, 2^30 - 1]) */
static void gte_emit_guard_2p30(Emitter *e, ArmReg val_reg, ArmReg scratch, u16 lbl_bounce) {
    u32 enc40 = 0u;
    (void)arm_encode_imm(0x40000000u, &enc40);
    emit_alu_imm_enc(e, ARM_CC_AL, ARM_OP_ADD, 1u /*S*/, scratch, val_reg, enc40);
    emit_b_label(e, ARM_CC_MI, lbl_bounce);
}

/* Emit one vertex of RTPS / RTPT (with matrix already verified |TR| < 2^30) */
static void gte_emit_rtp_vertex(Emitter *e, u32 v_idx, bool sf12, bool lm, bool do_dq) {
    /* Load vertex VXY -> r0, VZ -> r1; matrix words loaded as needed */
    emit_ldr_imm(e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, OFF_D(v_idx * 2u));
    emit_ldr_imm(e, ARM_CC_AL, ARM_R1, JIT_REG_CPU, OFF_D(v_idx * 2u + 1u));

    /* --- Row 0: MAC1 = (TRX << 12) + R11*VX + R12*VY + R13*VZ --- */
    emit_ldr_imm(e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, OFF_C(5u)); /* TRX */
    emit_ldr_imm(e, ARM_CC_AL, ARM_R5, JIT_REG_CPU, OFF_C(0u)); /* R12:R11 */
    emit_ldr_imm(e, ARM_CC_AL, ARM_R6, JIT_REG_CPU, OFF_C(1u)); /* R21:R13 */
    emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R3, ARM_R2, ARM_SH_LSL, 12u);
    emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R4, ARM_R2, ARM_SH_ASR, 20u);
    emit_smlald(e, ARM_CC_AL, ARM_R3, ARM_R4, ARM_R0, ARM_R5);
    emit_smlalxy(e, ARM_CC_AL, ARM_R3, ARM_R4, ARM_R1, ARM_R6, false, false);
    gte_emit_shift_sf64(e, ARM_R2, ARM_R3, ARM_R4, sf12);
    emit_str_imm(e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, OFF_D(25u));
    gte_emit_sat_ir(e, ARM_R5, ARM_R2, 1u, lm);
    emit_str_imm(e, ARM_CC_AL, ARM_R5, JIT_REG_CPU, OFF_D(9u));

    /* --- Row 1: MAC2 = (TRY << 12) + R21*VX + R22*VY + R23*VZ --- */
    emit_ldr_imm(e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, OFF_C(6u)); /* TRY */
    emit_ldr_imm(e, ARM_CC_AL, ARM_R7, JIT_REG_CPU, OFF_C(2u)); /* R23:R22 */
    emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R3, ARM_R2, ARM_SH_LSL, 12u);
    emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R4, ARM_R2, ARM_SH_ASR, 20u);
    emit_smlalxy(e, ARM_CC_AL, ARM_R3, ARM_R4, ARM_R0, ARM_R6, false, true);  /* VX * R21 */
    emit_smlalxy(e, ARM_CC_AL, ARM_R3, ARM_R4, ARM_R0, ARM_R7, true, false);  /* VY * R22 */
    emit_smlalxy(e, ARM_CC_AL, ARM_R3, ARM_R4, ARM_R1, ARM_R7, false, true);  /* VZ * R23 */
    gte_emit_shift_sf64(e, ARM_R2, ARM_R3, ARM_R4, sf12);
    emit_str_imm(e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, OFF_D(26u));
    gte_emit_sat_ir(e, ARM_R6, ARM_R2, 2u, lm);
    emit_str_imm(e, ARM_CC_AL, ARM_R6, JIT_REG_CPU, OFF_D(10u));

    /* --- Row 2: MAC3 = (TRZ << 12) + R31*VX + R32*VY + R33*VZ --- */
    emit_ldr_imm(e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, OFF_C(7u)); /* TRZ */
    emit_ldr_imm(e, ARM_CC_AL, ARM_R5, JIT_REG_CPU, OFF_C(3u)); /* R32:R31 */
    emit_ldr_imm(e, ARM_CC_AL, ARM_R6, JIT_REG_CPU, OFF_C(4u)); /* ---:R33 */
    emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R3, ARM_R2, ARM_SH_LSL, 12u);
    emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R4, ARM_R2, ARM_SH_ASR, 20u);
    emit_smlald(e, ARM_CC_AL, ARM_R3, ARM_R4, ARM_R0, ARM_R5);
    emit_smlalxy(e, ARM_CC_AL, ARM_R3, ARM_R4, ARM_R1, ARM_R6, false, false);

    /* Store MAC3 = (raw2 >> sf) */
    gte_emit_shift_sf64(e, ARM_R2, ARM_R3, ARM_R4, sf12);
    emit_str_imm(e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, OFF_D(27u));

    /* r5 = raw2 >> 12 (from 44-bit sum, safe in s32 since |raw2| < 2^43 => |raw2>>12| < 2^31) */
    gte_emit_shift_sf64(e, ARM_R5, ARM_R3, ARM_R4, true);

    /* RTPS/RTPT IR3 quirk: FLAG.22 tests (raw2 >> 12) against [-0x8000, 0x7FFF],
     * whereas IR3 value clamps MAC3 with actual lm! */
    emit_ssat(e, ARM_CC_AL, ARM_R0, 16u, ARM_R5, 0u);
    emit_cmp_reg(e, ARM_CC_AL, ARM_R0, ARM_R5);
    gte_emit_flag_bit_cc(e, ARM_CC_NE, 22u);

    if (lm) {
        emit_usat(e, ARM_CC_AL, ARM_R0, 15u, ARM_R2, 0u);
    } else {
        emit_ssat(e, ARM_CC_AL, ARM_R0, 16u, ARM_R2, 0u);
    }
    emit_str_imm(e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, OFF_D(11u));

    /* SZ3 = clamp(raw2 >> 12, 0..0xFFFF) -> r7, push SZ FIFO */
    emit_usat(e, ARM_CC_AL, ARM_R7, 16u, ARM_R5, 0u);
    emit_cmp_reg(e, ARM_CC_AL, ARM_R7, ARM_R5);
    gte_emit_flag_bit_cc(e, ARM_CC_NE, 18u);

    emit_ldr_imm(e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, OFF_D(17u));
    emit_ldr_imm(e, ARM_CC_AL, ARM_R1, JIT_REG_CPU, OFF_D(18u));
    emit_ldr_imm(e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, OFF_D(19u));
    emit_str_imm(e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, OFF_D(16u));
    emit_str_imm(e, ARM_CC_AL, ARM_R1, JIT_REG_CPU, OFF_D(17u));
    emit_str_imm(e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, OFF_D(18u));
    emit_str_imm(e, ARM_CC_AL, ARM_R7, JIT_REG_CPU, OFF_D(19u));

    /* UNR reciprocal division: r0 = H, r1 = SZ3 (r7), r2 = unr_table_ptr */
    emit_ldr_imm(e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, OFF_C(26u));
    emit_mov_reg(e, ARM_R1, ARM_R7);
    emit_const(e, ARM_R2, (u32)(uintptr_t)jit_gte_unr_table_ptr());
    emit_call(e, (const void *)(uintptr_t)jit_gte_div_unr_asm, false);
    /* Returns r0 = q (0..0x1FFFF), r1 = overflow (0/1). Save q in r7. */
    emit_mov_reg(e, ARM_R7, ARM_R0);
    emit_cmp_imm_enc(e, ARM_CC_AL, ARM_R1, 0u);
    gte_emit_flag_bit_cc(e, ARM_CC_NE, 17u);

    /* Screen projection X: px = (s64)OFX + (s64)q * (s16)IR1 */
    emit_ldr_imm(e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, OFF_C(24u)); /* OFX */
    emit_ldr_imm(e, ARM_CC_AL, ARM_R1, JIT_REG_CPU, OFF_D(9u));  /* IR1 */
    emit_sxth(e, ARM_CC_AL, ARM_R1, ARM_R1, 0u);
    emit_mov_reg(e, ARM_R3, ARM_R2);
    emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R4, ARM_R2, ARM_SH_ASR, 31u);
    emit_smlal(e, ARM_CC_AL, ARM_R3, ARM_R4, ARM_R7, ARM_R1);
    gte_emit_check_mac0(e, ARM_R3, ARM_R4);
    /* r0 = (s32)(px >> 16) (exact from 64-bit: |px| <= 2^31 + 2^32 < 2^34) */
    emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R0, ARM_R3, ARM_SH_LSR, 16u);
    emit_alu_reg_sh(e, ARM_CC_AL, ARM_OP_ORR, 0u, ARM_R0, ARM_R0, ARM_R4, ARM_SH_LSL, 16u);
    emit_ssat(e, ARM_CC_AL, ARM_R5, 11u, ARM_R0, 0u); /* SX2 in r5 */
    emit_cmp_reg(e, ARM_CC_AL, ARM_R5, ARM_R0);
    gte_emit_flag_bit_cc(e, ARM_CC_NE, 14u);

    /* Screen projection Y: py = (s64)OFY + (s64)q * (s16)IR2 */
    emit_ldr_imm(e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, OFF_C(25u)); /* OFY */
    emit_ldr_imm(e, ARM_CC_AL, ARM_R1, JIT_REG_CPU, OFF_D(10u)); /* IR2 */
    emit_sxth(e, ARM_CC_AL, ARM_R1, ARM_R1, 0u);
    emit_mov_reg(e, ARM_R3, ARM_R2);
    emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R4, ARM_R2, ARM_SH_ASR, 31u);
    emit_smlal(e, ARM_CC_AL, ARM_R3, ARM_R4, ARM_R7, ARM_R1);
    gte_emit_check_mac0(e, ARM_R3, ARM_R4);
    emit_str_imm(e, ARM_CC_AL, ARM_R3, JIT_REG_CPU, OFF_D(24u)); /* MAC0 = py (if !do_dq) */

    emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R0, ARM_R3, ARM_SH_LSR, 16u);
    emit_alu_reg_sh(e, ARM_CC_AL, ARM_OP_ORR, 0u, ARM_R0, ARM_R0, ARM_R4, ARM_SH_LSL, 16u);
    emit_ssat(e, ARM_CC_AL, ARM_R6, 11u, ARM_R0, 0u); /* SY2 in r6 */
    emit_cmp_reg(e, ARM_CC_AL, ARM_R6, ARM_R0);
    gte_emit_flag_bit_cc(e, ARM_CC_NE, 13u);

    /* Pack SXY2 = (SY2 << 16) | (SX2 & 0xFFFF) via ARMv6 PKHBT and push SXY FIFO */
    emit_pkhbt(e, ARM_CC_AL, ARM_R5, ARM_R5, ARM_R6, 16u);
    emit_ldr_imm(e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, OFF_D(13u));
    emit_ldr_imm(e, ARM_CC_AL, ARM_R1, JIT_REG_CPU, OFF_D(14u));
    emit_str_imm(e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, OFF_D(12u));
    emit_str_imm(e, ARM_CC_AL, ARM_R1, JIT_REG_CPU, OFF_D(13u));
    emit_str_imm(e, ARM_CC_AL, ARM_R5, JIT_REG_CPU, OFF_D(14u));
    emit_str_imm(e, ARM_CC_AL, ARM_R5, JIT_REG_CPU, OFF_D(15u));

    /* Depth cueing (RTPS and V2 of RTPT): dq = (s64)DQB + (s16)DQA * (s64)q */
    if (do_dq) {
        emit_ldr_imm(e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, OFF_C(28u)); /* DQB */
        emit_ldr_imm(e, ARM_CC_AL, ARM_R1, JIT_REG_CPU, OFF_C(27u)); /* DQA */
        emit_sxth(e, ARM_CC_AL, ARM_R1, ARM_R1, 0u);
        emit_mov_reg(e, ARM_R3, ARM_R2);
        emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R4, ARM_R2, ARM_SH_ASR, 31u);
        emit_smlal(e, ARM_CC_AL, ARM_R3, ARM_R4, ARM_R7, ARM_R1);
        gte_emit_check_mac0(e, ARM_R3, ARM_R4);
        emit_str_imm(e, ARM_CC_AL, ARM_R3, JIT_REG_CPU, OFF_D(24u)); /* MAC0 = dq */

        /* r0 = (s32)(dq >> 12); clamp to [0, 0x1000] inclusive (NOT USAT #12!) */
        gte_emit_shift_sf64(e, ARM_R0, ARM_R3, ARM_R4, true);
        u32 enc1000 = 0u;
        (void)arm_encode_imm(0x1000u, &enc1000);
        emit_cmp_imm_enc(e, ARM_CC_AL, ARM_R0, 0u);
        emit_const_cc(e, ARM_CC_LT, ARM_R0, 0u);
        gte_emit_flag_bit_cc(e, ARM_CC_LT, 12u);
        emit_cmp_imm_enc(e, ARM_CC_AL, ARM_R0, enc1000);
        emit_alu_imm_enc(e, ARM_CC_GT, ARM_OP_MOV, 0u, ARM_R0, 0u, enc1000);
        gte_emit_flag_bit_cc(e, ARM_CC_GT, 12u);
        emit_str_imm(e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, OFF_D(8u)); /* IR0 */
    }
}

bool jit_emit_gte_inline(Emitter *e, u32 opcode) {
    u32 fn = opcode & 0x3Fu;
    bool sf12 = ((opcode >> 19) & 1u) != 0u;
    bool lm = ((opcode >> 10) & 1u) != 0u;

    switch (fn) {
        case 0x06u: { /* NCLIP: MAC0 = SX0*(SY1-SY2) + SX1*(SY2-SY0) + SX2*(SY0-SY1) */
            emit_const(e, GTE_REG_FLAG, 0u);
            emit_ldr_imm(e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, OFF_D(12u)); /* SXY0 */
            emit_ldr_imm(e, ARM_CC_AL, ARM_R1, JIT_REG_CPU, OFF_D(13u)); /* SXY1 */
            emit_ldr_imm(e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, OFF_D(14u)); /* SXY2 */

            emit_sxth(e, ARM_CC_AL, ARM_R5, ARM_R0, 0u); /* SX0 */
            emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R0, ARM_R0, ARM_SH_ASR, 16u); /* SY0 */
            emit_sxth(e, ARM_CC_AL, ARM_R6, ARM_R1, 0u); /* SX1 */
            emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R1, ARM_R1, ARM_SH_ASR, 16u); /* SY1 */
            emit_sxth(e, ARM_CC_AL, ARM_R7, ARM_R2, 0u); /* SX2 */
            emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R2, ARM_R2, ARM_SH_ASR, 16u); /* SY2 */

            /* r3:r4 = SX0 * (SY1 - SY2) */
            emit_alu_reg(e, ARM_CC_AL, ARM_OP_SUB, 0u, ARM_R12, ARM_R1, ARM_R2);
            emit_smull(e, ARM_CC_AL, ARM_R3, ARM_R4, ARM_R5, ARM_R12);
            /* += SX1 * (SY2 - SY0) */
            emit_alu_reg(e, ARM_CC_AL, ARM_OP_SUB, 0u, ARM_R12, ARM_R2, ARM_R0);
            emit_smlal(e, ARM_CC_AL, ARM_R3, ARM_R4, ARM_R6, ARM_R12);
            /* += SX2 * (SY0 - SY1) */
            emit_alu_reg(e, ARM_CC_AL, ARM_OP_SUB, 0u, ARM_R12, ARM_R0, ARM_R1);
            emit_smlal(e, ARM_CC_AL, ARM_R3, ARM_R4, ARM_R7, ARM_R12);

            gte_emit_check_mac0(e, ARM_R3, ARM_R4);
            emit_str_imm(e, ARM_CC_AL, ARM_R3, JIT_REG_CPU, OFF_D(24u));
            gte_emit_epilogue(e, false, 0u, opcode);
            return true;
        }

        case 0x2Du:   /* AVSZ3 */
        case 0x2Eu: { /* AVSZ4 */
            emit_const(e, GTE_REG_FLAG, 0u);
            emit_ldr_imm(e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, OFF_D(17u));
            emit_uxth(e, ARM_CC_AL, ARM_R0, ARM_R0, 0u);
            emit_ldr_imm(e, ARM_CC_AL, ARM_R1, JIT_REG_CPU, OFF_D(18u));
            emit_uxth(e, ARM_CC_AL, ARM_R1, ARM_R1, 0u);
            emit_alu_reg(e, ARM_CC_AL, ARM_OP_ADD, 0u, ARM_R0, ARM_R0, ARM_R1);
            emit_ldr_imm(e, ARM_CC_AL, ARM_R1, JIT_REG_CPU, OFF_D(19u));
            emit_uxth(e, ARM_CC_AL, ARM_R1, ARM_R1, 0u);
            emit_alu_reg(e, ARM_CC_AL, ARM_OP_ADD, 0u, ARM_R0, ARM_R0, ARM_R1);
            if (fn == 0x2Eu) {
                emit_ldr_imm(e, ARM_CC_AL, ARM_R1, JIT_REG_CPU, OFF_D(16u));
                emit_uxth(e, ARM_CC_AL, ARM_R1, ARM_R1, 0u);
                emit_alu_reg(e, ARM_CC_AL, ARM_OP_ADD, 0u, ARM_R0, ARM_R0, ARM_R1);
            }

            emit_ldr_imm(e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, OFF_C((fn == 0x2Du) ? 29u : 30u));
            emit_sxth(e, ARM_CC_AL, ARM_R2, ARM_R2, 0u);
            emit_smull(e, ARM_CC_AL, ARM_R3, ARM_R4, ARM_R2, ARM_R0);

            gte_emit_check_mac0(e, ARM_R3, ARM_R4);
            emit_str_imm(e, ARM_CC_AL, ARM_R3, JIT_REG_CPU, OFF_D(24u));

            gte_emit_shift_sf64(e, ARM_R0, ARM_R3, ARM_R4, true);
            emit_usat(e, ARM_CC_AL, ARM_R1, 16u, ARM_R0, 0u);
            emit_cmp_reg(e, ARM_CC_AL, ARM_R1, ARM_R0);
            gte_emit_flag_bit_cc(e, ARM_CC_NE, 18u);
            emit_str_imm(e, ARM_CC_AL, ARM_R1, JIT_REG_CPU, OFF_D(7u));

            gte_emit_epilogue(e, false, 0u, opcode);
            return true;
        }

        case 0x01u:   /* RTPS */
        case 0x30u: { /* RTPT */
            u16 lbl_bounce = arm_label_create(e);
            /* Guard |TRX|, |TRY|, |TRZ| < 2^30 before modifying any FIFO or register */
            for (u32 k = 0u; k < 3u; ++k) {
                emit_ldr_imm(e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, OFF_C(5u + k));
                gte_emit_guard_2p30(e, ARM_R2, ARM_R3, lbl_bounce);
            }
            emit_const(e, GTE_REG_FLAG, 0u);
            if (fn == 0x01u) {
                gte_emit_rtp_vertex(e, 0u, sf12, lm, true);
            } else {
                gte_emit_rtp_vertex(e, 0u, sf12, lm, false);
                gte_emit_rtp_vertex(e, 1u, sf12, lm, false);
                gte_emit_rtp_vertex(e, 2u, sf12, lm, true);
            }
            gte_emit_epilogue(e, true, lbl_bounce, opcode);
            return true;
        }

        case 0x12u: { /* MVMVA (inline when mx < 3 && cv != 2; bounce quirky mx=3 / cv=2) */
            u32 mx = (opcode >> 17) & 3u;
            u32 vx = (opcode >> 15) & 3u;
            u32 cv = (opcode >> 13) & 3u;
            if (mx == 3u || cv == 2u) {
                return false;
            }

            bool has_tr = (cv == 0u || cv == 1u);
            u32 tr_base = (cv == 0u) ? 5u : 13u;
            u32 mat_base = mx * 8u;

            u16 lbl_bounce = 0u;
            if (has_tr) {
                lbl_bounce = arm_label_create(e);
                for (u32 k = 0u; k < 3u; ++k) {
                    emit_ldr_imm(e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, OFF_C(tr_base + k));
                    gte_emit_guard_2p30(e, ARM_R2, ARM_R3, lbl_bounce);
                }
            }

            emit_const(e, GTE_REG_FLAG, 0u);

            /* Load input vector into r0 = VY:VX packed, r1 = VZ (low16) BEFORE writing IR1..3 */
            if (vx < 3u) {
                emit_ldr_imm(e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, OFF_D(vx * 2u));
                emit_ldr_imm(e, ARM_CC_AL, ARM_R1, JIT_REG_CPU, OFF_D(vx * 2u + 1u));
            } else {
                emit_ldr_imm(e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, OFF_D(9u));  /* IR1 */
                emit_ldr_imm(e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, OFF_D(10u)); /* IR2 */
                emit_ldr_imm(e, ARM_CC_AL, ARM_R1, JIT_REG_CPU, OFF_D(11u)); /* IR3 */
                emit_pkhbt(e, ARM_CC_AL, ARM_R0, ARM_R0, ARM_R2, 16u);
            }

            /* Row 0 */
            if (has_tr) {
                emit_ldr_imm(e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, OFF_C(tr_base + 0u));
                emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R3, ARM_R2, ARM_SH_LSL, 12u);
                emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R4, ARM_R2, ARM_SH_ASR, 20u);
            } else {
                emit_const(e, ARM_R3, 0u);
                emit_const(e, ARM_R4, 0u);
            }
            emit_ldr_imm(e, ARM_CC_AL, ARM_R5, JIT_REG_CPU, OFF_C(mat_base + 0u));
            emit_ldr_imm(e, ARM_CC_AL, ARM_R6, JIT_REG_CPU, OFF_C(mat_base + 1u));
            emit_smlald(e, ARM_CC_AL, ARM_R3, ARM_R4, ARM_R0, ARM_R5);
            emit_smlalxy(e, ARM_CC_AL, ARM_R3, ARM_R4, ARM_R1, ARM_R6, false, false);
            gte_emit_shift_sf64(e, ARM_R2, ARM_R3, ARM_R4, sf12);
            emit_str_imm(e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, OFF_D(25u));
            gte_emit_sat_ir(e, ARM_R5, ARM_R2, 1u, lm);
            emit_str_imm(e, ARM_CC_AL, ARM_R5, JIT_REG_CPU, OFF_D(9u));

            /* Row 1 */
            if (has_tr) {
                emit_ldr_imm(e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, OFF_C(tr_base + 1u));
                emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R3, ARM_R2, ARM_SH_LSL, 12u);
                emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R4, ARM_R2, ARM_SH_ASR, 20u);
            } else {
                emit_const(e, ARM_R3, 0u);
                emit_const(e, ARM_R4, 0u);
            }
            emit_ldr_imm(e, ARM_CC_AL, ARM_R7, JIT_REG_CPU, OFF_C(mat_base + 2u));
            emit_smlalxy(e, ARM_CC_AL, ARM_R3, ARM_R4, ARM_R0, ARM_R6, false, true);
            emit_smlalxy(e, ARM_CC_AL, ARM_R3, ARM_R4, ARM_R0, ARM_R7, true, false);
            emit_smlalxy(e, ARM_CC_AL, ARM_R3, ARM_R4, ARM_R1, ARM_R7, false, true);
            gte_emit_shift_sf64(e, ARM_R2, ARM_R3, ARM_R4, sf12);
            emit_str_imm(e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, OFF_D(26u));
            gte_emit_sat_ir(e, ARM_R5, ARM_R2, 2u, lm);
            emit_str_imm(e, ARM_CC_AL, ARM_R5, JIT_REG_CPU, OFF_D(10u));

            /* Row 2 */
            if (has_tr) {
                emit_ldr_imm(e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, OFF_C(tr_base + 2u));
                emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R3, ARM_R2, ARM_SH_LSL, 12u);
                emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R4, ARM_R2, ARM_SH_ASR, 20u);
            } else {
                emit_const(e, ARM_R3, 0u);
                emit_const(e, ARM_R4, 0u);
            }
            emit_ldr_imm(e, ARM_CC_AL, ARM_R5, JIT_REG_CPU, OFF_C(mat_base + 3u));
            emit_ldr_imm(e, ARM_CC_AL, ARM_R6, JIT_REG_CPU, OFF_C(mat_base + 4u));
            emit_smlald(e, ARM_CC_AL, ARM_R3, ARM_R4, ARM_R0, ARM_R5);
            emit_smlalxy(e, ARM_CC_AL, ARM_R3, ARM_R4, ARM_R1, ARM_R6, false, false);
            gte_emit_shift_sf64(e, ARM_R2, ARM_R3, ARM_R4, sf12);
            emit_str_imm(e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, OFF_D(27u));
            gte_emit_sat_ir(e, ARM_R5, ARM_R2, 3u, lm);
            emit_str_imm(e, ARM_CC_AL, ARM_R5, JIT_REG_CPU, OFF_D(11u));

            gte_emit_epilogue(e, has_tr, lbl_bounce, opcode);
            return true;
        }

        case 0x0Cu: { /* OP: outer product of (R11, R22, R33) x (IR1, IR2, IR3) */
            emit_const(e, GTE_REG_FLAG, 0u);
            emit_ldr_imm(e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, OFF_C(0u));
            emit_sxth(e, ARM_CC_AL, ARM_R0, ARM_R0, 0u); /* D1 = R11 */
            emit_ldr_imm(e, ARM_CC_AL, ARM_R1, JIT_REG_CPU, OFF_C(2u));
            emit_sxth(e, ARM_CC_AL, ARM_R1, ARM_R1, 0u); /* D2 = R22 */
            emit_ldr_imm(e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, OFF_C(4u));
            emit_sxth(e, ARM_CC_AL, ARM_R2, ARM_R2, 0u); /* D3 = R33 */

            emit_ldr_imm(e, ARM_CC_AL, ARM_R3, JIT_REG_CPU, OFF_D(9u));
            emit_sxth(e, ARM_CC_AL, ARM_R3, ARM_R3, 0u); /* IR1 */
            emit_ldr_imm(e, ARM_CC_AL, ARM_R4, JIT_REG_CPU, OFF_D(10u));
            emit_sxth(e, ARM_CC_AL, ARM_R4, ARM_R4, 0u); /* IR2 */
            emit_ldr_imm(e, ARM_CC_AL, ARM_R5, JIT_REG_CPU, OFF_D(11u));
            emit_sxth(e, ARM_CC_AL, ARM_R5, ARM_R5, 0u); /* IR3 */

            /* MAC1 = (D2*IR3 - D3*IR2) >> sf */
            emit_mul(e, ARM_CC_AL, ARM_R6, ARM_R1, ARM_R5);
            emit_mul(e, ARM_CC_AL, ARM_R7, ARM_R2, ARM_R4);
            emit_alu_reg(e, ARM_CC_AL, ARM_OP_SUB, 0u, ARM_R6, ARM_R6, ARM_R7);
            if (sf12) emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R6, ARM_R6, ARM_SH_ASR, 12u);
            emit_str_imm(e, ARM_CC_AL, ARM_R6, JIT_REG_CPU, OFF_D(25u));

            /* MAC2 = (D3*IR1 - D1*IR3) >> sf */
            emit_mul(e, ARM_CC_AL, ARM_R7, ARM_R2, ARM_R3);
            emit_mul(e, ARM_CC_AL, ARM_R12, ARM_R0, ARM_R5);
            emit_alu_reg(e, ARM_CC_AL, ARM_OP_SUB, 0u, ARM_R7, ARM_R7, ARM_R12);
            if (sf12) emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R7, ARM_R7, ARM_SH_ASR, 12u);
            emit_str_imm(e, ARM_CC_AL, ARM_R7, JIT_REG_CPU, OFF_D(26u));

            /* MAC3 = (D1*IR2 - D2*IR1) >> sf */
            emit_mul(e, ARM_CC_AL, ARM_R12, ARM_R0, ARM_R4);
            emit_mul(e, ARM_CC_AL, ARM_LR, ARM_R1, ARM_R3);
            emit_alu_reg(e, ARM_CC_AL, ARM_OP_SUB, 0u, ARM_R12, ARM_R12, ARM_LR);
            if (sf12) emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R12, ARM_R12, ARM_SH_ASR, 12u);
            emit_str_imm(e, ARM_CC_AL, ARM_R12, JIT_REG_CPU, OFF_D(27u));

            gte_emit_sat_ir(e, ARM_R0, ARM_R6, 1u, lm);
            emit_str_imm(e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, OFF_D(9u));
            gte_emit_sat_ir(e, ARM_R0, ARM_R7, 2u, lm);
            emit_str_imm(e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, OFF_D(10u));
            gte_emit_sat_ir(e, ARM_R0, ARM_R12, 3u, lm);
            emit_str_imm(e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, OFF_D(11u));

            gte_emit_epilogue(e, false, 0u, opcode);
            return true;
        }

        case 0x28u: { /* SQR: MAC_k = (IR_k * IR_k) >> sf */
            emit_const(e, GTE_REG_FLAG, 0u);
            for (u32 k = 0u; k < 3u; ++k) {
                emit_ldr_imm(e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, OFF_D(9u + k));
                emit_sxth(e, ARM_CC_AL, ARM_R0, ARM_R0, 0u);
                emit_mul(e, ARM_CC_AL, ARM_R1, ARM_R0, ARM_R0);
                if (sf12) emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R1, ARM_R1, ARM_SH_ASR, 12u);
                emit_str_imm(e, ARM_CC_AL, ARM_R1, JIT_REG_CPU, OFF_D(25u + k));
                gte_emit_sat_ir(e, ARM_R2, ARM_R1, k + 1u, lm);
                emit_str_imm(e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, OFF_D(9u + k));
            }
            gte_emit_epilogue(e, false, 0u, opcode);
            return true;
        }

        case 0x3Du: { /* GPF: MAC_k = (IR0 * IR_k) >> sf + push RGB FIFO */
            emit_const(e, GTE_REG_FLAG, 0u);
            emit_ldr_imm(e, ARM_CC_AL, ARM_R5, JIT_REG_CPU, OFF_D(8u));
            emit_sxth(e, ARM_CC_AL, ARM_R5, ARM_R5, 0u); /* IR0 */
            for (u32 k = 0u; k < 3u; ++k) {
                emit_ldr_imm(e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, OFF_D(9u + k));
                emit_sxth(e, ARM_CC_AL, ARM_R0, ARM_R0, 0u);
                emit_mul(e, ARM_CC_AL, ARM_R1, ARM_R5, ARM_R0);
                if (sf12) emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R1, ARM_R1, ARM_SH_ASR, 12u);
                emit_str_imm(e, ARM_CC_AL, ARM_R1, JIT_REG_CPU, OFF_D(25u + k));
                gte_emit_sat_ir(e, ARM_R2, ARM_R1, k + 1u, lm);
                emit_str_imm(e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, OFF_D(9u + k));
            }
            gte_emit_push_rgb_fifo(e);
            gte_emit_epilogue(e, false, 0u, opcode);
            return true;
        }

        case 0x3Eu: { /* GPL: MAC_k = ((MAC_k << sf) + IR0 * IR_k) >> sf + push RGB FIFO */
            u16 lbl_bounce = arm_label_create(e);
            for (u32 k = 0u; k < 3u; ++k) {
                emit_ldr_imm(e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, OFF_D(25u + k));
                gte_emit_guard_2p30(e, ARM_R2, ARM_R3, lbl_bounce);
            }
            emit_const(e, GTE_REG_FLAG, 0u);
            emit_ldr_imm(e, ARM_CC_AL, ARM_R5, JIT_REG_CPU, OFF_D(8u));
            emit_sxth(e, ARM_CC_AL, ARM_R5, ARM_R5, 0u); /* IR0 */
            for (u32 k = 0u; k < 3u; ++k) {
                emit_ldr_imm(e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, OFF_D(25u + k));
                if (sf12) {
                    emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R3, ARM_R2, ARM_SH_LSL, 12u);
                    emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R4, ARM_R2, ARM_SH_ASR, 20u);
                } else {
                    emit_mov_reg(e, ARM_R3, ARM_R2);
                    emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R4, ARM_R2, ARM_SH_ASR, 31u);
                }
                emit_ldr_imm(e, ARM_CC_AL, ARM_R0, JIT_REG_CPU, OFF_D(9u + k));
                emit_sxth(e, ARM_CC_AL, ARM_R0, ARM_R0, 0u);
                emit_smlal(e, ARM_CC_AL, ARM_R3, ARM_R4, ARM_R5, ARM_R0);
                /* Check 32-bit overflow when sf=0 (Note: 44-bit cannot overflow since |MAC|<2^30) */
                gte_emit_shift_sf64(e, ARM_R1, ARM_R3, ARM_R4, sf12);
                emit_str_imm(e, ARM_CC_AL, ARM_R1, JIT_REG_CPU, OFF_D(25u + k));
                gte_emit_sat_ir(e, ARM_R2, ARM_R1, k + 1u, lm);
                emit_str_imm(e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, OFF_D(9u + k));
            }
            gte_emit_push_rgb_fifo(e);
            gte_emit_epilogue(e, true, lbl_bounce, opcode);
            return true;
        }

        case 0x10u:   /* DPCS */
        case 0x11u: { /* INTPL */
            u16 lbl_bounce = arm_label_create(e);
            for (u32 k = 0u; k < 3u; ++k) {
                emit_ldr_imm(e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, OFF_C(21u + k)); /* RFC/GFC/BFC */
                gte_emit_guard_2p30(e, ARM_R2, ARM_R3, lbl_bounce);
            }
            emit_const(e, GTE_REG_FLAG, 0u);
            emit_ldr_imm(e, ARM_CC_AL, ARM_R6, JIT_REG_CPU, OFF_D(8u));
            emit_sxth(e, ARM_CC_AL, ARM_R6, ARM_R6, 0u); /* IR0 */
            if (fn == 0x10u) {
                emit_ldr_imm(e, ARM_CC_AL, ARM_R7, JIT_REG_CPU, OFF_D(6u)); /* RGBC */
            }
            for (u32 k = 0u; k < 3u; ++k) {
                /* r5 = base[k] (fits in 32-bit signed: <= 0xFF0000 or +-0x8000000) */
                if (fn == 0x10u) {
                    if (k == 0u) {
                        emit_uxtb(e, ARM_CC_AL, ARM_R5, ARM_R7);
                    } else {
                        emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R5, ARM_R7, ARM_SH_LSR, k * 8u);
                        emit_uxtb(e, ARM_CC_AL, ARM_R5, ARM_R5);
                    }
                    emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R5, ARM_R5, ARM_SH_LSL, 16u);
                } else {
                    emit_ldr_imm(e, ARM_CC_AL, ARM_R5, JIT_REG_CPU, OFF_D(9u + k));
                    emit_sxth(e, ARM_CC_AL, ARM_R5, ARM_R5, 0u);
                    emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R5, ARM_R5, ARM_SH_LSL, 12u);
                }

                /* a1 = (fc << 12) - base[k] in 64-bit r4:r3 */
                emit_ldr_imm(e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, OFF_C(21u + k));
                emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R3, ARM_R2, ARM_SH_LSL, 12u);
                emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R4, ARM_R2, ARM_SH_ASR, 20u);
                emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R0, ARM_R5, ARM_SH_ASR, 31u);
                emit_alu_reg(e, ARM_CC_AL, ARM_OP_SUB, 1u /*S*/, ARM_R3, ARM_R3, ARM_R5);
                emit_alu_reg(e, ARM_CC_AL, ARM_OP_SBC, 0u, ARM_R4, ARM_R4, ARM_R0);

                /* Step 1 temporary IR always saturates (s32)(a1 >> sf) with lm = false */
                gte_emit_shift_sf64(e, ARM_R1, ARM_R3, ARM_R4, sf12);
                gte_emit_sat_ir(e, ARM_R2, ARM_R1, k + 1u, false);

                /* Step 2: a2 = base[k] + ir0 * tmp_ir (fits in 32-bit signed!) */
                emit_mul(e, ARM_CC_AL, ARM_R1, ARM_R6, ARM_R2);
                emit_alu_reg(e, ARM_CC_AL, ARM_OP_ADD, 0u, ARM_R1, ARM_R5, ARM_R1);
                if (sf12) emit_shift_imm(e, ARM_CC_AL, 0u, ARM_R1, ARM_R1, ARM_SH_ASR, 12u);
                emit_str_imm(e, ARM_CC_AL, ARM_R1, JIT_REG_CPU, OFF_D(25u + k));
                gte_emit_sat_ir(e, ARM_R2, ARM_R1, k + 1u, lm);
                emit_str_imm(e, ARM_CC_AL, ARM_R2, JIT_REG_CPU, OFF_D(9u + k));
            }
            gte_emit_push_rgb_fifo(e);
            gte_emit_epilogue(e, true, lbl_bounce, opcode);
            return true;
        }

        default:
            return false;
    }
}
