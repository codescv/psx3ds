#include "periph_internal.h"

#include <string.h>

enum {
    REV_dAPF1   = 0,
    REV_dAPF2   = 1,
    REV_vIIR    = 2,
    REV_vCOMB1  = 3,
    REV_vCOMB2  = 4,
    REV_vCOMB3  = 5,
    REV_vCOMB4  = 6,
    REV_vWALL   = 7,
    REV_vAPF1   = 8,
    REV_vAPF2   = 9,
    REV_mLSAME  = 10,
    REV_mRSAME  = 11,
    REV_mLCOMB1 = 12,
    REV_mRCOMB1 = 13,
    REV_mLCOMB2 = 14,
    REV_mRCOMB2 = 15,
    REV_dLSAME  = 16,
    REV_dRSAME  = 17,
    REV_mLDIFF  = 18,
    REV_mRDIFF  = 19,
    REV_mLCOMB3 = 20,
    REV_mRCOMB3 = 21,
    REV_mLCOMB4 = 22,
    REV_mRCOMB4 = 23,
    REV_dLDIFF  = 24,
    REV_dRDIFF  = 25,
    REV_mLAPF1  = 26,
    REV_mRAPF1  = 27,
    REV_mLAPF2  = 28,
    REV_mRAPF2  = 29,
    REV_vLIN    = 30,
    REV_vRIN    = 31
};

PSX_INLINE s16 rev_clamp16(s32 v) {
    if (v > 32767) {
        return 32767;
    }
    if (v < -32768) {
        return -32768;
    }
    return (s16)v;
}

PSX_INLINE s32 rev_mul(s32 a, s32 b) {
    return (s32)(((s64)a * (s64)b) >> 15);
}

/*
 * v0.2.8: division-free ring wrap. reverb_pos < size_bytes and |rel_bytes| < 0x80000, so
 * one add/subtract of size_bytes covers every normal case; the 64-bit "%" (software
 * __aeabi_ldivmod on ARM11, 29x per 22.05 kHz tick = ~640k calls/s) only remains as a
 * cold fallback for tiny work areas (mBASE near the end of SPU RAM).
 */
static u32 rev_calc_addr(const PsxSpu *spu, u32 base_byte, u32 size_bytes, s32 rel_bytes) {
    s32 off = (s32)spu->reverb_pos + rel_bytes;
    if (off < 0) {
        off += (s32)size_bytes;
        if (PSX_UNLIKELY(off < 0)) {
            off %= (s32)size_bytes;
            if (off < 0) {
                off += (s32)size_bytes;
            }
        }
    } else if ((u32)off >= size_bytes) {
        off -= (s32)size_bytes;
        if (PSX_UNLIKELY((u32)off >= size_bytes)) {
            off = (s32)((u32)off % size_bytes);
        }
    }
    return (base_byte + (u32)off) & 0x7FFFEu;
}

PSX_INLINE u32 rev_next_pos(u32 pos, u32 size_bytes) {
    u32 next_pos = pos + 2u;
    if (next_pos >= size_bytes) {
        next_pos -= size_bytes;
        if (PSX_UNLIKELY(next_pos >= size_bytes)) {
            next_pos %= size_bytes;
        }
    }
    return next_pos;
}

static s32 rev_read16(PsxSystem *sys, u32 base_byte, u32 size_bytes, s32 rel_bytes) {
    PsxSpu *spu = &sys->spu;
    u32 addr = rev_calc_addr(spu, base_byte, size_bytes, rel_bytes);
    spu_check_irq_addr(sys, addr);
    u16 lo = spu->ram[addr];
    u16 hi = spu->ram[addr + 1u];
    return (s32)(s16)(lo | (u16)(hi << 8));
}

static void rev_write16(PsxSystem *sys, u32 base_byte, u32 size_bytes, s32 rel_bytes, s16 val) {
    PsxSpu *spu = &sys->spu;
    u32 addr = rev_calc_addr(spu, base_byte, size_bytes, rel_bytes);
    spu_check_irq_addr(sys, addr);
    if ((spu->spucnt & (1u << 7)) != 0u) {
        u16 u = (u16)val;
        spu->ram[addr]      = (u8)(u & 0xFFu);
        spu->ram[addr + 1u] = (u8)(u >> 8);
    }
}

void spu_reverb_reset(PsxSpu *spu) {
    memset(spu->reverb_regs, 0, sizeof(spu->reverb_regs));
    spu->reverb_pos = 0u;
    if (spu->reverb_buf) {
        spu->reverb_buf[0] = 0;
        spu->reverb_buf[1] = 0;
        spu->reverb_buf[2] = 0;
    }
}

