#include "periph_internal.h"

static const s32 k_adpcm_pos[8] = { 0, 60, 115,  98, 122, 0, 0, 0 };
static const s32 k_adpcm_neg[8] = { 0,  0, -52, -55, -60, 0, 0, 0 };

PSX_INLINE s16 spu_clamp16(s32 v) {
    if (v > 32767) {
        return 32767;
    }
    if (v < -32768) {
        return -32768;
    }
    return (s16)v;
}

void spu_adpcm_decode_block_c(const u8 block[16], s16 out[28], s16 hist[2]) {
    u32 hdr    = block[0];
    u32 shift  = hdr & 0x0Fu;
    u32 filter = (hdr >> 4) & 0x07u;
    if (shift > 12u) {
        shift = 9u;
    }

    s32 f0 = k_adpcm_pos[filter];
    s32 f1 = k_adpcm_neg[filter];
    s32 h0 = hist[0];
    s32 h1 = hist[1];

    for (u32 i = 0; i < 14u; i++) {
        u32 b = block[2u + i];

        s32 s0   = (s32)(s16)((u16)(b & 0x0Fu) << 12) >> shift;
        s32 p0   = (h0 * f0 + h1 * f1 + 32) >> 6;
        s16 y0   = spu_clamp16(s0 + p0);
        h1       = h0;
        h0       = y0;
        out[i * 2u + 0u] = y0;

        s32 s1   = (s32)(s16)((u16)((b >> 4) & 0x0Fu) << 12) >> shift;
        s32 p1   = (h0 * f0 + h1 * f1 + 32) >> 6;
        s16 y1   = spu_clamp16(s1 + p1);
        h1       = h0;
        h0       = y1;
        out[i * 2u + 1u] = y1;
    }

    hist[0] = (s16)h0;
    hist[1] = (s16)h1;
}

PSX_HOT void spu_adpcm_decode_block(const u8 block[16], s16 out[28], s16 hist[2]) {
#if PSX3DS_ARCH_ARMV6K
    spu_adpcm_decode_block_armv6(block, out, hist);
#else
    spu_adpcm_decode_block_c(block, out, hist);
#endif
}
