#include "psx3ds/gte.h"
#include "psx3ds/system.h"

#include <string.h>

#define GTE_FLAG_ERR_MASK 0x7F87E000u
#define GTE_FLAG_RW_MASK  0x7FFFF000u

/* --------------------------------------------------------------------------
 * Exact 44-bit / 32-bit Saturation & Overflow Helpers (100% UBSan-safe)
 * -------------------------------------------------------------------------- */
PSX_INLINE s64 gte_shl_s64(s64 v, u32 sh) {
    return (s64)((u64)v << sh);
}

PSX_INLINE s64 gte_check_mac44(s64 val, u32 idx, u32 *flag) {
    /* idx in {1, 2, 3}: pos overflow -> bits 30,29,28; neg underflow -> bits 27,26,25 */
    const s64 min44 = -(1LL << 43);
    const s64 max44 = (1LL << 43) - 1LL;
    if (val < min44) {
        *flag |= (1u << (28u - idx));
    } else if (val > max44) {
        *flag |= (1u << (31u - idx));
    }
    /* True 44-bit signed accumulator wrap */
    u64 u = (u64)val & 0x00000FFFFFFFFFFFULL;
    if ((u & 0x0000080000000000ULL) != 0ULL) {
        u |= 0xFFFFF00000000000ULL;
    }
    return (s64)u;
}

PSX_INLINE s64 gte_check_mac0(s64 val, u32 *flag) {
    /* 32-bit signed overflow -> bit 16 (pos), bit 15 (neg) */
    if (val < -0x80000000LL) {
        *flag |= (1u << 15);
    } else if (val > 0x7FFFFFFFLL) {
        *flag |= (1u << 16);
    }
    return val;
}

PSX_INLINE s32 gte_sat_ir(s32 val, u32 idx, bool lm, u32 *flag) {
    /* idx in {1, 2, 3} -> bits 24, 23, 22 */
    s32 lo = lm ? 0 : -0x8000;
    if (val < lo) {
        *flag |= (1u << (25u - idx));
        return lo;
    }
    if (val > 0x7FFF) {
        *flag |= (1u << (25u - idx));
        return 0x7FFF;
    }
    return val;
}

PSX_INLINE s32 gte_sat_ir0(s64 val, u32 *flag) {
    /* IR0 clamps to [0, 0x1000] inclusive -> bit 12 */
    if (val < 0LL) {
        *flag |= (1u << 12);
        return 0;
    }
    if (val > 0x1000LL) {
        *flag |= (1u << 12);
        return 0x1000;
    }
    return (s32)val;
}

PSX_INLINE u32 gte_sat_sz(s64 val, u32 *flag) {
    /* SZ3 / OTZ clamps to [0, 0xFFFF] -> bit 18 */
    if (val < 0LL) {
        *flag |= (1u << 18);
        return 0u;
    }
    if (val > 0xFFFFLL) {
        *flag |= (1u << 18);
        return 0xFFFFu;
    }
    return (u32)val;
}

PSX_INLINE s32 gte_sat_sxy(s64 val, u32 bit, u32 *flag) {
    /* SX2 (bit=14), SY2 (bit=13) clamp to [-0x400, 0x3FF] */
    if (val < -0x400LL) {
        *flag |= (1u << bit);
        return -0x400;
    }
    if (val > 0x3FFLL) {
        *flag |= (1u << bit);
        return 0x3FF;
    }
    return (s32)val;
}

PSX_INLINE u32 gte_sat_cd(s32 val, u32 idx, u32 *flag) {
    /* Color FIFO R/G/B (idx=1,2,3 -> bits 21,20,19) clamp to [0, 0xFF] */
    if (val < 0) {
        *flag |= (1u << (22u - idx));
        return 0u;
    }
    if (val > 0xFF) {
        *flag |= (1u << (22u - idx));
        return 0xFFu;
    }
    return (u32)val;
}

PSX_INLINE u32 gte_pack_irgb(const PsxCpu *cpu) {
    s32 r = ((s32)(s16)cpu->gte_d[9]) >> 7;
    s32 g = ((s32)(s16)cpu->gte_d[10]) >> 7;
    s32 b = ((s32)(s16)cpu->gte_d[11]) >> 7;
    if (r < 0) r = 0; else if (r > 0x1F) r = 0x1F;
    if (g < 0) g = 0; else if (g > 0x1F) g = 0x1F;
    if (b < 0) b = 0; else if (b > 0x1F) b = 0x1F;
    return (u32)r | ((u32)g << 5) | ((u32)b << 10);
}

