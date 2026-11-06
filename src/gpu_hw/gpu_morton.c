#include "psx3ds/gpu_hw.h"

#include <string.h>

/*
 * PICA200 8x8 Z-order (Morton) intra-tile lookup table:
 *   index = morton_x[x] | morton_y[y]
 *   morton_x = { 0, 1, 4, 5, 16, 17, 20, 21 }
 *   morton_y = { 0, 2, 8, 10, 32, 34, 40, 42 }
 */
const u8 gpu_morton8x8_lut[8][8] = {
    {  0,  1,  4,  5, 16, 17, 20, 21 },
    {  2,  3,  6,  7, 18, 19, 22, 23 },
    {  8,  9, 12, 13, 24, 25, 28, 29 },
    { 10, 11, 14, 15, 26, 27, 30, 31 },
    { 32, 33, 36, 37, 48, 49, 52, 53 },
    { 34, 35, 38, 39, 50, 51, 54, 55 },
    { 40, 41, 44, 45, 56, 57, 60, 61 },
    { 42, 43, 46, 47, 58, 59, 62, 63 }
};

static const u8 k_morton_y[8] = { 0, 2, 8, 10, 32, 34, 40, 42 };
static const u8 k_morton_x[8] = { 0, 1, 4, 5, 16, 17, 20, 21 };

u32 gpu_morton_index(u32 x, u32 y, u32 width) {
    u32 tile_idx = (y >> 3) * (width >> 3) + (x >> 3);
    return (tile_idx << 6) + (u32)gpu_morton8x8_lut[y & 7u][x & 7u];
}

/* bit0 = non-zero STP=0 texel present, bit1 = STP=1 texel present (0x0000 contributes nothing) */
static inline u8 psx_texel_class_bits(u16 psx) {
    if (psx == 0x0000u) {
        return 0u;
    }
    return (psx & 0x8000u) ? 2u : 1u;
}

static inline GpuTexAlphaClass mask_to_alpha_class(u32 cls_mask) {
    if ((cls_mask & 3u) == 3u) {
        return GPU_TEX_ALPHA_MIXED;
    }
    if (cls_mask & 2u) {
        return GPU_TEX_ALPHA_ALL_STP;
    }
    return GPU_TEX_ALPHA_ALL_OPAQUE;
}

static inline bool tw_is_active(const GpuTexWindowKey *tw) {
    return tw != NULL && ((tw->mask_x & 0x1Fu) != 0u || (tw->mask_y & 0x1Fu) != 0u);
}

static inline void tile_masks_clear(GpuTexTileMasks *tm) {
    if (tm != NULL) {
        memset(tm, 0, sizeof(*tm));
    }
}

static inline void tile_masks_record(GpuTexTileMasks *tm, u32 tx, u32 ty, u32 tcls) {
    if (tm != NULL) {
        u32 bit = 1u << tx;
        if (tcls & 1u) {
            tm->stp0[ty] |= bit;
        }
        if (tcls & 2u) {
            tm->stp1[ty] |= bit;
        }
    }
}

static void build_tw_maps(const GpuTexWindowKey *tw, u8 u_map[256], u8 v_map[256]) {
    u32 mx = (u32)(tw->mask_x & 0x1Fu) * 8u;
    u32 my = (u32)(tw->mask_y & 0x1Fu) * 8u;
    u32 ox = (u32)((tw->off_x & tw->mask_x) & 0x1Fu) * 8u;
    u32 oy = (u32)((tw->off_y & tw->mask_y) & 0x1Fu) * 8u;
    for (u32 i = 0; i < 256u; ++i) {
        u_map[i] = (u8)((i & ~mx) | ox);
        v_map[i] = (u8)((i & ~my) | oy);
    }
}

/*
 * Region STP classification from per-8x8-tile masks. Coordinates are inclusive texel
 * bounds inside the 256x256 page; spans >= 256 (wrapping / repeat) cover the full axis.
 */
