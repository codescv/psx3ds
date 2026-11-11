#include "periph_internal.h"
#include "psx3ds/arena.h"
#include "psx3ds/bus.h"
#include "psx3ds/state.h"

#include <string.h>

enum {
    ADSR_OFF     = 0,
    ADSR_ATTACK  = 1,
    ADSR_DECAY   = 2,
    ADSR_SUSTAIN = 3,
    ADSR_RELEASE = 4
};

/*
 * Per-instance extended SPU state stored inside the upper portion of
 * spu->reverb_buf (allocated from sys->arena, zero heap / zero globals).
 * spu_reverb.c uses reverb_buf[0..2]; offset 16 (32-byte aligned) onward holds SpuExt.
 */
typedef struct SpuExt {
    u32 adsr_ticks[24];
    u32 sweep_ticks_l[24];
    u32 sweep_ticks_r[24];
    s16 cur_vol_l[24];
    s16 cur_vol_r[24];
    s16 last_vout[24];
    s16 cur_main_l;
    s16 cur_main_r;
    u32 main_ticks_l;
    u32 main_ticks_r;
    u32 noise_timer;
    u32 capture_pos;
    /* v0.2.8: last CD-DA / XA frame, decayed (not snapped to 0) across xa_ring gaps */
    s16 last_cd_l;
    s16 last_cd_r;
} SpuExt;

#define SPU_EXT_OFFSET_HWORD 16u
#define SPU_REVERB_BUF_BYTES (2048u)

_Static_assert(SPU_EXT_OFFSET_HWORD * sizeof(s16) + sizeof(SpuExt) <= SPU_REVERB_BUF_BYTES,
               "SpuExt fits inside spu->reverb_buf arena allocation");

PSX_INLINE SpuExt *spu_ext(PsxSpu *spu) {
    return (SpuExt *)(void *)(spu->reverb_buf + SPU_EXT_OFFSET_HWORD);
}

/* Exact Sony CXD2922BQ / CXD2925Q 512-entry 4-tap Gaussian interpolation table */
static const s16 k_spu_gauss[512] = {
    -0x0001, -0x0001, -0x0001, -0x0001, -0x0001, -0x0001, -0x0001, -0x0001, /* 0x000 */
    -0x0001, -0x0001, -0x0001, -0x0001, -0x0001, -0x0001, -0x0001, -0x0001, /* 0x008 */
     0x0000,  0x0000,  0x0000,  0x0000,  0x0000,  0x0000,  0x0000,  0x0001, /* 0x010 */
     0x0001,  0x0001,  0x0001,  0x0002,  0x0002,  0x0002,  0x0003,  0x0003, /* 0x018 */
     0x0003,  0x0004,  0x0004,  0x0005,  0x0005,  0x0006,  0x0007,  0x0007, /* 0x020 */
     0x0008,  0x0009,  0x0009,  0x000A,  0x000B,  0x000C,  0x000D,  0x000E, /* 0x028 */
     0x000F,  0x0010,  0x0011,  0x0012,  0x0013,  0x0015,  0x0016,  0x0018, /* 0x030 */
     0x0019,  0x001B,  0x001C,  0x001E,  0x0020,  0x0021,  0x0023,  0x0025, /* 0x038 */
     0x0027,  0x0029,  0x002C,  0x002E,  0x0030,  0x0033,  0x0035,  0x0038, /* 0x040 */
     0x003A,  0x003D,  0x0040,  0x0043,  0x0046,  0x0049,  0x004D,  0x0050, /* 0x048 */
     0x0054,  0x0057,  0x005B,  0x005F,  0x0063,  0x0067,  0x006B,  0x006F, /* 0x050 */
     0x0073,  0x0078,  0x007C,  0x0081,  0x0086,  0x008B,  0x0090,  0x0095, /* 0x058 */
     0x009A,  0x00A0,  0x00A5,  0x00AB,  0x00B1,  0x00B7,  0x00BD,  0x00C3, /* 0x060 */
     0x00C9,  0x00CF,  0x00D6,  0x00DD,  0x00E4,  0x00EB,  0x00F2,  0x00F9, /* 0x068 */
     0x0100,  0x0108,  0x010F,  0x0117,  0x011F,  0x0127,  0x0130,  0x0138, /* 0x070 */
     0x0141,  0x0149,  0x0152,  0x015B,  0x0164,  0x016D,  0x0177,  0x0180, /* 0x078 */
     0x018A,  0x0193,  0x019D,  0x01A7,  0x01B1,  0x01BC,  0x01C6,  0x01D1, /* 0x080 */
     0x01DF,  0x01EA,  0x01F8,  0x0205,  0x0213,  0x0221,  0x0230,  0x023F, /* 0x088 */
     0x024E,  0x025C,  0x026C,  0x027D,  0x028D,  0x029E,  0x02AF,  0x02C1, /* 0x090 */
     0x02D2,  0x02E5,  0x02F7,  0x030A,  0x031E,  0x0332,  0x0346,  0x035B, /* 0x098 */
     0x0370,  0x0384,  0x039B,  0x03B0,  0x03C7,  0x03DF,  0x03F4,  0x040E, /* 0x0A0 */
     0x0424,  0x043D,  0x0456,  0x046E,  0x0488,  0x04A2,  0x04BE,  0x04D7, /* 0x0A8 */
     0x04F3,  0x050E,  0x052A,  0x0546,  0x0563,  0x0580,  0x059E,  0x05BB, /* 0x0B0 */
     0x05D9,  0x05F8,  0x0617,  0x0637,  0x0657,  0x0677,  0x0698,  0x06BA, /* 0x0B8 */
     0x06DB,  0x06FE,  0x0720,  0x0742,  0x0766,  0x078B,  0x07AF,  0x07D4, /* 0x0C0 */
     0x07F9,  0x081E,  0x0844,  0x086C,  0x0892,  0x08BA,  0x08E1,  0x090A, /* 0x0C8 */
     0x0932,  0x095C,  0x0986,  0x09AF,  0x09DA,  0x0A06,  0x0A30,  0x0A5C, /* 0x0D0 */
     0x0A89,  0x0AB6,  0x0AE4,  0x0B12,  0x0B41,  0x0B6F,  0x0B9F,  0x0BCE, /* 0x0D8 */
     0x0BFE,  0x0C2F,  0x0C61,  0x0C93,  0x0CC4,  0x0CF8,  0x0D2A,  0x0D5E, /* 0x0E0 */
     0x0D91,  0x0DC7,  0x0DFC,  0x0E31,  0x0E68,  0x0E9E,  0x0ED5,  0x0F0D, /* 0x0E8 */
     0x0F45,  0x0F7D,  0x0FB6,  0x0FEF,  0x1029,  0x1064,  0x109E,  0x10DB, /* 0x0F0 */
     0x1116,  0x1153,  0x1190,  0x11CD,  0x120B,  0x1249,  0x1288,  0x12C7, /* 0x0F8 */
     0x1307,  0x1347,  0x1388,  0x13C9,  0x140B,  0x144D,  0x1490,  0x14D4, /* 0x100 */
     0x1517,  0x155C,  0x15A0,  0x15E6,  0x162C,  0x1672,  0x16B9,  0x1700, /* 0x108 */
     0x1747,  0x1790,  0x17D8,  0x1821,  0x186B,  0x18B5,  0x1900,  0x194B, /* 0x110 */
     0x1996,  0x19E2,  0x1A2E,  0x1A7B,  0x1AC8,  0x1B16,  0x1B64,  0x1BB3, /* 0x118 */
     0x1C02,  0x1C51,  0x1CA1,  0x1CF1,  0x1D42,  0x1D93,  0x1DE5,  0x1E37, /* 0x120 */
     0x1E89,  0x1EDC,  0x1F2F,  0x1F82,  0x1FD6,  0x202A,  0x207F,  0x20D4, /* 0x128 */
     0x2129,  0x217F,  0x21D5,  0x222C,  0x2282,  0x22DA,  0x2331,  0x2389, /* 0x130 */
     0x23E1,  0x2439,  0x2492,  0x24EB,  0x2545,  0x259E,  0x25F8,  0x2653, /* 0x138 */
     0x26AD,  0x2708,  0x2763,  0x27BE,  0x281A,  0x2876,  0x28D2,  0x292E, /* 0x140 */
     0x298B,  0x29E7,  0x2A44,  0x2AA1,  0x2AFF,  0x2B5C,  0x2BBA,  0x2C18, /* 0x148 */
     0x2C76,  0x2CD4,  0x2D33,  0x2D91,  0x2DF0,  0x2E4F,  0x2EAE,  0x2F0D, /* 0x150 */
     0x2F6C,  0x2FCC,  0x302B,  0x308B,  0x30EA,  0x314A,  0x31AA,  0x3209, /* 0x158 */
     0x3269,  0x32C9,  0x3329,  0x3389,  0x33E9,  0x3449,  0x34A9,  0x3509, /* 0x160 */
     0x3569,  0x35C9,  0x3629,  0x3689,  0x36E8,  0x3748,  0x37A8,  0x3807, /* 0x168 */
     0x3867,  0x38C6,  0x3926,  0x3985,  0x39E4,  0x3A43,  0x3AA2,  0x3B00, /* 0x170 */
     0x3B5F,  0x3BBD,  0x3C1B,  0x3C79,  0x3CD7,  0x3D35,  0x3D92,  0x3DEF, /* 0x178 */
     0x3E4C,  0x3EA9,  0x3F05,  0x3F62,  0x3FBD,  0x4019,  0x4074,  0x40D0, /* 0x180 */
     0x412A,  0x4185,  0x41DF,  0x4239,  0x4292,  0x42EB,  0x4344,  0x439C, /* 0x188 */
     0x43F4,  0x444C,  0x44A3,  0x44FA,  0x4550,  0x45A6,  0x45FC,  0x4651, /* 0x190 */
     0x46A6,  0x46FA,  0x474E,  0x47A1,  0x47F4,  0x4846,  0x4898,  0x48E9, /* 0x198 */
     0x493A,  0x498A,  0x49D9,  0x4A29,  0x4A77,  0x4AC5,  0x4B13,  0x4B5F, /* 0x1A0 */
     0x4BAC,  0x4BF7,  0x4C42,  0x4C8D,  0x4CD7,  0x4D20,  0x4D68,  0x4DB0, /* 0x1A8 */
     0x4DF7,  0x4E3E,  0x4E84,  0x4EC9,  0x4F0E,  0x4F52,  0x4F95,  0x4FD7, /* 0x1B0 */
     0x5019,  0x505A,  0x509A,  0x50DA,  0x5118,  0x5156,  0x5194,  0x51D0, /* 0x1B8 */
     0x520C,  0x5247,  0x5281,  0x52BA,  0x52F3,  0x532A,  0x5361,  0x5397, /* 0x1C0 */
     0x53CC,  0x5401,  0x5434,  0x5467,  0x5499,  0x54CA,  0x54FA,  0x5529, /* 0x1C8 */
     0x5558,  0x5585,  0x55B2,  0x55DE,  0x5609,  0x5632,  0x565B,  0x5684, /* 0x1D0 */
     0x56AB,  0x56D1,  0x56F6,  0x571B,  0x573E,  0x5761,  0x5782,  0x57A3, /* 0x1D8 */
     0x57C3,  0x57E2,  0x57FF,  0x581C,  0x5838,  0x5853,  0x586D,  0x5886, /* 0x1E0 */
     0x589E,  0x58B5,  0x58CB,  0x58E0,  0x58F4,  0x5907,  0x5919,  0x592A, /* 0x1E8 */
     0x593A,  0x5949,  0x5958,  0x5965,  0x5971,  0x597C,  0x5986,  0x598F, /* 0x1F0 */
     0x5997,  0x599E,  0x59A4,  0x59A9,  0x59AD,  0x59B0,  0x59B2,  0x59B3, /* 0x1F8 */
};

