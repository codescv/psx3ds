#include "gpu_internal.h"
#include "psx3ds/arena.h"
#include "psx3ds/bus.h"
#include "psx3ds/gpu_hw.h"
#include "psx3ds/irq.h"
#include "psx3ds/scheduler.h"
#include "psx3ds/state.h"
#include "psx3ds/timers.h"

#include <string.h>

/* GP0 packet word length table (including command word; 0 = variable/special) */
static u8 s_gp0_len[256];
static bool s_gp0_len_ready = false;

/* Chained timer HBlank/VBlank handlers (timers_init runs before gpu_init) */
static PsxEventFn s_prev_hblank = NULL;
static PsxEventFn s_prev_vblank = NULL;

static void init_gp0_len_table(void) {
    if (s_gp0_len_ready) {
        return;
    }
    for (u32 op = 0u; op < 256u; op++) {
        s_gp0_len[op] = 1u; /* default 1-word NOP / env command */
    }
    s_gp0_len[0x02] = 3u; /* FillRect */

    /* Polygons 0x20..0x3F */
    for (u32 op = 0x20u; op <= 0x3Fu; op++) {
        u32 is_quad = (op >> 3) & 1u;
        u32 is_tex  = (op >> 2) & 1u;
        u32 is_gou  = (op >> 4) & 1u;
        u32 nverts  = is_quad ? 4u : 3u;
        s_gp0_len[op] = (u8)(1u + nverts * (1u + is_tex + is_gou) - is_gou);
    }

    /* Lines & Polylines 0x40..0x5F */
    for (u32 op = 0x40u; op <= 0x5Fu; op++) {
        u32 is_gou  = (op >> 4) & 1u;
        /* Initial minimum words before polyline enters streaming/terminator state */
        s_gp0_len[op] = (u8)(is_gou ? 4u : 3u);
    }

    /* Rectangles / Sprites 0x60..0x7F */
    for (u32 op = 0x60u; op <= 0x7Fu; op++) {
        u32 size_code = (op >> 3) & 3u;
        u32 is_tex    = (op >> 2) & 1u;
        s_gp0_len[op] = (u8)(2u + is_tex + (size_code == 0u ? 1u : 0u));
    }

    /* VRAM -> VRAM copy 0x80..0x9F */
    for (u32 op = 0x80u; op <= 0x9Fu; op++) {
        s_gp0_len[op] = 4u;
    }

    /* CPU -> VRAM load 0xA0..0xBF & VRAM -> CPU store 0xC0..0xDF (3-word header) */
    for (u32 op = 0xA0u; op <= 0xDFu; op++) {
        s_gp0_len[op] = 3u;
    }

    s_gp0_len_ready = true;
}

static inline s16 gpu_sext11(u32 v) {
    return (s16)(((s32)(v << 21)) >> 21);
}

static inline void unpack_rgb24(u32 word, GpuVertex *v) {
    v->r = (u8)(word & 0xFFu);
    v->g = (u8)((word >> 8) & 0xFFu);
    v->b = (u8)((word >> 16) & 0xFFu);
}

static inline void unpack_xy_offset(u32 word, const GpuDrawEnv *env, GpuVertex *v) {
    v->x = (s16)(gpu_sext11(word & 0xFFFFu) + env->off_x);
    v->y = (s16)(gpu_sext11(word >> 16) + env->off_y);
}

static void notify_vram_written_wrapped(PsxGpu *g, u16 x, u16 y, u16 w, u16 h) {
    if (!g->be || !g->be->vram_written || w == 0u || h == 0u) {
        return;
    }
    u32 x0 = (u32)x & 0x3FFu;
    u32 y0 = (u32)y & 0x1FFu;
    u32 cw = (w > PSX_VRAM_W) ? PSX_VRAM_W : (u32)w;
    u32 ch = (h > PSX_VRAM_H) ? PSX_VRAM_H : (u32)h;

    u32 w1 = (x0 + cw <= PSX_VRAM_W) ? cw : (PSX_VRAM_W - x0);
    u32 w2 = cw - w1;
    u32 h1 = (y0 + ch <= PSX_VRAM_H) ? ch : (PSX_VRAM_H - y0);
    u32 h2 = ch - h1;

    g->be->vram_written(g->be_self, (u16)x0, (u16)y0, (u16)w1, (u16)h1);
    if (w2 > 0u) {
        g->be->vram_written(g->be_self, 0u, (u16)y0, (u16)w2, (u16)h1);
    }
    if (h2 > 0u) {
        g->be->vram_written(g->be_self, (u16)x0, 0u, (u16)w1, (u16)h2);
        if (w2 > 0u) {
            g->be->vram_written(g->be_self, 0u, 0u, (u16)w2, (u16)h2);
        }
    }
}

static void sync_shadow_wrapped(PsxGpu *g, u16 x, u16 y, u16 w, u16 h) {
    if (!g->be || !g->be->sync_to_shadow || w == 0u || h == 0u) {
        return;
    }
    u32 x0 = (u32)x & 0x3FFu;
    u32 y0 = (u32)y & 0x1FFu;
    u32 cw = (w > PSX_VRAM_W) ? PSX_VRAM_W : (u32)w;
    u32 ch = (h > PSX_VRAM_H) ? PSX_VRAM_H : (u32)h;

    u32 w1 = (x0 + cw <= PSX_VRAM_W) ? cw : (PSX_VRAM_W - x0);
    u32 w2 = cw - w1;
    u32 h1 = (y0 + ch <= PSX_VRAM_H) ? ch : (PSX_VRAM_H - y0);
    u32 h2 = ch - h1;

    g->be->sync_to_shadow(g->be_self, (u16)x0, (u16)y0, (u16)w1, (u16)h1);
    if (w2 > 0u) {
        g->be->sync_to_shadow(g->be_self, 0u, (u16)y0, (u16)w2, (u16)h1);
    }
    if (h2 > 0u) {
        g->be->sync_to_shadow(g->be_self, (u16)x0, 0u, (u16)w1, (u16)h2);
        if (w2 > 0u) {
            g->be->sync_to_shadow(g->be_self, 0u, 0u, (u16)w2, (u16)h2);
        }
    }
}

/* --------------------------------------------------------------------------
 * Scheduler Hooks (EV_HBLANK, EV_VBLANK, EV_GPU) & Live GPUSTAT
 * -------------------------------------------------------------------------- */
