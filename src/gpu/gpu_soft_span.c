#include "gpu_internal.h"

u32 g_gpu_rgb555_to_888[32768];
static bool s_tables_initialized = false;

/* Sony PSX 4x4 signed dithering matrix (indexed by [y & 3][x & 3]) */
static const s8 s_psx_dither[4][4] = {
    { -4,  0, -3, +1 },
    { +2, -2, +3, -1 },
    { -3, +1, -4,  0 },
    { +3, -1, +2, -2 }
};

void gpu_soft_span_init_tables(void) {
    if (s_tables_initialized) {
        return;
    }
    for (u32 c = 0u; c < 32768u; c++) {
        u32 r5 = c & 0x1Fu;
        u32 g5 = (c >> 5) & 0x1Fu;
        u32 b5 = (c >> 10) & 0x1Fu;
        g_gpu_rgb555_to_888[c] = (r5 << 3) | (g5 << 11) | (b5 << 19);
    }
    s_tables_initialized = true;
}

static inline u32 sat5_asr19(s32 v) {
    s32 s = v >> 19;
    if (s < 0) return 0u;
    if (s > 31) return 31u;
    return (u32)s;
}

static inline u32 sat5_mod(u32 tex5, u32 col8) {
    u32 m = (tex5 * col8) >> 7;
    return (m > 31u) ? 31u : m;
}

static inline u32 dither_chan8_to_5(s32 c8, s32 d) {
    s32 s = (c8 + d) >> 3;
    if (s < 0) return 0u;
    if (s > 31) return 31u;
    return (u32)s;
}

static inline u32 step_uv_uadd16(u32 uv, u32 duv) {
    u32 u_lo = (uv + duv) & 0xFFFFu;
    u32 v_hi = (uv & 0xFFFF0000u) + (duv & 0xFFFF0000u);
    return v_hi | u_lo;
}

/* --------------------------------------------------------------------------
 * 1. Fast 16-bit Solid Fill Span
 * -------------------------------------------------------------------------- */
void gpu_soft_span_fill16_c(u16 *dst, u16 val, u32 count) {
    if (count == 0u) {
        return;
    }
    if (((uintptr_t)dst & 2u) != 0u) {
        *dst++ = val;
        count--;
    }
    u32 pair = (u32)val | ((u32)val << 16);
    u32 *dst32 = (u32 *)dst;
    u32 pairs = count >> 1;
    for (u32 i = 0u; i < pairs; i++) {
        dst32[i] = pair;
    }
    if ((count & 1u) != 0u) {
        dst[count - 1u] = val;
    }
}

/* --------------------------------------------------------------------------
 * 2. Flat Untextured ABR Blended Span
 * -------------------------------------------------------------------------- */