PSX_INLINE s16 spu_sat16(s32 x) {
    if (x > 32767) {
        return 32767;
    }
    if (x < -32768) {
        return -32768;
    }
    return (s16)x;
}

PSX_INLINE s32 spu_asr15(s64 x) {
    if (x < 0) {
        return (s32)(~((~x) >> 15));
    }
    return (s32)(x >> 15);
}

/* --------------------------------------------------------------------------
 * Core 0 / Core 1 serialization (ARMv6K LDREX/STREX via __atomic, no ARMv7 dmb)
 * -------------------------------------------------------------------------- */
static inline void spu_lock(PsxSpu *spu) {
    u32 spins = 0u;
    while (__atomic_exchange_n(&spu->render_lock, 1u, __ATOMIC_ACQUIRE) != 0u) {
        if (++spins >= 64u) {
            plat_yield(); /* holder may be a time-sliced SysCore thread */
            spins = 0u;
        }
    }
}

static inline bool spu_trylock(PsxSpu *spu) {
    return __atomic_exchange_n(&spu->render_lock, 1u, __ATOMIC_ACQUIRE) == 0u;
}

static inline void spu_unlock(PsxSpu *spu) {
    __atomic_store_n(&spu->render_lock, 0u, __ATOMIC_RELEASE);
}

/* IRQ9 assertion: Core 1 (and Core-0 batch renders) latch it; Core 0 delivers it */
static void spu_raise_irq9(PsxSystem *sys) {
    PsxSpu *spu = &sys->spu;
    if (spu->defer_irq != 0u) {
        __atomic_store_n(&spu->irq_pending, 1u, __ATOMIC_RELEASE);
    } else {
        irq_raise(sys, IRQ_SPU);
    }
}

/* Core 0 only */
static void spu_deliver_pending_irq(PsxSystem *sys) {
    PsxSpu *spu = &sys->spu;
    if (__atomic_load_n(&spu->irq_pending, __ATOMIC_ACQUIRE) != 0u &&
        __atomic_exchange_n(&spu->irq_pending, 0u, __ATOMIC_ACQ_REL) != 0u) {
        irq_raise(sys, IRQ_SPU);
    }
}

u32 spu_cmd_backlog(const PsxSystem *sys) {
    if (!sys) {
        return 0u;
    }
    u32 h = __atomic_load_n(&sys->spu.cmd_head, __ATOMIC_ACQUIRE);
    u32 t = __atomic_load_n(&sys->spu.cmd_tail, __ATOMIC_ACQUIRE);
    return h - t;
}

static void spu_update_stat(PsxSpu *spu) {
    u32 mode = (spu->spucnt >> 4) & 3u;
    u32 st   = spu->spucnt & 0x003Fu;
    if ((spu->spucnt & 0x0040u) != 0u && (spu->spustat & (1u << 6)) != 0u) {
        st |= (1u << 6);
    }
    if (mode == 2u) {
        st |= (1u << 7) | (1u << 8);
    } else if (mode == 3u) {
        st |= (1u << 7) | (1u << 9);
    }
    if ((spu->spustat & (1u << 11)) != 0u) {
        st |= (1u << 11);
    }
    spu->spustat = st;
    spu->regs[(PSX_REG_SPUSTAT - PSX_REG_SPU_BASE) >> 1] = (u16)st;
}

void spu_check_irq_addr(PsxSystem *sys, u32 byte_addr) {
    PsxSpu *spu = &sys->spu;
    if ((spu->spucnt & 0x8040u) != 0x8040u) {
        return;
    }
    if (((byte_addr ^ spu->irq_addr) & 0x7FFFEu) == 0u) {
        if ((spu->spustat & (1u << 6)) == 0u) {
            spu->spustat |= (1u << 6);
            spu->regs[(PSX_REG_SPUSTAT - PSX_REG_SPU_BASE) >> 1] = (u16)spu->spustat;
            spu_raise_irq9(sys);
        }
    }
}

void spu_check_irq_range(PsxSystem *sys, u32 byte_addr, u32 len) {
    PsxSpu *spu = &sys->spu;
    if ((spu->spucnt & 0x8040u) != 0x8040u || len == 0u) {
        return;
    }
    u32 start = byte_addr & 0x7FFFFu;
    u32 irq   = spu->irq_addr & 0x7FFFEu;
    u32 end   = start + len;
    bool hit;
    if (end <= PSX_SPU_RAM_BYTES) {
        hit = (irq >= start) && (irq < end);
    } else {
        hit = (irq >= start) || (irq < (end & 0x7FFFFu));
    }
    if (hit && (spu->spustat & (1u << 6)) == 0u) {
        spu->spustat |= (1u << 6);
        spu->regs[(PSX_REG_SPUSTAT - PSX_REG_SPU_BASE) >> 1] = (u16)spu->spustat;
        spu_raise_irq9(sys);
    }
}