static void gpu_refresh_scanline_bits(PsxSystem *sys) {
    PsxGpu *g = &sys->gpu;
    u32 scanline = sys->timers.scanline;
    bool in_vb   = sys->timers.in_vblank;

    u32 bits = g->gpustat & ~((1u << 13) | (1u << 31));
    if (!g->disp.interlace) {
        bits |= (1u << 13);
        if (!in_vb && ((scanline & 1u) != 0u)) {
            bits |= (1u << 31);
        }
    } else {
        u32 odd_field = g->frame & 1u;
        if (odd_field) {
            bits |= (1u << 13);
        }
        if (!in_vb) {
            if (g->disp.vres >= 480u) {
                if (odd_field) {
                    bits |= (1u << 31);
                }
            } else if ((scanline & 1u) != 0u) {
                bits |= (1u << 31);
            }
        }
    }
    g->gpustat = bits;
}

/*
 * VBlank entry (scanline == vblank_line). This is the ONLY place sys->gpu.frame advances
 * when the GPU owns EV_VBLANK (timers_hblank_cb skips its standalone increment), so the
 * interlace field parity (frame & 1) alternates exactly once per frame. The new field is
 * latched into GPUSTAT bits 13/31 BEFORE IRQ_VBLANK is raised, so a VBlank ISR (or Psy-Q
 * VSync(0), which spins on GPUSTAT bit 31 toggling in 480i) observes the new field.
 */
static void gpu_on_vblank(PsxSystem *sys, u32 late_cycles) {
    PsxGpu *g = &sys->gpu;
    g->frame++;
    gpu_refresh_scanline_bits(sys);

    if (s_prev_vblank) {
        s_prev_vblank(sys, late_cycles);
    } else {
        irq_raise(sys, IRQ_VBLANK);
    }

    if (g->be && g->be->present) {
        g->be->present(g->be_self, &g->disp);
    }
    sys->frame_done = true;
}

static void gpu_on_hblank(PsxSystem *sys, u32 late_cycles) {
    if (s_prev_hblank) {
        s_prev_hblank(sys, late_cycles);
    } else {
        u32 total_lines = sys->gpu.disp.pal ? PSX_LINES_PAL : PSX_LINES_NTSC;
        u32 vblank_line = sys->gpu.disp.pal ? PSX_VBLANK_LINE_PAL : PSX_VBLANK_LINE_NTSC;
        sys->timers.scanline = (sys->timers.scanline + 1u) % total_lines;
        bool now_vb = (sys->timers.scanline >= vblank_line);
        if (now_vb && !sys->timers.in_vblank) {
            sys->timers.in_vblank = true;
            gpu_on_vblank(sys, late_cycles);
        } else if (!now_vb) {
            sys->timers.in_vblank = false;
        }
        u32 cpl = sys->gpu.disp.pal ? PSX_CYCLES_PER_LINE_PAL : PSX_CYCLES_PER_LINE_NTSC;
        u32 delta = (cpl > late_cycles) ? (cpl - late_cycles) : 1u;
        sched_schedule(sys, EV_HBLANK, delta);
    }
    gpu_refresh_scanline_bits(sys);
}

static void gpu_on_cmd_done(PsxSystem *sys, u32 late_cycles) {
    (void)sys;
    (void)late_cycles;
}

static void gpu_schedule_cmd_cost(PsxSystem *sys, u32 approx_cycles) {
    if (sys->cfg.gpu_timing && approx_cycles > 0u) {
        sched_schedule(sys, EV_GPU, approx_cycles);
    }
}

u32 gpu_stat(PsxSystem *sys) {
    if (!sys) {
        return 0x1C002000u;
    }
    PsxGpu *g = &sys->gpu;
    gpu_refresh_scanline_bits(sys);

    u32 st_bits = g->gpustat;
    /* Clear dynamic bits: 24 (IRQ1), 25 (DMA req), 26..28 (Ready), 29..30 (DMA dir) */
    st_bits &= ~0x7F000000u;

    if (g->irq1) {
        st_bits |= (1u << 24);
    }

    u32 rdy_cmd  = (g->st == GP0_IDLE) ? 1u : 0u;
    u32 rdy_vram = (g->st == GP0_IMG_STORE && g->xfer_words_left > 0u) ? 1u : 0u;
    u32 rdy_dma  = (g->st != GP0_IMG_STORE) ? 1u : 0u;

    st_bits |= (rdy_cmd << 26) | (rdy_vram << 27) | (rdy_dma << 28);
    st_bits |= ((u32)(g->dma_dir & 3u) << 29);

    u32 dma_req = 0u;
    switch (g->dma_dir & 3u) {
        case 0u: dma_req = 0u;       break;
        case 1u: dma_req = 1u;       break; /* FIFO not full */
        case 2u: dma_req = rdy_dma;  break; /* CPU -> GP0 */
        case 3u: dma_req = rdy_vram; break; /* GPUREAD -> CPU */
    }
    st_bits |= (dma_req << 25);

    g->gpustat = st_bits;
    return st_bits;
}

/* --------------------------------------------------------------------------
 * Primitive Submission & Culling Helpers
 * -------------------------------------------------------------------------- */
static bool tri_within_max_span(const GpuVertex *a, const GpuVertex *b, const GpuVertex *c) {
    s32 min_x = a->x;
    if (b->x < min_x) min_x = b->x;
    if (c->x < min_x) min_x = c->x;
    s32 max_x = a->x;
    if (b->x > max_x) max_x = b->x;
    if (c->x > max_x) max_x = c->x;

    s32 min_y = a->y;
    if (b->y < min_y) min_y = b->y;
    if (c->y < min_y) min_y = c->y;
    s32 max_y = a->y;
    if (b->y > max_y) max_y = b->y;
    if (c->y > max_y) max_y = c->y;

    return ((max_x - min_x) <= 1023) && ((max_y - min_y) <= 511);
}

static bool line_within_max_span(const GpuVertex *a, const GpuVertex *b) {
    s32 dx = b->x - a->x;
    s32 dy = b->y - a->y;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return (dx <= 1023) && (dy <= 511);
}

static void submit_prim(PsxSystem *sys, const GpuPrim *p) {
    PsxGpu *g = &sys->gpu;
    if (g->stats) {
        g->stats->gpu_prims++;
    }
    if (g->be && g->be->draw) {
        g->be->draw(g->be_self, p, &g->env);
    }
}

/* --------------------------------------------------------------------------
 * GP0 Command Execution
 * -------------------------------------------------------------------------- */