void gpu_soft_span_flat_abr_c(u16 *dst, u32 count, u16 col15, u32 abr, u16 mask_or) {
    u32 f8 = g_gpu_rgb555_to_888[col15 & 0x7FFFu];
    switch (abr & 3u) {
        case 0u: {
            u32 fh = f8 >> 1;
            for (u32 i = 0u; i < count; i++) {
                u32 b8 = g_gpu_rgb555_to_888[dst[i] & 0x7FFFu];
                u32 out8 = (b8 >> 1) + fh;
                dst[i] = (u16)(((out8 >> 3) & 0x1Fu) |
                               (((out8 >> 11) & 0x1Fu) << 5) |
                               (((out8 >> 19) & 0x1Fu) << 10) |
                               mask_or);
            }
            break;
        }
        case 1u: {
            u32 fr = f8 & 0xFFu, fg = (f8 >> 8) & 0xFFu, fb = (f8 >> 16) & 0xFFu;
            for (u32 i = 0u; i < count; i++) {
                u32 b8 = g_gpu_rgb555_to_888[dst[i] & 0x7FFFu];
                u32 r = (b8 & 0xFFu) + fr;
                u32 g = ((b8 >> 8) & 0xFFu) + fg;
                u32 b = ((b8 >> 16) & 0xFFu) + fb;
                if (r > 255u) r = 255u;
                if (g > 255u) g = 255u;
                if (b > 255u) b = 255u;
                dst[i] = (u16)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | mask_or);
            }
            break;
        }
        case 2u: {
            s32 fr = (s32)(f8 & 0xFFu), fg = (s32)((f8 >> 8) & 0xFFu), fb = (s32)((f8 >> 16) & 0xFFu);
            for (u32 i = 0u; i < count; i++) {
                u32 b8 = g_gpu_rgb555_to_888[dst[i] & 0x7FFFu];
                s32 r = (s32)(b8 & 0xFFu) - fr;
                s32 g = (s32)((b8 >> 8) & 0xFFu) - fg;
                s32 b = (s32)((b8 >> 16) & 0xFFu) - fb;
                if (r < 0) r = 0;
                if (g < 0) g = 0;
                if (b < 0) b = 0;
                dst[i] = (u16)(((u32)r >> 3) | (((u32)g >> 3) << 5) | (((u32)b >> 3) << 10) | mask_or);
            }
            break;
        }
        default: {
            u32 fq = f8 >> 2;
            u32 fr = fq & 0xFFu, fg = (fq >> 8) & 0xFFu, fb = (fq >> 16) & 0xFFu;
            for (u32 i = 0u; i < count; i++) {
                u32 b8 = g_gpu_rgb555_to_888[dst[i] & 0x7FFFu];
                u32 r = (b8 & 0xFFu) + fr;
                u32 g = ((b8 >> 8) & 0xFFu) + fg;
                u32 b = ((b8 >> 16) & 0xFFu) + fb;
                if (r > 255u) r = 255u;
                if (g > 255u) g = 255u;
                if (b > 255u) b = 255u;
                dst[i] = (u16)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | mask_or);
            }
            break;
        }
    }
}

/* --------------------------------------------------------------------------
 * 3. Gouraud Untextured Spans (Opaque & ABR)
 * -------------------------------------------------------------------------- */
void gpu_soft_span_gouraud_opaque_c(u16 *dst, u32 count,
                                    s32 r_q16, s32 g_q16, s32 b_q16,
                                    s32 dr_q16, s32 dg_q16, s32 db_q16,
                                    u16 mask_or) {
    for (u32 i = 0u; i < count; i++) {
        u32 r5 = sat5_asr19(r_q16);
        u32 g5 = sat5_asr19(g_q16);
        u32 b5 = sat5_asr19(b_q16);
        dst[i] = (u16)(r5 | (g5 << 5) | (b5 << 10) | mask_or);
        r_q16 += dr_q16;
        g_q16 += dg_q16;
        b_q16 += db_q16;
    }
}

void gpu_soft_span_gouraud_abr_c(u16 *dst, u32 count,
                                 s32 r_q16, s32 g_q16, s32 b_q16,
                                 s32 dr_q16, s32 dg_q16, s32 db_q16,
                                 u32 abr, u16 mask_or) {
    u32 mode = abr & 3u;
    for (u32 i = 0u; i < count; i++) {
        u32 r5 = sat5_asr19(r_q16);
        u32 g5 = sat5_asr19(g_q16);
        u32 b5 = sat5_asr19(b_q16);
        u16 fg15 = (u16)(r5 | (g5 << 5) | (b5 << 10));
        dst[i] = (u16)(gpu_blend_abr15(dst[i], fg15, mode) | mask_or);
        r_q16 += dr_q16;
        g_q16 += dg_q16;
        b_q16 += db_q16;
    }
}

/* --------------------------------------------------------------------------
 * 4. Textured Raw Opaque Spans (4bpp, 8bpp, 15bpp)
 * -------------------------------------------------------------------------- */
void gpu_soft_span_tex4_raw_opaque_c(u16 *dst, u32 count, const u16 *tex_base,
                                     const u16 *clut, u32 uv_pack, u32 duv_pack, u16 mask_or) {
    for (u32 i = 0u; i < count; i++) {
        u32 u = (uv_pack >> 8) & 0xFFu;
        u32 v = (uv_pack >> 24) & 0xFFu;
        u16 word = tex_base[(v << 10) + (u >> 2)];
        u16 texel = clut[(word >> ((u & 3u) << 2)) & 0xFu];
        if (texel != 0x0000u) {
            dst[i] = (u16)(texel | mask_or);
        }
        uv_pack = step_uv_uadd16(uv_pack, duv_pack);
    }
}

