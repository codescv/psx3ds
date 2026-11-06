#include "periph_internal.h"

#include <string.h>

static const s32 k_xa_pos[4] = { 0, 60, 115,  98 };
static const s32 k_xa_neg[4] = { 0,  0, -52, -55 };

/* XA predictor history across consecutive sectors + resampling edge sample */
static s16 g_xa_hist[2][2];
static s16 g_xa_prev_lr[2];
static u32 g_xa_phase;

PSX_INLINE s16 xa_clamp16(s32 v) {
    if (v > 32767) {
        return 32767;
    }
    if (v < -32768) {
        return -32768;
    }
    return (s16)v;
}

void spu_xa_reset(void) {
    memset(g_xa_hist, 0, sizeof(g_xa_hist));
    g_xa_prev_lr[0] = 0;
    g_xa_prev_lr[1] = 0;
    g_xa_phase      = 0u;
}

static s16 xa_step(s32 sample, u32 filter, s16 hist[2]) {
    s32 pred = ((s32)hist[0] * k_xa_pos[filter] + (s32)hist[1] * k_xa_neg[filter] + 32) >> 6;
    s16 y    = xa_clamp16(sample + pred);
    hist[1]  = hist[0];
    hist[0]  = y;
    return y;
}

static void xa_apply_cd_vol(s32 l, s32 r, const u8 vol[4], s16 *out_l, s16 *out_r) {
    s32 ml = (l * (s32)vol[0] + r * (s32)vol[2]) >> 7;
    s32 mr = (l * (s32)vol[1] + r * (s32)vol[3]) >> 7;
    *out_l = xa_clamp16(ml);
    *out_r = xa_clamp16(mr);
}