static void exec_gp0_polygon(PsxSystem *sys, u8 cmd) {
    PsxGpu *g = &sys->gpu;
    const u32 *f = g->fifo;

    bool is_quad = ((cmd >> 3) & 1u) != 0u;
    bool is_tex  = ((cmd >> 2) & 1u) != 0u;
    bool is_gou  = ((cmd >> 4) & 1u) != 0u;
    bool is_semi = ((cmd >> 1) & 1u) != 0u;
    bool is_raw  = is_tex && ((cmd & 1u) != 0u);
    u32  nverts  = is_quad ? 4u : 3u;

    GpuVertex verts[4];
    memset(verts, 0, sizeof(verts));

    u16 clut  = 0u;
    u16 tpage = 0u;
    u32 idx   = 0u;

    for (u32 i = 0u; i < nverts; i++) {
        if (i == 0u || is_gou) {
            unpack_rgb24(f[idx++], &verts[i]);
        } else {
            verts[i].r = verts[0].r;
            verts[i].g = verts[0].g;
            verts[i].b = verts[0].b;
        }
        unpack_xy_offset(f[idx++], &g->env, &verts[i]);
        if (is_tex) {
            u32 uv_word = f[idx++];
            verts[i].u = (u8)(uv_word & 0xFFu);
            verts[i].v = (u8)((uv_word >> 8) & 0xFFu);
            if (i == 0u) {
                clut = (u16)(uv_word >> 16);
            } else if (i == 1u) {
                tpage = (u16)(uv_word >> 16);
            }
        }
    }

    /* Textured polygons update GPUSTAT[8:0] and GPUSTAT[15] (gp0-e1.exe rule) */
    if (is_tex) {
        u32 td_bit = (g->tex_disable && ((tpage & 0x0800u) != 0u)) ? 1u : 0u;
        g->gpustat = (g->gpustat & ~0x81FFu) | (u32)(tpage & 0x01FFu) | (td_bit << 15);
        g->env.texpage_e1 = (u16)((g->env.texpage_e1 & ~0x09FFu) |
                                  (tpage & 0x01FFu) |
                                  (td_bit ? 0x0800u : 0u));
        if (g->be && g->be->set_draw_env) {
            g->be->set_draw_env(g->be_self, &g->env);
        }
    }

    bool tex_disabled_active = ((g->gpustat & (1u << 15)) != 0u);
    bool eff_tex = is_tex && !tex_disabled_active;

    u8 flags = 0u;
    if (is_gou)               flags |= PF_GOURAUD;
    if (eff_tex)              flags |= PF_TEXTURED;
    if (eff_tex && is_raw)    flags |= PF_RAW_TEX;
    if (is_semi)              flags |= PF_SEMI;

    u16 active_tp = is_tex ? tpage : (u16)(g->gpustat & 0x07FFu);
    GpuPrim prim;
    memset(&prim, 0, sizeof(prim));
    prim.kind      = PRIM_TRI;
    prim.flags     = flags;
    prim.abr       = (u8)((active_tp >> 5) & 3u);
    prim.tex_depth = (u8)((active_tp >> 7) & 3u);
    prim.texpage   = (u16)(active_tp & 0x09FFu);
    prim.clut      = clut;

    /*
     * PSX hardware applies the max-span cull (|dx| > 1023 || |dy| > 511) independently to each
     * constituent triangle (0,1,2) and (1,2,3) of a quad. When both halves survive, submit ONE
     * PRIM_QUAD (v[0..3] in PSX Z-order) so backends do a single draw/texcache lookup; backends
     * rasterize it as (0,1,2)+(1,2,3). Otherwise submit only the surviving half as PRIM_TRI.
     */
    bool ok0 = tri_within_max_span(&verts[0], &verts[1], &verts[2]);
    bool ok1 = is_quad && tri_within_max_span(&verts[1], &verts[2], &verts[3]);
    if (is_quad && ok0 && ok1) {
        prim.kind = PRIM_QUAD;
        prim.v[0] = verts[0];
        prim.v[1] = verts[1];
        prim.v[2] = verts[2];
        prim.v[3] = verts[3];
        submit_prim(sys, &prim);
    } else {
        if (ok0) {
            prim.v[0] = verts[0];
            prim.v[1] = verts[1];
            prim.v[2] = verts[2];
            submit_prim(sys, &prim);
        }
        if (ok1) {
            prim.v[0] = verts[1];
            prim.v[1] = verts[2];
            prim.v[2] = verts[3];
            submit_prim(sys, &prim);
        }
    }

    gpu_schedule_cmd_cost(sys, is_quad ? 64u : 32u);
}

static void exec_gp0_line_initial(PsxSystem *sys, u8 cmd) {
    PsxGpu *g = &sys->gpu;
    const u32 *f = g->fifo;

    bool is_poly = ((cmd >> 3) & 1u) != 0u;
    bool is_gou  = ((cmd >> 4) & 1u) != 0u;
    bool is_semi = ((cmd >> 1) & 1u) != 0u;

    GpuPrim prim;
    memset(&prim, 0, sizeof(prim));
    prim.kind  = is_poly ? PRIM_POLYLINE_SEG : PRIM_LINE;
    prim.flags = (u8)((is_gou ? PF_GOURAUD : 0u) | (is_semi ? PF_SEMI : 0u));
    prim.abr   = (u8)((g->gpustat >> 5) & 3u);

    unpack_rgb24(f[0], &prim.v[0]);
    unpack_xy_offset(f[1], &g->env, &prim.v[0]);
    if (is_gou) {
        unpack_rgb24(f[2], &prim.v[1]);
        unpack_xy_offset(f[3], &g->env, &prim.v[1]);
    } else {
        prim.v[1].r = prim.v[0].r;
        prim.v[1].g = prim.v[0].g;
        prim.v[1].b = prim.v[0].b;
        unpack_xy_offset(f[2], &g->env, &prim.v[1]);
    }

    if (line_within_max_span(&prim.v[0], &prim.v[1])) {
        submit_prim(sys, &prim);
    }

    if (is_poly) {
        /*
         * Enter GP0_POLYLINE:
         * Keep previous endpoint's color in fifo[0] and raw XY word in fifo[1];
         * fifo_n == 0 means awaiting next vertex's first word (which can terminate
         * via (word & 0xF000F000) == 0x50005000); fifo_n == 1 means awaiting Gouraud
         * vertex coordinate word (which NEVER terminates).
         */
        g->fifo[0] = is_gou ? f[2] : f[0];
        g->fifo[1] = is_gou ? f[3] : f[2];
        g->fifo_n  = 0u;
        g->st      = GP0_POLYLINE;
    } else {
        g->st = GP0_IDLE;
        gpu_schedule_cmd_cost(sys, 16u);
    }
}