static void spu_ram_write16(PsxSystem *sys, u32 addr, u16 val) {
    PsxSpu *spu = &sys->spu;
    u32 a = addr & 0x7FFFEu;
    spu_check_irq_addr(sys, a);
    spu->ram[a]      = (u8)(val & 0xFFu);
    spu->ram[a + 1u] = (u8)(val >> 8);
}

static u16 spu_ram_read16(PsxSystem *sys, u32 addr) {
    PsxSpu *spu = &sys->spu;
    u32 a = addr & 0x7FFFEu;
    spu_check_irq_addr(sys, a);
    return (u16)spu->ram[a] | ((u16)spu->ram[a + 1u] << 8);
}

static void spu_flush_fifo(PsxSystem *sys) {
    PsxSpu *spu = &sys->spu;
    if (!spu->ram) {
        spu->fifo_n = 0u;
        return;
    }
    for (u8 i = 0u; i < spu->fifo_n; i++) {
        spu_ram_write16(sys, spu->xfer_addr, spu->fifo[i]);
        spu->xfer_addr = (spu->xfer_addr + 2u) & 0x7FFFEu;
    }
    spu->fifo_n = 0u;
}

static void spu_step_rate(s32 *level, u32 *ticks, u32 shift, s32 step_base,
                          bool exp_mode, bool decreasing) {
    s32 cur = *level;
    u32 sh  = (shift > 31u) ? 31u : shift;

    u32 period_shift = (sh > 11u) ? (sh - 11u) : 0u;
    u32 step_shift   = (sh < 11u) ? (11u - sh) : 0u;

    u32 period = 1u << period_shift;
    s32 delta  = step_base * (s32)(1u << step_shift);

    if (exp_mode) {
        if (!decreasing) {
            if (cur > 0x6000) {
                if (sh < 10u) {
                    delta >>= 2;
                } else if (sh == 10u) {
                    delta >>= 1;
                    period <<= 1;
                } else {
                    if (period <= 0x3FFFFFFFu) {
                        period <<= 2;
                    } else {
                        period = 0xFFFFFFFFu;
                    }
                }
            }
        } else {
            delta = spu_asr15((s64)delta * (s64)cur);
        }
    }

    u32 t = *ticks + 1u;
    if (t >= period) {
        t = 0u;
        s32 next = cur + delta;
        if (next > 0x7FFF) {
            next = 0x7FFF;
        } else if (next < 0) {
            next = 0;
        }
        *level = next;
    }
    *ticks = t;
}

static void spu_step_sweep_vol(u16 reg_val, s16 *cur_vol, u32 *ticks) {
    if ((reg_val & 0x8000u) == 0u) {
        /* Fixed 15-bit signed volume shifted left by 1 (-0x8000..+0x7FFE) */
        *cur_vol = (s16)(reg_val << 1);
        *ticks   = 0u;
        return;
    }

    bool exp_mode   = ((reg_val >> 14) & 1u) != 0u;
    bool decreasing = ((reg_val >> 13) & 1u) != 0u;
    bool neg_phase  = ((reg_val >> 12) & 1u) != 0u;
    u32  shift      = (reg_val >> 2) & 0x1Fu;
    u32  step_idx   = reg_val & 3u;

    static const s32 k_inc_steps[4] = { +7, +6, +5, +4 };
    static const s32 k_dec_steps[4] = { -8, -7, -6, -5 };

    s32 lvl = *cur_vol;
    if (lvl < 0) {
        lvl = -lvl;
    }
    if (lvl > 0x7FFF) {
        lvl = 0x7FFF;
    }

    s32 step_base = decreasing ? k_dec_steps[step_idx] : k_inc_steps[step_idx];
    spu_step_rate(&lvl, ticks, shift, step_base, exp_mode, decreasing);
    *cur_vol = neg_phase ? (s16)(-lvl) : (s16)lvl;
}

static void spu_step_voice_adsr(SpuVoice *v, u32 *ticks) {
    static const s32 k_inc_steps[4] = { +7, +6, +5, +4 };
    static const s32 k_dec_steps[4] = { -8, -7, -6, -5 };

    switch (v->adsr_phase) {
    case ADSR_OFF:
        v->env_level = 0;
        v->adsr_vol  = 0u;
        return;

    case ADSR_ATTACK: {
        bool exp_mode = (v->adsr_lo & 0x8000u) != 0u;
        u32  shift    = (v->adsr_lo >> 10) & 0x1Fu;
        u32  step_idx = (v->adsr_lo >> 8) & 0x03u;
        spu_step_rate(&v->env_level, ticks, shift, k_inc_steps[step_idx], exp_mode, false);
        if (v->env_level >= 0x7FFF) {
            v->env_level  = 0x7FFF;
            v->adsr_phase = ADSR_DECAY;
            *ticks        = 0u;
            s32 sus_lvl   = (s32)(((v->adsr_lo & 0x0Fu) + 1u) << 11);
            if (v->env_level <= sus_lvl) {
                v->adsr_phase = ADSR_SUSTAIN;
            }
        }
        break;
    }

    case ADSR_DECAY: {
        u32 shift   = (v->adsr_lo >> 4) & 0x0Fu;
        s32 sus_lvl = (s32)(((v->adsr_lo & 0x0Fu) + 1u) << 11);
        if (v->env_level <= sus_lvl) {
            v->adsr_phase = ADSR_SUSTAIN;
            *ticks        = 0u;
        } else {
            spu_step_rate(&v->env_level, ticks, shift, -8, true, true);
            if (v->env_level <= sus_lvl) {
                v->adsr_phase = ADSR_SUSTAIN;
                *ticks        = 0u;
            }
        }
        break;
    }

    case ADSR_SUSTAIN: {
        bool exp_mode   = (v->adsr_hi & 0x8000u) != 0u;
        bool decreasing = (v->adsr_hi & 0x4000u) != 0u;
        u32  shift      = (v->adsr_hi >> 8) & 0x1Fu;
        u32  step_idx   = (v->adsr_hi >> 6) & 0x03u;
        s32  step_base  = decreasing ? k_dec_steps[step_idx] : k_inc_steps[step_idx];
        spu_step_rate(&v->env_level, ticks, shift, step_base, exp_mode, decreasing);
        break;
    }

    case ADSR_RELEASE: {
        bool exp_mode = (v->adsr_hi & 0x0020u) != 0u;
        u32  shift    = v->adsr_hi & 0x1Fu;
        spu_step_rate(&v->env_level, ticks, shift, -8, exp_mode, true);
        if (v->env_level <= 0) {
            v->env_level  = 0;
            v->adsr_phase = ADSR_OFF;
            v->keyed      = 0u;
            *ticks        = 0u;
        }
        break;
    }

    default:
        v->adsr_phase = ADSR_OFF;
        v->env_level  = 0;
        break;
    }

    v->adsr_vol = (u16)v->env_level;
}

static void spu_key_on_mask(PsxSpu *spu, u32 mask24) {
    SpuExt *ext = spu_ext(spu);
    mask24 &= 0x00FFFFFFu;
    if (mask24 == 0u) {
        return;
    }
    for (u32 i = 0u; i < 24u; i++) {
        if ((mask24 & (1u << i)) == 0u) {
            continue;
        }
        SpuVoice *v    = &spu->v[i];
        v->cur_addr    = v->start_addr & 0x7FFF8u;
        v->counter     = 28u << 12;
        v->hist[0]     = 0;
        v->hist[1]     = 0;
        memset(v->ring, 0, sizeof(v->ring));
        v->env_level   = 0;
        v->adsr_vol    = 0u;
        v->adsr_phase  = ADSR_ATTACK;
        v->block_flags = 0u;
        v->keyed       = 1u; /* 1 = awaiting first ADPCM block fetch at start_addr */
        (void)__atomic_and_fetch(&spu->endx, ~(1u << i), __ATOMIC_RELAXED);
        if (ext) {
            ext->adsr_ticks[i] = 0u;
        }
        spu->regs[((i << 4) + 0x0Cu) >> 1] = 0u;
    }
    u32 endx = __atomic_load_n(&spu->endx, __ATOMIC_RELAXED);
    spu->regs[(PSX_REG_SPU_ENDX_LO - PSX_REG_SPU_BASE) >> 1] = (u16)(endx & 0xFFFFu);
    spu->regs[(PSX_REG_SPU_ENDX_HI - PSX_REG_SPU_BASE) >> 1] = (u16)((endx >> 16) & 0x00FFu);
}