void gpu_soft_span_tex8_raw_opaque_c(u16 *dst, u32 count, const u16 *tex_base,
                                     const u16 *clut, u32 uv_pack, u32 duv_pack, u16 mask_or) {
    const u8 *tex_bytes = (const u8 *)tex_base;
    for (u32 i = 0u; i < count; i++) {
        u32 u = (uv_pack >> 8) & 0xFFu;
        u32 v = (uv_pack >> 24) & 0xFFu;
        u8  idx = tex_bytes[(v << 11) + u];
        u16 texel = clut[idx];
        if (texel != 0x0000u) {
            dst[i] = (u16)(texel | mask_or);
        }
        uv_pack = step_uv_uadd16(uv_pack, duv_pack);
    }
}

void gpu_soft_span_tex16_raw_opaque_c(u16 *dst, u32 count, const u16 *tex_base,
                                      u32 uv_pack, u32 duv_pack, u16 mask_or) {
    for (u32 i = 0u; i < count; i++) {
        u32 u = (uv_pack >> 8) & 0xFFu;
        u32 v = (uv_pack >> 24) & 0xFFu;
        u16 texel = tex_base[(v << 10) + u];
        if (texel != 0x0000u) {
            dst[i] = (u16)(texel | mask_or);
        }
        uv_pack = step_uv_uadd16(uv_pack, duv_pack);
    }
}

/* --------------------------------------------------------------------------
 * 5. Textured Flat-Modulated Opaque Spans (4bpp, 8bpp, 15bpp)
 * -------------------------------------------------------------------------- */
void gpu_soft_span_tex4_mod_flat_opaque_c(u16 *dst, u32 count, const u16 *tex_base,
                                          const u16 *clut, u32 uv_pack, u32 duv_pack,
                                          u32 rgb_mod, u16 mask_or) {
    u32 cr = rgb_mod & 0xFFu;
    u32 cg = (rgb_mod >> 8) & 0xFFu;
    u32 cb = (rgb_mod >> 16) & 0xFFu;
    for (u32 i = 0u; i < count; i++) {
        u32 u = (uv_pack >> 8) & 0xFFu;
        u32 v = (uv_pack >> 24) & 0xFFu;
        u16 word = tex_base[(v << 10) + (u >> 2)];
        u16 texel = clut[(word >> ((u & 3u) << 2)) & 0xFu];
        if (texel != 0x0000u) {
            u32 r5 = sat5_mod(texel & 0x1Fu, cr);
            u32 g5 = sat5_mod((texel >> 5) & 0x1Fu, cg);
            u32 b5 = sat5_mod((texel >> 10) & 0x1Fu, cb);
            dst[i] = (u16)(r5 | (g5 << 5) | (b5 << 10) | (texel & 0x8000u) | mask_or);
        }
        uv_pack = step_uv_uadd16(uv_pack, duv_pack);
    }
}

void gpu_soft_span_tex8_mod_flat_opaque_c(u16 *dst, u32 count, const u16 *tex_base,
                                          const u16 *clut, u32 uv_pack, u32 duv_pack,
                                          u32 rgb_mod, u16 mask_or) {
    const u8 *tex_bytes = (const u8 *)tex_base;
    u32 cr = rgb_mod & 0xFFu;
    u32 cg = (rgb_mod >> 8) & 0xFFu;
    u32 cb = (rgb_mod >> 16) & 0xFFu;
    for (u32 i = 0u; i < count; i++) {
        u32 u = (uv_pack >> 8) & 0xFFu;
        u32 v = (uv_pack >> 24) & 0xFFu;
        u16 texel = clut[tex_bytes[(v << 11) + u]];
        if (texel != 0x0000u) {
            u32 r5 = sat5_mod(texel & 0x1Fu, cr);
            u32 g5 = sat5_mod((texel >> 5) & 0x1Fu, cg);
            u32 b5 = sat5_mod((texel >> 10) & 0x1Fu, cb);
            dst[i] = (u16)(r5 | (g5 << 5) | (b5 << 10) | (texel & 0x8000u) | mask_or);
        }
        uv_pack = step_uv_uadd16(uv_pack, duv_pack);
    }
}