PSX_INLINE void gte_push_rgb_fifo(PsxCpu *c, u32 code_byte, u32 *flag) {
    u32 r = gte_sat_cd(((s32)c->gte_d[25]) >> 4, 1u, flag);
    u32 g = gte_sat_cd(((s32)c->gte_d[26]) >> 4, 2u, flag);
    u32 b = gte_sat_cd(((s32)c->gte_d[27]) >> 4, 3u, flag);
    c->gte_d[20] = c->gte_d[21];
    c->gte_d[21] = c->gte_d[22];
    c->gte_d[22] = r | (g << 8) | (b << 16) | (code_byte << 24);
}

/* --------------------------------------------------------------------------
 * UNR Division & Register Transfer Quirks
 * -------------------------------------------------------------------------- */
u32 gte_div_unr(u32 h, u32 sz3, bool *overflow) {
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
    /* sz3 > 0 guaranteed since h < 2 * sz3 */
    u32 z = (u32)__builtin_clz(sz3) - 16u;
    u32 n = h << z;
    u32 d = sz3 << z;
    u32 u = (u32)gte_unr_table[((d & 0x7FFFu) + 0x40u) >> 7] + 0x101u;
    u32 d2 = (0x2000080u - (d * u)) >> 8;
    u32 d3 = (0x0000080u + (d2 * u)) >> 8;
    u32 res = (u32)((((u64)n * (u64)d3) + 0x8000ULL) >> 16);
    if (res > 0x1FFFFu) {
        res = 0x1FFFFu;
    }
    return res;
}

u32 gte_read_data(PsxCpu *cpu, u32 reg) {
    reg &= 31u;
    switch (reg) {
        case 1u:  /* VZ0 */
        case 3u:  /* VZ1 */
        case 5u:  /* VZ2 */
        case 8u:  /* IR0 */
        case 9u:  /* IR1 */
        case 10u: /* IR2 */
        case 11u: /* IR3 */
            return (u32)(s32)(s16)cpu->gte_d[reg];
        case 7u:  /* OTZ */
        case 16u: /* SZ0 */
        case 17u: /* SZ1 */
        case 18u: /* SZ2 */
        case 19u: /* SZ3 */
            return cpu->gte_d[reg] & 0xFFFFu;
        case 15u: /* SXYP mirrors SXY2 */
            return cpu->gte_d[14];
        case 28u: /* IRGB */
        case 29u: /* ORGB */
            return gte_pack_irgb(cpu);
        default:
            return cpu->gte_d[reg];
    }
}

void gte_write_data(PsxCpu *cpu, u32 reg, u32 val) {
    reg &= 31u;
    switch (reg) {
        case 1u:
        case 3u:
        case 5u:
        case 8u:
        case 9u:
        case 10u:
        case 11u:
            cpu->gte_d[reg] = (u32)(s32)(s16)val;
            break;
        case 7u:
        case 16u:
        case 17u:
        case 18u:
        case 19u:
            cpu->gte_d[reg] = val & 0xFFFFu;
            break;
        case 15u: /* SXYP FIFO push */
            cpu->gte_d[12] = cpu->gte_d[13];
            cpu->gte_d[13] = cpu->gte_d[14];
            cpu->gte_d[14] = val;
            cpu->gte_d[15] = val;
            break;
        case 28u: /* IRGB expands 5-bit lanes * 0x80 into IR1..IR3 */
            cpu->gte_d[28] = val & 0x7FFFu;
            cpu->gte_d[9]  = (val & 0x1Fu) * 0x80u;
            cpu->gte_d[10] = ((val >> 5) & 0x1Fu) * 0x80u;
            cpu->gte_d[11] = ((val >> 10) & 0x1Fu) * 0x80u;
            break;
        case 29u: /* ORGB read-only */
        case 31u: /* LZCR read-only */
            break;
        case 30u: { /* LZCS -> LZCR leading sign-bit count */
            cpu->gte_d[30] = val;
            u32 tmp = ((val & 0x80000000u) != 0u) ? ~val : val;
            cpu->gte_d[31] = (tmp == 0u) ? 32u : (u32)__builtin_clz(tmp);
            break;
        }
        default:
            cpu->gte_d[reg] = val;
            break;
    }
}