static void spu_key_off_mask(PsxSpu *spu, u32 mask24) {
    SpuExt *ext = spu_ext(spu);
    mask24 &= 0x00FFFFFFu;
    if (mask24 == 0u) {
        return;
    }
    for (u32 i = 0u; i < 24u; i++) {
        if ((mask24 & (1u << i)) == 0u) {
            continue;
        }
        SpuVoice *v = &spu->v[i];
        if (v->adsr_phase != ADSR_OFF && v->adsr_phase != ADSR_RELEASE) {
            v->adsr_phase = ADSR_RELEASE;
            if (ext) {
                ext->adsr_ticks[i] = 0u;
            }
        }
    }
}

static void spu_apply_voice_flags(PsxSpu *spu) {
    u32 pmon = (u32)spu->regs[(PSX_REG_SPU_PMON_LO - PSX_REG_SPU_BASE) >> 1] |
               (((u32)spu->regs[(PSX_REG_SPU_PMON_HI - PSX_REG_SPU_BASE) >> 1] & 0xFFu) << 16);
    u32 non  = (u32)spu->regs[(PSX_REG_SPU_NON_LO - PSX_REG_SPU_BASE) >> 1] |
               (((u32)spu->regs[(PSX_REG_SPU_NON_HI - PSX_REG_SPU_BASE) >> 1] & 0xFFu) << 16);
    u32 eon  = (u32)spu->regs[(PSX_REG_SPU_EON_LO - PSX_REG_SPU_BASE) >> 1] |
               (((u32)spu->regs[(PSX_REG_SPU_EON_HI - PSX_REG_SPU_BASE) >> 1] & 0xFFu) << 16);

    pmon &= ~1u; /* Voice 0 cannot pitch-modulate */
    for (u32 i = 0u; i < 24u; i++) {
        spu->v[i].pmon      = (u8)((pmon >> i) & 1u);
        spu->v[i].noise_on  = (u8)((non >> i) & 1u);
        spu->v[i].reverb_on = (u8)((eon >> i) & 1u);
    }
}

/*
 * Apply a 16-bit register write at byte offset [0x000..0x3FE] to live synthesis state.
 * store_reg = false for writes replayed from the async cmd_ring: Core 0 already mirrored
 * spu->regs at MMIO-write time, and re-storing the (possibly older) queued value from
 * Core 1 would race with newer Core 0 writes to the same register (v0.2.7).
 */
static void spu_apply_reg16_ex(PsxSystem *sys, u32 off, u16 val, bool store_reg) {
    PsxSpu *spu = &sys->spu;
    SpuExt *ext = spu_ext(spu);
    u32 idx = (off >> 1) & 0x1FFu;

    /* Read-only hardware registers: ENDX (0x1D9C/1D9E), SPUSTAT (0x1DAE), internal vol mirrors */
    if (off == 0x19Cu || off == 0x19Eu || off == 0x1AEu ||
        off == 0x1B8u || off == 0x1BAu || (off >= 0x200u && off < 0x260u)) {
        return;
    }

    if (store_reg) {
        spu->regs[idx] = val;
    }

    if (off < 0x180u) {
        u32 v_idx = off >> 4;
        u32 sub   = off & 0x0Eu;
        SpuVoice *v = &spu->v[v_idx];
        switch (sub) {
        case 0x00u:
            v->vol_l = val;
            if ((val & 0x8000u) == 0u && ext) {
                ext->cur_vol_l[v_idx]   = (s16)(val << 1);
                ext->sweep_ticks_l[v_idx] = 0u;
                spu->regs[(0x200u + v_idx * 4u) >> 1] = (u16)ext->cur_vol_l[v_idx];
            }
            break;
        case 0x02u:
            v->vol_r = val;
            if ((val & 0x8000u) == 0u && ext) {
                ext->cur_vol_r[v_idx]   = (s16)(val << 1);
                ext->sweep_ticks_r[v_idx] = 0u;
                spu->regs[(0x202u + v_idx * 4u) >> 1] = (u16)ext->cur_vol_r[v_idx];
            }
            break;
        case 0x04u:
            v->pitch = val;
            break;
        case 0x06u:
            v->start_addr = ((u32)val << 3) & 0x7FFF8u;
            break;
        case 0x08u:
            v->adsr_lo = val;
            break;
        case 0x0Au:
            v->adsr_hi = val;
            break;
        case 0x0Cu:
            v->adsr_vol  = val;
            v->env_level = (s32)(val & 0x7FFFu);
            break;
        case 0x0Eu:
            v->repeat_addr = ((u32)val << 3) & 0x7FFF8u;
            break;
        default:
            break;
        }
        return;
    }

    switch (off) {
    case 0x180u: /* Main Volume Left */
        if ((val & 0x8000u) == 0u && ext) {
            ext->cur_main_l   = (s16)(val << 1);
            ext->main_ticks_l = 0u;
            spu->regs[0x1B8u >> 1] = (u16)ext->cur_main_l;
        }
        break;
    case 0x182u: /* Main Volume Right */
        if ((val & 0x8000u) == 0u && ext) {
            ext->cur_main_r   = (s16)(val << 1);
            ext->main_ticks_r = 0u;
            spu->regs[0x1BAu >> 1] = (u16)ext->cur_main_r;
        }
        break;
    case 0x188u: /* KON low */
        spu->kon = (spu->kon & 0x00FF0000u) | (u32)val;
        spu_key_on_mask(spu, (u32)val);
        break;
    case 0x18Au: /* KON high */
        spu->kon = (spu->kon & 0x0000FFFFu) | (((u32)val & 0xFFu) << 16);
        spu_key_on_mask(spu, ((u32)val & 0xFFu) << 16);
        break;
    case 0x18Cu: /* KOFF low */
        spu->koff = (spu->koff & 0x00FF0000u) | (u32)val;
        spu_key_off_mask(spu, (u32)val);
        break;
    case 0x18Eu: /* KOFF high */
        spu->koff = (spu->koff & 0x0000FFFFu) | (((u32)val & 0xFFu) << 16);
        spu_key_off_mask(spu, ((u32)val & 0xFFu) << 16);
        break;
    case 0x190u:
    case 0x192u:
    case 0x194u:
    case 0x196u:
    case 0x198u:
    case 0x19Au:
        spu_apply_voice_flags(spu);
        break;
    case 0x1A2u: /* mBASE */
        spu->reverb_pos = 0u;
        break;
    case 0x1A4u: /* SPU_IRQ_ADDR */
        spu->irq_addr = ((u32)val << 3) & 0x7FFF8u;
        break;
    case 0x1A6u: /* SPU_XFER_ADDR */
        spu->xfer_addr = ((u32)val << 3) & 0x7FFF8u;
        break;
    case 0x1A8u: /* SPU_XFER_FIFO */
        if (spu->fifo_n < 32u) {
            spu->fifo[spu->fifo_n++] = val;
        }
        if (((spu->spucnt >> 4) & 3u) == 1u) {
            spu_flush_fifo(sys);
        }
        break;
    case 0x1AAu: /* SPUCNT */
        spu->spucnt      = val;
        spu->irq_enabled = ((val & 0x8040u) == 0x8040u);
        if ((val & 0x0040u) == 0u) {
            spu->spustat &= ~(1u << 6);
        }
        if (((val >> 4) & 3u) == 1u && spu->fifo_n > 0u) {
            spu_flush_fifo(sys);
        }
        spu_update_stat(spu);
        break;
    case 0x1ACu: /* SPU_XFER_CTRL */
        spu->xfer_ctrl = val;
        break;
    default:
        if (off >= 0x1C0u && off < 0x200u) {
            spu->reverb_regs[(off - 0x1C0u) >> 1] = (s32)(s16)val;
        }
        break;
    }
}

static void spu_apply_reg16(PsxSystem *sys, u32 off, u16 val) {
    spu_apply_reg16_ex(sys, off, val, true);
}

/* Replay of a cmd_ring entry (spu->regs was already mirrored by Core 0) */
static void spu_apply_queued_reg16(PsxSystem *sys, u32 off, u16 val) {
    spu_apply_reg16_ex(sys, off, val, false);
}