GpuTexAlphaClass gpu_tex_tiles_classify(const GpuTexTileMasks *tm,
                                        s32 u_lo, s32 v_lo, s32 u_hi, s32 v_hi) {
    if (tm == NULL) {
        return GPU_TEX_ALPHA_MIXED;
    }
    u32 col_mask;
    if (u_hi < u_lo || (u_hi - u_lo) >= 255) {
        col_mask = 0xFFFFFFFFu;
    } else {
        u32 c0 = ((u32)u_lo & 0xFFu) >> 3;
        u32 c1 = ((u32)u_hi & 0xFFu) >> 3;
        if (c0 <= c1) {
            u32 n = c1 - c0 + 1u;
            col_mask = (n >= 32u) ? 0xFFFFFFFFu : (((1u << n) - 1u) << c0);
        } else {
            /* wrapped across u = 255 -> 0 */
            col_mask = (0xFFFFFFFFu << c0) | ((c1 >= 31u) ? 0xFFFFFFFFu : ((2u << c1) - 1u));
        }
    }

    u32 r0, r1;
    bool v_full = (v_hi < v_lo || (v_hi - v_lo) >= 255);
    if (v_full) {
        r0 = 0u;
        r1 = 31u;
    } else {
        r0 = ((u32)v_lo & 0xFFu) >> 3;
        r1 = ((u32)v_hi & 0xFFu) >> 3;
    }

    u32 acc0 = 0u, acc1 = 0u;
    if (r0 <= r1) {
        for (u32 r = r0; r <= r1; ++r) {
            acc0 |= tm->stp0[r];
            acc1 |= tm->stp1[r];
        }
    } else {
        for (u32 r = r0; r < 32u; ++r) {
            acc0 |= tm->stp0[r];
            acc1 |= tm->stp1[r];
        }
        for (u32 r = 0u; r <= r1; ++r) {
            acc0 |= tm->stp0[r];
            acc1 |= tm->stp1[r];
        }
    }
    bool has0 = (acc0 & col_mask) != 0u;
    bool has1 = (acc1 & col_mask) != 0u;
    if (has0 && has1) {
        return GPU_TEX_ALPHA_MIXED;
    }
    return has1 ? GPU_TEX_ALPHA_ALL_STP : GPU_TEX_ALPHA_ALL_OPAQUE;
}

/* --------------------------------------------------------------------------
 * Generic decode skeletons (T = u16 RGBA5551 or u32 RGBA8888)
 * -------------------------------------------------------------------------- */