static void step_gp0_polyline(PsxSystem *sys, u32 word) {
    PsxGpu *g = &sys->gpu;
    bool is_gou  = ((g->cmd >> 4) & 1u) != 0u;
    bool is_semi = ((g->cmd >> 1) & 1u) != 0u;

    if (g->fifo_n == 0u) {
        if ((word & 0xF000F000u) == 0x50005000u) {
            g->st     = GP0_IDLE;
            g->fifo_n = 0u;
            gpu_schedule_cmd_cost(sys, 16u);
            return;
        }
        if (is_gou) {
            g->fifo[2] = word; /* latch next color */
            g->fifo_n  = 1u;
            return;
        }
        /* Monochrome polyline: `word` is the next vertex XY */
        GpuPrim prim;
        memset(&prim, 0, sizeof(prim));
        prim.kind  = PRIM_POLYLINE_SEG;
        prim.flags = is_semi ? PF_SEMI : 0u;
        prim.abr   = (u8)((g->gpustat >> 5) & 3u);

        unpack_rgb24(g->fifo[0], &prim.v[0]);
        unpack_xy_offset(g->fifo[1], &g->env, &prim.v[0]);
        prim.v[1].r = prim.v[0].r;
        prim.v[1].g = prim.v[0].g;
        prim.v[1].b = prim.v[0].b;
        unpack_xy_offset(word, &g->env, &prim.v[1]);

        if (line_within_max_span(&prim.v[0], &prim.v[1])) {
            submit_prim(sys, &prim);
        }
        g->fifo[1] = word;
        return;
    }

    /* Gouraud polyline coordinate word */
    GpuPrim prim;
    memset(&prim, 0, sizeof(prim));
    prim.kind  = PRIM_POLYLINE_SEG;
    prim.flags = (u8)(PF_GOURAUD | (is_semi ? PF_SEMI : 0u));
    prim.abr   = (u8)((g->gpustat >> 5) & 3u);

    unpack_rgb24(g->fifo[0], &prim.v[0]);
    unpack_xy_offset(g->fifo[1], &g->env, &prim.v[0]);
    unpack_rgb24(g->fifo[2], &prim.v[1]);
    unpack_xy_offset(word, &g->env, &prim.v[1]);

    if (line_within_max_span(&prim.v[0], &prim.v[1])) {
        submit_prim(sys, &prim);
    }
    g->fifo[0] = g->fifo[2];
    g->fifo[1] = word;
    g->fifo_n  = 0u;
}

static void exec_gp0_rect(PsxSystem *sys, u8 cmd) {
    PsxGpu *g = &sys->gpu;
    const u32 *f = g->fifo;

    u32  size_code = (cmd >> 3) & 3u;
    bool is_tex    = ((cmd >> 2) & 1u) != 0u;
    bool is_semi   = ((cmd >> 1) & 1u) != 0u;
    bool is_raw    = is_tex && ((cmd & 1u) != 0u);
    bool tex_dis   = ((g->gpustat & (1u << 15)) != 0u);
    bool eff_tex   = is_tex && !tex_dis;

    GpuPrim prim;
    memset(&prim, 0, sizeof(prim));
    prim.kind      = PRIM_RECT;
    prim.texpage   = g->env.texpage_e1;
    prim.abr       = (u8)((g->gpustat >> 5) & 3u);
    prim.tex_depth = (u8)((g->gpustat >> 7) & 3u);

    u8 flags = 0u;
    if (eff_tex)           flags |= PF_TEXTURED;
    if (eff_tex && is_raw) flags |= PF_RAW_TEX;
    if (is_semi)           flags |= PF_SEMI;
    if (eff_tex) {
        if (g->env.texpage_e1 & (1u << 12)) flags |= PF_RECT_FLIP_X;
        if (g->env.texpage_e1 & (1u << 13)) flags |= PF_RECT_FLIP_Y;
    }
    prim.flags = flags;

    u32 idx = 0u;
    unpack_rgb24(f[idx++], &prim.v[0]);
    unpack_xy_offset(f[idx++], &g->env, &prim.v[0]);

    if (is_tex) {
        u32 uv_clut = f[idx++];
        prim.v[0].u = (u8)(uv_clut & 0xFFu);
        prim.v[0].v = (u8)((uv_clut >> 8) & 0xFFu);
        prim.clut   = (u16)(uv_clut >> 16);
    }

    switch (size_code) {
        case 0u: {
            u32 wh = f[idx];
            prim.w = (u16)(wh & 0x3FFu);
            prim.h = (u16)((wh >> 16) & 0x1FFu);
            break;
        }
        case 1u: prim.w = 1u;  prim.h = 1u;  break;
        case 2u: prim.w = 8u;  prim.h = 8u;  break;
        default: prim.w = 16u; prim.h = 16u; break;
    }

    if (prim.w > 0u && prim.h > 0u) {
        submit_prim(sys, &prim);
        gpu_schedule_cmd_cost(sys, ((u32)prim.w * (u32)prim.h) >> 2);
    }
}

