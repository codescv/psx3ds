#include "periph_internal.h"
#include "psx3ds/arena.h"
#include "psx3ds/bus.h"
#include "psx3ds/state.h"

#include <string.h>

#define MDEC_STREAM_IN_HW   65536u /* 128 KiB compressed halfword ring */
#define MDEC_STREAM_OUT_W   65536u /* 256 KiB decoded 32-bit pixel word ring */
#define MDEC_MAX_INSTANCES  8u

typedef struct MdecStream {
    PsxSystem *owner;
    u32        cmd_op;
    u32        param_idx;
    bool       quant_color;
    u32        in_cap_hw;
    u32        out_cap_w;
    u32        in_head;
    u32        in_tail;
    u32        out_head;
    u32        out_tail;
    u16       *in_buf;
    u32       *out_buf;
} MdecStream;

static MdecStream *g_mdec_streams[MDEC_MAX_INSTANCES];

static MdecStream *mdec_get_stream(const PsxSystem *sys) {
    for (u32 i = 0u; i < MDEC_MAX_INSTANCES; i++) {
        if (g_mdec_streams[i] && g_mdec_streams[i]->owner == sys) {
            return g_mdec_streams[i];
        }
    }
    return NULL;
}

static void mdec_register_stream(MdecStream *ms) {
    for (u32 i = 0u; i < MDEC_MAX_INSTANCES; i++) {
        if (g_mdec_streams[i] && g_mdec_streams[i]->owner == ms->owner) {
            g_mdec_streams[i] = ms;
            return;
        }
    }
    for (u32 i = 0u; i < MDEC_MAX_INSTANCES; i++) {
        if (g_mdec_streams[i] == NULL) {
            g_mdec_streams[i] = ms;
            return;
        }
    }
    g_mdec_streams[0] = ms;
}