#define DEFINE_DECODE_CLUT4(NAME, T, CONV)                                                     \
    GpuTexAlphaClass NAME(T *dst_morton, const u16 *vram, u16 tp_x, u16 tp_y,                  \
                          u16 clut_x, u16 clut_y, const GpuTexWindowKey *tw,                   \
                          GpuTexTileMasks *tm) {                                               \
        T  clut[16];                                                                           \
        u8 ccls[16];                                                                           \
        const u16 *clut_row = vram + (((u32)clut_y & 0x1FFu) << 10);                           \
        for (u32 i = 0; i < 16u; ++i) {                                                        \
            u16 c = clut_row[(clut_x + i) & 0x3FFu];                                           \
            clut[i] = CONV(c);                                                                 \
            ccls[i] = psx_texel_class_bits(c);                                                 \
        }                                                                                      \
        tile_masks_clear(tm);                                                                  \
        u32 cls_mask = 0u;                                                                     \
        if (!tw_is_active(tw)) {                                                               \
            for (u32 ty = 0; ty < 32u; ++ty) {                                                 \
                for (u32 tx = 0; tx < 32u; ++tx) {                                             \
                    T *tile = dst_morton + (((ty << 5) + tx) << 6);                            \
                    u32 vx0 = ((u32)tp_x + (tx << 1) + 0u) & 0x3FFu;                           \
                    u32 vx1 = ((u32)tp_x + (tx << 1) + 1u) & 0x3FFu;                           \
                    u32 tcls = 0u;                                                             \
                    for (u32 ry = 0; ry < 8u; ++ry) {                                          \
                        u32 vy = ((u32)tp_y + (ty << 3) + ry) & 0x1FFu;                        \
                        const u16 *vrow = vram + (vy << 10);                                   \
                        u32 w0 = vrow[vx0];                                                    \
                        u32 w1 = vrow[vx1];                                                    \
                        u32 n0 = w0 & 0xFu, n1 = (w0 >> 4) & 0xFu;                             \
                        u32 n2 = (w0 >> 8) & 0xFu, n3 = (w0 >> 12) & 0xFu;                     \
                        u32 n4 = w1 & 0xFu, n5 = (w1 >> 4) & 0xFu;                             \
                        u32 n6 = (w1 >> 8) & 0xFu, n7 = (w1 >> 12) & 0xFu;                     \
                        u32 my = k_morton_y[ry];                                               \
                        tile[my +  0u] = clut[n0];                                             \
                        tile[my +  1u] = clut[n1];                                             \
                        tile[my +  4u] = clut[n2];                                             \
                        tile[my +  5u] = clut[n3];                                             \
                        tile[my + 16u] = clut[n4];                                             \
                        tile[my + 17u] = clut[n5];                                             \
                        tile[my + 20u] = clut[n6];                                             \
                        tile[my + 21u] = clut[n7];                                             \
                        tcls |= (u32)(ccls[n0] | ccls[n1] | ccls[n2] | ccls[n3] |              \
                                      ccls[n4] | ccls[n5] | ccls[n6] | ccls[n7]);              \
                    }                                                                          \
                    tile_masks_record(tm, tx, ty, tcls);                                       \
                    cls_mask |= tcls;                                                          \
                }                                                                              \
            }                                                                                  \
        } else {                                                                               \
            u8 u_map[256], v_map[256];                                                         \
            build_tw_maps(tw, u_map, v_map);                                                   \
            for (u32 ty = 0; ty < 32u; ++ty) {                                                 \
                for (u32 tx = 0; tx < 32u; ++tx) {                                             \
                    T *tile = dst_morton + (((ty << 5) + tx) << 6);                            \
                    u32 tcls = 0u;                                                             \
                    for (u32 ry = 0; ry < 8u; ++ry) {                                          \
                        u32 v = v_map[(ty << 3) + ry];                                         \
                        const u16 *vrow = vram + ((((u32)tp_y + v) & 0x1FFu) << 10);           \
                        u32 my = k_morton_y[ry];                                               \
                        for (u32 rx = 0; rx < 8u; ++rx) {                                      \
                            u32 u = u_map[(tx << 3) + rx];                                     \
                            u16 w = vrow[((u32)tp_x + (u >> 2)) & 0x3FFu];                     \
                            u32 nib = (w >> ((u & 3u) << 2)) & 0xFu;                           \
                            tile[my + k_morton_x[rx]] = clut[nib];                             \
                            tcls |= ccls[nib];                                                 \
                        }                                                                      \
                    }                                                                          \
                    tile_masks_record(tm, tx, ty, tcls);                                       \
                    cls_mask |= tcls;                                                          \
                }                                                                              \
            }                                                                                  \
        }                                                                                      \
        return mask_to_alpha_class(cls_mask);                                                  \
    }