static void exec_gp0_packet(PsxSystem *sys) {
    PsxGpu *g = &sys->gpu;
    u8 cmd = g->cmd;
    const u32 *f = g->fifo;

    if (cmd >= 0x20u && cmd <= 0x3Fu) {
        g->st = GP0_IDLE;
        exec_gp0_polygon(sys, cmd);
        return;
    }
    if (cmd >= 0x40u && cmd <= 0x5Fu) {
        exec_gp0_line_initial(sys, cmd);
        return;
    }
    if (cmd >= 0x60u && cmd <= 0x7Fu) {
        g->st = GP0_IDLE;
        exec_gp0_rect(sys, cmd);
        return;
    }
    if (cmd >= 0x80u && cmd <= 0x9Fu) {
        /* VRAM -> VRAM copy */
        g->st = GP0_IDLE;
        u16 sx = (u16)(f[1] & 0x3FFu);
        u16 sy = (u16)((f[1] >> 16) & 0x1FFu);
        u16 dx = (u16)(f[2] & 0x3FFu);
        u16 dy = (u16)((f[2] >> 16) & 0x1FFu);
        u16 w  = (u16)(((f[3] - 1u) & 0x3FFu) + 1u);
        u16 h  = (u16)((((f[3] >> 16) - 1u) & 0x1FFu) + 1u);

        sync_shadow_wrapped(g, sx, sy, w, h);
        gpu_vram_copy_rect(g->vram, sx, sy, dx, dy, w, h,
                           g->env.check_mask, g->env.set_mask);
        if (g->be && g->be->vram_copy) {
            g->be->vram_copy(g->be_self, sx, sy, dx, dy, w, h);
        }
        gpu_schedule_cmd_cost(sys, (u32)w * (u32)h);
        return;
    }
    if (cmd >= 0xA0u && cmd <= 0xBFu) {
        /* CPU -> VRAM streaming transfer */
        g->xfer_x  = (u16)(f[1] & 0x3FFu);
        g->xfer_y  = (u16)((f[1] >> 16) & 0x1FFu);
        g->xfer_w  = (u16)(((f[2] - 1u) & 0x3FFu) + 1u);
        g->xfer_h  = (u16)((((f[2] >> 16) - 1u) & 0x1FFu) + 1u);
        g->xfer_cx = 0u;
        g->xfer_cy = 0u;
        u32 npix   = (u32)g->xfer_w * (u32)g->xfer_h;
        g->xfer_words_left = (npix + 1u) >> 1;
        g->st = (g->xfer_words_left > 0u) ? GP0_IMG_LOAD : GP0_IDLE;
        return;
    }
    if (cmd >= 0xC0u && cmd <= 0xDFu) {
        /* VRAM -> CPU readout mode */
        g->xfer_x  = (u16)(f[1] & 0x3FFu);
        g->xfer_y  = (u16)((f[1] >> 16) & 0x1FFu);
        g->xfer_w  = (u16)(((f[2] - 1u) & 0x3FFu) + 1u);
        g->xfer_h  = (u16)((((f[2] >> 16) - 1u) & 0x1FFu) + 1u);
        g->xfer_cx = 0u;
        g->xfer_cy = 0u;
        u32 npix   = (u32)g->xfer_w * (u32)g->xfer_h;
        g->xfer_words_left = (npix + 1u) >> 1;
        if (g->stats) {
            g->stats->gpu_readbacks++;
        }
        sync_shadow_wrapped(g, g->xfer_x, g->xfer_y, g->xfer_w, g->xfer_h);
        g->st = (g->xfer_words_left > 0u) ? GP0_IMG_STORE : GP0_IDLE;
        return;
    }

    g->st = GP0_IDLE;
    u32 w0 = f[0];
    switch (cmd) {
        case 0x00u: /* NOP */
            break;
        case 0x01u: /* Clear Texture/CLUT Cache */
            if (gpu_soft_is_backend(g->be)) {
                gpu_soft_invalidate_clut(g->be_self);
            }
            break;
        case 0x02u: { /* FillRect in VRAM */
            u32 bgr24 = w0 & 0xFFFFFFu;
            u16 fx = (u16)(f[1] & 0x3F0u);
            u16 fy = (u16)((f[1] >> 16) & 0x1FFu);
            u16 fw = (u16)(((f[2] & 0x3FFu) + 0xFu) & ~0xFu);
            u16 fh = (u16)((f[2] >> 16) & 0x1FFu);
            if (fw > 0u && fh > 0u) {
                gpu_vram_fill_rect(g->vram, fx, fy, fw, fh, bgr24);
                if (g->be && g->be->fill_rect) {
                    g->be->fill_rect(g->be_self, fx, fy, fw, fh, bgr24);
                }
                gpu_schedule_cmd_cost(sys, ((u32)fw * (u32)fh) >> 3);
            }
            break;
        }
        case 0x1Fu: /* Interrupt Request (IRQ1) */
            if (!g->irq1) {
                g->irq1 = 1u;
                irq_raise(sys, IRQ_GPU);
            }
            break;
        case 0xE1u: { /* Draw Mode / TexPage */
            u32 td_bit = (g->tex_disable && ((w0 & 0x0800u) != 0u)) ? 1u : 0u;
            g->env.dither          = (u8)((w0 >> 9) & 1u);
            g->env.draw_to_display = (u8)((w0 >> 10) & 1u);
            /* Preserve bits 13:12 (textured rectangle X/Y flip) + bits 10:0 + effective bit 11 */
            g->env.texpage_e1 = (u16)((w0 & 0x37FFu) | (td_bit ? 0x0800u : 0u));
            g->gpustat = (g->gpustat & ~0x87FFu) | (w0 & 0x07FFu) | (td_bit << 15);
            if (g->be && g->be->set_draw_env) {
                g->be->set_draw_env(g->be_self, &g->env);
            }
            break;
        }
        case 0xE2u: /* Texture Window */
            g->env.tw_mask_x = (u8)(w0 & 0x1Fu);
            g->env.tw_mask_y = (u8)((w0 >> 5) & 0x1Fu);
            g->env.tw_off_x  = (u8)((w0 >> 10) & 0x1Fu);
            g->env.tw_off_y  = (u8)((w0 >> 15) & 0x1Fu);
            if (g->be && g->be->set_draw_env) {
                g->be->set_draw_env(g->be_self, &g->env);
            }
            break;
        case 0xE3u: /* Drawing Area Top-Left (v2 GPU: 20-bit) */
            g->env.clip_x0 = (s16)(w0 & 0x3FFu);
            g->env.clip_y0 = (s16)((w0 >> 10) & 0x3FFu);
            if (g->be && g->be->set_draw_env) {
                g->be->set_draw_env(g->be_self, &g->env);
            }
            break;
        case 0xE4u: /* Drawing Area Bottom-Right (v2 GPU: 20-bit) */
            g->env.clip_x1 = (s16)(w0 & 0x3FFu);
            g->env.clip_y1 = (s16)((w0 >> 10) & 0x3FFu);
            if (g->be && g->be->set_draw_env) {
                g->be->set_draw_env(g->be_self, &g->env);
            }
            break;
        case 0xE5u: /* Drawing Offset (two signed 11-bit values) */
            g->env.off_x = gpu_sext11(w0 & 0x7FFu);
            g->env.off_y = gpu_sext11((w0 >> 11) & 0x7FFu);
            if (g->be && g->be->set_draw_env) {
                g->be->set_draw_env(g->be_self, &g->env);
            }
            break;
        case 0xE6u: /* Mask Bit Setting */
            g->env.set_mask   = (u8)(w0 & 1u);
            g->env.check_mask = (u8)((w0 >> 1) & 1u);
            g->gpustat = (g->gpustat & ~0x1800u) | ((w0 & 3u) << 11);
            if (g->be && g->be->set_draw_env) {
                g->be->set_draw_env(g->be_self, &g->env);
            }
            break;
        default:
            break;
    }
}