static bool spu_cmd_push(PsxSystem *sys, u16 reg, u16 val) {
    PsxSpu *spu = &sys->spu;
    if (!spu->cmd_ring) {
        return false;
    }
    u32 slot = 0u;
    if (!spsc_begin_push(&spu->cmd_head, &spu->cmd_tail, PSX_SPU_CMD_CAP, &slot)) {
        return false;
    }
    spu->cmd_ring[slot].tick = sched_now(sys);
    spu->cmd_ring[slot].reg  = reg;
    spu->cmd_ring[slot].val  = val;
    spsc_commit_push(&spu->cmd_head);
    return true;
}

static void spu_render_frames_core(PsxSystem *sys, u32 frames);

/*
 * Core 0: consume every queued command in issue order — register writes are applied and
 * queued SPU_CMD_REG_ADVANCE emulated time is RENDERED into out_ring (v0.2.8: it used to
 * be dropped, losing up to a whole frame of audio whenever a game enabled SPU IRQ9, DMA'd
 * new samples or a savestate was taken). IRQ9 raised while rendering is latched and
 * delivered after the lock is released.
 */
static void spu_drain_all_cmds(PsxSystem *sys) {
    PsxSpu *spu = &sys->spu;
    if (!spu->cmd_ring) {
        return;
    }
    spu_lock(spu);
    u8 prev_defer = spu->defer_irq;
    spu->defer_irq = 1u;
    u32 slot = 0u;
    u32 rendered = 0u;
    while (spsc_begin_pop(&spu->cmd_head, &spu->cmd_tail, PSX_SPU_CMD_CAP, &slot)) {
        SpuCmd cmd = spu->cmd_ring[slot];
        spsc_commit_pop(&spu->cmd_tail);
        if (cmd.reg == SPU_CMD_REG_ADVANCE) {
            if (cmd.val > 0u) {
                spu_render_frames_core(sys, cmd.val);
                rendered += cmd.val;
            }
        } else if (cmd.reg < 0x400u) {
            spu_apply_queued_reg16(sys, cmd.reg, cmd.val);
        }
    }
    spu->defer_irq = prev_defer;
    spu_unlock(spu);
    if (rendered > 0u) {
        __atomic_fetch_add(&sys->stats.spu_batches, 1u, __ATOMIC_RELAXED);
    }
    if (prev_defer == 0u) {
        spu_deliver_pending_irq(sys);
    }
}

void spu_drain_all_cmds_sync(PsxSystem *sys) {
    if (!sys || !sys->spu.async_mode || spu_cmd_backlog(sys) == 0u) {
        return;
    }
    spu_drain_all_cmds(sys);
    spu_deliver_pending_irq(sys);
}

/* Core 0: apply one register write immediately, serialized against Core 1 */
static void spu_apply_reg16_locked(PsxSystem *sys, u32 off, u16 val) {
    spu_lock(&sys->spu);
    spu_apply_reg16(sys, off, val);
    spu_unlock(&sys->spu);
}

static void spu_write_reg16_mmio(PsxSystem *sys, u32 off, u16 val) {
    PsxSpu *spu = &sys->spu;
    off &= 0x3FEu;

    /*
     * Transfer / IRQ control registers (0x1A4..0x1AC) affect SPU RAM DMA/FIFO
     * and SPUSTAT on Core 0 immediately. When async_mode is active without IRQ9,
     * synthesis register writes are queued via SPSC cmd_ring for Core 1 while
     * mirroring spu->regs for immediate CPU readback.
     */
    bool is_xfer_or_cnt = (off >= 0x1A4u && off <= 0x1ACu);
    if (!spu->async_mode || spu->irq_enabled || is_xfer_or_cnt) {
        if (is_xfer_or_cnt && spu->async_mode && off == 0x1AAu && ((val & 0x8040u) == 0x8040u)) {
            /* Switching into sample-exact IRQ9 mode: drain queued commands first */
            spu_drain_all_cmds(sys);
        }
        spu_apply_reg16_locked(sys, off, val);
        return;
    }

    /* Async SPSC path */
    if (off != 0x19Cu && off != 0x19Eu && off != 0x1AEu &&
        off != 0x1B8u && off != 0x1BAu && !(off >= 0x200u && off < 0x260u)) {
        spu->regs[off >> 1] = val;
    }
    /*
     * v0.2.7: update the Core-0-readable shadow state that spu_read_reg16_mmio() serves
     * from SpuVoice / ENDX (not spu->regs) right away, so a read-back issued before Core 1
     * drains cmd_ring sees the value just written (repeat_addr, ADSR volume, ENDX cleared
     * and ADSR volume zeroed on KON) exactly as the synchronous path would.
     */
    if (off < 0x180u) {
        u32 v_idx = off >> 4;
        u32 sub   = off & 0x0Eu;
        if (sub == 0x06u) {
            spu->v[v_idx].start_addr = ((u32)val << 3) & 0x7FFF8u;
        } else if (sub == 0x0Cu) {
            spu->v[v_idx].adsr_vol = val;
        } else if (sub == 0x0Eu) {
            spu->v[v_idx].repeat_addr = ((u32)val << 3) & 0x7FFF8u;
        }
    } else if (off == 0x188u) { /* KON low */
        u32 m = (u32)val;
        (void)__atomic_and_fetch(&spu->endx, ~m, __ATOMIC_RELAXED);
        for (u32 i = 0u; i < 16u; i++) {
            if ((m & (1u << i)) != 0u) {
                spu->v[i].adsr_vol = 0u;
            }
        }
    } else if (off == 0x18Au) { /* KON high */
        u32 m = ((u32)val & 0xFFu) << 16;
        (void)__atomic_and_fetch(&spu->endx, ~m, __ATOMIC_RELAXED);
        for (u32 i = 0u; i < 8u; i++) {
            if (((u32)val & (1u << i)) != 0u) {
                spu->v[16u + i].adsr_vol = 0u;
            }
        }
    }
    if (!spu_cmd_push(sys, (u16)off, val)) {
        /* Ring full (e.g. single-threaded test without Core 1 consumer): render & apply */
        spu_render_batch(sys, PSX_SPU_BATCH_FRAMES);
        if (!spu_cmd_push(sys, (u16)off, val)) {
            spu_apply_reg16_locked(sys, off, val);
        }
    }
}

static u16 spu_read_reg16_mmio(PsxSystem *sys, u32 off) {
    PsxSpu *spu = &sys->spu;
    off &= 0x3FEu;

    if (off == 0x19Cu) {
        return (u16)(__atomic_load_n(&spu->endx, __ATOMIC_RELAXED) & 0xFFFFu);
    }
    if (off == 0x19Eu) {
        return (u16)((__atomic_load_n(&spu->endx, __ATOMIC_RELAXED) >> 16) & 0x00FFu);
    }
    if (off == 0x1AEu) {
        spu_update_stat(spu);
        return (u16)spu->spustat;
    }
    if (off < 0x180u) {
        u32 v_idx = off >> 4;
        u32 sub   = off & 0x0Eu;
        if (sub == 0x0Cu) {
            return spu->v[v_idx].adsr_vol;
        }
        if (sub == 0x0Eu) {
            return (u16)(spu->v[v_idx].repeat_addr >> 3);
        }
    }
    return spu->regs[off >> 1];
}

u32 spu_mmio_read(PsxSystem *sys, u32 addr, u32 width) {
    u32 off = addr - PSX_REG_SPU_BASE;
    if (off >= PSX_REG_SPU_SIZE) {
        return 0u;
    }
    if (width == 4u) {
        u32 lo = spu_read_reg16_mmio(sys, off & ~1u);
        u32 hi = (off + 2u < PSX_REG_SPU_SIZE) ? spu_read_reg16_mmio(sys, (off + 2u) & ~1u) : 0u;
        return lo | (hi << 16);
    }
    if (width == 1u) {
        u16 w = spu_read_reg16_mmio(sys, off & ~1u);
        return ((addr & 1u) != 0u) ? (u32)(w >> 8) : (u32)(w & 0xFFu);
    }
    return spu_read_reg16_mmio(sys, off & ~1u);
}