#define DEFINE_DECODE_CLUT8(NAME, T, CONV)                                                     \
    GpuTexAlphaClass NAME(T *dst_morton, const u16 *vram, u16 tp_x, u16 tp_y,                  \
                          u16 clut_x, u16 clut_y, const GpuTexWindowKey *tw,                   \
                          GpuTexTileMasks *tm) {                                               \
        T  clut[256];                                                                          \
        u8 ccls[256];                                                                          \
        const u16 *clut_row = vram + (((u32)clut_y & 0x1FFu) << 10);                           \
        for (u32 i = 0; i < 256u; ++i) {                                                       \
            u16 c = clut_row[(clut_x + i) & 0x3FFu];                                           \
            clut[i] = CONV(c);                                                                 \
            ccls[i] = psx_texel_class_bits(c);                                                 \
        }                                                                                      \
        tile_masks_clear(tm);                                                                  \
        u32 cls_mask = 0u;                                                                     \
        if (!tw_is_active(tw)) {                                                               \
            for (u32 ty = 0; ty < 32u; ++ty) {                                                 \
                for (u32 tx = 0; tx < 32u; ++tx) {                                             \
                    T *tile = dst_morton + (((ty << 5) + tx) << 6);                            \
                    u32 vx0 = ((u32)tp_x + (tx << 2) + 0u) & 0x3FFu;                           \
                    u32 vx1 = ((u32)tp_x + (tx << 2) + 1u) & 0x3FFu;                           \
                    u32 vx2 = ((u32)tp_x + (tx << 2) + 2u) & 0x3FFu;                           \
                    u32 vx3 = ((u32)tp_x + (tx << 2) + 3u) & 0x3FFu;                           \
                    u32 tcls = 0u;                                                             \
                    for (u32 ry = 0; ry < 8u; ++ry) {                                          \
                        u32 vy = ((u32)tp_y + (ty << 3) + ry) & 0x1FFu;                        \
                        const u16 *vrow = vram + (vy << 10);                                   \
                        u32 w0 = vrow[vx0], w1 = vrow[vx1], w2 = vrow[vx2], w3 = vrow[vx3];    \
                        u32 b0 = w0 & 0xFFu, b1 = w0 >> 8;                                     \
                        u32 b2 = w1 & 0xFFu, b3 = w1 >> 8;                                     \
                        u32 b4 = w2 & 0xFFu, b5 = w2 >> 8;                                     \
                        u32 b6 = w3 & 0xFFu, b7 = w3 >> 8;                                     \
                        u32 my = k_morton_y[ry];                                               \
                        tile[my +  0u] = clut[b0];                                             \
                        tile[my +  1u] = clut[b1];                                             \
                        tile[my +  4u] = clut[b2];                                             \
                        tile[my +  5u] = clut[b3];                                             \
                        tile[my + 16u] = clut[b4];                                             \
                        tile[my + 17u] = clut[b5];                                             \
                        tile[my + 20u] = clut[b6];                                             \
                        tile[my + 21u] = clut[b7];                                             \
                        tcls |= (u32)(ccls[b0] | ccls[b1] | ccls[b2] | ccls[b3] |              \
                                      ccls[b4] | ccls[b5] | ccls[b6] | ccls[b7]);              \
                    }                                                                          \
                    tile_masks_record(tm, tx, ty, tcls);                                       \
                    cls_mask |= tcls;                                                          \
                }                                                                              \
            }                                                                                  \
        } else {                                                                               \
            u8 u_map[256], v_map[256];                                                         \
            build_tw_maps(tw, u_map, v_map);                                                   \
            for (u32 ty = 0; ty < 32u; ++ty) {                                                 \
                for (u32 tx = 0; tx < 32u; ++tx) {                                             \
                    T *tile = dst_morton + (((ty << 5) + tx) << 6);                            \
                    u32 tcls = 0u;                                                             \
                    for (u32 ry = 0; ry < 8u; ++ry) {                                          \
                        u32 v = v_map[(ty << 3) + ry];                                         \
                        const u16 *vrow = vram + ((((u32)tp_y + v) & 0x1FFu) << 10);           \
                        u32 my = k_morton_y[ry];                                               \
                        for (u32 rx = 0; rx < 8u; ++rx) {                                      \
                            u32 u = u_map[(tx << 3) + rx];                                     \
                            u16 w = vrow[((u32)tp_x + (u >> 1)) & 0x3FFu];                     \
                            u32 idx = (w >> ((u & 1u) << 3)) & 0xFFu;                          \
                            tile[my + k_morton_x[rx]] = clut[idx];                             \
                            tcls |= ccls[idx];                                                 \
                        }                                                                      \
                    }                                                                          \
                    tile_masks_record(tm, tx, ty, tcls);                                       \
                    cls_mask |= tcls;                                                          \
                }                                                                              \
            }                                                                                  \
        }                                                                                      \
        return mask_to_alpha_class(cls_mask);                                                  \
    }

#define DEFINE_DECODE_15BPP(NAME, T, CONV)                                                     \
    GpuTexAlphaClass NAME(T *dst_morton, const u16 *vram, u16 tp_x, u16 tp_y,                  \
                          const GpuTexWindowKey *tw, GpuTexTileMasks *tm) {                    \
        tile_masks_clear(tm);                                                                  \
        u32 cls_mask = 0u;                                                                     \
        bool twa = tw_is_active(tw);                                                           \
        u8 u_map[256], v_map[256];                                                             \
        if (twa) {                                                                             \
            build_tw_maps(tw, u_map, v_map);                                                   \
        }                                                                                      \
        for (u32 ty = 0; ty < 32u; ++ty) {                                                     \
            for (u32 tx = 0; tx < 32u; ++tx) {                                                 \
                T *tile = dst_morton + (((ty << 5) + tx) << 6);                                \
                u32 tcls = 0u;                                                                 \
                for (u32 ry = 0; ry < 8u; ++ry) {                                              \
                    u32 v = twa ? v_map[(ty << 3) + ry] : ((ty << 3) + ry);                    \
                    const u16 *vrow = vram + ((((u32)tp_y + v) & 0x1FFu) << 10);               \
                    u32 my = k_morton_y[ry];                                                   \
                    for (u32 rx = 0; rx < 8u; ++rx) {                                          \
                        u32 u = twa ? u_map[(tx << 3) + rx] : ((tx << 3) + rx);                \
                        u16 c = vrow[((u32)tp_x + u) & 0x3FFu];                                \
                        tile[my + k_morton_x[rx]] = CONV(c);                                   \
                        tcls |= psx_texel_class_bits(c);                                       \
                    }                                                                          \
                }                                                                              \
                tile_masks_record(tm, tx, ty, tcls);                                           \
                cls_mask |= tcls;                                                              \
            }                                                                                  \
        }                                                                                      \
        return mask_to_alpha_class(cls_mask);                                                  \
    }