void spu_reverb_process(PsxSystem *sys, s32 in_l, s32 in_r, s32 *out_l, s32 *out_r) {
    PsxSpu *spu = &sys->spu;

    if (!sys->cfg.spu_reverb || !spu->ram) {
        *out_l = 0;
        *out_r = 0;
        return;
    }

    /* SPU reverb executes at 22.05 kHz (every 2nd 44.1 kHz sample) */
    s16 phase = spu->reverb_buf ? spu->reverb_buf[2] : 0;
    if (spu->reverb_buf) {
        spu->reverb_buf[2] = (s16)(phase ^ 1);
    }

    s32 vlout = (s16)spu->regs[(0x1D84u - 0x1C00u) >> 1];
    s32 vrout = (s16)spu->regs[(0x1D86u - 0x1C00u) >> 1];

    if (phase != 0 && spu->reverb_buf) {
        *out_l = rev_mul(spu->reverb_buf[0], vlout);
        *out_r = rev_mul(spu->reverb_buf[1], vrout);
        return;
    }

    u32 mbase     = spu->regs[(0x1DA2u - 0x1C00u) >> 1];
    u32 base_byte = (mbase << 3) & 0x7FFFEu;
    u32 size_bytes = PSX_SPU_RAM_BYTES - base_byte;
    if (size_bytes < 8u) {
        size_bytes = 8u;
    }

    /*
     * v0.2.8 fast path: reverb output muted (vLOUT = vROUT = 0), work-area writes disabled
     * (SPUCNT.7) and SPU IRQ9 disabled (SPUCNT.6): the 28 RAM reads / 8 writes can have no
     * audible or guest-visible effect, so only advance the work-area position.
     */
    if (vlout == 0 && vrout == 0 && (spu->spucnt & 0x00C0u) == 0u) {
        spu->reverb_pos = rev_next_pos(spu->reverb_pos, size_bytes);
        if (spu->reverb_buf) {
            spu->reverb_buf[0] = 0;
            spu->reverb_buf[1] = 0;
        }
        *out_l = 0;
        *out_r = 0;
        return;
    }

    const s32 *r = spu->reverb_regs;
    s32 dAPF1   = ((u16)r[REV_dAPF1])   << 3;
    s32 dAPF2   = ((u16)r[REV_dAPF2])   << 3;
    s32 vIIR    = (s16)r[REV_vIIR];
    s32 vCOMB1  = (s16)r[REV_vCOMB1];
    s32 vCOMB2  = (s16)r[REV_vCOMB2];
    s32 vCOMB3  = (s16)r[REV_vCOMB3];
    s32 vCOMB4  = (s16)r[REV_vCOMB4];
    s32 vWALL   = (s16)r[REV_vWALL];
    s32 vAPF1   = (s16)r[REV_vAPF1];
    s32 vAPF2   = (s16)r[REV_vAPF2];
    s32 mLSAME  = ((u16)r[REV_mLSAME])  << 3;
    s32 mRSAME  = ((u16)r[REV_mRSAME])  << 3;
    s32 mLCOMB1 = ((u16)r[REV_mLCOMB1]) << 3;
    s32 mRCOMB1 = ((u16)r[REV_mRCOMB1]) << 3;
    s32 mLCOMB2 = ((u16)r[REV_mLCOMB2]) << 3;
    s32 mRCOMB2 = ((u16)r[REV_mRCOMB2]) << 3;
    s32 dLSAME  = ((u16)r[REV_dLSAME])  << 3;
    s32 dRSAME  = ((u16)r[REV_dRSAME])  << 3;
    s32 mLDIFF  = ((u16)r[REV_mLDIFF])  << 3;
    s32 mRDIFF  = ((u16)r[REV_mRDIFF])  << 3;
    s32 mLCOMB3 = ((u16)r[REV_mLCOMB3]) << 3;
    s32 mRCOMB3 = ((u16)r[REV_mRCOMB3]) << 3;
    s32 mLCOMB4 = ((u16)r[REV_mLCOMB4]) << 3;
    s32 mRCOMB4 = ((u16)r[REV_mRCOMB4]) << 3;
    s32 dLDIFF  = ((u16)r[REV_dLDIFF])  << 3;
    s32 dRDIFF  = ((u16)r[REV_dRDIFF])  << 3;
    s32 mLAPF1  = ((u16)r[REV_mLAPF1])  << 3;
    s32 mRAPF1  = ((u16)r[REV_mRAPF1])  << 3;
    s32 mLAPF2  = ((u16)r[REV_mLAPF2])  << 3;
    s32 mRAPF2  = ((u16)r[REV_mRAPF2])  << 3;
    s32 vLIN    = (s16)r[REV_vLIN];
    s32 vRIN    = (s16)r[REV_vRIN];

    s32 lin = rev_mul(rev_clamp16(in_l), vLIN);
    s32 rin = rev_mul(rev_clamp16(in_r), vRIN);

    /* Same-side reflections */
    s32 lsame_prev = rev_read16(sys, base_byte, size_bytes, mLSAME - 2);
    s32 dlsame_val = rev_read16(sys, base_byte, size_bytes, dLSAME);
    s32 lsame_new  = rev_clamp16(rev_mul(rev_clamp16(lin + rev_mul(dlsame_val, vWALL) - lsame_prev), vIIR) + lsame_prev);
    rev_write16(sys, base_byte, size_bytes, mLSAME, (s16)lsame_new);

    s32 rsame_prev = rev_read16(sys, base_byte, size_bytes, mRSAME - 2);
    s32 drsame_val = rev_read16(sys, base_byte, size_bytes, dRSAME);
    s32 rsame_new  = rev_clamp16(rev_mul(rev_clamp16(rin + rev_mul(drsame_val, vWALL) - rsame_prev), vIIR) + rsame_prev);
    rev_write16(sys, base_byte, size_bytes, mRSAME, (s16)rsame_new);

    /* Cross-side reflections */
    s32 ldiff_prev = rev_read16(sys, base_byte, size_bytes, mLDIFF - 2);
    s32 drdiff_val = rev_read16(sys, base_byte, size_bytes, dRDIFF);
    s32 ldiff_new  = rev_clamp16(rev_mul(rev_clamp16(lin + rev_mul(drdiff_val, vWALL) - ldiff_prev), vIIR) + ldiff_prev);
    rev_write16(sys, base_byte, size_bytes, mLDIFF, (s16)ldiff_new);

    s32 rdiff_prev = rev_read16(sys, base_byte, size_bytes, mRDIFF - 2);
    s32 dldiff_val = rev_read16(sys, base_byte, size_bytes, dLDIFF);
    s32 rdiff_new  = rev_clamp16(rev_mul(rev_clamp16(rin + rev_mul(dldiff_val, vWALL) - rdiff_prev), vIIR) + rdiff_prev);
    rev_write16(sys, base_byte, size_bytes, mRDIFF, (s16)rdiff_new);

    /* Early echo 4-tap comb filter */
    s32 lout = rev_clamp16(
        rev_mul(vCOMB1, rev_read16(sys, base_byte, size_bytes, mLCOMB1)) +
        rev_mul(vCOMB2, rev_read16(sys, base_byte, size_bytes, mLCOMB2)) +
        rev_mul(vCOMB3, rev_read16(sys, base_byte, size_bytes, mLCOMB3)) +
        rev_mul(vCOMB4, rev_read16(sys, base_byte, size_bytes, mLCOMB4)));

    s32 rout = rev_clamp16(
        rev_mul(vCOMB1, rev_read16(sys, base_byte, size_bytes, mRCOMB1)) +
        rev_mul(vCOMB2, rev_read16(sys, base_byte, size_bytes, mRCOMB2)) +
        rev_mul(vCOMB3, rev_read16(sys, base_byte, size_bytes, mRCOMB3)) +
        rev_mul(vCOMB4, rev_read16(sys, base_byte, size_bytes, mRCOMB4)));

    /* Late reverb all-pass filter 1 */
    s32 lapf1_del = rev_read16(sys, base_byte, size_bytes, mLAPF1 - dAPF1);
    lout = rev_clamp16(lout - rev_mul(vAPF1, lapf1_del));
    rev_write16(sys, base_byte, size_bytes, mLAPF1, (s16)lout);
    lout = rev_clamp16(lapf1_del + rev_mul(lout, vAPF1));

    s32 rapf1_del = rev_read16(sys, base_byte, size_bytes, mRAPF1 - dAPF1);
    rout = rev_clamp16(rout - rev_mul(vAPF1, rapf1_del));
    rev_write16(sys, base_byte, size_bytes, mRAPF1, (s16)rout);
    rout = rev_clamp16(rapf1_del + rev_mul(rout, vAPF1));

    /* Late reverb all-pass filter 2 */
    s32 lapf2_del = rev_read16(sys, base_byte, size_bytes, mLAPF2 - dAPF2);
    lout = rev_clamp16(lout - rev_mul(vAPF2, lapf2_del));
    rev_write16(sys, base_byte, size_bytes, mLAPF2, (s16)lout);
    lout = rev_clamp16(lapf2_del + rev_mul(lout, vAPF2));

    s32 rapf2_del = rev_read16(sys, base_byte, size_bytes, mRAPF2 - dAPF2);
    rout = rev_clamp16(rout - rev_mul(vAPF2, rapf2_del));
    rev_write16(sys, base_byte, size_bytes, mRAPF2, (s16)rout);
    rout = rev_clamp16(rapf2_del + rev_mul(rout, vAPF2));

    spu->reverb_pos = rev_next_pos(spu->reverb_pos, size_bytes);

    if (spu->reverb_buf) {
        spu->reverb_buf[0] = (s16)lout;
        spu->reverb_buf[1] = (s16)rout;
    }

    *out_l = rev_mul(lout, vlout);
    *out_r = rev_mul(rout, vrout);
}