void spu_mmio_write(PsxSystem *sys, u32 addr, u32 val, u32 width) {
    u32 off = addr - PSX_REG_SPU_BASE;
    if (off >= PSX_REG_SPU_SIZE) {
        return;
    }
    if (width == 4u) {
        spu_write_reg16_mmio(sys, off & ~1u, (u16)(val & 0xFFFFu));
        if (off + 2u < PSX_REG_SPU_SIZE) {
            spu_write_reg16_mmio(sys, (off + 2u) & ~1u, (u16)(val >> 16));
        }
        return;
    }
    if (width == 1u) {
        /* 8-bit writes on 16-bit SPU bus: odd byte writes ignored on PS1 hardware */
        if ((addr & 1u) == 0u) {
            spu_write_reg16_mmio(sys, off & ~1u, (u16)(val & 0xFFu));
        }
        return;
    }
    spu_write_reg16_mmio(sys, off & ~1u, (u16)(val & 0xFFFFu));
}

void spu_dma_write_words(PsxSystem *sys, const u32 *src, u32 n) {
    PsxSpu *spu = &sys->spu;
    if (!spu->ram || !src || n == 0u) {
        return;
    }
    /*
     * v0.2.8: render queued ADVANCE time / KON / KOFF against the SPU RAM contents that
     * existed when they were issued, before this DMA overwrites the sample data.
     */
    if (spu->async_mode && spu_cmd_backlog(sys) > 0u) {
        spu_drain_all_cmds(sys);
    }
    spu_lock(spu);
    for (u32 i = 0u; i < n; i++) {
        u32 w = src[i];
        spu_ram_write16(sys, spu->xfer_addr, (u16)(w & 0xFFFFu));
        spu->xfer_addr = (spu->xfer_addr + 2u) & 0x7FFFEu;
        spu_ram_write16(sys, spu->xfer_addr, (u16)(w >> 16));
        spu->xfer_addr = (spu->xfer_addr + 2u) & 0x7FFFEu;
    }
    spu_unlock(spu);
}

u32 spu_dma_read_words(PsxSystem *sys, u32 *dst, u32 n) {
    PsxSpu *spu = &sys->spu;
    if (!spu->ram || !dst || n == 0u) {
        return 0u;
    }
    spu_lock(spu);
    for (u32 i = 0u; i < n; i++) {
        u32 lo = spu_ram_read16(sys, spu->xfer_addr);
        spu->xfer_addr = (spu->xfer_addr + 2u) & 0x7FFFEu;
        u32 hi = spu_ram_read16(sys, spu->xfer_addr);
        spu->xfer_addr = (spu->xfer_addr + 2u) & 0x7FFFEu;
        dst[i] = lo | (hi << 16);
    }
    spu_unlock(spu);
    return n;
}

void spu_push_xa(PsxSystem *sys, const s16 *stereo, u32 frames) {
    if (!sys || !stereo || frames == 0u) {
        return;
    }
    PsxSpu *spu = &sys->spu;
    /*
     * SPSC producer (Core 0): xa_r belongs to the renderer (possibly Core 1). On overflow
     * the incoming samples are dropped instead of advancing the consumer cursor.
     */
    u32 w = spu->xa_w;
    u32 r = __atomic_load_n(&spu->xa_r, __ATOMIC_ACQUIRE);
    for (u32 i = 0u; i < frames; i++) {
        if ((w - r) >= 4096u) {
            break;
        }
        u32 idx = w & 4095u;
        spu->xa_ring[0][idx] = stereo[i * 2u + 0u];
        spu->xa_ring[1][idx] = stereo[i * 2u + 1u];
        w++;
    }
    __atomic_store_n(&spu->xa_w, w, __ATOMIC_RELEASE);
}

static void spu_step_noise(PsxSpu *spu, SpuExt *ext) {
    u32 shift  = (spu->spucnt >> 10) & 0x0Fu;
    u32 step   = 4u + ((spu->spucnt >> 8) & 0x03u);
    u32 period = 0x20000u >> shift;

    ext->noise_timer += step;
    if (ext->noise_timer >= period) {
        ext->noise_timer -= period;
        if (ext->noise_timer >= period) {
            ext->noise_timer = 0u;
        }
        u32 lfsr = spu->noise_lfsr & 0xFFFFu;
        if (lfsr == 0xFFFFu) {
            lfsr = 0u;
        }
        u32 parity = ((lfsr >> 15) ^ (lfsr >> 12) ^ (lfsr >> 11) ^ (lfsr >> 10) ^ 1u) & 1u;
        spu->noise_lfsr = ((lfsr << 1) | parity) & 0xFFFFu;
    }
}

static void spu_fetch_voice_block(PsxSystem *sys, u32 v_idx) {
    PsxSpu   *spu = &sys->spu;
    SpuVoice *v   = &spu->v[v_idx];

    if (v->keyed == 1u) {
        /* First block fetch right after KON */
        v->keyed = 2u;
    } else {
        /* Handle previous block's Loop End flag after its 28 samples finished */
        if ((v->block_flags & 0x01u) != 0u) {
            u32 endx = __atomic_or_fetch(&spu->endx, (1u << v_idx), __ATOMIC_RELAXED);
            spu->regs[(PSX_REG_SPU_ENDX_LO - PSX_REG_SPU_BASE) >> 1] = (u16)(endx & 0xFFFFu);
            spu->regs[(PSX_REG_SPU_ENDX_HI - PSX_REG_SPU_BASE) >> 1] = (u16)((endx >> 16) & 0x00FFu);
            v->cur_addr = v->repeat_addr & 0x7FFF8u;
            if ((v->block_flags & 0x02u) == 0u) {
                v->env_level  = 0;
                v->adsr_vol   = 0u;
                v->adsr_phase = ADSR_OFF;
                v->keyed      = 0u;
            }
        } else {
            v->cur_addr = (v->cur_addr + 16u) & 0x7FFF8u;
        }
    }

    /* Shift last 3 samples into ring[0..2] for seamless 4-tap Gaussian continuity */
    v->ring[0] = v->ring[28];
    v->ring[1] = v->ring[29];
    v->ring[2] = v->ring[30];

    spu_check_irq_range(sys, v->cur_addr, 16u);

    u8 blk_local[16];
    const u8 *blk;
    if (PSX_LIKELY(v->cur_addr <= PSX_SPU_RAM_BYTES - 16u)) {
        blk = &spu->ram[v->cur_addr];
    } else {
        for (u32 b = 0u; b < 16u; b++) {
            blk_local[b] = spu->ram[(v->cur_addr + b) & 0x7FFFFu];
        }
        blk = blk_local;
    }

    u8 flags = blk[1];
    v->block_flags = flags;
    if ((flags & 0x04u) != 0u) {
        v->repeat_addr = v->cur_addr;
        spu->regs[((v_idx << 4) + 0x0Eu) >> 1] = (u16)(v->repeat_addr >> 3);
    }

    spu_adpcm_decode_block(blk, &v->ring[3], v->hist);
}