static inline void advance_xfer_pos(PsxGpu *g) {
    g->xfer_cx++;
    if (g->xfer_cx >= g->xfer_w) {
        g->xfer_cx = 0u;
        g->xfer_cy++;
    }
}

static void step_gp0_img_load(PsxSystem *sys, u32 word) {
    PsxGpu *g = &sys->gpu;
    u8 chk = g->env.check_mask;
    u8 set = g->env.set_mask;

    if (g->xfer_cy < g->xfer_h) {
        gpu_vram_write_pixel_masked(g->vram,
                                    (u32)g->xfer_x + (u32)g->xfer_cx,
                                    (u32)g->xfer_y + (u32)g->xfer_cy,
                                    (u16)(word & 0xFFFFu), chk, set);
        advance_xfer_pos(g);
    }
    if (g->xfer_cy < g->xfer_h) {
        gpu_vram_write_pixel_masked(g->vram,
                                    (u32)g->xfer_x + (u32)g->xfer_cx,
                                    (u32)g->xfer_y + (u32)g->xfer_cy,
                                    (u16)(word >> 16), chk, set);
        advance_xfer_pos(g);
    }

    if (g->xfer_words_left > 0u) {
        g->xfer_words_left--;
    }
    if (g->xfer_words_left == 0u) {
        g->st = GP0_IDLE;
        notify_vram_written_wrapped(g, g->xfer_x, g->xfer_y, g->xfer_w, g->xfer_h);
    }
}

void gpu_gp0_write(PsxSystem *sys, u32 word) {
    if (!sys) {
        return;
    }
    PsxGpu *g = &sys->gpu;

    switch (g->st) {
        case GP0_IMG_LOAD:
            step_gp0_img_load(sys, word);
            return;

        case GP0_POLYLINE:
            step_gp0_polyline(sys, word);
            return;

        case GP0_COLLECT:
            g->fifo[g->fifo_n++] = word;
            if (g->fifo_n >= g->fifo_need) {
                exec_gp0_packet(sys);
            }
            return;

        case GP0_IMG_STORE:
            /* Writing a new GP0 command while C0h readout is active aborts C0h */
            g->xfer_words_left = 0u;
            g->st = GP0_IDLE;
            /* fallthrough */
        case GP0_IDLE:
        default: {
            u8 op   = (u8)(word >> 24);
            u8 need = s_gp0_len[op];
            g->cmd       = op;
            g->fifo[0]   = word;
            g->fifo_n    = 1u;
            g->fifo_need = need;
            if (need <= 1u) {
                exec_gp0_packet(sys);
            } else {
                g->st = GP0_COLLECT;
            }
            return;
        }
    }
}

void gpu_dma_write_words(PsxSystem *sys, const u32 *src, u32 n) {
    if (!sys || !src) {
        return;
    }
    for (u32 i = 0u; i < n; i++) {
        gpu_gp0_write(sys, src[i]);
    }
}

u32 gpu_read(PsxSystem *sys) {
    if (!sys) {
        return 0u;
    }
    PsxGpu *g = &sys->gpu;
    if (g->st == GP0_IMG_STORE && g->xfer_words_left > 0u) {
        u16 p0 = 0u, p1 = 0u;
        if (g->xfer_cy < g->xfer_h) {
            p0 = gpu_vram_read_pixel(g->vram,
                                     (u32)g->xfer_x + (u32)g->xfer_cx,
                                     (u32)g->xfer_y + (u32)g->xfer_cy);
            advance_xfer_pos(g);
        }
        if (g->xfer_cy < g->xfer_h) {
            p1 = gpu_vram_read_pixel(g->vram,
                                     (u32)g->xfer_x + (u32)g->xfer_cx,
                                     (u32)g->xfer_y + (u32)g->xfer_cy);
            advance_xfer_pos(g);
        }
        g->gpuread_latch = (u32)p0 | ((u32)p1 << 16);
        g->gpuread_valid = true;
        g->xfer_words_left--;
        if (g->xfer_words_left == 0u) {
            g->st = GP0_IDLE;
        }
    }
    return g->gpuread_latch;
}

u32 gpu_dma_read_words(PsxSystem *sys, u32 *dst, u32 n) {
    if (!sys || !dst) {
        return 0u;
    }
    for (u32 i = 0u; i < n; i++) {
        dst[i] = gpu_read(sys);
    }
    return n;
}

/* --------------------------------------------------------------------------
 * GP1 Display & Control Commands
 * -------------------------------------------------------------------------- */
static void apply_gp1_soft_reset(PsxSystem *sys) {
    PsxGpu *g = &sys->gpu;

    g->st              = GP0_IDLE;
    g->fifo_n          = 0u;
    g->fifo_need       = 0u;
    g->cmd             = 0u;
    g->xfer_words_left = 0u;
    g->irq1            = 0u;
    g->dma_dir         = 0u;
    g->tex_disable     = 0u;

    memset(&g->env, 0, sizeof(g->env));

    g->disp.vram_x    = 0u;
    g->disp.vram_y    = 0u;
    g->disp.hres      = 256u;
    g->disp.vres      = 240u;
    g->disp.interlace = 0u;
    g->disp.color24   = 0u;
    g->disp.blank     = 1u;
    g->disp.pal       = (sys->cfg.region == PSX_REGION_PAL) ? 1u : 0u;
    g->disp.x1        = 0x200u;
    g->disp.x2        = 0xC00u;
    g->disp.y1        = 0x010u;
    g->disp.y2        = 0x100u;

    /* Power-on / GP1(00h) GPUSTAT = 0x14802000 (+ PAL bit 20 if region==PAL) */
    g->gpustat = 0x14802000u | (g->disp.pal ? (1u << 20) : 0u);

    if (g->be) {
        if (g->be->reset) {
            g->be->reset(g->be_self);
        }
        if (g->be->set_draw_env) {
            g->be->set_draw_env(g->be_self, &g->env);
        }
    }
}