void spu_xa_decode_sector(PsxSystem *sys, const u8 sector_2352[2352], const u8 vol[4]) {
    if (!sys || !sector_2352) {
        return;
    }

    u8 coding   = sector_2352[19];
    bool stereo = (coding & 0x03u) == 1u;
    bool half_r = ((coding >> 2) & 0x03u) == 1u; /* 1 = 18900 Hz, 0 = 37800 Hz */
    bool bits8  = ((coding >> 4) & 0x03u) == 1u;
    u32 src_hz  = half_r ? 18900u : 37800u;

    /* Max raw frames per XA sector: 18 * 8 * 28 = 4032 (mono 4-bit) */
    s16 raw_lr[4032 * 2];
    u32 raw_frames = 0u;

    for (u32 sg_idx = 0u; sg_idx < 18u; sg_idx++) {
        const u8 *sg = &sector_2352[24u + sg_idx * 128u];

        if (!bits8) {
            /* 4-bit ADPCM: 8 sound units of 28 samples */
            if (stereo) {
                for (u32 pair = 0u; pair < 4u; pair++) {
                    u32 ul = pair * 2u;
                    u32 ur = ul + 1u;
                    u8 hl  = sg[(ul & 3u) + ((ul & 4u) << 1)];
                    u8 hr  = sg[(ur & 3u) + ((ur & 4u) << 1)];
                    u32 sl = hl & 0x0Fu; if (sl > 12u) sl = 9u;
                    u32 sr = hr & 0x0Fu; if (sr > 12u) sr = 9u;
                    u32 fl = (hl >> 4) & 0x03u;
                    u32 fr = (hr >> 4) & 0x03u;

                    for (u32 s = 0u; s < 28u; s++) {
                        u8 b    = sg[16u + s * 4u + pair];
                        s32 xl  = (s32)(s16)((u16)(b & 0x0Fu) << 12) >> sl;
                        s32 xr  = (s32)(s16)((u16)((b >> 4) & 0x0Fu) << 12) >> sr;
                        s16 yl  = xa_step(xl, fl, g_xa_hist[0]);
                        s16 yr  = xa_step(xr, fr, g_xa_hist[1]);
                        raw_lr[raw_frames * 2u + 0u] = yl;
                        raw_lr[raw_frames * 2u + 1u] = yr;
                        raw_frames++;
                    }
                }
            } else {
                for (u32 u = 0u; u < 8u; u++) {
                    u8 h   = sg[(u & 3u) + ((u & 4u) << 1)];
                    u32 sh = h & 0x0Fu; if (sh > 12u) sh = 9u;
                    u32 fl = (h >> 4) & 0x03u;

                    for (u32 s = 0u; s < 28u; s++) {
                        u8 b   = sg[16u + s * 4u + (u >> 1)];
                        u8 nib = (u & 1u) ? ((b >> 4) & 0x0Fu) : (b & 0x0Fu);
                        s32 x  = (s32)(s16)((u16)nib << 12) >> sh;
                        s16 y  = xa_step(x, fl, g_xa_hist[0]);
                        raw_lr[raw_frames * 2u + 0u] = y;
                        raw_lr[raw_frames * 2u + 1u] = y;
                        raw_frames++;
                    }
                }
            }
        } else {
            /* 8-bit ADPCM: 4 sound units of 28 samples */
            if (stereo) {
                for (u32 pair = 0u; pair < 2u; pair++) {
                    u32 ul = pair * 2u;
                    u32 ur = ul + 1u;
                    u8 hl  = sg[ul];
                    u8 hr  = sg[ur];
                    u32 sl = hl & 0x0Fu; if (sl > 8u) sl = 8u;
                    u32 sr = hr & 0x0Fu; if (sr > 8u) sr = 8u;
                    u32 fl = (hl >> 4) & 0x03u;
                    u32 fr = (hr >> 4) & 0x03u;

                    for (u32 s = 0u; s < 28u; s++) {
                        u8 bl  = sg[16u + s * 4u + ul];
                        u8 br  = sg[16u + s * 4u + ur];
                        s32 xl = (s32)(s16)((u16)bl << 8) >> sl;
                        s32 xr = (s32)(s16)((u16)br << 8) >> sr;
                        s16 yl = xa_step(xl, fl, g_xa_hist[0]);
                        s16 yr = xa_step(xr, fr, g_xa_hist[1]);
                        raw_lr[raw_frames * 2u + 0u] = yl;
                        raw_lr[raw_frames * 2u + 1u] = yr;
                        raw_frames++;
                    }
                }
            } else {
                for (u32 u = 0u; u < 4u; u++) {
                    u8 h   = sg[u];
                    u32 sh = h & 0x0Fu; if (sh > 8u) sh = 8u;
                    u32 fl = (h >> 4) & 0x03u;

                    for (u32 s = 0u; s < 28u; s++) {
                        u8 b  = sg[16u + s * 4u + u];
                        s32 x = (s32)(s16)((u16)b << 8) >> sh;
                        s16 y = xa_step(x, fl, g_xa_hist[0]);
                        raw_lr[raw_frames * 2u + 0u] = y;
                        raw_lr[raw_frames * 2u + 1u] = y;
                        raw_frames++;
                    }
                }
            }
        }
    }

    /* Resample from 18.9 kHz or 37.8 kHz to 44.1 kHz with linear interpolation */
    s16 out_batch[512 * 2];
    u32 batch_n = 0u;

    for (u32 i = 0u; i < raw_frames; i++) {
        s32 cur_l = raw_lr[i * 2u + 0u];
        s32 cur_r = raw_lr[i * 2u + 1u];
        s32 prv_l = g_xa_prev_lr[0];
        s32 prv_r = g_xa_prev_lr[1];

        while (g_xa_phase < 44100u) {
            s32 frac_num = (s32)g_xa_phase;
            s32 l = prv_l + ((cur_l - prv_l) * frac_num) / (s32)44100;
            s32 r = prv_r + ((cur_r - prv_r) * frac_num) / (s32)44100;
            xa_apply_cd_vol(l, r, vol, &out_batch[batch_n * 2u + 0u], &out_batch[batch_n * 2u + 1u]);
            batch_n++;
            if (batch_n == 512u) {
                spu_push_xa(sys, out_batch, batch_n);
                batch_n = 0u;
            }
            g_xa_phase += src_hz;
        }
        g_xa_phase -= 44100u;
        g_xa_prev_lr[0] = (s16)cur_l;
        g_xa_prev_lr[1] = (s16)cur_r;
    }

    if (batch_n > 0u) {
        spu_push_xa(sys, out_batch, batch_n);
    }
}

void spu_cdda_push_sector(PsxSystem *sys, const u8 sector_2352[2352], const u8 vol[4]) {
    if (!sys || !sector_2352) {
        return;
    }
    s16 out_pcm[588 * 2];
    for (u32 i = 0u; i < 588u; i++) {
        const u8 *p = &sector_2352[i * 4u];
        s16 l = (s16)((u16)p[0] | ((u16)p[1] << 8));
        s16 r = (s16)((u16)p[2] | ((u16)p[3] << 8));
        xa_apply_cd_vol(l, r, vol, &out_pcm[i * 2u + 0u], &out_pcm[i * 2u + 1u]);
    }
    spu_push_xa(sys, out_pcm, 588u);
}