/* Standard JPEG / PS1 MDEC zig-zag scan LUT: scan index k -> 8x8 raster y*8 + x */
static const u8 k_mdec_zagzig[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

PSX_INLINE s32 mdec_asr(s32 x, u32 sh) {
    if (x < 0) {
        return (s32)(~((~(u32)x) >> sh));
    }
    return (s32)((u32)x >> sh);
}

PSX_INLINE s16 mdec_clamp_s11(s32 x) {
    if (x > 1023) {
        return 1023;
    }
    if (x < -1024) {
        return -1024;
    }
    return (s16)x;
}

PSX_INLINE s16 mdec_clamp_s8(s32 x) {
    if (x > 127) {
        return 127;
    }
    if (x < -128) {
        return -128;
    }
    return (s16)x;
}

PSX_INLINE s32 mdec_sext10(u16 hw) {
    u32 v = hw & 0x03FFu;
    return (v >= 0x0200u) ? ((s32)v - 1024) : (s32)v;
}

/*
 * Exact 2-pass separable integer IDCT matching mdec_idct_armv6.S bit-for-bit.
 */
void mdec_idct_c(s16 blk[64], const s16 scale[64]) {
    s16 tmp[64];

    for (u32 x = 0u; x < 8u; x++) {
        for (u32 y = 0u; y < 8u; y++) {
            s32 sum = 0;
            for (u32 z = 0u; z < 8u; z++) {
                sum += (s32)blk[z * 8u + x] * (s32)scale[z * 8u + y];
            }
            tmp[y * 8u + x] = (s16)mdec_asr(sum + 0x8000, 16u);
        }
    }

    for (u32 y = 0u; y < 8u; y++) {
        for (u32 x = 0u; x < 8u; x++) {
            s32 sum = 0;
            for (u32 z = 0u; z < 8u; z++) {
                sum += (s32)tmp[y * 8u + z] * (s32)scale[z * 8u + x];
            }
            blk[y * 8u + x] = mdec_clamp_s8(mdec_asr(sum + 0x8000, 16u));
        }
    }
}

PSX_HOT void mdec_idct(s16 blk[64], const s16 scale[64]) {
#if PSX3DS_ARCH_ARMV6K
    mdec_idct_armv6(blk, scale);
#else
    mdec_idct_c(blk, scale);
#endif
}

static void mdec_refresh_status(PsxSystem *sys) {
    PsxMdec    *m  = &sys->mdec;
    MdecStream *ms = mdec_get_stream(sys);

    u32 out_avail = ms ? (ms->out_head - ms->out_tail) : (m->out_w - m->out_r);
    u32 in_avail  = ms ? (ms->in_head - ms->in_tail)   : (m->in_w - m->in_r);

    u32 st = 0u;
    if (out_avail == 0u) {
        st |= (1u << 31); /* Data-Out FIFO Empty */
    }
    if (ms && in_avail >= ms->in_cap_hw) {
        st |= (1u << 30); /* Data-In FIFO Full */
    }
    /*
     * Command Busy: a command is still receiving parameters / decoding, or an armed DMA1
     * is still draining its output. Once the command has fully completed and DMA1 is idle,
     * leftover output (e.g. extra padding macroblocks) must not hold DecDCTinSync() busy.
     */
    bool dma1_armed = (sys->dma.ch[PSX_DMA_CH_MDEC_OUT].chcr & (1u << 24)) != 0u;
    if ((ms && ms->cmd_op != 0u) || m->words_left > 0u || (dma1_armed && out_avail > 0u)) {
        st |= (1u << 29); /* Command Busy */
    }
    if ((m->control & (1u << 30)) != 0u && m->words_left > 0u) {
        st |= (1u << 28); /* DMA0 Data-In Request */
    }
    if ((m->control & (1u << 29)) != 0u && out_avail > 0u) {
        st |= (1u << 27); /* DMA1 Data-Out Request */
    }

    st |= ((u32)(m->out_depth & 3u)  << 25);
    st |= ((u32)(m->out_signed & 1u) << 24);
    st |= ((u32)(m->out_bit15 & 1u)  << 23);
    st |= ((m->cur_block & 7u)       << 16);
    st |= ((m->words_left - 1u) & 0xFFFFu);

    m->status = st;

    /* Data-Out DREQ edge: wake an armed DMA1 that is waiting for decoded words */
    if (out_avail > 0u && dma1_armed) {
        u32 bs = sys->dma.ch[PSX_DMA_CH_MDEC_OUT].bcr & 0xFFFFu;
        if (mdec_dma_out_blocks_ready(sys, (bs == 0u) ? 0x10000u : bs) > 0u) {
            dma_request_wakeup(sys, PSX_DMA_CH_MDEC_OUT);
        }
    }
}

u32 mdec_dma_out_blocks_ready(const PsxSystem *sys, u32 block_words) {
    if (!sys || block_words == 0u) {
        return 0u;
    }
    const MdecStream *ms = mdec_get_stream(sys);
    if (!ms) {
        /* No stream (should not happen after mdec_init): never block DMA1 forever */
        return 1u;
    }
    u32 out_avail = ms->out_head - ms->out_tail;
    if (out_avail == 0u) {
        return 0u;
    }
    u32 blocks = out_avail / block_words;
    if (blocks == 0u && ms->cmd_op != 1u) {
        /* Decode finished with a short tail: let DMA1 flush it (zero-padded) */
        blocks = 1u;
    }
    return blocks;
}

static void mdec_push_out_word(PsxMdec *m, MdecStream *ms, u32 w) {
    if (ms && ms->out_buf && (ms->out_head - ms->out_tail) < ms->out_cap_w) {
        ms->out_buf[ms->out_head & (ms->out_cap_w - 1u)] = w;
        ms->out_head++;
    }
    if (m->out_w < 768u) {
        m->out_fifo[m->out_w++] = w;
    }
}

static void mdec_emit_macroblock(PsxSystem *sys) {
    PsxMdec    *m  = &sys->mdec;
    MdecStream *ms = mdec_get_stream(sys);

    bool is_signed = (m->out_signed != 0u);
    u16  mask15    = (m->out_bit15 != 0u) ? 0x8000u : 0u;

    if (m->out_depth == 0u) {
        /* 4-bit monochrome (8x8 = 64 pixels = 8 u32 words) */
        const s16 *yblk = m->blocks[0];
        for (u32 i = 0u; i < 64u; i += 8u) {
            u32 word = 0u;
            for (u32 j = 0u; j < 8u; j++) {
                s32 yv = mdec_clamp_s8(yblk[i + j]);
                u8  ub = is_signed ? (u8)(s8)yv : (u8)(yv + 128);
                word |= ((u32)(ub >> 4) & 0x0Fu) << (j * 4u);
            }
            mdec_push_out_word(m, ms, word);
        }
        return;
    }

    if (m->out_depth == 1u) {
        /* 8-bit monochrome (8x8 = 64 pixels = 16 u32 words) */
        const s16 *yblk = m->blocks[0];
        for (u32 i = 0u; i < 64u; i += 4u) {
            u32 word = 0u;
            for (u32 j = 0u; j < 4u; j++) {
                s32 yv = mdec_clamp_s8(yblk[i + j]);
                u8  ub = is_signed ? (u8)(s8)yv : (u8)(yv + 128);
                word |= (u32)ub << (j * 8u);
            }
            mdec_push_out_word(m, ms, word);
        }
        return;
    }

    /* Color 16x16 macroblock (blocks: 0=Cr, 1=Cb, 2=Y1, 3=Y2, 4=Y3, 5=Y4) */
    u8 rgb_bytes[16u * 16u * 3u];
    u32 bpos = 0u;

    for (u32 py = 0u; py < 16u; py++) {
        u32 cy_off = (py >> 1) * 8u;
        u32 y_row  = (py & 7u) * 8u;
        u32 yb_row = 2u + ((py >> 3) << 1);

        for (u32 px = 0u; px < 16u; px++) {
            u32 c_idx = cy_off + (px >> 1);
            s32 cr    = m->blocks[0][c_idx];
            s32 cb    = m->blocks[1][c_idx];
            s32 yv    = m->blocks[yb_row + (px >> 3)][y_row + (px & 7u)];

            s32 r = mdec_clamp_s8(yv + mdec_asr(359 * cr + 128, 8u));
            s32 g = mdec_clamp_s8(yv + mdec_asr(-88 * cb - 183 * cr + 128, 8u));
            s32 b = mdec_clamp_s8(yv + mdec_asr(454 * cb + 128, 8u));

            u8 ru = is_signed ? (u8)(s8)r : (u8)(r + 128);
            u8 gu = is_signed ? (u8)(s8)g : (u8)(g + 128);
            u8 bu = is_signed ? (u8)(s8)b : (u8)(b + 128);

            rgb_bytes[bpos + 0u] = ru;
            rgb_bytes[bpos + 1u] = gu;
            rgb_bytes[bpos + 2u] = bu;
            bpos += 3u;
        }
    }

    if (m->out_depth == 3u) {
        /* 15-bit RGB5A1 (256 pixels = 128 u32 words) */
        for (u32 i = 0u; i < 256u; i += 2u) {
            u16 p0 = (u16)(((u16)(rgb_bytes[i * 3u + 0u] >> 3)) |
                           ((u16)(rgb_bytes[i * 3u + 1u] >> 3) << 5) |
                           ((u16)(rgb_bytes[i * 3u + 2u] >> 3) << 10) | mask15);
            u16 p1 = (u16)(((u16)(rgb_bytes[(i + 1u) * 3u + 0u] >> 3)) |
                           ((u16)(rgb_bytes[(i + 1u) * 3u + 1u] >> 3) << 5) |
                           ((u16)(rgb_bytes[(i + 1u) * 3u + 2u] >> 3) << 10) | mask15);
            mdec_push_out_word(m, ms, (u32)p0 | ((u32)p1 << 16));
        }
    } else {
        /* 24-bit RGB888 (768 bytes = 192 u32 words) */
        for (u32 i = 0u; i < 768u; i += 4u) {
            u32 w = (u32)rgb_bytes[i + 0u] |
                    ((u32)rgb_bytes[i + 1u] << 8) |
                    ((u32)rgb_bytes[i + 2u] << 16) |
                    ((u32)rgb_bytes[i + 3u] << 24);
            mdec_push_out_word(m, ms, w);
        }
    }
}

/*
 * Non-destructive RLE peek-decode of one 8x8 block from ms->in_buf[*cursor .. ms->in_head).
 * Returns:
 *   1 = decoded complete 8x8 block (and advanced *cursor)
 *   0 = need more input halfwords
 *  -1 = only 0xFE00 EOB padding remained up to ms->in_head
 */
static int mdec_try_decode_block(const MdecStream *ms, u32 *cursor,
                                 const u8 qt[64], const s16 scale[64],
                                 bool eof_reached, s16 out_blk[64]) {
    u32 p   = *cursor;
    u32 end = ms->in_head;
    u32 mask = ms->in_cap_hw - 1u;

    while (p != end && ms->in_buf[p & mask] == 0xFE00u) {
        p++;
    }
    if (p == end) {
        if (eof_reached) {
            *cursor = p;
            return -1;
        }
        return 0;
    }

    memset(out_blk, 0, 64u * sizeof(s16));

    u16 dc_hw   = ms->in_buf[p++ & mask];
    u32 q_scale = (dc_hw >> 10) & 0x3Fu;
    s32 dc_s10  = mdec_sext10(dc_hw);
    s32 dc_val  = (q_scale == 0u) ? (dc_s10 * 2) : (dc_s10 * (s32)qt[0]);
    out_blk[(q_scale == 0u) ? 0u : k_mdec_zagzig[0]] = mdec_clamp_s11(dc_val);

    u32 k = 0u;
    while (k < 63u) {
        if (p == end) {
            if (!eof_reached) {
                return 0; /* wait for next DMA0/PIO chunk */
            }
            break; /* stream ended mid-block: treat as EOB */
        }
        u16 ac_hw = ms->in_buf[p++ & mask];
        if (ac_hw == 0xFE00u) {
            break;
        }
        u32 run = (ac_hw >> 10) & 0x3Fu;
        k += run + 1u;
        if (k >= 64u) {
            break;
        }
        s32 ac_s10 = mdec_sext10(ac_hw);
        if (q_scale == 0u) {
            out_blk[k] = mdec_clamp_s11(ac_s10 * 2);
        } else {
            s32 prod = ac_s10 * (s32)qt[k] * (s32)q_scale + 4;
            out_blk[k_mdec_zagzig[k]] = mdec_clamp_s11(mdec_asr(prod, 3u));
        }
    }

    mdec_idct(out_blk, scale);
    *cursor = p;
    return 1;
}

static void mdec_pump_decode(PsxSystem *sys) {
    PsxMdec    *m  = &sys->mdec;
    MdecStream *ms = mdec_get_stream(sys);
    if (!ms || ms->cmd_op != 1u) {
        return;
    }

    bool mono   = (m->out_depth <= 1u);
    u32  n_blks = mono ? 1u : 6u;
    u32  out_per_mb = (m->out_depth == 0u) ? 8u :
                      (m->out_depth == 1u) ? 16u :
                      (m->out_depth == 3u) ? 128u : 192u;

    while (ms->in_tail != ms->in_head) {
        if ((ms->out_head - ms->out_tail) + out_per_mb > ms->out_cap_w) {
            break; /* output stream full; will resume as DMA1/PIO drains */
        }

        bool eof = (m->words_left == 0u);
        u32  cur = ms->in_tail;
        bool ok  = true;

        for (u32 b = 0u; b < n_blks; b++) {
            /* Hardware status block numbering: 4=Cr, 5=Cb, 0..3=Y1..Y4 */
            m->cur_block = mono ? 0u : ((b < 2u) ? (4u + b) : (b - 2u));
            const u8 *qt = (!mono && b < 2u) ? m->iq_uv : m->iq_y;
            int rc = mdec_try_decode_block(ms, &cur, qt, m->idct, eof, m->blocks[b]);
            if (rc == 0) {
                ok = false;
                break;
            }
            if (rc < 0) {
                if (b == 0u) {
                    /* Only trailing 0xFE00 padding left at end of stream */
                    ms->in_tail = cur;
                    ok = false;
                    break;
                }
                /* Trailing blocks padded with zeros at EOF */
                memset(m->blocks[b], 0, 64u * sizeof(s16));
            }
        }

        if (!ok) {
            break;
        }

        ms->in_tail  = cur;
        m->cur_block = 4u;
        mdec_emit_macroblock(sys);
        sched_schedule(sys, EV_MDEC, PSX_MDEC_BLOCK_CYCLES);
    }

    if (m->words_left == 0u) {
        /* Strip any remaining 0xFE00 EOB padding words at end of command */
        u32 mask = ms->in_cap_hw - 1u;
        while (ms->in_tail != ms->in_head && ms->in_buf[ms->in_tail & mask] == 0xFE00u) {
            ms->in_tail++;
        }
        if (ms->in_tail == ms->in_head) {
            ms->cmd_op  = 0u;
            m->decoding = false;
            m->cur_block = 4u;
        }
    }
}

static void mdec_feed_word(PsxSystem *sys, u32 val) {
    PsxMdec    *m  = &sys->mdec;
    MdecStream *ms = mdec_get_stream(sys);
    if (!ms) {
        return;
    }

    if (ms->cmd_op == 0u && m->words_left == 0u) {
        /* New MDEC command word */
        u32 op         = (val >> 29) & 7u;
        m->out_depth   = (u8)((val >> 27) & 3u);
        m->out_signed  = (u8)((val >> 26) & 1u);
        m->out_bit15   = (u8)((val >> 25) & 1u);
        ms->param_idx  = 0u;

        if (op == 1u) {
            u32 nwords  = val & 0xFFFFu;
            m->words_left = nwords;
            if (nwords > 0u) {
                ms->cmd_op    = 1u;
                ms->in_head   = 0u;
                ms->in_tail   = 0u;
                ms->out_head  = 0u;
                ms->out_tail  = 0u;
                m->out_r      = 0u;
                m->out_w      = 0u;
                m->decoding   = true;
                m->cur_block  = 4u;
            }
        } else if (op == 2u) {
            ms->cmd_op      = 2u;
            ms->quant_color = (val & 1u) != 0u;
            m->words_left   = ms->quant_color ? 32u : 16u;
            m->cur_block    = 4u;
        } else if (op == 3u) {
            ms->cmd_op    = 3u;
            m->words_left = 32u;
            m->cur_block  = 4u;
        } else {
            /* Nop / unused command: discard (val & 0xFFFF) parameter words */
            u32 nwords    = val & 0xFFFFu;
            m->words_left = nwords;
            ms->cmd_op    = (nwords > 0u) ? 4u : 0u;
            m->cur_block  = 4u;
        }
        mdec_refresh_status(sys);
        return;
    }

    /* Parameter word for active command */
    if (m->words_left > 0u) {
        m->words_left--;
    }

    if (ms->cmd_op == 2u) {
        u32 byte_idx = ms->param_idx * 4u;
        u8 *dst = (byte_idx < 64u) ? &m->iq_y[byte_idx] : &m->iq_uv[byte_idx - 64u];
        if (byte_idx < 128u) {
            dst[0] = (u8)(val & 0xFFu);
            dst[1] = (u8)((val >> 8) & 0xFFu);
            dst[2] = (u8)((val >> 16) & 0xFFu);
            dst[3] = (u8)((val >> 24) & 0xFFu);
        }
        ms->param_idx++;
        if (m->words_left == 0u) {
            ms->cmd_op = 0u;
        }
    } else if (ms->cmd_op == 3u) {
        u32 hw_idx = ms->param_idx * 2u;
        if (hw_idx + 1u < 64u) {
            m->idct[hw_idx + 0u] = (s16)(u16)(val & 0xFFFFu);
            m->idct[hw_idx + 1u] = (s16)(u16)(val >> 16);
        }
        ms->param_idx++;
        if (m->words_left == 0u) {
            ms->cmd_op = 0u;
        }
    } else if (ms->cmd_op == 1u) {
        u32 mask = ms->in_cap_hw - 1u;
        if ((ms->in_head - ms->in_tail) + 2u <= ms->in_cap_hw) {
            ms->in_buf[ms->in_head++ & mask] = (u16)(val & 0xFFFFu);
            ms->in_buf[ms->in_head++ & mask] = (u16)(val >> 16);
        }
        m->in_fifo[(m->in_w++) & 63u] = (u16)(val & 0xFFFFu);
        m->in_fifo[(m->in_w++) & 63u] = (u16)(val >> 16);
    } else {
        if (m->words_left == 0u) {
            ms->cmd_op = 0u;
        }
    }
}

static u32 mdec_pop_out_word(PsxSystem *sys) {
    PsxMdec    *m  = &sys->mdec;
    MdecStream *ms = mdec_get_stream(sys);
    if (!ms || ms->out_tail == ms->out_head) {
        return 0u;
    }
    u32 w = ms->out_buf[ms->out_tail & (ms->out_cap_w - 1u)];
    ms->out_tail++;
    if (m->out_r < m->out_w) {
        m->out_r++;
    }
    /* If compressed input was waiting on output space, resume decoding */
    if (ms->cmd_op == 1u && ms->in_tail != ms->in_head) {
        mdec_pump_decode(sys);
    }
    return w;
}

u32 mdec_mmio_read(PsxSystem *sys, u32 addr, u32 width) {
    u32 off = addr & 0x0Cu;
    u32 val;
    if (off == 0u) {
        val = mdec_pop_out_word(sys);
        mdec_refresh_status(sys);
    } else if (off == 4u) {
        mdec_refresh_status(sys);
        val = sys->mdec.status;
    } else {
        val = 0u;
    }

    if (width == 1u) {
        return (val >> ((addr & 3u) * 8u)) & 0xFFu;
    }
    if (width == 2u) {
        return (val >> ((addr & 2u) * 8u)) & 0xFFFFu;
    }
    return val;
}

void mdec_mmio_write(PsxSystem *sys, u32 addr, u32 val, u32 width) {
    (void)width;
    PsxMdec    *m  = &sys->mdec;
    MdecStream *ms = mdec_get_stream(sys);
    u32 off = addr & 0x0Cu;

    if (off == 0u) {
        mdec_feed_word(sys, val);
        if (ms && ms->cmd_op == 1u) {
            mdec_pump_decode(sys);
        }
        mdec_refresh_status(sys);
    } else if (off == 4u) {
        if ((val & (1u << 31)) != 0u) {
            /* Warm reset MDEC */
            m->control    = val & 0x60000000u;
            m->in_r       = 0u;
            m->in_w       = 0u;
            m->out_r      = 0u;
            m->out_w      = 0u;
            m->cur_block  = 4u;
            m->cur_coeff  = 0u;
            m->words_left = 0u;
            m->out_depth  = 0u;
            m->out_signed = 0u;
            m->out_bit15  = 0u;
            m->decoding   = false;
            if (ms) {
                ms->cmd_op    = 0u;
                ms->param_idx = 0u;
                ms->in_head   = 0u;
                ms->in_tail   = 0u;
                ms->out_head  = 0u;
                ms->out_tail  = 0u;
            }
            sched_cancel(sys, EV_MDEC);
        } else {
            m->control = val & 0x60000000u;
        }
        mdec_refresh_status(sys);
    }
}

u32 mdec_dma_in_words(PsxSystem *sys, const u32 *src, u32 n) {
    if (!sys || !src || n == 0u) {
        return 0u;
    }
    MdecStream *ms = mdec_get_stream(sys);
    for (u32 i = 0u; i < n; i++) {
        mdec_feed_word(sys, src[i]);
        /* Batch decode periodically so input halfword buffer never saturates on huge frames */
        if (ms && ms->cmd_op == 1u && ((i & 63u) == 63u)) {
            mdec_pump_decode(sys);
        }
    }
    if (ms && ms->cmd_op == 1u) {
        mdec_pump_decode(sys);
    }
    mdec_refresh_status(sys);
    return n;
}

u32 mdec_dma_out_words(PsxSystem *sys, u32 *dst, u32 n) {
    if (!sys || !dst || n == 0u) {
        return 0u;
    }
    MdecStream *ms = mdec_get_stream(sys);
    if (!ms) {
        return 0u;
    }
    if (ms->cmd_op == 1u && ms->out_tail == ms->out_head && ms->in_tail != ms->in_head) {
        mdec_pump_decode(sys);
    }
    u32 count = 0u;
    while (count < n && ms->out_tail != ms->out_head) {
        dst[count++] = mdec_pop_out_word(sys);
    }
    mdec_refresh_status(sys);
    return count;
}

void mdec_event_cb(PsxSystem *sys, u32 late_cycles) {
    (void)late_cycles;
    MdecStream *ms = mdec_get_stream(sys);
    if (ms && ms->cmd_op == 1u && ms->in_tail != ms->in_head) {
        mdec_pump_decode(sys);
    }
    mdec_refresh_status(sys);
}

void mdec_init(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    memset(&sys->mdec, 0, sizeof(sys->mdec));

    MdecStream *ms = (MdecStream *)arena_alloc(&sys->arena, (u32)sizeof(MdecStream), 32u);
    ms->owner      = sys;
    ms->in_cap_hw  = sys->cfg.small_mem ? 32768u : MDEC_STREAM_IN_HW;
    ms->out_cap_w  = MDEC_STREAM_OUT_W;
    ms->in_buf     = (u16 *)arena_alloc(&sys->arena, ms->in_cap_hw * (u32)sizeof(u16), 32u);
    ms->out_buf    = (u32 *)arena_alloc(&sys->arena, ms->out_cap_w * (u32)sizeof(u32), 32u);
    mdec_register_stream(ms);

    bus_mmio_register(sys, PSX_REG_MDEC_BASE, 0x10u, mdec_mmio_read, mdec_mmio_write);
    sched_set_handler(&sys->sched, EV_MDEC, mdec_event_cb);

    mdec_reset(sys);
}

void mdec_reset(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    PsxMdec    *m  = &sys->mdec;
    MdecStream *ms = mdec_get_stream(sys);

    memset(m, 0, sizeof(*m));
    m->cur_block = 4u;
    m->status    = 0x8004FFFFu;

    if (ms) {
        ms->cmd_op      = 0u;
        ms->param_idx   = 0u;
        ms->quant_color = false;
        ms->in_head     = 0u;
        ms->in_tail     = 0u;
        ms->out_head    = 0u;
        ms->out_tail    = 0u;
    }
    sched_cancel(sys, EV_MDEC);
}

void mdec_state(PsxSystem *sys, StateIO *st) {
    if (!sys || !st) {
        return;
    }
    PsxMdec    *m  = &sys->mdec;
    MdecStream *ms = mdec_get_stream(sys);

    state_u32(st, &m->status);
    state_u32(st, &m->control);
    state_bytes(st, m->iq_y, 64u);
    state_bytes(st, m->iq_uv, 64u);
    state_bytes(st, m->idct, 128u);
    state_bytes(st, m->blocks, (u32)sizeof(m->blocks));
    state_u32(st, &m->cur_block);
    state_u32(st, &m->cur_coeff);
    state_u32(st, &m->words_left);
    state_u8(st, &m->out_depth);
    state_u8(st, &m->out_signed);
    state_u8(st, &m->out_bit15);
    state_bool(st, &m->decoding);

    if (ms) {
        state_u32(st, &ms->cmd_op);
        state_u32(st, &ms->param_idx);
        state_bool(st, &ms->quant_color);
        if (!st->writing) {
            ms->in_head  = 0u;
            ms->in_tail  = 0u;
            ms->out_head = 0u;
            ms->out_tail = 0u;
        }
    }
}

void mdec_shutdown(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    for (u32 i = 0u; i < MDEC_MAX_INSTANCES; i++) {
        if (g_mdec_streams[i] && g_mdec_streams[i]->owner == sys) {
            g_mdec_streams[i] = NULL;
        }
    }
}
