#include "gpu_internal.h"
#include "psx3ds/arena.h"

#include <string.h>

typedef struct GpuSoft {
    PsxSystem *sys;
    u16       *disp_buf; /* 640*480*3 bytes in arena for wrapped/blanked/24bpp display */
    GpuDrawEnv env;

    u16        clut_cache[256];
    u16        cached_clut_key; /* bits 14:0 of clut attr ((y<<6)|x) */
    u8         cached_clut_depth;
    bool       clut_valid;
} GpuSoft;

/* Reciprocal table (1 << 20) / d for d in 1..1023 — avoids software __aeabi_idiv on ARM11! */
static u32 s_recip_q20[1024];
static bool s_recip_ready = false;

static const s8 s_line_dither[4][4] = {
    { -4,  0, -3, +1 },
    { +2, -2, +3, -1 },
    { -3, +1, -4,  0 },
    { +3, -1, +2, -2 }
};

static void init_recip_table(void) {
    if (s_recip_ready) {
        return;
    }
    s_recip_q20[0] = 0u;
    for (u32 d = 1u; d < 1024u; d++) {
        s_recip_q20[d] = (1u << 20) / d;
    }
    s_recip_ready = true;
}

static inline s32 min3_s32(s32 a, s32 b, s32 c) {
    s32 m = (a < b) ? a : b;
    return (m < c) ? m : c;
}

static inline s32 max3_s32(s32 a, s32 b, s32 c) {
    s32 m = (a > b) ? a : b;
    return (m > c) ? m : c;
}

/* Exact floor(rhs / d) for d in 1..1023 using Q20 reciprocal + <=1-step correction
 * (called only during per-edge triangle setup, never per scanline) */
static inline s32 div_floor_pos(s32 rhs, s32 d) {
    s32 q = (s32)(((s64)rhs * (s64)s_recip_q20[d]) >> 20);
    while (d * q > rhs) {
        q--;
    }
    while (d * (q + 1) <= rhs) {
        q++;
    }
    return q;
}

/* Exact incremental floor(rhs(y) / D) walker for one sloped triangle edge */
typedef struct EdgeStepper {
    s32 q, r;   /* floor(rhs / D), rhs mod D (0 <= r < D) */
    s32 qs, rs; /* floor(dx / D),  dx mod D  (0 <= rs < D) */
    s32 D;      /* |dy| (1..511) */
} EdgeStepper;

/* Directed edge e_k connects vertex k -> (k + 1) % 3 */
static inline int edge_of_verts(int i, int j) {
    return (j == (i + 1) % 3) ? i : j;
}

static inline void edge_stepper_init(EdgeStepper *s, s32 dx, s32 dy, s32 px, s32 py, s32 y) {
    s32 bias = (dy < 0) ? 0 : -1; /* sloped edges: left (dy < 0) edges are top-left */
    s32 rhs  = dx * (y - py) + dy * px + bias;
    s32 D    = (dy >= 0) ? dy : -dy;
    s->D  = D;
    s->q  = div_floor_pos(rhs, D);
    s->r  = rhs - s->q * D;
    s->qs = div_floor_pos(dx, D);
    s->rs = dx - s->qs * D;
}

static inline void edge_stepper_step(EdgeStepper *s) {
    s32 r = s->r + s->rs;
    s->q += s->qs;
    if (r >= s->D) {
        s->q++;
        r -= s->D;
    }
    s->r = r;
}

static const u16 *ensure_clut(GpuSoft *gs, const u16 *vram, u16 clut_attr, u8 depth) {
    if (depth >= 2u) {
        return NULL;
    }
    u16 key = (u16)(clut_attr & 0x7FFFu);
    if (gs->clut_valid && gs->cached_clut_key == key && gs->cached_clut_depth == depth) {
        return gs->clut_cache;
    }
    u32 cx = ((u32)clut_attr & 0x3Fu) << 4;
    u32 cy = ((u32)clut_attr >> 6) & 0x1FFu;
    u32 entries = (depth == 0u) ? 16u : 256u;
    const u16 *row = vram + (cy * PSX_VRAM_W);

    if (cx + entries <= PSX_VRAM_W) {
        memcpy(gs->clut_cache, row + cx, entries * sizeof(u16));
    } else {
        for (u32 i = 0u; i < entries; i++) {
            gs->clut_cache[i] = row[(cx + i) & 0x3FFu];
        }
    }
    gs->cached_clut_key   = key;
    gs->cached_clut_depth = depth;
    gs->clut_valid        = true;
    return gs->clut_cache;
}

static inline void compute_scissor(const GpuDrawEnv *env,
                                   s32 *sc_x0, s32 *sc_y0, s32 *sc_x1, s32 *sc_y1) {
    s32 x0 = env->clip_x0;
    s32 y0 = env->clip_y0;
    s32 x1 = env->clip_x1;
    s32 y1 = env->clip_y1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (s32)(PSX_VRAM_W - 1u)) x1 = (s32)(PSX_VRAM_W - 1u);
    if (y1 > (s32)(PSX_VRAM_H - 1u)) y1 = (s32)(PSX_VRAM_H - 1u);
    *sc_x0 = x0;
    *sc_y0 = y0;
    *sc_x1 = x1;
    *sc_y1 = y1;
}