void gpu_soft_span_tex16_mod_flat_opaque_c(u16 *dst, u32 count, const u16 *tex_base,
                                           u32 uv_pack, u32 duv_pack,
                                           u32 rgb_mod, u16 mask_or) {
    u32 cr = rgb_mod & 0xFFu;
    u32 cg = (rgb_mod >> 8) & 0xFFu;
    u32 cb = (rgb_mod >> 16) & 0xFFu;
    for (u32 i = 0u; i < count; i++) {
        u32 u = (uv_pack >> 8) & 0xFFu;
        u32 v = (uv_pack >> 24) & 0xFFu;
        u16 texel = tex_base[(v << 10) + u];
        if (texel != 0x0000u) {
            u32 r5 = sat5_mod(texel & 0x1Fu, cr);
            u32 g5 = sat5_mod((texel >> 5) & 0x1Fu, cg);
            u32 b5 = sat5_mod((texel >> 10) & 0x1Fu, cb);
            dst[i] = (u16)(r5 | (g5 << 5) | (b5 << 10) | (texel & 0x8000u) | mask_or);
        }
        uv_pack = step_uv_uadd16(uv_pack, duv_pack);
    }
}

/* --------------------------------------------------------------------------
 * 6. Textured Flat-Modulated Semi-Transparent (ABR, Per-Texel STP Gated)
 * -------------------------------------------------------------------------- */
void gpu_soft_span_tex4_mod_flat_abr_c(u16 *dst, u32 count, const u16 *tex_base,
                                       const u16 *clut, u32 uv_pack, u32 duv_pack,
                                       u32 rgb_mod, u32 abr, u16 mask_or) {
    u32 cr = rgb_mod & 0xFFu;
    u32 cg = (rgb_mod >> 8) & 0xFFu;
    u32 cb = (rgb_mod >> 16) & 0xFFu;
    u32 mode = abr & 3u;
    for (u32 i = 0u; i < count; i++) {
        u32 u = (uv_pack >> 8) & 0xFFu;
        u32 v = (uv_pack >> 24) & 0xFFu;
        u16 word = tex_base[(v << 10) + (u >> 2)];
        u16 texel = clut[(word >> ((u & 3u) << 2)) & 0xFu];
        if (texel != 0x0000u) {
            u32 r5 = sat5_mod(texel & 0x1Fu, cr);
            u32 g5 = sat5_mod((texel >> 5) & 0x1Fu, cg);
            u32 b5 = sat5_mod((texel >> 10) & 0x1Fu, cb);
            u16 fg15 = (u16)(r5 | (g5 << 5) | (b5 << 10));
            if ((texel & 0x8000u) != 0u) {
                dst[i] = (u16)(gpu_blend_abr15(dst[i], fg15, mode) | 0x8000u | mask_or);
            } else {
                dst[i] = (u16)(fg15 | mask_or);
            }
        }
        uv_pack = step_uv_uadd16(uv_pack, duv_pack);
    }
}

void gpu_soft_span_tex8_mod_flat_abr_c(u16 *dst, u32 count, const u16 *tex_base,
                                       const u16 *clut, u32 uv_pack, u32 duv_pack,
                                       u32 rgb_mod, u32 abr, u16 mask_or) {
    const u8 *tex_bytes = (const u8 *)tex_base;
    u32 cr = rgb_mod & 0xFFu;
    u32 cg = (rgb_mod >> 8) & 0xFFu;
    u32 cb = (rgb_mod >> 16) & 0xFFu;
    u32 mode = abr & 3u;
    for (u32 i = 0u; i < count; i++) {
        u32 u = (uv_pack >> 8) & 0xFFu;
        u32 v = (uv_pack >> 24) & 0xFFu;
        u16 texel = clut[tex_bytes[(v << 11) + u]];
        if (texel != 0x0000u) {
            u32 r5 = sat5_mod(texel & 0x1Fu, cr);
            u32 g5 = sat5_mod((texel >> 5) & 0x1Fu, cg);
            u32 b5 = sat5_mod((texel >> 10) & 0x1Fu, cb);
            u16 fg15 = (u16)(r5 | (g5 << 5) | (b5 << 10));
            if ((texel & 0x8000u) != 0u) {
                dst[i] = (u16)(gpu_blend_abr15(dst[i], fg15, mode) | 0x8000u | mask_or);
            } else {
                dst[i] = (u16)(fg15 | mask_or);
            }
        }
        uv_pack = step_uv_uadd16(uv_pack, duv_pack);
    }
}