u32 gte_read_ctrl(const PsxCpu *cpu, u32 reg) {
    reg &= 31u;
    switch (reg) {
        case 4u:  /* R33  (36) */
        case 12u: /* L33  (44) */
        case 20u: /* LB3  (52) */
        case 26u: /* H    (58) */
        case 27u: /* DQA  (59) */
        case 29u: /* ZSF3 (61) */
        case 30u: /* ZSF4 (62) */
            return (u32)(s32)(s16)cpu->gte_c[reg];
        case 31u: { /* FLAG (63) */
            u32 f = cpu->gte_c[31] & GTE_FLAG_RW_MASK;
            if ((f & GTE_FLAG_ERR_MASK) != 0u) {
                f |= 0x80000000u;
            }
            return f;
        }
        default:
            return cpu->gte_c[reg];
    }
}

void gte_write_ctrl(PsxCpu *cpu, u32 reg, u32 val) {
    reg &= 31u;
    switch (reg) {
        case 4u:
        case 12u:
        case 20u:
        case 26u:
        case 27u:
        case 29u:
        case 30u:
            cpu->gte_c[reg] = (u32)(s32)(s16)val;
            break;
        case 31u: {
            u32 f = val & GTE_FLAG_RW_MASK;
            if ((f & GTE_FLAG_ERR_MASK) != 0u) {
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

/* --------------------------------------------------------------------------
 * Shared Matrix / Depth-Cue Subroutines for the 22 GTE Opcodes
 * -------------------------------------------------------------------------- */
static void gte_unpack_matrix(const u32 *base, s16 m[3][3]) {
    m[0][0] = (s16)(base[0] & 0xFFFFu);
    m[0][1] = (s16)(base[0] >> 16);
    m[0][2] = (s16)(base[1] & 0xFFFFu);
    m[1][0] = (s16)(base[1] >> 16);
    m[1][1] = (s16)(base[2] & 0xFFFFu);
    m[1][2] = (s16)(base[2] >> 16);
    m[2][0] = (s16)(base[3] & 0xFFFFu);
    m[2][1] = (s16)(base[3] >> 16);
    m[2][2] = (s16)(base[4] & 0xFFFFu);
}

static void gte_mat_vec(PsxCpu *c, const s16 m[3][3], const s16 v[3],
                        const s32 tr[3], u32 sf, bool lm, u32 *flag) {
    for (u32 k = 0u; k < 3u; k++) {
        u32 idx = k + 1u;
        s64 a = gte_check_mac44(gte_shl_s64((s64)tr[k], 12u) + (s64)m[k][0] * (s64)v[0], idx, flag);
        a = gte_check_mac44(a + (s64)m[k][1] * (s64)v[1], idx, flag);
        a = gte_check_mac44(a + (s64)m[k][2] * (s64)v[2], idx, flag);
        s32 mac = (s32)(a >> sf);
        c->gte_d[25u + k] = (u32)mac;
        c->gte_d[9u + k] = (u32)gte_sat_ir(mac, idx, lm, flag);
    }
}

static void gte_depth_cue_vec(PsxCpu *c, const s64 base[3], u32 sf, bool lm,
                              u32 code_byte, u32 *flag) {
    s64 ir0 = (s64)(s16)c->gte_d[8];
    for (u32 k = 0u; k < 3u; k++) {
        u32 idx = k + 1u;
        s32 fc = (s32)c->gte_c[21u + k];
        s64 a1 = gte_check_mac44(gte_shl_s64((s64)fc, 12u) - base[k], idx, flag);
        /* Hardware step 1 always saturates temporary IR with lm = false */
        s32 tmp_ir = gte_sat_ir((s32)(a1 >> sf), idx, false, flag);
        s64 a2 = gte_check_mac44(base[k] + ir0 * (s64)tmp_ir, idx, flag);
        s32 mac = (s32)(a2 >> sf);
        c->gte_d[25u + k] = (u32)mac;
        c->gte_d[9u + k] = (u32)gte_sat_ir(mac, idx, lm, flag);
    }
    gte_push_rgb_fifo(c, code_byte, flag);
}

static void gte_do_rtp_vertex(PsxCpu *c, u32 v_idx, u32 sf, bool lm,
                              bool do_depth_cue, u32 *flag) {
    s16 rt[3][3];
    gte_unpack_matrix(&c->gte_c[0], rt);

    s16 vx = (s16)(c->gte_d[v_idx * 2u] & 0xFFFFu);
    s16 vy = (s16)(c->gte_d[v_idx * 2u] >> 16);
    s16 vz = (s16)(c->gte_d[v_idx * 2u + 1u] & 0xFFFFu);

    s64 raw[3];
    for (u32 k = 0u; k < 3u; k++) {
        u32 idx = k + 1u;
        s32 tr = (s32)c->gte_c[5u + k];
        s64 a = gte_check_mac44(gte_shl_s64((s64)tr, 12u) + (s64)rt[k][0] * (s64)vx, idx, flag);
        a = gte_check_mac44(a + (s64)rt[k][1] * (s64)vy, idx, flag);
        a = gte_check_mac44(a + (s64)rt[k][2] * (s64)vz, idx, flag);
        raw[k] = a;
        c->gte_d[25u + k] = (u32)(s32)(a >> sf);
    }

    c->gte_d[9]  = (u32)gte_sat_ir((s32)c->gte_d[25], 1u, lm, flag);
    c->gte_d[10] = (u32)gte_sat_ir((s32)c->gte_d[26], 2u, lm, flag);

    /*
     * RTPS/RTPT hardware IR3 quirk:
     * FLAG bit 22 is evaluated on (raw[2] >> 12) against [-0x8000, 0x7FFF] ignoring sf & lm,
     * while the IR3 register value itself uses the real (raw[2] >> sf) clamped with lm!
     */
    s64 ir3_flag_val = raw[2] >> 12;
    if (ir3_flag_val < -0x8000LL || ir3_flag_val > 0x7FFFLL) {
        *flag |= (1u << 22);
    }
    s32 mac3 = (s32)c->gte_d[27];
    s32 ir3_lo = lm ? 0 : -0x8000;
    s32 ir3_val = (mac3 < ir3_lo) ? ir3_lo : ((mac3 > 0x7FFF) ? 0x7FFF : mac3);
    c->gte_d[11] = (u32)ir3_val;

    /* SZ FIFO push (from 44-bit raw[2] >> 12, NOT 32-bit MAC3 when sf=0) */
    u32 sz3 = gte_sat_sz(raw[2] >> 12, flag);
    c->gte_d[16] = c->gte_d[17];
    c->gte_d[17] = c->gte_d[18];
    c->gte_d[18] = c->gte_d[19];
    c->gte_d[19] = sz3;

    bool div_ovf = false;
    u32 h = c->gte_c[26] & 0xFFFFu;
    u32 q = gte_div_unr(h, sz3, &div_ovf);
    if (div_ovf) {
        *flag |= (1u << 17);
    }

    s64 ofx = (s64)(s32)c->gte_c[24];
    s64 ofy = (s64)(s32)c->gte_c[25];
    s64 px = gte_check_mac0(ofx + (s64)q * (s64)(s16)c->gte_d[9], flag);
    s64 py = gte_check_mac0(ofy + (s64)q * (s64)(s16)c->gte_d[10], flag);
    c->gte_d[24] = (u32)(s32)py;

    s32 sx2 = gte_sat_sxy(px >> 16, 14u, flag);
    s32 sy2 = gte_sat_sxy(py >> 16, 13u, flag);
    u32 sxy2 = ((u32)sx2 & 0xFFFFu) | ((u32)sy2 << 16);
    c->gte_d[12] = c->gte_d[13];
    c->gte_d[13] = c->gte_d[14];
    c->gte_d[14] = sxy2;
    c->gte_d[15] = sxy2;

    if (do_depth_cue) {
        s64 dqa = (s64)(s16)c->gte_c[27];
        s64 dqb = (s64)(s32)c->gte_c[28];
        s64 dq = gte_check_mac0(dqb + dqa * (s64)q, flag);
        c->gte_d[24] = (u32)(s32)dq;
        c->gte_d[8] = (u32)gte_sat_ir0(dq >> 12, flag);
    }
}

static void gte_do_ncd_vertex(PsxCpu *c, u32 v_idx, u32 sf, bool lm,
                              bool with_color, bool with_dp, u32 *flag) {
    s16 llm[3][3];
    s16 lcm[3][3];
    gte_unpack_matrix(&c->gte_c[8], llm);
    gte_unpack_matrix(&c->gte_c[16], lcm);

    s16 v[3] = {
        (s16)(c->gte_d[v_idx * 2u] & 0xFFFFu),
        (s16)(c->gte_d[v_idx * 2u] >> 16),
        (s16)(c->gte_d[v_idx * 2u + 1u] & 0xFFFFu)
    };
    const s32 zero3[3] = { 0, 0, 0 };
    const s32 bk[3] = { (s32)c->gte_c[13], (s32)c->gte_c[14], (s32)c->gte_c[15] };

    /* Step 1: IR = LLM * V */
    gte_mat_vec(c, llm, v, zero3, sf, lm, flag);

    /* Step 2: IR = BK + LCM * IR */
    s16 ir_vec[3] = { (s16)c->gte_d[9], (s16)c->gte_d[10], (s16)c->gte_d[11] };
    gte_mat_vec(c, lcm, ir_vec, bk, sf, lm, flag);

    u32 rgbc = c->gte_d[6];
    u32 code = rgbc >> 24;
    if (!with_color) {
        /* NCS / NCT */
        gte_push_rgb_fifo(c, code, flag);
        return;
    }

    s64 r = (s64)(rgbc & 0xFFu);
    s64 g = (s64)((rgbc >> 8) & 0xFFu);
    s64 b = (s64)((rgbc >> 16) & 0xFFu);
    s64 base[3] = {
        gte_shl_s64(r * (s64)(s16)c->gte_d[9], 4u),
        gte_shl_s64(g * (s64)(s16)c->gte_d[10], 4u),
        gte_shl_s64(b * (s64)(s16)c->gte_d[11], 4u)
    };

    if (!with_dp) {
        /* NCCS / NCCT */
        for (u32 k = 0u; k < 3u; k++) {
            u32 idx = k + 1u;
            s64 a = gte_check_mac44(base[k], idx, flag);
            s32 mac = (s32)(a >> sf);
            c->gte_d[25u + k] = (u32)mac;
            c->gte_d[9u + k] = (u32)gte_sat_ir(mac, idx, lm, flag);
        }
        gte_push_rgb_fifo(c, code, flag);
    } else {
        /* NCDS / NCDT */
        gte_depth_cue_vec(c, base, sf, lm, code, flag);
    }
}

/* --------------------------------------------------------------------------
 * Reference COP2 Command Executor (All 22 Opcodes)
 * -------------------------------------------------------------------------- */
void gte_exec_c(PsxCpu *c, u32 opcode) {
    u32 fn = opcode & 0x3Fu;
    u32 sf = ((opcode >> 19) & 1u) ? 12u : 0u;
    bool lm = ((opcode >> 10) & 1u) != 0u;
    u32 flag = 0u;

    switch (fn) {
        case 0x01u: /* RTPS */
            gte_do_rtp_vertex(c, 0u, sf, lm, true, &flag);
            break;

        case 0x06u: { /* NCLIP */
            s64 sx0 = (s64)(s16)(c->gte_d[12] & 0xFFFFu);
            s64 sy0 = (s64)(s16)(c->gte_d[12] >> 16);
            s64 sx1 = (s64)(s16)(c->gte_d[13] & 0xFFFFu);
            s64 sy1 = (s64)(s16)(c->gte_d[13] >> 16);
            s64 sx2 = (s64)(s16)(c->gte_d[14] & 0xFFFFu);
            s64 sy2 = (s64)(s16)(c->gte_d[14] >> 16);
            s64 sum = sx0 * (sy1 - sy2) + sx1 * (sy2 - sy0) + sx2 * (sy0 - sy1);
            c->gte_d[24] = (u32)(s32)gte_check_mac0(sum, &flag);
            break;
        }

        case 0x0Cu: { /* OP */
            s64 d1 = (s64)(s16)(c->gte_c[0] & 0xFFFFu); /* R11 */
            s64 d2 = (s64)(s16)(c->gte_c[2] & 0xFFFFu); /* R22 */
            s64 d3 = (s64)(s16)(c->gte_c[4] & 0xFFFFu); /* R33 */
            s64 ir1 = (s64)(s16)c->gte_d[9];
            s64 ir2 = (s64)(s16)c->gte_d[10];
            s64 ir3 = (s64)(s16)c->gte_d[11];

            s64 a1 = gte_check_mac44(d2 * ir3 - d3 * ir2, 1u, &flag);
            s64 a2 = gte_check_mac44(d3 * ir1 - d1 * ir3, 2u, &flag);
            s64 a3 = gte_check_mac44(d1 * ir2 - d2 * ir1, 3u, &flag);

            c->gte_d[25] = (u32)(s32)(a1 >> sf);
            c->gte_d[26] = (u32)(s32)(a2 >> sf);
            c->gte_d[27] = (u32)(s32)(a3 >> sf);
            c->gte_d[9]  = (u32)gte_sat_ir((s32)c->gte_d[25], 1u, lm, &flag);
            c->gte_d[10] = (u32)gte_sat_ir((s32)c->gte_d[26], 2u, lm, &flag);
            c->gte_d[11] = (u32)gte_sat_ir((s32)c->gte_d[27], 3u, lm, &flag);
            break;
        }

        case 0x10u: { /* DPCS */
            u32 rgbc = c->gte_d[6];
            s64 base[3] = {
                (s64)((rgbc & 0xFFu) << 16),
                (s64)(((rgbc >> 8) & 0xFFu) << 16),
                (s64)(((rgbc >> 16) & 0xFFu) << 16)
            };
            gte_depth_cue_vec(c, base, sf, lm, rgbc >> 24, &flag);
            break;
        }

        case 0x11u: { /* INTPL */
            s64 base[3] = {
                gte_shl_s64((s64)(s16)c->gte_d[9], 12u),
                gte_shl_s64((s64)(s16)c->gte_d[10], 12u),
                gte_shl_s64((s64)(s16)c->gte_d[11], 12u)
            };
            gte_depth_cue_vec(c, base, sf, lm, c->gte_d[6] >> 24, &flag);
            break;
        }

        case 0x12u: { /* MVMVA */
            u32 mx = (opcode >> 17) & 3u;
            u32 vx = (opcode >> 15) & 3u;
            u32 cv = (opcode >> 13) & 3u;

            s16 m[3][3];
            if (mx < 3u) {
                gte_unpack_matrix(&c->gte_c[mx * 8u], m);
            } else {
                /* Sony hardware mx=3 garbage matrix */
                s16 r4 = (s16)((c->gte_d[6] & 0xFFu) << 4);
                s16 r13 = (s16)(c->gte_c[1] & 0xFFFFu);
                s16 r22 = (s16)(c->gte_c[2] & 0xFFFFu);
                m[0][0] = (s16)(-r4);
                m[0][1] = r4;
                m[0][2] = (s16)c->gte_d[8];
                m[1][0] = r13;
                m[1][1] = r13;
                m[1][2] = r13;
                m[2][0] = r22;
                m[2][1] = r22;
                m[2][2] = r22;
            }

            s16 v[3];
            if (vx < 3u) {
                v[0] = (s16)(c->gte_d[vx * 2u] & 0xFFFFu);
                v[1] = (s16)(c->gte_d[vx * 2u] >> 16);
                v[2] = (s16)(c->gte_d[vx * 2u + 1u] & 0xFFFFu);
            } else {
                v[0] = (s16)c->gte_d[9];
                v[1] = (s16)c->gte_d[10];
                v[2] = (s16)c->gte_d[11];
            }

            s32 tr[3] = { 0, 0, 0 };
            if (cv == 0u) {
                tr[0] = (s32)c->gte_c[5];
                tr[1] = (s32)c->gte_c[6];
                tr[2] = (s32)c->gte_c[7];
            } else if (cv == 1u) {
                tr[0] = (s32)c->gte_c[13];
                tr[1] = (s32)c->gte_c[14];
                tr[2] = (s32)c->gte_c[15];
            } else if (cv == 2u) {
                tr[0] = (s32)c->gte_c[21];
                tr[1] = (s32)c->gte_c[22];
                tr[2] = (s32)c->gte_c[23];
            }

            if (cv == 2u) {
                /* Sony hardware Far Color bug: step 1 flushed to dummy IR(lm=0), omitted from MAC */
                for (u32 k = 0u; k < 3u; k++) {
                    u32 idx = k + 1u;
                    s64 a1 = gte_check_mac44(gte_shl_s64((s64)tr[k], 12u) + (s64)m[k][0] * (s64)v[0],
                                             idx, &flag);
                    (void)gte_sat_ir((s32)(a1 >> sf), idx, false, &flag);
                    s64 a2 = gte_check_mac44((s64)m[k][1] * (s64)v[1], idx, &flag);
                    s64 a3 = gte_check_mac44(a2 + (s64)m[k][2] * (s64)v[2], idx, &flag);
                    s32 mac = (s32)(a3 >> sf);
                    c->gte_d[25u + k] = (u32)mac;
                    c->gte_d[9u + k] = (u32)gte_sat_ir(mac, idx, lm, &flag);
                }
            } else {
                gte_mat_vec(c, m, v, tr, sf, lm, &flag);
            }
            break;
        }

        case 0x13u: /* NCDS */
            gte_do_ncd_vertex(c, 0u, sf, lm, true, true, &flag);
            break;

        case 0x14u: { /* CDP */
            s16 lcm[3][3];
            gte_unpack_matrix(&c->gte_c[16], lcm);
            s16 ir_vec[3] = { (s16)c->gte_d[9], (s16)c->gte_d[10], (s16)c->gte_d[11] };
            const s32 bk[3] = { (s32)c->gte_c[13], (s32)c->gte_c[14], (s32)c->gte_c[15] };
            gte_mat_vec(c, lcm, ir_vec, bk, sf, lm, &flag);

            u32 rgbc = c->gte_d[6];
            s64 base[3] = {
                gte_shl_s64((s64)(rgbc & 0xFFu) * (s64)(s16)c->gte_d[9], 4u),
                gte_shl_s64((s64)((rgbc >> 8) & 0xFFu) * (s64)(s16)c->gte_d[10], 4u),
                gte_shl_s64((s64)((rgbc >> 16) & 0xFFu) * (s64)(s16)c->gte_d[11], 4u)
            };
            gte_depth_cue_vec(c, base, sf, lm, rgbc >> 24, &flag);
            break;
        }

        case 0x16u: /* NCDT */
            gte_do_ncd_vertex(c, 0u, sf, lm, true, true, &flag);
            gte_do_ncd_vertex(c, 1u, sf, lm, true, true, &flag);
            gte_do_ncd_vertex(c, 2u, sf, lm, true, true, &flag);
            break;

        case 0x1Bu: /* NCCS */
            gte_do_ncd_vertex(c, 0u, sf, lm, true, false, &flag);
            break;

        case 0x1Cu: { /* CC */
            s16 lcm[3][3];
            gte_unpack_matrix(&c->gte_c[16], lcm);
            s16 ir_vec[3] = { (s16)c->gte_d[9], (s16)c->gte_d[10], (s16)c->gte_d[11] };
            const s32 bk[3] = { (s32)c->gte_c[13], (s32)c->gte_c[14], (s32)c->gte_c[15] };
            gte_mat_vec(c, lcm, ir_vec, bk, sf, lm, &flag);

            u32 rgbc = c->gte_d[6];
            s64 base[3] = {
                gte_shl_s64((s64)(rgbc & 0xFFu) * (s64)(s16)c->gte_d[9], 4u),
                gte_shl_s64((s64)((rgbc >> 8) & 0xFFu) * (s64)(s16)c->gte_d[10], 4u),
                gte_shl_s64((s64)((rgbc >> 16) & 0xFFu) * (s64)(s16)c->gte_d[11], 4u)
            };
            for (u32 k = 0u; k < 3u; k++) {
                u32 idx = k + 1u;
                s64 a = gte_check_mac44(base[k], idx, &flag);
                s32 mac = (s32)(a >> sf);
                c->gte_d[25u + k] = (u32)mac;
                c->gte_d[9u + k] = (u32)gte_sat_ir(mac, idx, lm, &flag);
            }
            gte_push_rgb_fifo(c, rgbc >> 24, &flag);
            break;
        }

        case 0x1Eu: /* NCS */
            gte_do_ncd_vertex(c, 0u, sf, lm, false, false, &flag);
            break;

        case 0x20u: /* NCT */
            gte_do_ncd_vertex(c, 0u, sf, lm, false, false, &flag);
            gte_do_ncd_vertex(c, 1u, sf, lm, false, false, &flag);
            gte_do_ncd_vertex(c, 2u, sf, lm, false, false, &flag);
            break;

        case 0x28u: { /* SQR */
            for (u32 k = 0u; k < 3u; k++) {
                u32 idx = k + 1u;
                s64 ir = (s64)(s16)c->gte_d[9u + k];
                s64 a = gte_check_mac44(ir * ir, idx, &flag);
                s32 mac = (s32)(a >> sf);
                c->gte_d[25u + k] = (u32)mac;
                c->gte_d[9u + k] = (u32)gte_sat_ir(mac, idx, lm, &flag);
            }
            break;
        }

        case 0x29u: { /* DCPL */
            u32 rgbc = c->gte_d[6];
            s64 base[3] = {
                gte_shl_s64((s64)(rgbc & 0xFFu) * (s64)(s16)c->gte_d[9], 4u),
                gte_shl_s64((s64)((rgbc >> 8) & 0xFFu) * (s64)(s16)c->gte_d[10], 4u),
                gte_shl_s64((s64)((rgbc >> 16) & 0xFFu) * (s64)(s16)c->gte_d[11], 4u)
            };
            gte_depth_cue_vec(c, base, sf, lm, rgbc >> 24, &flag);
            break;
        }

        case 0x2Au: { /* DPCT (3x from RGB0 as FIFO shifts; CODE byte from RGBC) */
            u32 code = c->gte_d[6] >> 24;
            for (u32 step = 0u; step < 3u; step++) {
                u32 rgb0 = c->gte_d[20];
                s64 base[3] = {
                    (s64)((rgb0 & 0xFFu) << 16),
                    (s64)(((rgb0 >> 8) & 0xFFu) << 16),
                    (s64)(((rgb0 >> 16) & 0xFFu) << 16)
                };
                gte_depth_cue_vec(c, base, sf, lm, code, &flag);
            }
            break;
        }

        case 0x2Du: { /* AVSZ3 */
            s64 zsf3 = (s64)(s16)c->gte_c[29];
            s64 sz_sum = (s64)(c->gte_d[17] & 0xFFFFu) +
                         (s64)(c->gte_d[18] & 0xFFFFu) +
                         (s64)(c->gte_d[19] & 0xFFFFu);
            s64 prod = gte_check_mac0(zsf3 * sz_sum, &flag);
            c->gte_d[24] = (u32)(s32)prod;
            c->gte_d[7] = gte_sat_sz(prod >> 12, &flag);
            break;
        }

        case 0x2Eu: { /* AVSZ4 */
            s64 zsf4 = (s64)(s16)c->gte_c[30];
            s64 sz_sum = (s64)(c->gte_d[16] & 0xFFFFu) +
                         (s64)(c->gte_d[17] & 0xFFFFu) +
                         (s64)(c->gte_d[18] & 0xFFFFu) +
                         (s64)(c->gte_d[19] & 0xFFFFu);
            s64 prod = gte_check_mac0(zsf4 * sz_sum, &flag);
            c->gte_d[24] = (u32)(s32)prod;
            c->gte_d[7] = gte_sat_sz(prod >> 12, &flag);
            break;
        }

        case 0x30u: /* RTPT (depth cueing on V2 only) */
            gte_do_rtp_vertex(c, 0u, sf, lm, false, &flag);
            gte_do_rtp_vertex(c, 1u, sf, lm, false, &flag);
            gte_do_rtp_vertex(c, 2u, sf, lm, true, &flag);
            break;

        case 0x3Du: { /* GPF */
            s64 ir0 = (s64)(s16)c->gte_d[8];
            for (u32 k = 0u; k < 3u; k++) {
                u32 idx = k + 1u;
                s64 a = gte_check_mac44(ir0 * (s64)(s16)c->gte_d[9u + k], idx, &flag);
                s32 mac = (s32)(a >> sf);
                c->gte_d[25u + k] = (u32)mac;
                c->gte_d[9u + k] = (u32)gte_sat_ir(mac, idx, lm, &flag);
            }
            gte_push_rgb_fifo(c, c->gte_d[6] >> 24, &flag);
            break;
        }

        case 0x3Eu: { /* GPL */
            s64 ir0 = (s64)(s16)c->gte_d[8];
            for (u32 k = 0u; k < 3u; k++) {
                u32 idx = k + 1u;
                s64 prev_mac = gte_shl_s64((s64)(s32)c->gte_d[25u + k], sf);
                s64 a = gte_check_mac44(prev_mac + ir0 * (s64)(s16)c->gte_d[9u + k], idx, &flag);
                s32 mac = (s32)(a >> sf);
                c->gte_d[25u + k] = (u32)mac;
                c->gte_d[9u + k] = (u32)gte_sat_ir(mac, idx, lm, &flag);
            }
            gte_push_rgb_fifo(c, c->gte_d[6] >> 24, &flag);
            break;
        }

        case 0x3Fu: /* NCCT */
            gte_do_ncd_vertex(c, 0u, sf, lm, true, false, &flag);
            gte_do_ncd_vertex(c, 1u, sf, lm, true, false, &flag);
            gte_do_ncd_vertex(c, 2u, sf, lm, true, false, &flag);
            break;

        default:
            break;
    }

    if ((flag & GTE_FLAG_ERR_MASK) != 0u) {
        flag |= 0x80000000u;
    }
    c->gte_c[31] = flag;
}

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */
void gte_init(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    for (u32 i = 0u; i < 257u; i++) {
        sys->gte.unr_table[i] = (u32)gte_unr_table[i];
    }
    gte_reset(sys);
}

void gte_reset(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    memset(sys->cpu.gte_d, 0, sizeof(sys->cpu.gte_d));
    memset(sys->cpu.gte_c, 0, sizeof(sys->cpu.gte_c));
}

void gte_state(PsxSystem *sys, StateIO *st) {
    /* Architectural registers gte_d[32] / gte_c[32] are serialized in cpu_state() */
    (void)sys;
    (void)st;
}

void gte_shutdown(PsxSystem *sys) {
    (void)sys;
}