static inline void compute_tex_window(const GpuDrawEnv *env,
                                      u8 *and_x, u8 *or_x, u8 *and_y, u8 *or_y) {
    u8 mx = (u8)((env->tw_mask_x & 0x1Fu) << 3);
    u8 my = (u8)((env->tw_mask_y & 0x1Fu) << 3);
    *and_x = (u8)(~mx);
    *or_x  = (u8)(((env->tw_off_x & env->tw_mask_x) & 0x1Fu) << 3);
    *and_y = (u8)(~my);
    *or_y  = (u8)(((env->tw_off_y & env->tw_mask_y) & 0x1Fu) << 3);
}

static inline bool texpage_can_fast_span(u32 tx_base, u8 depth) {
    if (depth == 0u) return (tx_base + 64u)  <= PSX_VRAM_W;
    if (depth == 1u) return (tx_base + 128u) <= PSX_VRAM_W;
    return (tx_base + 256u) <= PSX_VRAM_W;
}

/* --------------------------------------------------------------------------
 * Half-Space Edge Triangle Rasterizer (Exact PSX Top-Left Fill Rule)
 * -------------------------------------------------------------------------- */
static void rasterize_triangle(GpuSoft *gs, const GpuPrim *p,
                               const GpuVertex *va, const GpuVertex *vb, const GpuVertex *vc,
                               const GpuDrawEnv *env) {
    u16 *vram = gs->sys->gpu.vram;
    if (!vram) {
        return;
    }

    s32 ax = va->x, ay = va->y;
    s32 bx = vb->x, by = vb->y;
    s32 cx = vc->x, cy = vc->y;

    s32 min_x = min3_s32(ax, bx, cx);
    s32 max_x = max3_s32(ax, bx, cx);
    s32 min_y = min3_s32(ay, by, cy);
    s32 max_y = max3_s32(ay, by, cy);

    /* PSX hardware drops triangles spanning > 1023 px wide or > 511 px tall */
    if ((max_x - min_x) > 1023 || (max_y - min_y) > 511) {
        return;
    }

    s32 area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (area == 0) {
        return;
    }
    if (area < 0) {
        const GpuVertex *tmp_v = vb; vb = vc; vc = tmp_v;
        bx = vb->x; by = vb->y;
        cx = vc->x; cy = vc->y;
        area = -area;
    }

    s32 sc_x0, sc_y0, sc_x1, sc_y1;
    compute_scissor(env, &sc_x0, &sc_y0, &sc_x1, &sc_y1);
    if (sc_x0 > sc_x1 || sc_y0 > sc_y1) {
        return;
    }

    s32 y_start = (min_y > sc_y0) ? min_y : sc_y0;
    s32 y_end   = (max_y < sc_y1) ? max_y : sc_y1;
    if (y_start > y_end || max_x < sc_x0 || min_x > sc_x1) {
        return;
    }

    /*
     * Directed edges e0=(A->B), e1=(B->C), e2=(C->A) with area > 0:
     * E_i(x, y) = dx_i * (y - py_i) - dy_i * (x - px_i)
     * Top-left fill rule in Y-down screen space:
     *   top  edge: dy_i == 0 && dx_i > 0
     *   left edge: dy_i < 0
     * Bias = 0 if top-left, -1 otherwise -> E_i(x, y) + bias_i >= 0
     *   <=> dy_i * x <= dx_i * (y - py_i) + dy_i * px_i + bias_i = rhs_i(y)
     *
     * Per-row bounds: dy_i > 0 -> x <= floor(rhs_i / dy_i)          (right bound)
     *                 dy_i < 0 -> x >= -floor(rhs_i / |dy_i|)       (left bound)
     *                 dy_i == 0 -> top edge: no constraint; bottom edge: excludes row y = max_y
     *
     * Two-active-edge scanline walk (exactly equivalent to the 3-half-space test): with vertices
     * sorted y0 <= y1 <= y2, rows [y0, y1) are bounded by the long edge (top->bot) and short edge
     * s1 (top->mid); rows [y1, y2] by the long edge and s2 (mid->bot). s1 and s2 lie on the same
     * side (same role, same bias), and by convexity the inactive short edge is never tighter, so
     * dropping it is exact (at row y1 both short edges pass through the middle vertex).
     *
     * rhs(y + 1) = rhs(y) + dx, so floor(rhs / |dy|) is walked with an exact Bresenham
     * quotient/remainder stepper set up once per edge: zero divisions/multiplies per scanline.
     */
    s32 edx[3] = { bx - ax, cx - bx, ax - cx };
    s32 edy[3] = { by - ay, cy - by, ay - cy };
    s32 epx[3] = { ax, bx, cx };
    s32 epy[3] = { ay, by, cy };

    int v_top = 0, v_mid = 1, v_bot = 2;
    {
        const s32 vy[3] = { ay, by, cy };
        int t;
        if (vy[v_mid] < vy[v_top]) { t = v_top; v_top = v_mid; v_mid = t; }
        if (vy[v_bot] < vy[v_mid]) { t = v_mid; v_mid = v_bot; v_bot = t; }
        if (vy[v_mid] < vy[v_top]) { t = v_top; v_top = v_mid; v_mid = t; }
    }
    s32 y1 = epy[v_mid];
    s32 y2 = epy[v_bot];
    /* Flat bottom: the horizontal bottom edge (dx < 0, bias -1) excludes row y2 */
    if (y1 == y2 && y_end >= y2) {
        y_end = y2 - 1;
        if (y_start > y_end) {
            return;
        }
    }
    int e_long = edge_of_verts(v_top, v_bot);
    int e_s1   = edge_of_verts(v_top, v_mid);
    int e_s2   = edge_of_verts(v_mid, v_bot);
    bool long_is_right = edy[e_long] > 0;

    EdgeStepper st_long, st_short;
    edge_stepper_init(&st_long, edx[e_long], edy[e_long], epx[e_long], epy[e_long], y_start);
    bool in_phase1 = (y_start < y1);
    if (in_phase1) {
        edge_stepper_init(&st_short, edx[e_s1], edy[e_s1], epx[e_s1], epy[e_s1], y_start);
    } else {
        edge_stepper_init(&st_short, edx[e_s2], edy[e_s2], epx[e_s2], epy[e_s2], y_start);
    }
    EdgeStepper *st_r = long_is_right ? &st_long : &st_short;
    EdgeStepper *st_l = long_is_right ? &st_short : &st_long;

    u8   flags  = p->flags;
    bool is_tex = (flags & PF_TEXTURED) != 0u;
    bool is_raw = (flags & PF_RAW_TEX) != 0u;
    bool is_gou = (flags & PF_GOURAUD) != 0u;
    bool is_sem = (flags & PF_SEMI) != 0u;
    bool do_dither = (env->dither != 0u) && (is_gou || (is_tex && !is_raw));
    bool need_gou_grad = is_gou && !(is_tex && is_raw);

    /* Exact floor(2^32 / area) via 32-bit unsigned division (no 64-bit __aeabi_ldivmod),
     * computed only when colour or UV gradients are actually interpolated. */
    s64 inv_area_q32 = 0;
    if (need_gou_grad || is_tex) {
        u32 a = (u32)area;
        if (a == 1u) {
            inv_area_q32 = (s64)1 << 32;
        } else {
            u32 q = 0xFFFFFFFFu / a;
            if ((0xFFFFFFFFu - q * a) == (a - 1u)) {
                q++;
            }
            inv_area_q32 = (s64)q;
        }
    }

    s32 dr_dx = 0, dr_dy = 0, dg_dx = 0, dg_dy = 0, db_dx = 0, db_dy = 0;
    s32 r0_base = ((s32)va->r << 16) + 0x8000;
    s32 g0_base = ((s32)va->g << 16) + 0x8000;
    s32 b0_base = ((s32)va->b << 16) + 0x8000;

    if (need_gou_grad) {
        s32 dcy = cy - ay, dby = by - ay;
        s32 dbx = bx - ax, dcx = cx - ax;

        s32 nr_x = ((s32)vb->r - (s32)va->r) * dcy - ((s32)vc->r - (s32)va->r) * dby;
        s32 nr_y = dbx * ((s32)vc->r - (s32)va->r) - dcx * ((s32)vb->r - (s32)va->r);
        s32 ng_x = ((s32)vb->g - (s32)va->g) * dcy - ((s32)vc->g - (s32)va->g) * dby;
        s32 ng_y = dbx * ((s32)vc->g - (s32)va->g) - dcx * ((s32)vb->g - (s32)va->g);
        s32 nb_x = ((s32)vb->b - (s32)va->b) * dcy - ((s32)vc->b - (s32)va->b) * dby;
        s32 nb_y = dbx * ((s32)vc->b - (s32)va->b) - dcx * ((s32)vb->b - (s32)va->b);

        dr_dx = (s32)(((s64)nr_x * inv_area_q32) >> 16);
        dr_dy = (s32)(((s64)nr_y * inv_area_q32) >> 16);
        dg_dx = (s32)(((s64)ng_x * inv_area_q32) >> 16);
        dg_dy = (s32)(((s64)ng_y * inv_area_q32) >> 16);
        db_dx = (s32)(((s64)nb_x * inv_area_q32) >> 16);
        db_dy = (s32)(((s64)nb_y * inv_area_q32) >> 16);
    }

    s32 du_dx = 0, du_dy = 0, dv_dx = 0, dv_dy = 0;
    s32 u0_base = 0, v0_base = 0;
    u16 tx_base = 0u, ty_base = 0u;
    u8  depth   = p->tex_depth & 3u;
    const u16 *clut = NULL;
    const u16 *tex_base_ptr = NULL;
    u8 tw_and_x = 0xFFu, tw_or_x = 0u, tw_and_y = 0xFFu, tw_or_y = 0u;
    bool no_tw = true;

    if (is_tex) {
        s32 dcy = cy - ay, dby = by - ay;
        s32 dbx = bx - ax, dcx = cx - ax;
        s32 nu_x = ((s32)vb->u - (s32)va->u) * dcy - ((s32)vc->u - (s32)va->u) * dby;
        s32 nu_y = dbx * ((s32)vc->u - (s32)va->u) - dcx * ((s32)vb->u - (s32)va->u);
        s32 nv_x = ((s32)vb->v - (s32)va->v) * dcy - ((s32)vc->v - (s32)va->v) * dby;
        s32 nv_y = dbx * ((s32)vc->v - (s32)va->v) - dcx * ((s32)vb->v - (s32)va->v);

        du_dx = (s32)(((s64)nu_x * inv_area_q32) >> 16);
        du_dy = (s32)(((s64)nu_y * inv_area_q32) >> 16);
        dv_dx = (s32)(((s64)nv_x * inv_area_q32) >> 16);
        dv_dy = (s32)(((s64)nv_y * inv_area_q32) >> 16);
        u0_base = ((s32)va->u << 16) + 0x8000;
        v0_base = ((s32)va->v << 16) + 0x8000;

        tx_base = (u16)((p->texpage & 0xFu) << 6);
        ty_base = (u16)(((p->texpage >> 4) & 1u) << 8);
        tex_base_ptr = vram + ((u32)ty_base * PSX_VRAM_W) + (u32)tx_base;
        clut = ensure_clut(gs, vram, p->clut, depth);
        compute_tex_window(env, &tw_and_x, &tw_or_x, &tw_and_y, &tw_or_y);
        no_tw = ((env->tw_mask_x & 0x1Fu) == 0u) && ((env->tw_mask_y & 0x1Fu) == 0u);
    }

    u16 mask_or = env->set_mask ? 0x8000u : 0u;
    u16 flat_col15 = (u16)(((u32)(va->r >> 3)) |
                           (((u32)(va->g >> 3)) << 5) |
                           (((u32)(va->b >> 3)) << 10));
    u32 flat_mod = is_raw
        ? 0x00808080u
        : ((u32)va->r | ((u32)va->g << 8) | ((u32)va->b << 16));
    u32 duv_pack = (((u32)(dv_dx >> 8) & 0xFFFFu) << 16) | ((u32)(du_dx >> 8) & 0xFFFFu);
    bool can_fast_tex = is_tex && !is_gou && !do_dither && !env->check_mask &&
                        no_tw && texpage_can_fast_span(tx_base, depth);

    GpuSpanParams sp;
    memset(&sp, 0, sizeof(sp));
    sp.vram       = vram;
    sp.clut       = clut;
    sp.dr_q16     = dr_dx;
    sp.dg_q16     = dg_dx;
    sp.db_q16     = db_dx;
    sp.du_q16     = du_dx;
    sp.dv_q16     = dv_dx;
    sp.tx_base    = tx_base;
    sp.ty_base    = ty_base;
    sp.tex_depth  = depth;
    sp.abr        = p->abr & 3u;
    sp.flags      = flags;
    sp.dither     = do_dither ? 1u : 0u;
    sp.check_mask = env->check_mask;
    sp.set_mask   = env->set_mask;
    sp.tw_and_x   = tw_and_x;
    sp.tw_or_x    = tw_or_x;
    sp.tw_and_y   = tw_and_y;
    sp.tw_or_y    = tw_or_y;

    for (s32 y = y_start; y <= y_end; y++) {
        if (in_phase1 && y == y1) {
            edge_stepper_init(&st_short, edx[e_s2], edy[e_s2], epx[e_s2], epy[e_s2], y);
            in_phase1 = false;
        }
        s32 xl = -st_l->q;
        s32 xr = st_r->q;
        if (xl < sc_x0) xl = sc_x0;
        if (xr > sc_x1) xr = sc_x1;
        edge_stepper_step(&st_long);
        edge_stepper_step(&st_short);

        if (xl > xr) {
            continue;
        }

        u32 count = (u32)(xr - xl + 1);
        u16 *row_ptr = vram + ((u32)y * PSX_VRAM_W);
        u16 *dst = row_ptr + xl;

        if (!is_tex && !env->check_mask) {
            if (!is_gou) {
                if (!is_sem) {
                    gpu_soft_span_fill16(dst, (u16)(flat_col15 | mask_or), count);
                } else {
                    gpu_soft_span_flat_abr(dst, count, flat_col15, p->abr, mask_or);
                }
                continue;
            } else if (!do_dither || !is_sem) {
                s32 dx_a = xl - ax;
                s32 dy_a = y - ay;
                s32 rq = r0_base + dr_dx * dx_a + dr_dy * dy_a;
                s32 gq = g0_base + dg_dx * dx_a + dg_dy * dy_a;
                s32 bq = b0_base + db_dx * dx_a + db_dy * dy_a;
                if (do_dither) {
                    gpu_soft_span_gouraud_dither_opaque(dst, count, rq, gq, bq,
                                                        dr_dx, dg_dx, db_dx,
                                                        g_gpu_dither_pack[y & 3][xl & 3],
                                                        mask_or);
                } else if (!is_sem) {
                    gpu_soft_span_gouraud_opaque(dst, count, rq, gq, bq,
                                                 dr_dx, dg_dx, db_dx, mask_or);
                } else {
                    gpu_soft_span_gouraud_abr(dst, count, rq, gq, bq,
                                              dr_dx, dg_dx, db_dx, p->abr, mask_or);
                }
                continue;
            }
        }

        s32 dx_a = xl - ax;
        s32 dy_a = y - ay;
        s32 uq = u0_base + du_dx * dx_a + du_dy * dy_a;
        s32 vq = v0_base + dv_dx * dx_a + dv_dy * dy_a;

        if (can_fast_tex && count <= 256u) {
            u32 uv_pack = (((u32)(vq >> 8) & 0xFFFFu) << 16) | ((u32)(uq >> 8) & 0xFFFFu);
            if (!is_sem) {
                if (is_raw) {
                    if (depth == 0u) {
                        gpu_soft_span_tex4_raw_opaque(dst, count, tex_base_ptr, clut, uv_pack, duv_pack, mask_or);
                    } else if (depth == 1u) {
                        gpu_soft_span_tex8_raw_opaque(dst, count, tex_base_ptr, clut, uv_pack, duv_pack, mask_or);
                    } else {
                        gpu_soft_span_tex16_raw_opaque(dst, count, tex_base_ptr, uv_pack, duv_pack, mask_or);
                    }
                } else {
                    if (depth == 0u) {
                        gpu_soft_span_tex4_mod_flat_opaque(dst, count, tex_base_ptr, clut, uv_pack, duv_pack, flat_mod, mask_or);
                    } else if (depth == 1u) {
                        gpu_soft_span_tex8_mod_flat_opaque(dst, count, tex_base_ptr, clut, uv_pack, duv_pack, flat_mod, mask_or);
                    } else {
                        gpu_soft_span_tex16_mod_flat_opaque(dst, count, tex_base_ptr, uv_pack, duv_pack, flat_mod, mask_or);
                    }
                }
            } else {
                if (depth == 0u) {
                    gpu_soft_span_tex4_mod_flat_abr(dst, count, tex_base_ptr, clut, uv_pack, duv_pack, flat_mod, p->abr, mask_or);
                } else if (depth == 1u) {
                    gpu_soft_span_tex8_mod_flat_abr(dst, count, tex_base_ptr, clut, uv_pack, duv_pack, flat_mod, p->abr, mask_or);
                } else {
                    gpu_soft_span_tex16_mod_flat_abr(dst, count, tex_base_ptr, uv_pack, duv_pack, flat_mod, p->abr, mask_or);
                }
            }
            continue;
        }

        sp.dst_row = row_ptr;
        sp.x_start = xl;
        sp.y       = y;
        sp.count   = count;
        sp.r_q16   = r0_base + dr_dx * dx_a + dr_dy * dy_a;
        sp.g_q16   = g0_base + dg_dx * dx_a + dg_dy * dy_a;
        sp.b_q16   = b0_base + db_dx * dx_a + db_dy * dy_a;
        sp.u_q16   = uq;
        sp.v_q16   = vq;
        gpu_soft_span_general(&sp);
    }
}