void gpu_soft_span_tex16_mod_flat_abr_c(u16 *dst, u32 count, const u16 *tex_base,
                                        u32 uv_pack, u32 duv_pack,
                                        u32 rgb_mod, u32 abr, u16 mask_or) {
    u32 cr = rgb_mod & 0xFFu;
    u32 cg = (rgb_mod >> 8) & 0xFFu;
    u32 cb = (rgb_mod >> 16) & 0xFFu;
    u32 mode = abr & 3u;
    for (u32 i = 0u; i < count; i++) {
        u32 u = (uv_pack >> 8) & 0xFFu;
        u32 v = (uv_pack >> 24) & 0xFFu;
        u16 texel = tex_base[(v << 10) + u];
        if (texel != 0x0000u) {
            u32 r5 = sat5_mod(texel & 0x1Fu, cr);
            u32 g5 = sat5_mod((texel >> 5) & 0x1Fu, cg);
            u32 b5 = sat5_mod((texel >> 10) & 0x1Fu, cb);
            u16 fg15 = (u16)(r5 | (g5 << 5) | (b5 << 10));
            if ((texel & 0x8000u) != 0u) {
                dst[i] = (u16)(gpu_blend_abr15(dst[i], fg15, mode) | 0x8000u | mask_or);
            } else {
                dst[i] = (u16)(fg15 | mask_or);
            }
        }
        uv_pack = step_uv_uadd16(uv_pack, duv_pack);
    }
}

/* --------------------------------------------------------------------------
 * 7. General Full-Featured Span (handles Dither, MaskCheck, TexWindow,
 *    Gouraud+Textured, Raw+ABR, and wrapping 8bpp/15bpp right-edge texpages)
 * -------------------------------------------------------------------------- */
static inline u16 sample_vram_texel(const u16 *vram, const u16 *clut,
                                    u32 tx_base, u32 ty_base, u32 depth,
                                    u32 u, u32 v) {
    u32 vy = (ty_base + v) & 0x1FFu;
    const u16 *row = vram + (vy * PSX_VRAM_W);
    if (depth == 0u) { /* 4bpp CLUT */
        u32 vx = (tx_base + (u >> 2)) & 0x3FFu;
        u16 w  = row[vx];
        return clut[(w >> ((u & 3u) << 2)) & 0xFu];
    } else if (depth == 1u) { /* 8bpp CLUT */
        u32 vx = (tx_base + (u >> 1)) & 0x3FFu;
        u16 w  = row[vx];
        u32 idx = (u & 1u) ? (w >> 8) : (w & 0xFFu);
        return clut[idx];
    } else { /* 15bpp direct */
        u32 vx = (tx_base + u) & 0x3FFu;
        return row[vx];
    }
}