void gpu_gp1_write(PsxSystem *sys, u32 word) {
    if (!sys) {
        return;
    }
    PsxGpu *g = &sys->gpu;
    u8 op = (u8)((word >> 24) & 0x3Fu); /* PSX GP1 mirrors commands across 0x40/0x80/0xC0 */

    switch (op) {
        case 0x00u: /* Soft Reset */
            apply_gp1_soft_reset(sys);
            break;

        case 0x01u: /* Reset Command Buffer / FIFO */
            g->st              = GP0_IDLE;
            g->fifo_n          = 0u;
            g->fifo_need       = 0u;
            g->xfer_words_left = 0u;
            break;

        case 0x02u: /* Acknowledge GPU IRQ1 */
            g->irq1 = 0u;
            g->gpustat &= ~(1u << 24);
            break;

        case 0x03u: /* Display Disable (1 = blanked, 0 = enabled) */
            g->disp.blank = (u8)(word & 1u);
            g->gpustat = (g->gpustat & ~(1u << 23)) | ((word & 1u) << 23);
            break;

        case 0x04u: /* DMA Direction */
            g->dma_dir = (u8)(word & 3u);
            g->gpustat = (g->gpustat & ~(3u << 29)) | ((word & 3u) << 29);
            break;

        case 0x05u: /* Start of Display Area in VRAM */
            g->disp.vram_x = (u16)(word & 0x3FEu); /* hardware forces even X alignment */
            g->disp.vram_y = (u16)((word >> 10) & 0x1FFu);
            break;

        case 0x06u: /* Horizontal Display Range */
            g->disp.x1 = (u16)(word & 0xFFFu);
            g->disp.x2 = (u16)((word >> 12) & 0xFFFu);
            break;

        case 0x07u: /* Vertical Display Range */
            g->disp.y1 = (u16)(word & 0x3FFu);
            g->disp.y2 = (u16)((word >> 10) & 0x3FFu);
            break;

        case 0x08u: { /* Display Mode */
            u32 hres1   = word & 3u;
            u32 vres_b  = (word >> 2) & 1u;
            u32 pal     = (word >> 3) & 1u;
            u32 col24   = (word >> 4) & 1u;
            u32 inter   = (word >> 5) & 1u;
            u32 hres2   = (word >> 6) & 1u;
            u32 rev     = (word >> 7) & 1u;

            static const u16 s_hres_tab[4] = { 256u, 320u, 512u, 640u };
            g->disp.hres      = hres2 ? 368u : s_hres_tab[hres1];
            g->disp.vres      = (inter && vres_b) ? 480u : 240u;
            g->disp.pal       = (u8)pal;
            g->disp.color24   = (u8)col24;
            g->disp.interlace = (u8)inter;

            u32 mode_mask = (1u << 14) | (0x7Fu << 16);
            u32 mode_bits = (rev << 14) |
                            (hres2 << 16) |
                            (hres1 << 17) |
                            (vres_b << 19) |
                            (pal << 20) |
                            (col24 << 21) |
                            (inter << 22);
            g->gpustat = (g->gpustat & ~mode_mask) | mode_bits;
            gpu_refresh_scanline_bits(sys);
            break;
        }

        case 0x09u: /* Allow Texture Disable (preserves GPUSTAT.15 when cleared!) */
            g->tex_disable = (u8)(word & 1u);
            break;

        default:
            /* GP1(10h..1Fh): Get GPU Info (208-pin v2 GPU) */
            if (op >= 0x10u && op <= 0x1Fu) {
                switch (word & 0xFu) {
                    case 2u: /* Texture Window (20-bit) */
                        g->gpuread_latch =
                            ((u32)(g->env.tw_mask_x & 0x1Fu)) |
                            ((u32)(g->env.tw_mask_y & 0x1Fu) << 5) |
                            ((u32)(g->env.tw_off_x  & 0x1Fu) << 10) |
                            ((u32)(g->env.tw_off_y  & 0x1Fu) << 15);
                        g->gpuread_valid = true;
                        break;
                    case 3u: /* Draw Area Top-Left (20-bit on v2 GPU) */
                        g->gpuread_latch =
                            ((u32)(g->env.clip_x0 & 0x3FF)) |
                            ((u32)(g->env.clip_y0 & 0x3FF) << 10);
                        g->gpuread_valid = true;
                        break;
                    case 4u: /* Draw Area Bottom-Right (20-bit on v2 GPU) */
                        g->gpuread_latch =
                            ((u32)(g->env.clip_x1 & 0x3FF)) |
                            ((u32)(g->env.clip_y1 & 0x3FF) << 10);
                        g->gpuread_valid = true;
                        break;
                    case 5u: /* Draw Offset (22-bit) */
                        g->gpuread_latch =
                            ((u32)g->env.off_x & 0x7FFu) |
                            (((u32)g->env.off_y & 0x7FFu) << 11);
                        g->gpuread_valid = true;
                        break;
                    case 7u: /* GPU Type / Version (2 = standard retail 208-pin GPU) */
                        g->gpuread_latch = 2u;
                        g->gpuread_valid = true;
                        break;
                    case 8u: /* Unknown / 0 on retail GPU */
                        g->gpuread_latch = 0u;
                        g->gpuread_valid = true;
                        break;
                    default:
                        /* Indices 0,1,6,9..15 leave GPUREAD latch unchanged */
                        break;
                }
            }
            break;
    }
}

/* --------------------------------------------------------------------------
 * MMIO Dispatch (0x1F801810..0x1F801817, supporting 8/16/32-bit widths)
 * -------------------------------------------------------------------------- */
static u32 gpu_mmio_read(PsxSystem *sys, u32 addr, u32 width) {
    u32 off = addr & 0xFu;
    u32 val = 0xFFFFFFFFu;
    if (off < 4u) {
        val = gpu_read(sys) >> ((off & 3u) * 8u);
    } else if (off < 8u) {
        val = gpu_stat(sys) >> ((off & 3u) * 8u);
    }
    if (width == 1u) return val & 0xFFu;
    if (width == 2u) return val & 0xFFFFu;
    return val;
}

static void gpu_mmio_write(PsxSystem *sys, u32 addr, u32 v, u32 width) {
    u32 off = addr & 0xFu;
    u32 shifted = v << ((off & 3u) * 8u);
    (void)width;
    if (off < 4u) {
        gpu_gp0_write(sys, shifted);
    } else if (off < 8u) {
        gpu_gp1_write(sys, shifted);
    }
}

/* --------------------------------------------------------------------------
 * Lifecycle & SaveState
 * -------------------------------------------------------------------------- */