/* --------------------------------------------------------------------------
 * Fast Aligned Rectangle / Sprite Blitter (GP0 0x60..0x7F)
 * -------------------------------------------------------------------------- */
static void rasterize_rect(GpuSoft *gs, const GpuPrim *p, const GpuDrawEnv *env) {
    u16 *vram = gs->sys->gpu.vram;
    if (!vram || p->w == 0u || p->h == 0u) {
        return;
    }

    s32 sc_x0, sc_y0, sc_x1, sc_y1;
    compute_scissor(env, &sc_x0, &sc_y0, &sc_x1, &sc_y1);
    if (sc_x0 > sc_x1 || sc_y0 > sc_y1) {
        return;
    }

    s32 x0 = p->v[0].x;
    s32 y0 = p->v[0].y;
    s32 x1 = x0 + (s32)p->w - 1;
    s32 y1 = y0 + (s32)p->h - 1;

    s32 cx0 = (x0 > sc_x0) ? x0 : sc_x0;
    s32 cy0 = (y0 > sc_y0) ? y0 : sc_y0;
    s32 cx1 = (x1 < sc_x1) ? x1 : sc_x1;
    s32 cy1 = (y1 < sc_y1) ? y1 : sc_y1;
    if (cx0 > cx1 || cy0 > cy1) {
        return;
    }

    u32 count   = (u32)(cx1 - cx0 + 1);
    u8  flags   = p->flags;
    bool is_tex = (flags & PF_TEXTURED) != 0u;
    bool is_raw = (flags & PF_RAW_TEX) != 0u;
    bool is_sem = (flags & PF_SEMI) != 0u;
    u16 mask_or = env->set_mask ? 0x8000u : 0u;

    /* Untextured Rectangles (never dithered on PSX hardware) */
    if (!is_tex) {
        u16 col15 = (u16)(((u32)(p->v[0].r >> 3)) |
                          (((u32)(p->v[0].g >> 3)) << 5) |
                          (((u32)(p->v[0].b >> 3)) << 10));
        if (!env->check_mask) {
            for (s32 y = cy0; y <= cy1; y++) {
                u16 *dst = vram + ((u32)y * PSX_VRAM_W) + cx0;
                if (!is_sem) {
                    gpu_soft_span_fill16(dst, (u16)(col15 | mask_or), count);
                } else {
                    gpu_soft_span_flat_abr(dst, count, col15, p->abr, mask_or);
                }
            }
        } else {
            GpuSpanParams sp;
            memset(&sp, 0, sizeof(sp));
            sp.vram       = vram;
            sp.x_start    = cx0;
            sp.count      = count;
            sp.r_q16      = (s32)p->v[0].r << 16;
            sp.g_q16      = (s32)p->v[0].g << 16;
            sp.b_q16      = (s32)p->v[0].b << 16;
            sp.abr        = p->abr & 3u;
            sp.flags      = flags;
            sp.check_mask = env->check_mask;
            sp.set_mask   = env->set_mask;
            for (s32 y = cy0; y <= cy1; y++) {
                sp.dst_row = vram + ((u32)y * PSX_VRAM_W);
                sp.y       = y;
                gpu_soft_span_general(&sp);
            }
        }
        return;
    }

    /* Textured Sprites / Rectangles */
    s32 skip_x = cx0 - x0;
    s32 skip_y = cy0 - y0;
    s32 step_u = (flags & PF_RECT_FLIP_X) ? -1 : 1;
    s32 step_v = (flags & PF_RECT_FLIP_Y) ? -1 : 1;
    u8  u_start = (u8)((s32)p->v[0].u + skip_x * step_u);
    u8  v_cur   = (u8)((s32)p->v[0].v + skip_y * step_v);

    u16 tx_base = (u16)((p->texpage & 0xFu) << 6);
    u16 ty_base = (u16)(((p->texpage >> 4) & 1u) << 8);
    u8  depth   = p->tex_depth & 3u;
    const u16 *clut = ensure_clut(gs, vram, p->clut, depth);
    const u16 *tex_base_ptr = vram + ((u32)ty_base * PSX_VRAM_W) + (u32)tx_base;

    u8 tw_and_x, tw_or_x, tw_and_y, tw_or_y;
    compute_tex_window(env, &tw_and_x, &tw_or_x, &tw_and_y, &tw_or_y);
    bool no_tw = ((env->tw_mask_x & 0x1Fu) == 0u) && ((env->tw_mask_y & 0x1Fu) == 0u);

    if (!env->check_mask && no_tw && texpage_can_fast_span(tx_base, depth)) {
        u32 duv_pack = (step_u > 0) ? 0x00000100u : 0x0000FF00u;
        u32 flat_mod = is_raw
            ? 0x00808080u
            : ((u32)p->v[0].r | ((u32)p->v[0].g << 8) | ((u32)p->v[0].b << 16));

        for (s32 y = cy0; y <= cy1; y++) {
            u16 *dst = vram + ((u32)y * PSX_VRAM_W) + cx0;
            if (!is_sem && step_u > 0) {
                /* Unflipped opaque sprite row: 4bpp = 4 texels/load, 8bpp = 2 texels/load */
                gpu_soft_sprite_row_c(dst, count, tex_base_ptr + ((u32)v_cur << 10), clut,
                                      u_start, depth, is_raw, flat_mod, mask_or);
                v_cur = (u8)((s32)v_cur + step_v);
                continue;
            }
            u32 uv_pack = ((u32)v_cur << 24) | ((u32)u_start << 8);
            if (!is_sem) {
                if (is_raw) {
                    if (depth == 0u) {
                        gpu_soft_span_tex4_raw_opaque(dst, count, tex_base_ptr, clut, uv_pack, duv_pack, mask_or);
                    } else if (depth == 1u) {
                        gpu_soft_span_tex8_raw_opaque(dst, count, tex_base_ptr, clut, uv_pack, duv_pack, mask_or);
                    } else {
                        gpu_soft_span_tex16_raw_opaque(dst, count, tex_base_ptr, uv_pack, duv_pack, mask_or);
                    }
                } else {
                    if (depth == 0u) {
                        gpu_soft_span_tex4_mod_flat_opaque(dst, count, tex_base_ptr, clut, uv_pack, duv_pack, flat_mod, mask_or);
                    } else if (depth == 1u) {
                        gpu_soft_span_tex8_mod_flat_opaque(dst, count, tex_base_ptr, clut, uv_pack, duv_pack, flat_mod, mask_or);
                    } else {
                        gpu_soft_span_tex16_mod_flat_opaque(dst, count, tex_base_ptr, uv_pack, duv_pack, flat_mod, mask_or);
                    }
                }
            } else {
                if (depth == 0u) {
                    gpu_soft_span_tex4_mod_flat_abr(dst, count, tex_base_ptr, clut, uv_pack, duv_pack, flat_mod, p->abr, mask_or);
                } else if (depth == 1u) {
                    gpu_soft_span_tex8_mod_flat_abr(dst, count, tex_base_ptr, clut, uv_pack, duv_pack, flat_mod, p->abr, mask_or);
                } else {
                    gpu_soft_span_tex16_mod_flat_abr(dst, count, tex_base_ptr, uv_pack, duv_pack, flat_mod, p->abr, mask_or);
                }
            }
            v_cur = (u8)((s32)v_cur + step_v);
        }
        return;
    }

    GpuSpanParams sp;
    memset(&sp, 0, sizeof(sp));
    sp.vram       = vram;
    sp.clut       = clut;
    sp.x_start    = cx0;
    sp.count      = count;
    sp.r_q16      = (s32)p->v[0].r << 16;
    sp.g_q16      = (s32)p->v[0].g << 16;
    sp.b_q16      = (s32)p->v[0].b << 16;
    sp.u_q16      = (s32)u_start << 16;
    sp.du_q16     = step_u << 16;
    sp.tx_base    = tx_base;
    sp.ty_base    = ty_base;
    sp.tex_depth  = depth;
    sp.abr        = p->abr & 3u;
    sp.flags      = flags;
    sp.dither     = 0u; /* Rectangles never dither */
    sp.check_mask = env->check_mask;
    sp.set_mask   = env->set_mask;
    sp.tw_and_x   = tw_and_x;
    sp.tw_or_x    = tw_or_x;
    sp.tw_and_y   = tw_and_y;
    sp.tw_or_y    = tw_or_y;

    for (s32 y = cy0; y <= cy1; y++) {
        sp.dst_row = vram + ((u32)y * PSX_VRAM_W);
        sp.y       = y;
        sp.v_q16   = (s32)v_cur << 16;
        gpu_soft_span_general(&sp);
        v_cur = (u8)((s32)v_cur + step_v);
    }
}