void gpu_soft_span_general(const GpuSpanParams *sp) {
    u16 *dst = sp->dst_row + sp->x_start;
    u32  n   = sp->count;
    s32  x   = sp->x_start;
    const s8 *drow = s_psx_dither[sp->y & 3];

    s32 r_q16 = sp->r_q16, g_q16 = sp->g_q16, b_q16 = sp->b_q16;
    s32 dr = sp->dr_q16,   dg = sp->dg_q16,   db = sp->db_q16;
    s32 u_q16 = sp->u_q16, v_q16 = sp->v_q16;
    s32 du = sp->du_q16,   dv = sp->dv_q16;

    bool is_tex   = (sp->flags & PF_TEXTURED) != 0u;
    bool is_raw   = (sp->flags & PF_RAW_TEX) != 0u;
    bool is_semi  = (sp->flags & PF_SEMI) != 0u;
    bool do_dith  = (sp->dither != 0u);
    bool chk_mask = (sp->check_mask != 0u);
    u16  mask_or  = sp->set_mask ? 0x8000u : 0u;

    for (u32 i = 0u; i < n; i++, x++) {
        if (chk_mask && (dst[i] & 0x8000u) != 0u) {
            r_q16 += dr; g_q16 += dg; b_q16 += db;
            u_q16 += du; v_q16 += dv;
            continue;
        }

        s32 cr = r_q16 >> 16;
        s32 cg = g_q16 >> 16;
        s32 cb = b_q16 >> 16;
        if (cr < 0) cr = 0; else if (cr > 255) cr = 255;
        if (cg < 0) cg = 0; else if (cg > 255) cg = 255;
        if (cb < 0) cb = 0; else if (cb > 255) cb = 255;

        u16 out_pix;
        if (!is_tex) {
            u32 r5, g5, b5;
            if (do_dith) {
                s32 d = (s32)drow[x & 3];
                r5 = dither_chan8_to_5(cr, d);
                g5 = dither_chan8_to_5(cg, d);
                b5 = dither_chan8_to_5(cb, d);
            } else {
                r5 = (u32)cr >> 3;
                g5 = (u32)cg >> 3;
                b5 = (u32)cb >> 3;
            }
            u16 fg15 = (u16)(r5 | (g5 << 5) | (b5 << 10));
            if (is_semi) {
                out_pix = (u16)(gpu_blend_abr15(dst[i], fg15, sp->abr) | mask_or);
            } else {
                out_pix = (u16)(fg15 | mask_or);
            }
        } else {
            u32 tu = (((u32)(u_q16 >> 16) & 0xFFu) & sp->tw_and_x) | sp->tw_or_x;
            u32 tv = (((u32)(v_q16 >> 16) & 0xFFu) & sp->tw_and_y) | sp->tw_or_y;
            u16 texel = sample_vram_texel(sp->vram, sp->clut, sp->tx_base, sp->ty_base,
                                          sp->tex_depth, tu, tv);
            if (texel == 0x0000u) {
                r_q16 += dr; g_q16 += dg; b_q16 += db;
                u_q16 += du; v_q16 += dv;
                continue;
            }
            u16 stp = (u16)(texel & 0x8000u);
            u16 fg15;
            if (is_raw) {
                fg15 = (u16)(texel & 0x7FFFu);
            } else if (do_dith) {
                s32 d = (s32)drow[x & 3];
                s32 mr8 = ((s32)(texel & 0x1Fu) * cr) >> 4;
                s32 mg8 = ((s32)((texel >> 5) & 0x1Fu) * cg) >> 4;
                s32 mb8 = ((s32)((texel >> 10) & 0x1Fu) * cb) >> 4;
                u32 r5 = dither_chan8_to_5(mr8, d);
                u32 g5 = dither_chan8_to_5(mg8, d);
                u32 b5 = dither_chan8_to_5(mb8, d);
                fg15 = (u16)(r5 | (g5 << 5) | (b5 << 10));
            } else {
                u32 r5 = sat5_mod(texel & 0x1Fu, (u32)cr);
                u32 g5 = sat5_mod((texel >> 5) & 0x1Fu, (u32)cg);
                u32 b5 = sat5_mod((texel >> 10) & 0x1Fu, (u32)cb);
                fg15 = (u16)(r5 | (g5 << 5) | (b5 << 10));
            }

            if (is_semi && stp != 0u) {
                out_pix = (u16)(gpu_blend_abr15(dst[i], fg15, sp->abr) | stp | mask_or);
            } else {
                out_pix = (u16)(fg15 | stp | mask_or);
            }
        }

        dst[i] = out_pix;
        r_q16 += dr; g_q16 += dg; b_q16 += db;
        u_q16 += du; v_q16 += dv;
    }
}