void gpu_init(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    init_gp0_len_table();

    PsxGpu *g = &sys->gpu;
    memset(g, 0, sizeof(*g));
    g->stats = &sys->stats;
    g->vram  = (u16 *)arena_alloc(&sys->arena, PSX_VRAM_BYTES, 4096u);

    PsxResult rc = PSX_ERR_NOT_IMPL;
    if (sys->cfg.gpu_backend == GPU_BACKEND_HW) {
        rc = gpu_hw_backend_create(sys, &g->be, &g->be_self);
    }
    if (rc != PSX_OK || !g->be) {
        (void)gpu_soft_backend_create(sys, &g->be, &g->be_self);
    }

    bus_mmio_register(sys, PSX_REG_GP0, 16u, gpu_mmio_read, gpu_mmio_write);

    if (sys->sched.fn[EV_HBLANK] && sys->sched.fn[EV_HBLANK] != gpu_on_hblank) {
        s_prev_hblank = sys->sched.fn[EV_HBLANK];
    }
    if (sys->sched.fn[EV_VBLANK] && sys->sched.fn[EV_VBLANK] != gpu_on_vblank) {
        s_prev_vblank = sys->sched.fn[EV_VBLANK];
    }
    sched_set_handler(&sys->sched, EV_HBLANK, gpu_on_hblank);
    sched_set_handler(&sys->sched, EV_VBLANK, gpu_on_vblank);
    sched_set_handler(&sys->sched, EV_GPU, gpu_on_cmd_done);

    gpu_reset(sys);
}

void gpu_reset(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    PsxGpu *g = &sys->gpu;
    gpu_vram_clear(g->vram);
    g->gpuread_latch = 0u;
    g->gpuread_valid = false;
    g->frame         = 0u;

    apply_gp1_soft_reset(sys);

    if (sys->sched.fn[EV_HBLANK] && sys->sched.fn[EV_HBLANK] != gpu_on_hblank) {
        s_prev_hblank = sys->sched.fn[EV_HBLANK];
    }
    if (sys->sched.fn[EV_VBLANK] && sys->sched.fn[EV_VBLANK] != gpu_on_vblank) {
        s_prev_vblank = sys->sched.fn[EV_VBLANK];
    }
    sched_set_handler(&sys->sched, EV_HBLANK, gpu_on_hblank);
    sched_set_handler(&sys->sched, EV_VBLANK, gpu_on_vblank);
    sched_set_handler(&sys->sched, EV_GPU, gpu_on_cmd_done);

    if ((sys->sched.active_mask & (1u << EV_HBLANK)) == 0u) {
        u32 cpl = g->disp.pal ? PSX_CYCLES_PER_LINE_PAL : PSX_CYCLES_PER_LINE_NTSC;
        sched_schedule(sys, EV_HBLANK, cpl);
    }
}

static void state_s16(StateIO *st, s16 *v) {
    u16 u = (u16)*v;
    state_u16(st, &u);
    *v = (s16)u;
}

void gpu_state(PsxSystem *sys, StateIO *st) {
    if (!sys || !st) {
        return;
    }
    PsxGpu *g = &sys->gpu;

    if (st->writing && g->be && g->be->sync_to_shadow) {
        g->be->sync_to_shadow(g->be_self, 0u, 0u, (u16)PSX_VRAM_W, (u16)PSX_VRAM_H);
    }

    state_u32(st, &g->gpustat);

    /* GpuDrawEnv */
    state_s16(st, &g->env.clip_x0);
    state_s16(st, &g->env.clip_y0);
    state_s16(st, &g->env.clip_x1);
    state_s16(st, &g->env.clip_y1);
    state_s16(st, &g->env.off_x);
    state_s16(st, &g->env.off_y);
    state_u8(st, &g->env.tw_mask_x);
    state_u8(st, &g->env.tw_mask_y);
    state_u8(st, &g->env.tw_off_x);
    state_u8(st, &g->env.tw_off_y);
    state_u8(st, &g->env.set_mask);
    state_u8(st, &g->env.check_mask);
    state_u8(st, &g->env.dither);
    state_u8(st, &g->env.draw_to_display);
    state_u16(st, &g->env.texpage_e1);

    /* GpuDisplay */
    state_u16(st, &g->disp.vram_x);
    state_u16(st, &g->disp.vram_y);
    state_u16(st, &g->disp.hres);
    state_u16(st, &g->disp.vres);
    state_u8(st, &g->disp.interlace);
    state_u8(st, &g->disp.color24);
    state_u8(st, &g->disp.blank);
    state_u8(st, &g->disp.pal);
    state_u16(st, &g->disp.x1);
    state_u16(st, &g->disp.x2);
    state_u16(st, &g->disp.y1);
    state_u16(st, &g->disp.y2);

    /* GP0 parser & FIFO */
    u8 st_mode = (u8)g->st;
    state_u8(st, &st_mode);
    g->st = st_mode;
    for (u32 i = 0u; i < 16u; i++) {
        state_u32(st, &g->fifo[i]);
    }
    state_u8(st, &g->fifo_n);
    state_u8(st, &g->fifo_need);
    state_u8(st, &g->cmd);

    state_u16(st, &g->xfer_x);
    state_u16(st, &g->xfer_y);
    state_u16(st, &g->xfer_w);
    state_u16(st, &g->xfer_h);
    state_u16(st, &g->xfer_cx);
    state_u16(st, &g->xfer_cy);
    state_u32(st, &g->xfer_words_left);
    state_u32(st, &g->gpuread_latch);
    state_bool(st, &g->gpuread_valid);

    state_u32(st, &g->irq1);
    state_u8(st, &g->dma_dir);
    state_u8(st, &g->tex_disable);
    state_u32(st, &g->frame);

    if (g->vram) {
        state_bytes(st, g->vram, PSX_VRAM_BYTES);
    }

    if (!st->writing && g->be) {
        if (g->be->set_draw_env) {
            g->be->set_draw_env(g->be_self, &g->env);
        }
        if (g->be->vram_written) {
            g->be->vram_written(g->be_self, 0u, 0u, (u16)PSX_VRAM_W, (u16)PSX_VRAM_H);
        }
    }
}

void gpu_shutdown(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    PsxGpu *g = &sys->gpu;
    if (g->be && g->be->destroy) {
        g->be->destroy(g->be_self);
    }
    g->be      = NULL;
    g->be_self = NULL;
}