/* --------------------------------------------------------------------------
 * PSX Shaded & Dithered DDA Line Rasterizer (GP0 0x40..0x5F)
 * -------------------------------------------------------------------------- */
static inline u32 clamp_dither_line(s32 c8, s32 d) {
    s32 s = (c8 + d) >> 3;
    if (s < 0) return 0u;
    if (s > 31) return 31u;
    return (u32)s;
}

static void rasterize_line(GpuSoft *gs, const GpuPrim *p, const GpuDrawEnv *env) {
    u16 *vram = gs->sys->gpu.vram;
    if (!vram) {
        return;
    }

    s32 x0 = p->v[0].x, y0 = p->v[0].y;
    s32 x1 = p->v[1].x, y1 = p->v[1].y;
    s32 dx = x1 - x0;
    s32 dy = y1 - y0;
    s32 adx = (dx >= 0) ? dx : -dx;
    s32 ady = (dy >= 0) ? dy : -dy;

    if (adx > 1023 || ady > 511) {
        return;
    }

    s32 sc_x0, sc_y0, sc_x1, sc_y1;
    compute_scissor(env, &sc_x0, &sc_y0, &sc_x1, &sc_y1);
    if (sc_x0 > sc_x1 || sc_y0 > sc_y1) {
        return;
    }

    s32 steps = (adx >= ady) ? adx : ady;
    bool is_gou = (p->flags & PF_GOURAUD) != 0u;
    bool is_sem = (p->flags & PF_SEMI) != 0u;
    bool do_dither = (env->dither != 0u);
    bool chk_mask  = (env->check_mask != 0u);
    u16  mask_or   = env->set_mask ? 0x8000u : 0u;

    s32 x_fx = (x0 << 16) + ((dx < 0) ? 0x7FFF : 0x8000);
    s32 y_fx = (y0 << 16) + ((dy < 0) ? 0x7FFF : 0x8000);
    s32 r_fx = ((s32)p->v[0].r << 16) + 0x8000;
    s32 g_fx = ((s32)p->v[0].g << 16) + 0x8000;
    s32 b_fx = ((s32)p->v[0].b << 16) + 0x8000;

    s32 sx_fx = 0, sy_fx = 0, sr_fx = 0, sg_fx = 0, sb_fx = 0;
    if (steps > 0) {
        sx_fx = (dx >= 0) ? ((adx << 16) / steps) : -((adx << 16) / steps);
        sy_fx = (dy >= 0) ? ((ady << 16) / steps) : -((ady << 16) / steps);
        if (is_gou) {
            sr_fx = (((s32)p->v[1].r - (s32)p->v[0].r) << 16) / steps;
            sg_fx = (((s32)p->v[1].g - (s32)p->v[0].g) << 16) / steps;
            sb_fx = (((s32)p->v[1].b - (s32)p->v[0].b) << 16) / steps;
        }
    }

    for (s32 i = 0; i <= steps; i++) {
        s32 px = x_fx >> 16;
        s32 py = y_fx >> 16;

        if (px >= sc_x0 && px <= sc_x1 && py >= sc_y0 && py <= sc_y1) {
            u16 *dp = &vram[((u32)py * PSX_VRAM_W) + (u32)px];
            if (!chk_mask || (*dp & 0x8000u) == 0u) {
                s32 cr = r_fx >> 16;
                s32 cg = g_fx >> 16;
                s32 cb = b_fx >> 16;
                if (cr < 0) cr = 0; else if (cr > 255) cr = 255;
                if (cg < 0) cg = 0; else if (cg > 255) cg = 255;
                if (cb < 0) cb = 0; else if (cb > 255) cb = 255;

                u32 r5, g5, b5;
                if (do_dither) {
                    s32 d = (s32)s_line_dither[py & 3][px & 3];
                    r5 = clamp_dither_line(cr, d);
                    g5 = clamp_dither_line(cg, d);
                    b5 = clamp_dither_line(cb, d);
                } else {
                    r5 = (u32)cr >> 3;
                    g5 = (u32)cg >> 3;
                    b5 = (u32)cb >> 3;
                }
                u16 fg15 = (u16)(r5 | (g5 << 5) | (b5 << 10));
                if (is_sem) {
                    *dp = (u16)(gpu_blend_abr15(*dp, fg15, p->abr) | mask_or);
                } else {
                    *dp = (u16)(fg15 | mask_or);
                }
            }
        }

        x_fx += sx_fx;
        y_fx += sy_fx;
        r_fx += sr_fx;
        g_fx += sg_fx;
        b_fx += sb_fx;
    }
}