static void spu_render_frames_core(PsxSystem *sys, u32 frames) {
    PsxSpu *spu = &sys->spu;
    SpuExt *ext = spu_ext(spu);
    if (!spu->ram || !ext || frames == 0u) {
        return;
    }

    u32 xa_w  = __atomic_load_n(&spu->xa_w, __ATOMIC_ACQUIRE);
    u32 xa_r  = spu->xa_r;
    u32 ring_w = spu->out_w;
    u32 ring_r = __atomic_load_n(&spu->out_r, __ATOMIC_ACQUIRE);

    for (u32 f = 0u; f < frames; f++) {
        bool spu_on   = (spu->spucnt & (1u << 15)) != 0u;
        bool unmuted  = (spu->spucnt & (1u << 14)) != 0u;
        bool cd_en    = (spu->spucnt & (1u << 0)) != 0u;
        bool cd_rev   = (spu->spucnt & (1u << 2)) != 0u;

        spu_step_noise(spu, ext);
        s16 noise_val = (s16)(u16)spu->noise_lfsr;

        s32 dry_l = 0, dry_r = 0;
        s32 rev_in_l = 0, rev_in_r = 0;
        s16 v1_out = 0, v3_out = 0;

        if (spu_on) {
            for (u32 i = 0u; i < 24u; i++) {
                SpuVoice *v = &spu->v[i];
                if (v->adsr_phase == ADSR_OFF && v->keyed == 0u) {
                    ext->last_vout[i] = 0;
                    continue;
                }

                while ((v->counter >> 12) >= 28u) {
                    v->counter -= (28u << 12);
                    spu_fetch_voice_block(sys, i);
                }

                s32 sample;
                if (v->noise_on != 0u) {
                    sample = noise_val;
                } else {
                    u32 idx = v->counter >> 12;
                    u32 gi  = (v->counter >> 4) & 0xFFu;
                    s32 s0  = v->ring[idx + 0u];
                    s32 s1  = v->ring[idx + 1u];
                    s32 s2  = v->ring[idx + 2u];
                    s32 s3  = v->ring[idx + 3u];
                    sample  = ((s32)k_spu_gauss[0x0FFu - gi] * s0) >> 15;
                    sample += ((s32)k_spu_gauss[0x1FFu - gi] * s1) >> 15;
                    sample += ((s32)k_spu_gauss[0x100u + gi] * s2) >> 15;
                    sample += ((s32)k_spu_gauss[0x000u + gi] * s3) >> 15;
                    sample  = spu_sat16(sample);
                }

                spu_step_voice_adsr(v, &ext->adsr_ticks[i]);
                spu->regs[((i << 4) + 0x0Cu) >> 1] = v->adsr_vol;

                s32 vout = (sample * v->env_level) >> 15;
                s16 vout16 = spu_sat16(vout);
                ext->last_vout[i] = vout16;
                if (i == 1u) {
                    v1_out = vout16;
                } else if (i == 3u) {
                    v3_out = vout16;
                }

                /* Advance pitch counter (with optional pitch modulation from voice i-1) */
                u32 step = v->pitch;
                if (v->pmon != 0u && i > 0u) {
                    s32 factor = (s32)ext->last_vout[i - 1u] + 0x8000;
                    s64 mod    = ((s64)(s16)v->pitch * (s64)factor) >> 15;
                    step       = (u32)mod & 0xFFFFu;
                }
                if (step > 0x3FFFu) {
                    step = 0x3FFFu;
                }
                v->counter += step;

                /* Voice L/R volume (fixed or sweep) */
                spu_step_sweep_vol(v->vol_l, &ext->cur_vol_l[i], &ext->sweep_ticks_l[i]);
                spu_step_sweep_vol(v->vol_r, &ext->cur_vol_r[i], &ext->sweep_ticks_r[i]);
                spu->regs[(0x200u + i * 4u) >> 1] = (u16)ext->cur_vol_l[i];
                spu->regs[(0x202u + i * 4u) >> 1] = (u16)ext->cur_vol_r[i];

                s32 vl = ((s32)vout16 * (s32)ext->cur_vol_l[i]) >> 15;
                s32 vr = ((s32)vout16 * (s32)ext->cur_vol_r[i]) >> 15;
                dry_l += vl;
                dry_r += vr;
                if (v->reverb_on != 0u) {
                    rev_in_l += vl;
                    rev_in_r += vr;
                }
            }
        }

        /* Pop one 44.1 kHz stereo frame from CD-DA / XA ring */
        s16 cd_raw_l, cd_raw_r;
        if (xa_r != xa_w) {
            u32 xidx = xa_r & 4095u;
            cd_raw_l = spu->xa_ring[0][xidx];
            cd_raw_r = spu->xa_ring[1][xidx];
            xa_r++;
        } else {
            /*
             * v0.2.8: brief xa_ring gap between XA/CD-DA sectors: decay the last frame
             * toward 0 (~1.6% per sample) instead of a full-scale step to 0 (click).
             * "/ 256" (shift + sign fixup, no division) truncates toward 0 so negative
             * values decay all the way too (an arithmetic ">> 8" sticks at -1..-63).
             */
            cd_raw_l = (s16)(((s32)ext->last_cd_l * 252) / 256);
            cd_raw_r = (s16)(((s32)ext->last_cd_r * 252) / 256);
        }
        ext->last_cd_l = cd_raw_l;
        ext->last_cd_r = cd_raw_r;

        if (cd_en) {
            s32 cd_vol_l = (s16)spu->regs[(0x1DB0u - 0x1C00u) >> 1];
            s32 cd_vol_r = (s16)spu->regs[(0x1DB2u - 0x1C00u) >> 1];
            s32 cdl      = ((s32)cd_raw_l * cd_vol_l) >> 15;
            s32 cdr      = ((s32)cd_raw_r * cd_vol_r) >> 15;
            dry_l += cdl;
            dry_r += cdr;
            if (cd_rev) {
                rev_in_l += cdl;
                rev_in_r += cdr;
            }
        }

        /* Write 4 hardware capture buffers (0x0000..0x0FFF) and update SPUSTAT.bit11 */
        if (spu_on) {
            u32 cap = ext->capture_pos & 0x3FEu;
            spu_ram_write16(sys, 0x0000u + cap, (u16)cd_raw_l);
            spu_ram_write16(sys, 0x0400u + cap, (u16)cd_raw_r);
            spu_ram_write16(sys, 0x0800u + cap, (u16)v1_out);
            spu_ram_write16(sys, 0x0C00u + cap, (u16)v3_out);
            ext->capture_pos = (cap + 2u) & 0x3FEu;
            if (ext->capture_pos >= 0x200u) {
                spu->spustat |= (1u << 11);
            } else {
                spu->spustat &= ~(1u << 11);
            }
            spu->regs[(PSX_REG_SPUSTAT - PSX_REG_SPU_BASE) >> 1] = (u16)spu->spustat;
        }

        /* 22.05 kHz SPU Reverb */
        s32 wet_l = 0, wet_r = 0;
        if (spu_on) {
            spu_reverb_process(sys, rev_in_l, rev_in_r, &wet_l, &wet_r);
        }

        /* Main Volume & Mute */
        u16 mvol_l_reg = spu->regs[(PSX_REG_SPU_MAIN_VOL_L - PSX_REG_SPU_BASE) >> 1];
        u16 mvol_r_reg = spu->regs[(PSX_REG_SPU_MAIN_VOL_R - PSX_REG_SPU_BASE) >> 1];
        spu_step_sweep_vol(mvol_l_reg, &ext->cur_main_l, &ext->main_ticks_l);
        spu_step_sweep_vol(mvol_r_reg, &ext->cur_main_r, &ext->main_ticks_r);
        spu->regs[0x1B8u >> 1] = (u16)ext->cur_main_l;
        spu->regs[0x1BAu >> 1] = (u16)ext->cur_main_r;

        s16 out_l = 0, out_r = 0;
        if (spu_on && unmuted) {
            s32 mix_l = spu_sat16(dry_l + wet_l);
            s32 mix_r = spu_sat16(dry_r + wet_r);
            out_l = spu_sat16((mix_l * (s32)ext->cur_main_l) >> 15);
            out_r = spu_sat16((mix_r * (s32)ext->cur_main_r) >> 15);
        }

        /*
         * Single output path: SPSC stereo out_ring (producer = renderer). The consumer is
         * psx_drain_sync_spu_ring() (sync mode, Core 0) or the NDSP feeder (async, Core 1),
         * which forward each frame exactly once. On overflow the newest frame is dropped.
         */
        if (spu->out_ring && spu->out_cap > 0u) {
            if ((ring_w - ring_r) >= spu->out_cap) {
                ring_r = __atomic_load_n(&spu->out_r, __ATOMIC_ACQUIRE);
            }
            if ((ring_w - ring_r) < spu->out_cap) {
                u32 oidx = ring_w & (spu->out_cap - 1u);
                spu->out_ring[oidx * 2u + 0u] = out_l;
                spu->out_ring[oidx * 2u + 1u] = out_r;
                ring_w++;
            }
        }
    }

    __atomic_store_n(&spu->xa_r, xa_r, __ATOMIC_RELEASE);
    __atomic_store_n(&spu->out_w, ring_w, __ATOMIC_RELEASE);
}

static void spu_render_batch_locked(PsxSystem *sys, u32 frames) {
    PsxSpu *spu = &sys->spu;
    u32 remaining = frames;

    if (spu->cmd_ring) {
        u32 slot = 0u;
        while (remaining > 0u &&
               spsc_begin_pop(&spu->cmd_head, &spu->cmd_tail, PSX_SPU_CMD_CAP, &slot)) {
            SpuCmd cmd = spu->cmd_ring[slot];
            if (cmd.reg == SPU_CMD_REG_ADVANCE) {
                u32 adv = cmd.val;
                if (adv <= remaining) {
                    spsc_commit_pop(&spu->cmd_tail);
                    if (adv > 0u) {
                        spu_render_frames_core(sys, adv);
                        remaining -= adv;
                    }
                } else {
                    spu->cmd_ring[slot].val = (u16)(adv - remaining);
                    spu_render_frames_core(sys, remaining);
                    remaining = 0u;
                    break;
                }
            } else {
                spsc_commit_pop(&spu->cmd_tail);
                if (cmd.reg < 0x400u) {
                    spu_apply_queued_reg16(sys, cmd.reg, cmd.val);
                }
            }
        }
    }

    if (remaining > 0u) {
        spu_render_frames_core(sys, remaining);
    }

    __atomic_fetch_add(&sys->stats.spu_batches, 1u, __ATOMIC_RELAXED); /* Core 0 or Core 1 */
}