DEFINE_DECODE_CLUT4(gpu_morton_decode_4bpp_5551, u16, gpu_psx_to_rgba5551)
DEFINE_DECODE_CLUT4(gpu_morton_decode_4bpp_8888, u32, gpu_psx_to_rgba8888)
DEFINE_DECODE_CLUT8(gpu_morton_decode_8bpp_5551, u16, gpu_psx_to_rgba5551)
DEFINE_DECODE_CLUT8(gpu_morton_decode_8bpp_8888, u32, gpu_psx_to_rgba8888)
DEFINE_DECODE_15BPP(gpu_morton_decode_15bpp_5551, u16, gpu_psx_to_rgba5551)
DEFINE_DECODE_15BPP(gpu_morton_decode_15bpp_8888, u32, gpu_psx_to_rgba8888)

/* --------------------------------------------------------------------------
 * Arbitrary Subrect Swizzlers for CPU->VRAM Uploads & 24bpp Display
 * -------------------------------------------------------------------------- */
void gpu_morton_swizzle_vram_rect_5551(u16 *dst_morton, u32 dst_w, u32 dst_h,
                                       const u16 *vram, u16 src_x, u16 src_y, u16 w, u16 h) {
    u32 max_x = w < dst_w ? (u32)w : dst_w;
    u32 max_y = h < dst_h ? (u32)h : dst_h;
    u32 tiles_x = dst_w >> 3;
    for (u32 y = 0; y < max_y; ++y) {
        u32 vy = ((u32)src_y + y) & 0x1FFu;
        const u16 *vrow = vram + (vy << 10);
        u32 ty_off = (y >> 3) * tiles_x;
        u32 my = k_morton_y[y & 7u];
        for (u32 x = 0; x < max_x; ++x) {
            u16 c = vrow[((u32)src_x + x) & 0x3FFu];
            u32 idx = ((ty_off + (x >> 3)) << 6) + my + k_morton_x[x & 7u];
            dst_morton[idx] = gpu_psx_to_rgba5551_opaque(c);
        }
    }
}

void gpu_morton_swizzle_vram_24bpp_8888(u32 *dst_morton, u32 dst_w, u32 dst_h,
                                        const u16 *vram, u16 src_x, u16 src_y, u16 w_px, u16 h_px) {
    u32 max_x = w_px < dst_w ? (u32)w_px : dst_w;
    u32 max_y = h_px < dst_h ? (u32)h_px : dst_h;
    u32 tiles_x = dst_w >> 3;
    u32 base_byte_x = ((u32)src_x & 0x3FFu) * 2u;

    for (u32 y = 0; y < max_y; ++y) {
        u32 vy = ((u32)src_y + y) & 0x1FFu;
        const u8 *row_bytes = (const u8 *)(vram + (vy << 10));
        u32 ty_off = (y >> 3) * tiles_x;
        u32 my = k_morton_y[y & 7u];
        u32 bx = base_byte_x;
        for (u32 x = 0; x < max_x; ++x) {
            u32 r8 = row_bytes[(bx + 0u) & 0x7FFu];
            u32 g8 = row_bytes[(bx + 1u) & 0x7FFu];
            u32 b8 = row_bytes[(bx + 2u) & 0x7FFu];
            bx += 3u;
            u32 idx = ((ty_off + (x >> 3)) << 6) + my + k_morton_x[x & 7u];
            dst_morton[idx] = (r8 << 24) | (g8 << 16) | (b8 << 8) | 0xFFu;
        }
    }
}