/* --------------------------------------------------------------------------
 * GpuBackend vtable implementation
 * -------------------------------------------------------------------------- */
static void soft_reset(void *self) {
    GpuSoft *gs = (GpuSoft *)self;
    if (!gs) {
        return;
    }
    memset(&gs->env, 0, sizeof(gs->env));
    gs->clut_valid = false;
}

static void soft_set_draw_env(void *self, const GpuDrawEnv *env) {
    GpuSoft *gs = (GpuSoft *)self;
    if (!gs || !env) {
        return;
    }
    gs->env = *env;
}

static void soft_draw(void *self, const GpuPrim *p, const GpuDrawEnv *env) {
    GpuSoft *gs = (GpuSoft *)self;
    if (!gs || !p) {
        return;
    }
    const GpuDrawEnv *active_env = env ? env : &gs->env;
    switch ((GpuPrimKind)p->kind) {
        case PRIM_TRI:
            rasterize_triangle(gs, p, &p->v[0], &p->v[1], &p->v[2], active_env);
            break;
        case PRIM_QUAD:
            rasterize_triangle(gs, p, &p->v[0], &p->v[1], &p->v[2], active_env);
            rasterize_triangle(gs, p, &p->v[1], &p->v[2], &p->v[3], active_env);
            break;
        case PRIM_LINE:
        case PRIM_POLYLINE_SEG:
            rasterize_line(gs, p, active_env);
            break;
        case PRIM_RECT:
            rasterize_rect(gs, p, active_env);
            break;
        default:
            break;
    }
}