PSX_HOT void spu_render_batch(PsxSystem *sys, u32 frames) {
    if (!sys || frames == 0u) {
        return;
    }
    PsxSpu *spu = &sys->spu;
    spu_lock(spu);
    spu->defer_irq = 1u;
    spu_render_batch_locked(sys, frames);
    spu->defer_irq = 0u;
    spu_unlock(spu);
    spu_deliver_pending_irq(sys);
}

PSX_HOT u32 spu_render_queued_async(PsxSystem *sys, u32 max_frames) {
    if (!sys || max_frames == 0u) {
        return 0u;
    }
    PsxSpu *spu = &sys->spu;
    if (!spu->cmd_ring || !spu_trylock(spu)) {
        return 0u;
    }
    spu->defer_irq = 1u;

    u32 done = 0u;
    u32 slot = 0u;
    while (done < max_frames &&
           spsc_begin_pop(&spu->cmd_head, &spu->cmd_tail, PSX_SPU_CMD_CAP, &slot)) {
        SpuCmd cmd = spu->cmd_ring[slot];
        if (cmd.reg == SPU_CMD_REG_ADVANCE) {
            u32 adv  = cmd.val;
            u32 room = max_frames - done;
            if (adv <= room) {
                spsc_commit_pop(&spu->cmd_tail);
                if (adv > 0u) {
                    spu_render_frames_core(sys, adv);
                    done += adv;
                }
            } else {
                spu->cmd_ring[slot].val = (u16)(adv - room);
                spu_render_frames_core(sys, room);
                done += room;
                break;
            }
        } else {
            spsc_commit_pop(&spu->cmd_tail);
            if (cmd.reg < 0x400u) {
                spu_apply_queued_reg16(sys, cmd.reg, cmd.val);
            }
        }
    }
    if (done > 0u) {
        __atomic_fetch_add(&sys->stats.spu_batches, 1u, __ATOMIC_RELAXED); /* Core 0 or Core 1 */
    }

    spu->defer_irq = 0u;
    spu_unlock(spu);
    return done;
}

void spu_event_cb(PsxSystem *sys, u32 late_cycles) {
    PsxSpu *spu = &sys->spu;
    spu->batch_tick = sched_now(sys) - late_cycles;

    /* Deliver any IRQ9 latched by the Core 1 renderer */
    spu_deliver_pending_irq(sys);

    if (!spu->async_mode || spu->irq_enabled) {
        spu_render_batch(sys, PSX_SPU_BATCH_FRAMES);
    } else {
        if (!spu_cmd_push(sys, SPU_CMD_REG_ADVANCE, (u16)PSX_SPU_BATCH_FRAMES)) {
            spu_render_batch(sys, PSX_SPU_BATCH_FRAMES);
        }
    }

    u32 next_delta = (late_cycles < PSX_SPU_BATCH_CYCLES)
                         ? (PSX_SPU_BATCH_CYCLES - late_cycles)
                         : PSX_SPU_BATCH_CYCLES;
    sched_schedule(sys, EV_SPU, next_delta);
}

void spu_init(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    PsxSpu *spu = &sys->spu;
    memset(spu, 0, sizeof(*spu));

    spu->ram        = (u8 *)arena_alloc(&sys->arena, PSX_SPU_RAM_BYTES, 32u);
    spu->out_cap    = PSX_SPU_OUT_CAP;
    spu->out_ring   = (s16 *)arena_alloc(&sys->arena, PSX_SPU_OUT_CAP * 2u * (u32)sizeof(s16), 32u);
    spu->cmd_ring   = (SpuCmd *)arena_alloc(&sys->arena, PSX_SPU_CMD_CAP * (u32)sizeof(SpuCmd), 32u);
    spu->reverb_buf = (s16 *)arena_alloc(&sys->arena, SPU_REVERB_BUF_BYTES, 32u);

    bus_mmio_register(sys, PSX_REG_SPU_BASE, PSX_REG_SPU_SIZE, spu_mmio_read, spu_mmio_write);
    sched_set_handler(&sys->sched, EV_SPU, spu_event_cb);

    spu_reset(sys);
}

void spu_reset(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    PsxSpu *spu = &sys->spu;

    u8     *ram        = spu->ram;
    s16    *out_ring   = spu->out_ring;
    u32     out_cap    = spu->out_cap;
    SpuCmd *cmd_ring   = spu->cmd_ring;
    s16    *reverb_buf = spu->reverb_buf;

    memset(spu, 0, sizeof(*spu));

    spu->ram        = ram;
    spu->out_ring   = out_ring;
    spu->out_cap    = out_cap;
    spu->cmd_ring   = cmd_ring;
    spu->reverb_buf = reverb_buf;

    if (spu->ram) {
        memset(spu->ram, 0, PSX_SPU_RAM_BYTES);
    }
    if (spu->out_ring && spu->out_cap > 0u) {
        memset(spu->out_ring, 0, spu->out_cap * 2u * sizeof(s16));
    }
    if (spu->cmd_ring) {
        memset(spu->cmd_ring, 0, PSX_SPU_CMD_CAP * sizeof(SpuCmd));
    }
    if (spu->reverb_buf) {
        memset(spu->reverb_buf, 0, SPU_REVERB_BUF_BYTES);
    }

    spsc_index_reset(&spu->cmd_head, &spu->cmd_tail);

    spu->xfer_ctrl                                       = 0x0004u;
    spu->regs[(PSX_REG_SPU_XFER_CTRL - PSX_REG_SPU_BASE) >> 1] = 0x0004u;
    spu->noise_lfsr                                      = 0u;
    spu->async_mode                                      = sys->cfg.spu_async;
    spu->batch_tick                                      = sched_now(sys);

    spu_reverb_reset(spu);
    spu_xa_reset();
    spu_update_stat(spu);

    sched_schedule(sys, EV_SPU, PSX_SPU_BATCH_CYCLES);
}

void spu_state(PsxSystem *sys, StateIO *st) {
    if (!sys || !st) {
        return;
    }
    PsxSpu *spu = &sys->spu;
    if (st->writing) {
        spu_drain_all_cmds(sys);
    }

    if (spu->ram) {
        state_bytes(st, spu->ram, PSX_SPU_RAM_BYTES);
    }
    state_bytes(st, spu->regs, (u32)sizeof(spu->regs));
    state_bytes(st, spu->v, (u32)sizeof(spu->v));

    state_u32(st, &spu->spucnt);
    state_u32(st, &spu->spustat);
    state_u32(st, &spu->irq_addr);
    state_u32(st, &spu->xfer_addr);
    state_u32(st, &spu->xfer_ctrl);
    state_u32(st, &spu->noise_lfsr);
    state_u32(st, &spu->kon);
    state_u32(st, &spu->koff);
    state_u32(st, &spu->endx);
    state_bytes(st, spu->reverb_regs, (u32)sizeof(spu->reverb_regs));
    state_u32(st, &spu->reverb_pos);
    state_bytes(st, spu->fifo, (u32)sizeof(spu->fifo));
    state_u8(st, &spu->fifo_n);
    state_bool(st, &spu->irq_enabled);
    state_u32(st, &spu->batch_tick);

    if (spu->reverb_buf) {
        state_bytes(st, spu->reverb_buf, SPU_REVERB_BUF_BYTES);
    }

    if (!st->writing) {
        spu->out_w      = 0u;
        spu->out_r      = 0u;
        spu->xa_w       = 0u;
        spu->xa_r       = 0u;
        spu->irq_pending = 0u;
        spu->defer_irq  = 0u;
        spu->async_mode = sys->cfg.spu_async;
        spsc_index_reset(&spu->cmd_head, &spu->cmd_tail);
    }
}

void spu_shutdown(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    spsc_index_reset(&sys->spu.cmd_head, &sys->spu.cmd_tail);
}