static void soft_fill_rect(void *self, u16 x, u16 y, u16 w, u16 h, u32 bgr24) {
    GpuSoft *gs = (GpuSoft *)self;
    if (!gs) {
        return;
    }
    /* Note: gpu.c updates shadow VRAM before invoking be->fill_rect; if called
     * standalone in a backend unit test, ensure shadow VRAM is still updated. */
    (void)x; (void)y; (void)w; (void)h; (void)bgr24;
    gs->clut_valid = false;
}

static void soft_vram_written(void *self, u16 x, u16 y, u16 w, u16 h) {
    GpuSoft *gs = (GpuSoft *)self;
    (void)x; (void)y; (void)w; (void)h;
    if (gs) {
        gs->clut_valid = false;
    }
}

static void soft_vram_copy(void *self, u16 sx, u16 sy, u16 dx, u16 dy, u16 w, u16 h) {
    GpuSoft *gs = (GpuSoft *)self;
    (void)sx; (void)sy; (void)dx; (void)dy; (void)w; (void)h;
    if (gs) {
        gs->clut_valid = false;
    }
}

static void soft_sync_to_shadow(void *self, u16 x, u16 y, u16 w, u16 h) {
    (void)self; (void)x; (void)y; (void)w; (void)h;
    /* Shadow VRAM is already authoritative for gpu_soft */
}

static void soft_present(void *self, const GpuDisplay *d) {
    GpuSoft *gs = (GpuSoft *)self;
    if (!gs || !gs->sys || !gs->sys->gpu.vram || !d) {
        return;
    }
    gpu_vram_present(gs->sys->gpu.vram, gs->disp_buf, d);
}

static void soft_destroy(void *self) {
    (void)self; /* Allocated from PsxArena; freed wholesale by arena_destroy() */
}

static const GpuBackend s_gpu_soft_backend = {
    .name           = "gpu_soft",
    .reset          = soft_reset,
    .set_draw_env   = soft_set_draw_env,
    .draw           = soft_draw,
    .fill_rect      = soft_fill_rect,
    .vram_written   = soft_vram_written,
    .vram_copy      = soft_vram_copy,
    .sync_to_shadow = soft_sync_to_shadow,
    .present        = soft_present,
    .destroy        = soft_destroy,
};

bool gpu_soft_is_backend(const GpuBackend *be) {
    return be == &s_gpu_soft_backend;
}

void gpu_soft_invalidate_clut(void *self) {
    GpuSoft *gs = (GpuSoft *)self;
    if (gs) {
        gs->clut_valid = false;
    }
}

PsxResult gpu_soft_backend_create(PsxSystem *sys, const GpuBackend **be, void **self) {
    if (!sys || !be || !self) {
        return PSX_ERR_BAD_ARG;
    }
    gpu_soft_span_init_tables();
    init_recip_table();

    GpuSoft *gs = (GpuSoft *)arena_alloc(&sys->arena, (u32)sizeof(GpuSoft), 32u);
    /* 640 * 480 * 3 = 921,600 bytes (supports max 640x480 24bpp & 15bpp extraction) */
    gs->disp_buf = (u16 *)arena_alloc(&sys->arena, 640u * 480u * 3u, 32u);
    gs->sys      = sys;
    gs->clut_valid = false;

    *be   = &s_gpu_soft_backend;
    *self = gs;
    return PSX_OK;
}
