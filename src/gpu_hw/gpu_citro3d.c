#include <3ds.h>
#include <citro3d.h>

#include "psx3ds/gpu_hw.h"
#include "psx3ds/system.h"
#include "psx_gpu_shbin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOP_DISPLAY_TRANSFER_FLAGS \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

typedef struct GpuHwVertex {
    s16 pos[2]; /* v0: GPU_SHORT x 2 (VRAM or screen X, Y) */
    s16 uv[2];  /* v1: GPU_SHORT x 2 (texel U, V) */
    u8  col[4]; /* v2: GPU_UNSIGNED_BYTE x 4 (R, G, B, A: 255 = textured, 0 = untextured) */
} GpuHwVertex;

_Static_assert(sizeof(GpuHwVertex) == 12, "GpuHwVertex must be 12 bytes");

/*
 * Fused TEV modes (primitives & fills of the VRAM pass):
 *   HW_TEX_FUSED     stage 0: rgb = clamp(tex * vtx * 2) (PS1 128 = 1.0x), a = tex.a
 *   HW_TEX_FUSED_RAW stage 0: rgb = tex (bit-exact raw texture), a = tex.a
 *   both             stage 1: rgb = lerp(vtx.rgb, stage0.rgb, vtx.a), a = lerp(1, stage0.a, vtx.a)
 * Vertex alpha 255 selects the textured path and 0 plain vertex colour, so untextured
 * primitives (HW_TEX_ANY + HW_SLOT_ANY) join either fused batch without a state change.
 * HW_TEX_RAW is a plain texture REPLACE used for bit-exact VRAM upload blits / present.
 */
typedef enum GpuHwTexMode {
    HW_TEX_NONE      = 0,
    HW_TEX_RAW       = 1,
    HW_TEX_FUSED     = 2,
    HW_TEX_FUSED_RAW = 3,
    HW_TEX_ANY       = 4  /* untextured wildcard: adopts the batch's fused mode */
} GpuHwTexMode;

typedef enum GpuHwAlphaMode {
    HW_ALPHA_OPAQUE_NOTEST = 0, /* untextured opaque / fill_rect */
    HW_ALPHA_OPAQUE_MASKED = 1, /* textured !semi, or textured semi ALL_OPAQUE */
    HW_ALPHA_SEMI_NOTEST   = 2, /* untextured semi */
    HW_ALPHA_SEMI_ALL_STP  = 3, /* textured semi ALL_STP (1 pass) */
    HW_ALPHA_SEMI_MIXED    = 4  /* textured semi MIXED (2 passes: >0xC0 opaque, ==0x80 ABR) */
} GpuHwAlphaMode;

enum {
    HW_SLOT_NONE   = -1,
    HW_SLOT_UPLOAD = -2,
    HW_SLOT_VRAM   = -3,
    HW_SLOT_DISP24 = -4,
    HW_SLOT_ANY    = -5  /* untextured: joins any bound texture (white 8x8 if none) */
};

typedef struct GpuHwBatchKey {
    s16 tex_slot;     /* HW_SLOT_* or 0..63 texcache slot index (HW_SLOT_ANY = wildcard) */
    u8  tex_mode;     /* GpuHwTexMode */
    u8  alpha_mode;   /* GpuHwAlphaMode */
    u8  abr;          /* 0..3 (canonicalized to 0 when opaque) */
    u8  stencil_mode; /* (check_mask << 1) | set_mask (0 when !has_stencil) */
    u16 sc_x0, sc_y0; /* inclusive VRAM scissor [0..1023], [0..511] */
    u16 sc_x1, sc_y1; /* exclusive VRAM scissor [1..1024], [1..512] */
} GpuHwBatchKey;

typedef struct GpuHwState {
    PsxSystem        *sys;

    u32              *shbin_copy;
    DVLB_s           *dvlb;
    shaderProgram_s   prog;
    C3D_AttrInfo      attr_info;
    C3D_BufInfo       buf_info;
    s8                u_proj;
    s8                u_uv_scale;
    s8                u_uv_bias;

    C3D_Mtx           proj_vram;

    C3D_Tex           vram_tex;
    C3D_RenderTarget *vram_rt;
    C3D_RenderTarget *top_rt;
    bool              has_stencil;

    C3D_Tex           upload_tex;
    bool              upload_tex_ready;

    C3D_Tex           disp24_tex;
    bool              disp24_tex_ready;

    C3D_Tex           white_tex;
    bool              white_tex_ready;
    u8                cur_tev;        /* GpuHwTexMode currently programmed, 0xFF = unknown */
    u32               gpu_frame_id;   /* incremented at every C3D_FrameBegin */

    /* Lifetime batch-break diagnostics (logged once at destroy) */
    u32               brk_slot, brk_alpha, brk_abr, brk_scissor, brk_other;
    u32               n_cpu_upload, n_readback, n_stall, n_ring_wrap, n_draw_calls, n_presents;

    u16              *readback_buf;

    GpuHwVertex      *vbo_ring[GPU_HW_VTX_RING_COUNT];
    u32               cur_ring;
    u32               ring_pos;
    u32               batch_first;
    u32               batch_count;
    bool              has_batch_key;
    GpuHwBatchKey     batch_key;

    bool              in_frame;
    bool              on_vram_rt;
    bool              vram_cleared_once;
    u32               skip_counter;
    u64               last_present_us;

    GpuDrawEnv        cur_env;
    GpuTexCache       texcache;

    /* 64x32 cell bitmasks (16x16 VRAM cells) for GPU<->CPU shadow coherence */
    u64               gpu_dirty_rows[GPU_HW_CELL_ROWS];
    u64               cpu_dirty_rows[GPU_HW_CELL_ROWS];
    bool              any_cpu_dirty;
    u16               cpu_dx0, cpu_dy0, cpu_dx1, cpu_dy1;
} GpuHwState;

/* --------------------------------------------------------------------------
 * Dirty Grid Helpers
 * -------------------------------------------------------------------------- */
static bool grid_overlaps_rect(const u64 grid[GPU_HW_CELL_ROWS], u16 x, u16 y, u16 w, u16 h) {
    u64 cols = 0ULL;
    u32 rows = 0u;
    gpu_vram_rect_cell_masks(x, y, w, h, &cols, &rows);
    if (cols == 0ULL || rows == 0u) {
        return false;
    }
    for (u32 r = 0; r < GPU_HW_CELL_ROWS; ++r) {
        if ((rows & (1u << r)) != 0u && (grid[r] & cols) != 0ULL) {
            return true;
        }
    }
    return false;
}

static void grid_mark_rect(u64 grid[GPU_HW_CELL_ROWS], u16 x, u16 y, u16 w, u16 h) {
    u64 cols = 0ULL;
    u32 rows = 0u;
    gpu_vram_rect_cell_masks(x, y, w, h, &cols, &rows);
    for (u32 r = 0; r < GPU_HW_CELL_ROWS; ++r) {
        if ((rows & (1u << r)) != 0u) {
            grid[r] |= cols;
        }
    }
}

static void grid_clear_rect(u64 grid[GPU_HW_CELL_ROWS], u16 x, u16 y, u16 w, u16 h) {
    u64 cols = 0ULL;
    u32 rows = 0u;
    gpu_vram_rect_cell_masks(x, y, w, h, &cols, &rows);
    for (u32 r = 0; r < GPU_HW_CELL_ROWS; ++r) {
        if ((rows & (1u << r)) != 0u) {
            grid[r] &= ~cols;
        }
    }
}

static void mark_cpu_dirty(GpuHwState *st, u16 x, u16 y, u16 w, u16 h) {
    if (w == 0u || h == 0u) {
        return;
    }
    grid_mark_rect(st->cpu_dirty_rows, x, y, w, h);

    u32 x0 = (u32)x & 0x3FFu;
    u32 y0 = (u32)y & 0x1FFu;
    u32 x1 = x0 + (u32)w;
    u32 y1 = y0 + (u32)h;
    if (x1 > GPU_HW_VRAM_W) {
        x0 = 0u;
        x1 = GPU_HW_VRAM_W;
    }
    if (y1 > GPU_HW_VRAM_H) {
        y0 = 0u;
        y1 = GPU_HW_VRAM_H;
    }
    if (!st->any_cpu_dirty) {
        st->any_cpu_dirty = true;
        st->cpu_dx0 = (u16)x0;
        st->cpu_dy0 = (u16)y0;
        st->cpu_dx1 = (u16)x1;
        st->cpu_dy1 = (u16)y1;
    } else {
        if (x0 < st->cpu_dx0) st->cpu_dx0 = (u16)x0;
        if (y0 < st->cpu_dy0) st->cpu_dy0 = (u16)y0;
        if (x1 > st->cpu_dx1) st->cpu_dx1 = (u16)x1;
        if (y1 > st->cpu_dy1) st->cpu_dy1 = (u16)y1;
    }
}

/* --------------------------------------------------------------------------
 * PICA200 State Configuration & Batch Flushing
 * -------------------------------------------------------------------------- */
static void bind_current_ring(GpuHwState *st) {
    BufInfo_Init(&st->buf_info);
    BufInfo_Add(&st->buf_info, st->vbo_ring[st->cur_ring], sizeof(GpuHwVertex), 3, 0x210);
    C3D_SetBufInfo(&st->buf_info);
}

static void configure_common_pipeline(GpuHwState *st) {
    C3D_BindProgram(&st->prog);
    C3D_SetAttrInfo(&st->attr_info);
    bind_current_ring(st);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    st->cur_tev = 0xFFu; /* force TEV re-programming on next batch */
}

/*
 * C3D_FrameBegin(0) blocks until every previously submitted GX command list has been
 * consumed by the PICA200, so after begin_frame() all earlier frames' vertex rings and
 * textures are free to be rewritten.
 */
static void begin_frame(GpuHwState *st, u8 flags) {
    C3D_FrameBegin(flags);
    st->in_frame = true;
    st->on_vram_rt = false;
    st->gpu_frame_id++;
    if (st->gpu_frame_id == 0u) {
        st->gpu_frame_id = 1u;
    }
    gpu_texcache_set_frame(&st->texcache, st->gpu_frame_id);
}

/* Submit the open frame (if any) and wait until the GPU has consumed it */
static void restart_frame_and_wait(GpuHwState *st) {
    if (st->in_frame) {
        C3D_FrameEnd(0);
        st->in_frame = false;
        st->on_vram_rt = false;
    }
    begin_frame(st, 0);
}

static void ensure_vram_pass_open(GpuHwState *st) {
    if (!st->in_frame) {
        begin_frame(st, 0);
    }
    if (!st->on_vram_rt) {
        C3D_FrameDrawOn(st->vram_rt);
        if (!st->vram_cleared_once) {
            C3D_ClearBits bits = st->has_stencil ? C3D_CLEAR_ALL : C3D_CLEAR_COLOR;
            C3D_RenderTargetClear(st->vram_rt, bits, 0x000000FFu, 0u);
            st->vram_cleared_once = true;
        }
        configure_common_pipeline(st);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, st->u_proj, &st->proj_vram);
        st->on_vram_rt = true;
    }
}

static void set_abr_blend_state(u8 abr) {
    switch (abr & 3u) {
        case 0u: /* 0.5 * B + 0.5 * F */
            C3D_BlendingColor(0x80808080u);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                           GPU_CONSTANT_COLOR, GPU_CONSTANT_COLOR,
                           GPU_ONE, GPU_ONE);
            break;
        case 1u: /* 1.0 * B + 1.0 * F */
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                           GPU_ONE, GPU_ONE,
                           GPU_ONE, GPU_ONE);
            break;
        case 2u: /* 1.0 * B - 1.0 * F */
            C3D_AlphaBlend(GPU_BLEND_REVERSE_SUBTRACT, GPU_BLEND_ADD,
                           GPU_ONE, GPU_ONE,
                           GPU_ONE, GPU_ONE);
            break;
        case 3u: /* 1.0 * B + 0.25 * F */
        default:
            C3D_BlendingColor(0x40404040u);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                           GPU_CONSTANT_COLOR, GPU_ONE,
                           GPU_ONE, GPU_ONE);
            break;
    }
}

static void set_opaque_blend_state(void) {
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
}

static void apply_stencil_state(GpuHwState *st, u8 stencil_mode) {
    if (!st->has_stencil) {
        C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0x00);
        return;
    }
    switch (stencil_mode & 3u) {
        case 0u: /* check=0, set=0 -> clear mask bit 0 on drawn pixels */
            C3D_StencilTest(true, GPU_ALWAYS, 0, 0x01, 0x01);
            C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_REPLACE);
            break;
        case 1u: /* check=0, set=1 -> set mask bit 0 on drawn pixels */
            C3D_StencilTest(true, GPU_ALWAYS, 1, 0x01, 0x01);
            C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_REPLACE);
            break;
        case 2u: /* check=1, set=0 -> draw only where bit 0 == 0, keep 0 */
            C3D_StencilTest(true, GPU_EQUAL, 0, 0x01, 0x00);
            C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_KEEP);
            break;
        case 3u: /* check=1, set=1 -> draw only where bit 0 == 0, invert 0 -> 1 */
        default:
            C3D_StencilTest(true, GPU_EQUAL, 0, 0x01, 0x01);
            C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_INVERT);
            break;
    }
}

/*
 * PICA200 C3D_Tex memory places tile row 0 (offset 0, PSX y = 0 / NDC y = +1)
 * at texture coordinate t = 1.0 and tile row H/8 - 1 (PSX y = H - 1 / NDC y = -1)
 * at t = 0.0, so PSX texel (u, v) maps to s = u / W, t = 1.0 - v / H (flip_v = true).
 */
static void set_uv_transform(GpuHwState *st, float inv_w, float inv_h, bool flip_v) {
    if (flip_v) {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, st->u_uv_scale, inv_w, -inv_h, 0.0f, 0.0f);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, st->u_uv_bias,  0.0f,  1.0f,  0.0f, 0.0f);
    } else {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, st->u_uv_scale, inv_w, inv_h, 0.0f, 0.0f);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, st->u_uv_bias,  0.0f,  0.0f,  0.0f, 0.0f);
    }
}

static void configure_tev_mode(GpuHwState *st, GpuHwTexMode mode) {
    if (st->cur_tev == (u8)mode) {
        return;
    }
    st->cur_tev = (u8)mode;

    C3D_TexEnv *env0 = C3D_GetTexEnv(0);
    C3D_TexEnv *env1 = C3D_GetTexEnv(1);
    C3D_TexEnvInit(env0);
    C3D_TexEnvInit(env1);
    C3D_TexEnvInit(C3D_GetTexEnv(2));
    C3D_TexEnvInit(C3D_GetTexEnv(3));

    if (mode == HW_TEX_RAW) {
        C3D_TexEnvSrc(env0, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvOpRgb(env0, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
        C3D_TexEnvOpAlpha(env0, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
        C3D_TexEnvFunc(env0, C3D_Both, GPU_REPLACE);
        C3D_TexEnvScale(env0, C3D_Both, GPU_TEVSCALE_1);
        return;
    }

    C3D_TexEnvSrc(env0, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvOpRgb(env0, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
    if (mode == HW_TEX_FUSED_RAW) {
        /* Stage 0: raw texture (exact, no modulation rounding) */
        C3D_TexEnvFunc(env0, C3D_RGB, GPU_REPLACE);
        C3D_TexEnvScale(env0, C3D_RGB, GPU_TEVSCALE_1);
    } else {
        /* Stage 0: PS1 texture modulation out_rgb = clamp(tex_rgb * vtx_rgb * 2.0) (128 => 1.0x) */
        C3D_TexEnvFunc(env0, C3D_RGB, GPU_MODULATE);
        C3D_TexEnvScale(env0, C3D_RGB, GPU_TEVSCALE_2);
    }
    C3D_TexEnvSrc(env0, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvOpAlpha(env0, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
    C3D_TexEnvFunc(env0, C3D_Alpha, GPU_REPLACE);
    C3D_TexEnvScale(env0, C3D_Alpha, GPU_TEVSCALE_1);

    /* Stage 1: per-vertex select (vtx.a = 255 textured, 0 untextured) via INTERPOLATE */
    C3D_TexEnvSrc(env1, C3D_RGB, GPU_PREVIOUS, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvOpRgb(env1, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
    C3D_TexEnvFunc(env1, C3D_RGB, GPU_INTERPOLATE);
    C3D_TexEnvScale(env1, C3D_RGB, GPU_TEVSCALE_1);
    C3D_TexEnvSrc(env1, C3D_Alpha, GPU_PREVIOUS, GPU_CONSTANT, GPU_PRIMARY_COLOR);
    C3D_TexEnvOpAlpha(env1, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
    C3D_TexEnvFunc(env1, C3D_Alpha, GPU_INTERPOLATE);
    C3D_TexEnvScale(env1, C3D_Alpha, GPU_TEVSCALE_1);
    C3D_TexEnvColor(env1, 0xFFFFFFFFu);
}

static void flush_batch(GpuHwState *st) {
    if (st->batch_count == 0u) {
        return;
    }

    ensure_vram_pass_open(st);

    const GpuHwBatchKey *k = &st->batch_key;
    if (k->sc_x1 <= k->sc_x0 || k->sc_y1 <= k->sc_y0) {
        st->batch_first = st->ring_pos;
        st->batch_count = 0u;
        return;
    }

    /*
     * On unrotated 1024x512 vram_rt with Mtx_Ortho(0, 1024, 512, 0):
     *   PSX y = 0 is at NDC y = +1 (PICA framebuffer y = 511),
     *   PSX y = 511 is at NDC y = -1 (PICA framebuffer y = 0).
     * So PSX [sc_y0, sc_y1) maps to PICA framebuffer Y [512 - sc_y1, 512 - sc_y0).
     */
    if (k->sc_x0 == 0u && k->sc_y0 == 0u &&
        k->sc_x1 == GPU_HW_VRAM_W && k->sc_y1 == GPU_HW_VRAM_H) {
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0u, 0u, GPU_HW_VRAM_W, GPU_HW_VRAM_H);
    } else {
        u32 fb_y0 = GPU_HW_VRAM_H - (u32)k->sc_y1;
        u32 fb_y1 = GPU_HW_VRAM_H - (u32)k->sc_y0;
        C3D_SetScissor(GPU_SCISSOR_NORMAL, k->sc_x0, fb_y0, k->sc_x1, fb_y1);
    }

    if (k->tex_slot >= 0 && k->tex_slot < (s16)GPU_HW_TEXCACHE_SLOTS) {
        C3D_Tex *tex = (C3D_Tex *)st->texcache.slots[k->tex_slot].hw_tex;
        C3D_TexSetFilter(tex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexBind(0, tex);
        set_uv_transform(st, 1.0f / 256.0f, 1.0f / 256.0f, true);
    } else if (k->tex_slot == HW_SLOT_UPLOAD) {
        C3D_TexSetFilter(&st->upload_tex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexBind(0, &st->upload_tex);
        set_uv_transform(st, 1.0f / 1024.0f, 1.0f / 512.0f, true);
    } else {
        /* untextured-only batch: vertex alpha 0 ignores the (white) texture sample */
        C3D_TexBind(0, &st->white_tex);
        set_uv_transform(st, 1.0f / 8.0f, 1.0f / 8.0f, true);
    }

    configure_tev_mode(st, k->tex_mode == HW_TEX_ANY ? HW_TEX_FUSED : (GpuHwTexMode)k->tex_mode);
    apply_stencil_state(st, k->stencil_mode);

    GSPGPU_FlushDataCache(&st->vbo_ring[st->cur_ring][st->batch_first],
                          st->batch_count * sizeof(GpuHwVertex));

    int first = (int)st->batch_first;
    int count = (int)st->batch_count;

    switch ((GpuHwAlphaMode)k->alpha_mode) {
        case HW_ALPHA_OPAQUE_NOTEST:
            C3D_AlphaTest(false, GPU_ALWAYS, 0);
            set_opaque_blend_state();
            C3D_DrawArrays(GPU_TRIANGLES, first, count);
            st->n_draw_calls++;
            if (st->sys) st->sys->stats.gpu_batches++;
            break;

        case HW_ALPHA_OPAQUE_MASKED:
            C3D_AlphaTest(true, GPU_GREATER, 0);
            set_opaque_blend_state();
            C3D_DrawArrays(GPU_TRIANGLES, first, count);
            st->n_draw_calls++;
            if (st->sys) st->sys->stats.gpu_batches++;
            break;

        case HW_ALPHA_SEMI_NOTEST:
            C3D_AlphaTest(false, GPU_ALWAYS, 0);
            set_abr_blend_state(k->abr);
            C3D_DrawArrays(GPU_TRIANGLES, first, count);
            st->n_draw_calls++;
            if (st->sys) st->sys->stats.gpu_batches++;
            break;

        case HW_ALPHA_SEMI_ALL_STP:
            C3D_AlphaTest(true, GPU_GREATER, 0);
            set_abr_blend_state(k->abr);
            C3D_DrawArrays(GPU_TRIANGLES, first, count);
            st->n_draw_calls++;
            if (st->sys) st->sys->stats.gpu_batches++;
            break;

        case HW_ALPHA_SEMI_MIXED:
            /* Pass A: opaque non-STP texels (alpha == 0xFF) */
            C3D_AlphaTest(true, GPU_GREATER, 0xC0);
            set_opaque_blend_state();
            C3D_DrawArrays(GPU_TRIANGLES, first, count);

            /* Pass B: semi-transparent STP texels (alpha == 0x80) */
            C3D_AlphaTest(true, GPU_EQUAL, 0x80);
            set_abr_blend_state(k->abr);
            C3D_DrawArrays(GPU_TRIANGLES, first, count);
            st->n_draw_calls += 2u;
            if (st->sys) st->sys->stats.gpu_batches += 2u;
            break;
    }

    st->batch_first = st->ring_pos;
    st->batch_count = 0u;
}

/*
 * Returns true when primitive key b can be appended to the open batch a. Untextured
 * primitives (HW_SLOT_ANY) join any texture; an untextured-only batch adopts the first
 * concrete texture slot appended to it (its vertices never sample the texture).
 */
static bool batch_keys_merge(GpuHwBatchKey *a, const GpuHwBatchKey *b) {
    bool same = a->alpha_mode == b->alpha_mode &&
           a->abr == b->abr &&
           a->stencil_mode == b->stencil_mode &&
           a->sc_x0 == b->sc_x0 &&
           a->sc_y0 == b->sc_y0 &&
           a->sc_x1 == b->sc_x1 &&
           a->sc_y1 == b->sc_y1;
    if (!same) {
        return false;
    }
    /* Untextured primitive joins any fused batch (vertex alpha 0 ignores the texture) */
    if (b->tex_slot == HW_SLOT_ANY && b->tex_mode == HW_TEX_ANY) {
        return a->tex_mode != HW_TEX_RAW;
    }
    if (a->tex_slot == b->tex_slot && a->tex_mode == b->tex_mode) {
        return true;
    }
    /* Untextured-only batch adopts the first textured primitive's slot and fused mode */
    if (a->tex_slot == HW_SLOT_ANY && a->tex_mode == HW_TEX_ANY && b->tex_slot >= 0) {
        a->tex_slot = b->tex_slot;
        a->tex_mode = b->tex_mode;
        return true;
    }
    return false;
}

static GpuHwVertex *alloc_batch_verts(GpuHwState *st, const GpuHwBatchKey *key, u32 count) {
    if (count == 0u || count > GPU_HW_VTX_RING_VERTS) {
        return NULL;
    }
    if (st->has_batch_key && st->batch_count > 0u && !batch_keys_merge(&st->batch_key, key)) {
        const GpuHwBatchKey *a = &st->batch_key;
        if (a->sc_x0 != key->sc_x0 || a->sc_y0 != key->sc_y0 ||
            a->sc_x1 != key->sc_x1 || a->sc_y1 != key->sc_y1) {
            st->brk_scissor++;
        } else if (a->alpha_mode != key->alpha_mode) {
            st->brk_alpha++;
        } else if (a->abr != key->abr) {
            st->brk_abr++;
        } else if (a->tex_slot != key->tex_slot) {
            st->brk_slot++;
        } else {
            st->brk_other++;
        }
        flush_batch(st);
    }

    if (st->ring_pos + count > GPU_HW_VTX_RING_VERTS) {
        /*
         * Ring full mid-frame: submit the current frame slice (persistent vram_rt keeps all
         * draws), switch rings, and wait for the GPU so the other ring is free to rewrite.
         */
        flush_batch(st);
        if (st->in_frame) {
            C3D_FrameEnd(0);
            st->in_frame = false;
            st->on_vram_rt = false;
        }
        st->cur_ring = (st->cur_ring + 1u) % GPU_HW_VTX_RING_COUNT;
        st->ring_pos = 0u;
        st->batch_first = 0u;
        st->batch_count = 0u;
        st->n_ring_wrap++;
        begin_frame(st, 0);
    }

    if (st->batch_count == 0u) {
        st->batch_first = st->ring_pos;
        st->batch_key = *key;
        st->has_batch_key = true;
    }

    GpuHwVertex *out = &st->vbo_ring[st->cur_ring][st->ring_pos];
    st->ring_pos += count;
    st->batch_count += count;
    return out;
}

static inline void write_hw_vtx(GpuHwVertex *dst, s16 x, s16 y, s16 u, s16 v,
                                u8 r, u8 g, u8 b, u8 a) {
    dst->pos[0] = x;
    dst->pos[1] = y;
    dst->uv[0]  = u;
    dst->uv[1]  = v;
    dst->col[0] = r;
    dst->col[1] = g;
    dst->col[2] = b;
    dst->col[3] = a;
}

/* --------------------------------------------------------------------------
 * CPU -> VRAM Streaming Sync (for pre-rendered backgrounds, HUDs & title screens)
 * -------------------------------------------------------------------------- */
static bool ensure_upload_tex(GpuHwState *st) {
    if (st->upload_tex_ready) {
        return true;
    }
    if (!C3D_TexInit(&st->upload_tex, GPU_HW_VRAM_W, GPU_HW_VRAM_H, GPU_RGBA5551)) {
        return false;
    }
    C3D_TexSetWrap(&st->upload_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexSetFilter(&st->upload_tex, GPU_NEAREST, GPU_NEAREST);
    st->upload_tex_ready = true;
    return true;
}

static void flush_cpu_dirty_if_needed(GpuHwState *st, u16 rx, u16 ry, u16 rw, u16 rh) {
    if (!st->any_cpu_dirty || !st->sys || !st->sys->gpu.vram) {
        return;
    }
    if (!grid_overlaps_rect(st->cpu_dirty_rows, rx, ry, rw, rh)) {
        return;
    }
    if (!ensure_upload_tex(st)) {
        return;
    }

    /* Submit pending draws and wait until any prior upload_tex sampling has completed */
    st->n_cpu_upload++;
    flush_batch(st);
    restart_frame_and_wait(st);

    u16 ux0 = (u16)(st->cpu_dx0 & ~7u);
    u16 uy0 = (u16)(st->cpu_dy0 & ~7u);
    u16 ux1 = (u16)((st->cpu_dx1 + 7u) & ~7u);
    u16 uy1 = (u16)((st->cpu_dy1 + 7u) & ~7u);
    if (ux1 > GPU_HW_VRAM_W) ux1 = GPU_HW_VRAM_W;
    if (uy1 > GPU_HW_VRAM_H) uy1 = GPU_HW_VRAM_H;
    u16 uw = (u16)(ux1 - ux0);
    u16 uh = (u16)(uy1 - uy0);

    if (uw > 0u && uh > 0u) {
        const u16 *vram = st->sys->gpu.vram;
        u16 *dst = (u16 *)st->upload_tex.data;
        u32 tx0 = ux0 >> 3, tx1 = ux1 >> 3;
        u32 ty0 = uy0 >> 3, ty1 = uy1 >> 3;
        for (u32 ty = ty0; ty < ty1; ++ty) {
            for (u32 tx = tx0; tx < tx1; ++tx) {
                u16 *tile = dst + (((ty * (GPU_HW_VRAM_W >> 3)) + tx) << 6);
                for (u32 py = 0; py < 8u; ++py) {
                    u32 vy = (ty << 3) + py;
                    const u16 *vrow = vram + (vy << 10);
                    for (u32 px = 0; px < 8u; ++px) {
                        u32 vx = (tx << 3) + px;
                        tile[gpu_morton8x8_lut[py][px]] = gpu_psx_to_rgba5551_opaque(vrow[vx]);
                    }
                }
            }
        }
        GSPGPU_FlushDataCache(st->upload_tex.data, GPU_HW_VRAM_W * GPU_HW_VRAM_H * sizeof(u16));

        GpuHwBatchKey ukey;
        memset(&ukey, 0, sizeof(ukey));
        ukey.tex_slot     = HW_SLOT_UPLOAD;
        ukey.tex_mode     = HW_TEX_RAW;
        ukey.alpha_mode   = HW_ALPHA_OPAQUE_NOTEST;
        ukey.abr          = 0u;
        ukey.stencil_mode = 0u;
        ukey.sc_x0        = 0u;
        ukey.sc_y0        = 0u;
        ukey.sc_x1        = GPU_HW_VRAM_W;
        ukey.sc_y1        = GPU_HW_VRAM_H;

        GpuHwVertex *v = alloc_batch_verts(st, &ukey, 6u);
        if (v != NULL) {
            s16 x0 = (s16)ux0, y0 = (s16)uy0;
            s16 x1 = (s16)ux1, y1 = (s16)uy1;
            write_hw_vtx(&v[0], x0, y0, x0, y0, 255, 255, 255, 255);
            write_hw_vtx(&v[1], x1, y0, x1, y0, 255, 255, 255, 255);
            write_hw_vtx(&v[2], x0, y1, x0, y1, 255, 255, 255, 255);
            write_hw_vtx(&v[3], x1, y0, x1, y0, 255, 255, 255, 255);
            write_hw_vtx(&v[4], x1, y1, x1, y1, 255, 255, 255, 255);
            write_hw_vtx(&v[5], x0, y1, x0, y1, 255, 255, 255, 255);
            flush_batch(st);
        }
    }

    memset(st->cpu_dirty_rows, 0, sizeof(st->cpu_dirty_rows));
    st->any_cpu_dirty = false;
}

/* --------------------------------------------------------------------------
 * GPU -> Shadow VRAM Coherence (sync_to_shadow)
 * -------------------------------------------------------------------------- */
static void hw_sync_to_shadow(void *self, u16 x, u16 y, u16 w, u16 h) {
    GpuHwState *st = (GpuHwState *)self;
    if (!st || !st->sys || !st->sys->gpu.vram || w == 0u || h == 0u) {
        return;
    }
    if (!grid_overlaps_rect(st->gpu_dirty_rows, x, y, w, h)) {
        return;
    }

    /*
     * Submit pending draws and leave the frame CLOSED: C3D_SyncTextureCopy() outside a
     * frame first waits for (and clears) the GX queue, i.e. for the PICA200 to finish
     * rendering into vram_tex, and then performs the copy synchronously.
     */
    st->n_readback++;
    flush_batch(st);
    if (st->in_frame) {
        C3D_FrameEnd(0);
        st->in_frame = false;
        st->on_vram_rt = false;
    }

    if (st->readback_buf == NULL) {
        st->readback_buf = (u16 *)linearMemAlign(GPU_HW_VRAM_W * GPU_HW_VRAM_H * sizeof(u16), 0x80);
        if (st->readback_buf == NULL) {
            return;
        }
    }

    u32 ty_first = 0u;
    u32 ty_last  = (GPU_HW_VRAM_H >> 3) - 1u;
    u32 y0_clamped = (u32)y & 0x1FFu;
    if ((u32)h < GPU_HW_VRAM_H && (y0_clamped + (u32)h) <= GPU_HW_VRAM_H) {
        ty_first = y0_clamped >> 3;
        ty_last  = (y0_clamped + (u32)h - 1u) >> 3;
    }
    u32 tile_row_u16 = GPU_HW_VRAM_W * 8u; /* 8192 u16s (16 KB) per 8-scanline Morton tile row */
    u32 off_u16      = ty_first * tile_row_u16;
    u32 copy_bytes   = (ty_last - ty_first + 1u) * tile_row_u16 * sizeof(u16);

    GSPGPU_InvalidateDataCache(st->readback_buf + off_u16, copy_bytes);
    C3D_SyncTextureCopy((u32 *)((u16 *)st->vram_tex.data + off_u16), 0u,
                        (u32 *)(st->readback_buf + off_u16), 0u,
                        copy_bytes, GX_TRANSFER_RAW_COPY(1));
    GSPGPU_InvalidateDataCache(st->readback_buf + off_u16, copy_bytes);

    /*
     * Un-Morton from vram_tex tile layout: Mtx_Ortho(0, 1024, 512, 0) places
     * PSX y = 0 (NDC y = +1, t = 1.0) at tile row 0 (offset 0) of vram_tex.data.
     */
    u16 *vram = st->sys->gpu.vram;
    for (u32 dy = 0; dy < (u32)h; ++dy) {
        u32 vy = ((u32)y + dy) & 0x1FFu;
        u32 ty = vy >> 3;
        u32 py = vy & 7u;
        u16 *dst_row = vram + (vy << 10);
        for (u32 dx = 0; dx < (u32)w; ++dx) {
            u32 vx = ((u32)x + dx) & 0x3FFu;
            u32 tx = vx >> 3;
            u32 px = vx & 7u;
            u16 raw = st->readback_buf[(((ty << 7) + tx) << 6) + gpu_morton8x8_lut[py][px]];
            dst_row[vx] = gpu_rgba5551_to_psx(raw);
        }
    }

    grid_clear_rect(st->gpu_dirty_rows, x, y, w, h);
    gpu_texcache_invalidate_vram(&st->texcache, x, y, w, h);
}

/* --------------------------------------------------------------------------
 * Texture Window Evaluation Helper
 * -------------------------------------------------------------------------- */
static void resolve_tex_window(const GpuDrawEnv *env, s16 u_vals[4], s16 v_vals[4], u32 nverts,
                               GpuTexWindowKey *out_tw_key) {
    out_tw_key->mask_x = 0u;
    out_tw_key->mask_y = 0u;
    out_tw_key->off_x  = 0u;
    out_tw_key->off_y  = 0u;

    u32 mx = (u32)(env->tw_mask_x & 0x1Fu) * 8u;
    u32 my = (u32)(env->tw_mask_y & 0x1Fu) * 8u;
    if (mx == 0u && my == 0u) {
        return;
    }

    u32 ox = (u32)((env->tw_off_x & env->tw_mask_x) & 0x1Fu) * 8u;
    u32 oy = (u32)((env->tw_off_y & env->tw_mask_y) & 0x1Fu) * 8u;

    s16 u_min = u_vals[0], u_max = u_vals[0];
    s16 v_min = v_vals[0], v_max = v_vals[0];
    for (u32 i = 1u; i < nverts; ++i) {
        if (u_vals[i] < u_min) u_min = u_vals[i];
        if (u_vals[i] > u_max) u_max = u_vals[i];
        if (v_vals[i] < v_min) v_min = v_vals[i];
        if (v_vals[i] > v_max) v_max = v_vals[i];
    }

    /*
     * Check if the sampled texel range [u_min, u_max_sample] stays inside a single
     * continuous window period. When true, we can translate vertex UVs directly and
     * share the un-windowed 256x256 TexPage slot!
     */
    s16 u_hi = (u_max > u_min) ? (s16)(u_max - 1) : u_min;
    s16 v_hi = (v_max > v_min) ? (s16)(v_max - 1) : v_min;
    if (u_min >= 0 && u_hi <= 255 && v_min >= 0 && v_hi <= 255) {
        u32 u_diff = (u32)u_min ^ (u32)u_hi;
        u32 v_diff = (u32)v_min ^ (u32)v_hi;
        u32 u_vary = (u_diff == 0u) ? 0u : ((1u << (32u - (u32)__builtin_clz(u_diff))) - 1u);
        u32 v_vary = (v_diff == 0u) ? 0u : ((1u << (32u - (u32)__builtin_clz(v_diff))) - 1u);
        if ((u_vary & mx) == 0u && (v_vary & my) == 0u) {
            s16 du = (s16)ox - (s16)((u32)u_min & mx);
            s16 dv = (s16)oy - (s16)((u32)v_min & my);
            for (u32 i = 0; i < nverts; ++i) {
                u_vals[i] = (s16)(u_vals[i] + du);
                v_vals[i] = (s16)(v_vals[i] + dv);
            }
            return;
        }
    }

    /* Primitive wraps across multiple window periods: use pre-wrapped cached 256x256 slot */
    out_tw_key->mask_x = (u8)(env->tw_mask_x & 0x1Fu);
    out_tw_key->mask_y = (u8)(env->tw_mask_y & 0x1Fu);
    out_tw_key->off_x  = (u8)((env->tw_off_x & env->tw_mask_x) & 0x1Fu);
    out_tw_key->off_y  = (u8)((env->tw_off_y & env->tw_mask_y) & 0x1Fu);
}

/* --------------------------------------------------------------------------
 * GpuBackend Callbacks
 * -------------------------------------------------------------------------- */
static void hw_reset(void *self) {
    GpuHwState *st = (GpuHwState *)self;
    if (!st) {
        return;
    }
    flush_batch(st);
    if (st->in_frame) {
        C3D_FrameEnd(0);
        st->in_frame = false;
        st->on_vram_rt = false;
    }
    st->cur_ring = 0u;
    st->ring_pos = 0u;
    st->batch_first = 0u;
    st->batch_count = 0u;
    st->has_batch_key = false;
    st->vram_cleared_once = false;
    st->any_cpu_dirty = false;
    memset(st->gpu_dirty_rows, 0, sizeof(st->gpu_dirty_rows));
    memset(st->cpu_dirty_rows, 0, sizeof(st->cpu_dirty_rows));
    memset(&st->cur_env, 0, sizeof(st->cur_env));
    st->cur_env.clip_x1 = (s16)(GPU_HW_VRAM_W - 1u);
    st->cur_env.clip_y1 = (s16)(GPU_HW_VRAM_H - 1u);
    gpu_texcache_reset(&st->texcache);
}

static void hw_set_draw_env(void *self, const GpuDrawEnv *env) {
    GpuHwState *st = (GpuHwState *)self;
    if (st && env) {
        st->cur_env = *env;
    }
}

static bool tri_oversized(s16 x0, s16 y0, s16 x1, s16 y1, s16 x2, s16 y2) {
    s32 min_x = x0;
    if (x1 < min_x) min_x = x1;
    if (x2 < min_x) min_x = x2;
    s32 max_x = x0;
    if (x1 > max_x) max_x = x1;
    if (x2 > max_x) max_x = x2;

    s32 min_y = y0;
    if (y1 < min_y) min_y = y1;
    if (y2 < min_y) min_y = y2;
    s32 max_y = y0;
    if (y1 > max_y) max_y = y1;
    if (y2 > max_y) max_y = y2;

    return (max_x - min_x) > 1023 || (max_y - min_y) > 511;
}

static void mark_gpu_bbox_clipped(GpuHwState *st, s32 bx0, s32 by0, s32 bx1, s32 by1,
                                  u16 sc_x0, u16 sc_y0, u16 sc_x1, u16 sc_y1) {
    if (bx0 < (s32)sc_x0) bx0 = (s32)sc_x0;
    if (by0 < (s32)sc_y0) by0 = (s32)sc_y0;
    if (bx1 > (s32)sc_x1) bx1 = (s32)sc_x1;
    if (by1 > (s32)sc_y1) by1 = (s32)sc_y1;
    if (bx1 > bx0 && by1 > by0) {
        grid_mark_rect(st->gpu_dirty_rows, (u16)bx0, (u16)by0,
                       (u16)(bx1 - bx0), (u16)(by1 - by0));
    }
}

static void hw_draw(void *self, const GpuPrim *p, const GpuDrawEnv *env_in) {
    GpuHwState *st = (GpuHwState *)self;
    if (!st || !p) {
        return;
    }
    const GpuDrawEnv *env = env_in ? env_in : &st->cur_env;

    /* Compute clipped VRAM scissor [sc_x0, sc_x1) x [sc_y0, sc_y1) */
    s32 cx0 = env->clip_x0 < 0 ? 0 : env->clip_x0;
    s32 cy0 = env->clip_y0 < 0 ? 0 : env->clip_y0;
    s32 cx1 = (s32)env->clip_x1 + 1;
    s32 cy1 = (s32)env->clip_y1 + 1;
    if (cx1 > (s32)GPU_HW_VRAM_W) cx1 = (s32)GPU_HW_VRAM_W;
    if (cy1 > (s32)GPU_HW_VRAM_H) cy1 = (s32)GPU_HW_VRAM_H;
    if (cx1 <= cx0 || cy1 <= cy0) {
        return;
    }

    /* Flush any CPU->VRAM uploaded background inside the active draw area before overdrawing */
    flush_cpu_dirty_if_needed(st, (u16)cx0, (u16)cy0, (u16)(cx1 - cx0), (u16)(cy1 - cy0));

    bool textured  = (p->flags & PF_TEXTURED) != 0u;
    bool raw_tex   = (p->flags & PF_RAW_TEX) != 0u;
    bool gouraud   = (p->flags & PF_GOURAUD) != 0u;
    bool semi      = (p->flags & PF_SEMI) != 0u;

    if (p->kind == PRIM_LINE || p->kind == PRIM_POLYLINE_SEG) {
        textured = false;
        raw_tex  = false;
    }

    /* Prepare primitive vertex UV coordinates before texture-window resolution */
    s16 u_coords[4] = { 0, 0, 0, 0 };
    s16 v_coords[4] = { 0, 0, 0, 0 };
    u32 uv_count = 0u;

    if (textured) {
        if (p->kind == PRIM_RECT) {
            if (p->w == 0u || p->h == 0u || p->w > 1023u || p->h > 511u) {
                return;
            }
            s16 u0 = (s16)p->v[0].u;
            s16 v0 = (s16)p->v[0].v;
            s16 uL = (p->flags & PF_RECT_FLIP_X) ? (s16)(u0 + 1) : u0;
            s16 uR = (p->flags & PF_RECT_FLIP_X) ? (s16)(u0 + 1 - (s16)p->w) : (s16)(u0 + (s16)p->w);
            s16 vT = (p->flags & PF_RECT_FLIP_Y) ? (s16)(v0 + 1) : v0;
            s16 vB = (p->flags & PF_RECT_FLIP_Y) ? (s16)(v0 + 1 - (s16)p->h) : (s16)(v0 + (s16)p->h);
            u_coords[0] = uL; v_coords[0] = vT;
            u_coords[1] = uR; v_coords[1] = vT;
            u_coords[2] = uL; v_coords[2] = vB;
            u_coords[3] = uR; v_coords[3] = vB;
            uv_count = 4u;
        } else {
            uv_count = (p->kind == PRIM_QUAD) ? 4u : 3u;
            for (u32 i = 0; i < uv_count; ++i) {
                u_coords[i] = (s16)p->v[i].u;
                v_coords[i] = (s16)p->v[i].v;
            }
        }
    }

    GpuTexWindowKey tw_key = { 0u, 0u, 0u, 0u };
    s16 tex_slot_idx = HW_SLOT_ANY;
    GpuTexAlphaClass alpha_cls = GPU_TEX_ALPHA_ALL_OPAQUE;
    bool slot_rgba8 = false;

    if (textured && st->sys && st->sys->gpu.vram) {
        resolve_tex_window(env, u_coords, v_coords, uv_count, &tw_key);

        /* Render-to-texture coherence: sync shadow VRAM if GPU rendered into this TexPage/CLUT */
        u8  eff_depth = (p->tex_depth >= 2u) ? 2u : p->tex_depth;
        u16 tp_x = (u16)((p->texpage & 0x0Fu) << 6);
        u16 tp_y = (u16)(((p->texpage >> 4) & 0x01u) << 8);
        u16 tp_w = (eff_depth == 0u) ? 64u : ((eff_depth == 1u) ? 128u : 256u);
        if (grid_overlaps_rect(st->gpu_dirty_rows, tp_x, tp_y, tp_w, GPU_HW_TEXPAGE_DIM)) {
            hw_sync_to_shadow(st, tp_x, tp_y, tp_w, GPU_HW_TEXPAGE_DIM);
        }
        if (eff_depth < 2u) {
            u16 cx = (u16)(((u32)p->clut & 0x3Fu) << 4);
            u16 cy = (u16)(((u32)p->clut >> 6) & 0x1FFu);
            u16 cw = (eff_depth == 0u) ? 16u : 256u;
            if (grid_overlaps_rect(st->gpu_dirty_rows, cx, cy, cw, 1u)) {
                hw_sync_to_shadow(st, cx, cy, cw, 1u);
            }
        }

        /*
         * Open the frame BEFORE any texcache decode: begin_frame() waits for the previous
         * frame's GPU work, and cur_frame protects slots already referenced this frame.
         */
        ensure_vram_pass_open(st);
        const GpuTexSlot *slot = gpu_texcache_lookup(&st->texcache, st->sys->gpu.vram,
                                                     p->texpage, p->tex_depth, p->clut,
                                                     &tw_key, &st->sys->stats, NULL);
        if (slot == NULL && st->texcache.stalled) {
            /* Every active slot is sampled by unsubmitted draws: submit, wait, retry */
            st->n_stall++;
            flush_batch(st);
            restart_frame_and_wait(st);
            slot = gpu_texcache_lookup(&st->texcache, st->sys->gpu.vram,
                                       p->texpage, p->tex_depth, p->clut,
                                       &tw_key, &st->sys->stats, NULL);
        }
        if (slot == NULL || slot->hw_tex == NULL) {
            return;
        }
        tex_slot_idx = (s16)(slot - st->texcache.slots);
        slot_rgba8 = slot->is_rgba8;
        alpha_cls = (GpuTexAlphaClass)slot->alpha_class;

        /* Per-primitive STP classification over the sampled UV box (8x8-tile masks) */
        if (semi && alpha_cls != GPU_TEX_ALPHA_ALL_OPAQUE && uv_count > 0u) {
            s32 u_lo = u_coords[0], u_hi = u_coords[0];
            s32 v_lo = v_coords[0], v_hi = v_coords[0];
            for (u32 i = 1u; i < uv_count; ++i) {
                if (u_coords[i] < u_lo) u_lo = u_coords[i];
                if (u_coords[i] > u_hi) u_hi = u_coords[i];
                if (v_coords[i] < v_lo) v_lo = v_coords[i];
                if (v_coords[i] > v_hi) v_hi = v_coords[i];
            }
            alpha_cls = gpu_tex_tiles_classify(&slot->tiles, u_lo, v_lo, u_hi, v_hi);
            if (alpha_cls == GPU_TEX_ALPHA_MIXED && !slot_rgba8) {
                alpha_cls = GPU_TEX_ALPHA_ALL_STP; /* 1-bit alpha: STP split unavailable */
            }
        }
    } else if (textured) {
        return;
    }

    /*
     * Unified alpha modes: untextured vertices emit alpha 255 from TEV stage 1, so they pass
     * the "alpha > 0" test and share OPAQUE_MASKED / SEMI_ALL_STP batches with textures.
     */
    GpuHwAlphaMode alpha_mode;
    if (!semi || (textured && alpha_cls == GPU_TEX_ALPHA_ALL_OPAQUE)) {
        alpha_mode = HW_ALPHA_OPAQUE_MASKED;
    } else if (!textured || alpha_cls == GPU_TEX_ALPHA_ALL_STP) {
        alpha_mode = HW_ALPHA_SEMI_ALL_STP;
    } else {
        alpha_mode = HW_ALPHA_SEMI_MIXED;
    }
    bool uses_blend = (alpha_mode == HW_ALPHA_SEMI_ALL_STP || alpha_mode == HW_ALPHA_SEMI_MIXED);

    GpuHwBatchKey key;
    memset(&key, 0, sizeof(key));
    key.tex_slot     = textured ? tex_slot_idx : HW_SLOT_ANY;
    key.tex_mode     = (u8)(!textured ? HW_TEX_ANY : (raw_tex ? HW_TEX_FUSED_RAW : HW_TEX_FUSED));
    key.alpha_mode   = (u8)alpha_mode;
    key.abr          = uses_blend ? (u8)(p->abr & 3u) : 0u;
    key.stencil_mode = st->has_stencil ? (u8)(((env->check_mask & 1u) << 1) | (env->set_mask & 1u)) : 0u;
    key.sc_x0        = (u16)cx0;
    key.sc_y0        = (u16)cy0;
    key.sc_x1        = (u16)cx1;
    key.sc_y1        = (u16)cy1;

    /* Raw textures use the exact REPLACE fused mode (vertex RGB unused) */
    u8 va = textured ? 255u : 0u;
    bool per_vtx_col = gouraud && !raw_tex;
    u8 r0 = p->v[0].r, g0 = p->v[0].g, b0 = p->v[0].b;
    if (raw_tex) {
        r0 = g0 = b0 = 255u;
    }

    switch ((GpuPrimKind)p->kind) {
        case PRIM_TRI: {
            if (tri_oversized(p->v[0].x, p->v[0].y, p->v[1].x, p->v[1].y, p->v[2].x, p->v[2].y)) {
                return;
            }
            GpuHwVertex *v = alloc_batch_verts(st, &key, 3u);
            if (!v) return;
            for (u32 i = 0; i < 3u; ++i) {
                u8 vr = per_vtx_col ? p->v[i].r : r0;
                u8 vg = per_vtx_col ? p->v[i].g : g0;
                u8 vb = per_vtx_col ? p->v[i].b : b0;
                write_hw_vtx(&v[i], p->v[i].x, p->v[i].y, u_coords[i], v_coords[i], vr, vg, vb, va);
            }
            s32 bx0 = p->v[0].x, bx1 = p->v[0].x, by0 = p->v[0].y, by1 = p->v[0].y;
            for (u32 i = 1u; i < 3u; ++i) {
                if (p->v[i].x < bx0) bx0 = p->v[i].x;
                if (p->v[i].x > bx1) bx1 = p->v[i].x;
                if (p->v[i].y < by0) by0 = p->v[i].y;
                if (p->v[i].y > by1) by1 = p->v[i].y;
            }
            mark_gpu_bbox_clipped(st, bx0, by0, bx1 + 1, by1 + 1,
                                  key.sc_x0, key.sc_y0, key.sc_x1, key.sc_y1);
            break;
        }

        case PRIM_QUAD: {
            bool ok0 = !tri_oversized(p->v[0].x, p->v[0].y, p->v[1].x, p->v[1].y, p->v[2].x, p->v[2].y);
            bool ok1 = !tri_oversized(p->v[1].x, p->v[1].y, p->v[2].x, p->v[2].y, p->v[3].x, p->v[3].y);
            u32 n_emit = (ok0 ? 3u : 0u) + (ok1 ? 3u : 0u);
            if (n_emit == 0u) return;
            GpuHwVertex *v = alloc_batch_verts(st, &key, n_emit);
            if (!v) return;

            u32 out_i = 0u;
            static const u8 k_quad_idx[2][3] = { { 0u, 1u, 2u }, { 1u, 2u, 3u } };
            for (u32 t = 0u; t < 2u; ++t) {
                if ((t == 0u && !ok0) || (t == 1u && !ok1)) {
                    continue;
                }
                for (u32 k = 0; k < 3u; ++k) {
                    u32 i = k_quad_idx[t][k];
                    u8 vr = per_vtx_col ? p->v[i].r : r0;
                    u8 vg = per_vtx_col ? p->v[i].g : g0;
                    u8 vb = per_vtx_col ? p->v[i].b : b0;
                    write_hw_vtx(&v[out_i++], p->v[i].x, p->v[i].y, u_coords[i], v_coords[i],
                                 vr, vg, vb, va);
                }
            }
            s32 bx0 = p->v[0].x, bx1 = p->v[0].x, by0 = p->v[0].y, by1 = p->v[0].y;
            for (u32 i = 1u; i < 4u; ++i) {
                if (p->v[i].x < bx0) bx0 = p->v[i].x;
                if (p->v[i].x > bx1) bx1 = p->v[i].x;
                if (p->v[i].y < by0) by0 = p->v[i].y;
                if (p->v[i].y > by1) by1 = p->v[i].y;
            }
            mark_gpu_bbox_clipped(st, bx0, by0, bx1 + 1, by1 + 1,
                                  key.sc_x0, key.sc_y0, key.sc_x1, key.sc_y1);
            break;
        }

        case PRIM_RECT: {
            if (p->w == 0u || p->h == 0u || p->w > 1023u || p->h > 511u) {
                return;
            }
            s16 x0 = p->v[0].x;
            s16 y0 = p->v[0].y;
            s16 x1 = (s16)(x0 + (s16)p->w);
            s16 y1 = (s16)(y0 + (s16)p->h);

            GpuHwVertex *v = alloc_batch_verts(st, &key, 6u);
            if (!v) return;

            write_hw_vtx(&v[0], x0, y0, u_coords[0], v_coords[0], r0, g0, b0, va);
            write_hw_vtx(&v[1], x1, y0, u_coords[1], v_coords[1], r0, g0, b0, va);
            write_hw_vtx(&v[2], x0, y1, u_coords[2], v_coords[2], r0, g0, b0, va);
            write_hw_vtx(&v[3], x1, y0, u_coords[1], v_coords[1], r0, g0, b0, va);
            write_hw_vtx(&v[4], x1, y1, u_coords[3], v_coords[3], r0, g0, b0, va);
            write_hw_vtx(&v[5], x0, y1, u_coords[2], v_coords[2], r0, g0, b0, va);

            mark_gpu_bbox_clipped(st, x0, y0, x1, y1,
                                  key.sc_x0, key.sc_y0, key.sc_x1, key.sc_y1);
            break;
        }

        case PRIM_LINE:
        case PRIM_POLYLINE_SEG: {
            s32 dx = (s32)p->v[1].x - (s32)p->v[0].x;
            s32 dy = (s32)p->v[1].y - (s32)p->v[0].y;
            s32 adx = dx < 0 ? -dx : dx;
            s32 ady = dy < 0 ? -dy : dy;
            if (adx > 1023 || ady > 511) {
                return;
            }

            GpuVertex e0 = p->v[0];
            GpuVertex e1 = p->v[1];
            if (!gouraud) {
                e1.r = e0.r;
                e1.g = e0.g;
                e1.b = e0.b;
            }

            GpuHwVertex *v = alloc_batch_verts(st, &key, 6u);
            if (!v) return;

            if (adx >= ady) {
                if (e0.x > e1.x) {
                    GpuVertex tmp = e0;
                    e0 = e1;
                    e1 = tmp;
                }
                s16 xA = e0.x, yA = e0.y;
                s16 xB = (s16)(e1.x + 1), yB = e1.y;
                write_hw_vtx(&v[0], xA, yA,            0, 0, e0.r, e0.g, e0.b, 0u);
                write_hw_vtx(&v[1], xB, yB,            0, 0, e1.r, e1.g, e1.b, 0u);
                write_hw_vtx(&v[2], xA, (s16)(yA + 1), 0, 0, e0.r, e0.g, e0.b, 0u);
                write_hw_vtx(&v[3], xB, yB,            0, 0, e1.r, e1.g, e1.b, 0u);
                write_hw_vtx(&v[4], xB, (s16)(yB + 1), 0, 0, e1.r, e1.g, e1.b, 0u);
                write_hw_vtx(&v[5], xA, (s16)(yA + 1), 0, 0, e0.r, e0.g, e0.b, 0u);
            } else {
                if (e0.y > e1.y) {
                    GpuVertex tmp = e0;
                    e0 = e1;
                    e1 = tmp;
                }
                s16 xA = e0.x, yA = e0.y;
                s16 xB = e1.x, yB = (s16)(e1.y + 1);
                write_hw_vtx(&v[0], xA,            yA, 0, 0, e0.r, e0.g, e0.b, 0u);
                write_hw_vtx(&v[1], (s16)(xA + 1), yA, 0, 0, e0.r, e0.g, e0.b, 0u);
                write_hw_vtx(&v[2], xB,            yB, 0, 0, e1.r, e1.g, e1.b, 0u);
                write_hw_vtx(&v[3], (s16)(xA + 1), yA, 0, 0, e0.r, e0.g, e0.b, 0u);
                write_hw_vtx(&v[4], (s16)(xB + 1), yB, 0, 0, e1.r, e1.g, e1.b, 0u);
                write_hw_vtx(&v[5], xB,            yB, 0, 0, e1.r, e1.g, e1.b, 0u);
            }

            s32 bx0 = p->v[0].x < p->v[1].x ? p->v[0].x : p->v[1].x;
            s32 bx1 = p->v[0].x > p->v[1].x ? p->v[0].x : p->v[1].x;
            s32 by0 = p->v[0].y < p->v[1].y ? p->v[0].y : p->v[1].y;
            s32 by1 = p->v[0].y > p->v[1].y ? p->v[0].y : p->v[1].y;
            mark_gpu_bbox_clipped(st, bx0, by0, bx1 + 1, by1 + 1,
                                  key.sc_x0, key.sc_y0, key.sc_x1, key.sc_y1);
            break;
        }
    }
}

static void emit_fill_subrect(GpuHwState *st, u16 x, u16 y, u16 w, u16 h, u8 r, u8 g, u8 b) {
    if (w == 0u || h == 0u) {
        return;
    }
    flush_cpu_dirty_if_needed(st, x, y, w, h);

    GpuHwBatchKey key;
    memset(&key, 0, sizeof(key));
    key.tex_slot     = HW_SLOT_ANY;
    key.tex_mode     = HW_TEX_ANY;
    key.alpha_mode   = HW_ALPHA_OPAQUE_MASKED;
    key.abr          = 0u;
    key.stencil_mode = 0u; /* GP0(02h) ignores check_mask and clears mask bit */
    key.sc_x0        = 0u;
    key.sc_y0        = 0u;
    key.sc_x1        = GPU_HW_VRAM_W;
    key.sc_y1        = GPU_HW_VRAM_H;

    /*
     * GP0(02h) ignores the drawing area, but a fill lying entirely inside the current
     * drawing area renders identically under that scissor, so it can share the batch of
     * the primitives that follow it (typical per-frame "clear + draw" sequence).
     */
    s32 cx0 = st->cur_env.clip_x0 < 0 ? 0 : st->cur_env.clip_x0;
    s32 cy0 = st->cur_env.clip_y0 < 0 ? 0 : st->cur_env.clip_y0;
    s32 cx1 = (s32)st->cur_env.clip_x1 + 1;
    s32 cy1 = (s32)st->cur_env.clip_y1 + 1;
    if (cx1 > (s32)GPU_HW_VRAM_W) cx1 = (s32)GPU_HW_VRAM_W;
    if (cy1 > (s32)GPU_HW_VRAM_H) cy1 = (s32)GPU_HW_VRAM_H;
    if ((s32)x >= cx0 && (s32)y >= cy0 &&
        (s32)x + (s32)w <= cx1 && (s32)y + (s32)h <= cy1) {
        key.sc_x0 = (u16)cx0;
        key.sc_y0 = (u16)cy0;
        key.sc_x1 = (u16)cx1;
        key.sc_y1 = (u16)cy1;
    }

    GpuHwVertex *v = alloc_batch_verts(st, &key, 6u);
    if (!v) return;

    s16 x0 = (s16)x, y0 = (s16)y;
    s16 x1 = (s16)(x + w), y1 = (s16)(y + h);
    write_hw_vtx(&v[0], x0, y0, 0, 0, r, g, b, 0u);
    write_hw_vtx(&v[1], x1, y0, 0, 0, r, g, b, 0u);
    write_hw_vtx(&v[2], x0, y1, 0, 0, r, g, b, 0u);
    write_hw_vtx(&v[3], x1, y0, 0, 0, r, g, b, 0u);
    write_hw_vtx(&v[4], x1, y1, 0, 0, r, g, b, 0u);
    write_hw_vtx(&v[5], x0, y1, 0, 0, r, g, b, 0u);
}

static void hw_fill_rect(void *self, u16 x, u16 y, u16 w, u16 h, u32 bgr24) {
    GpuHwState *st = (GpuHwState *)self;
    if (!st) {
        return;
    }
    x &= 0x3F0u;
    y &= 0x1FFu;
    w = (u16)(((w & 0x3FFu) + 0x0Fu) & ~0x0Fu);
    h &= 0x1FFu;
    if (w == 0u || h == 0u) {
        return;
    }

    gpu_texcache_invalidate_vram(&st->texcache, x, y, w, h);

    u32 r5 = (bgr24 >> 3) & 0x1Fu;
    u32 g5 = (bgr24 >> 11) & 0x1Fu;
    u32 b5 = (bgr24 >> 19) & 0x1Fu;
    u8  r8 = (u8)((r5 << 3) | (r5 >> 2));
    u8  g8 = (u8)((g5 << 3) | (g5 >> 2));
    u8  b8 = (u8)((b5 << 3) | (b5 >> 2));

    u16 w0 = (x + w <= GPU_HW_VRAM_W) ? w : (u16)(GPU_HW_VRAM_W - x);
    u16 w1 = (u16)(w - w0);
    u16 h0 = (y + h <= GPU_HW_VRAM_H) ? h : (u16)(GPU_HW_VRAM_H - y);
    u16 h1 = (u16)(h - h0);

    emit_fill_subrect(st, x,  y,  w0, h0, r8, g8, b8);
    emit_fill_subrect(st, 0u, y,  w1, h0, r8, g8, b8);
    emit_fill_subrect(st, x,  0u, w0, h1, r8, g8, b8);
    emit_fill_subrect(st, 0u, 0u, w1, h1, r8, g8, b8);

    /* Shadow VRAM was already updated by gpu.c before fill_rect, so shadow matches GPU */
    grid_clear_rect(st->gpu_dirty_rows, x, y, w, h);
}

static void hw_vram_written(void *self, u16 x, u16 y, u16 w, u16 h) {
    GpuHwState *st = (GpuHwState *)self;
    if (!st || w == 0u || h == 0u) {
        return;
    }
    gpu_texcache_invalidate_vram(&st->texcache, x, y, w, h);
    mark_cpu_dirty(st, x, y, w, h);
}

static void hw_vram_copy(void *self, u16 sx, u16 sy, u16 dx, u16 dy, u16 w, u16 h) {
    GpuHwState *st = (GpuHwState *)self;
    if (!st || w == 0u || h == 0u) {
        return;
    }

    /*
     * If PICA200 rendered into source or destination cells that shadow VRAM lacked,
     * sync those cells first and replay the masked copy on shadow VRAM.
     */
    bool src_gpu = grid_overlaps_rect(st->gpu_dirty_rows, sx, sy, w, h);
    bool dst_gpu = st->cur_env.check_mask && grid_overlaps_rect(st->gpu_dirty_rows, dx, dy, w, h);
    if ((src_gpu || dst_gpu) && st->sys && st->sys->gpu.vram) {
        if (src_gpu) hw_sync_to_shadow(st, sx, sy, w, h);
        if (dst_gpu) hw_sync_to_shadow(st, dx, dy, w, h);

        u16 *vram = st->sys->gpu.vram;
        u16 set_bit = st->cur_env.set_mask ? 0x8000u : 0x0000u;
        bool chk = st->cur_env.check_mask != 0u;
        bool rev_y = ((dy & 0x1FFu) > (sy & 0x1FFu));
        bool rev_x = ((dx & 0x3FFu) > (sx & 0x3FFu));
        for (u32 iy = 0; iy < (u32)h; ++iy) {
            u32 ry = rev_y ? ((u32)h - 1u - iy) : iy;
            u32 vsy = ((u32)sy + ry) & 0x1FFu;
            u32 vdy = ((u32)dy + ry) & 0x1FFu;
            for (u32 ix = 0; ix < (u32)w; ++ix) {
                u32 rx = rev_x ? ((u32)w - 1u - ix) : ix;
                u32 vsx = ((u32)sx + rx) & 0x3FFu;
                u32 vdx = ((u32)dx + rx) & 0x3FFu;
                if (chk && (vram[(vdy << 10) + vdx] & 0x8000u) != 0u) {
                    continue;
                }
                vram[(vdy << 10) + vdx] = (u16)(vram[(vsy << 10) + vsx] | set_bit);
            }
        }
    }

    gpu_texcache_invalidate_vram(&st->texcache, dx, dy, w, h);
    mark_cpu_dirty(st, dx, dy, w, h);
}

static void compute_display_quad(const PsxConfig *cfg, u16 hres, u16 vres,
                                 s16 *out_x0, s16 *out_y0, s16 *out_x1, s16 *out_y1) {
    u8 scale = cfg ? cfg->scale : (u8)PSX_SCALE_FIT_4_3;
    if (scale == (u8)PSX_SCALE_STRETCH) {
        *out_x0 = 0;
        *out_y0 = 0;
        *out_x1 = 400;
        *out_y1 = 240;
    } else if (scale == (u8)PSX_SCALE_1X1) {
        s16 dw = (hres <= 400u) ? (s16)hres : 400;
        s16 dh = (vres <= 240u) ? (s16)vres : 240;
        *out_x0 = (s16)((400 - dw) / 2);
        *out_y0 = (s16)((240 - dh) / 2);
        *out_x1 = (s16)(*out_x0 + dw);
        *out_y1 = (s16)(*out_y0 + dh);
    } else {
        /* PSX_SCALE_FIT_4_3: centered 320x240 with 40px black pillarboxes */
        *out_x0 = 40;
        *out_y0 = 0;
        *out_x1 = 360;
        *out_y1 = 240;
    }
}

static bool should_skip_display(GpuHwState *st) {
    if (!st->sys) {
        return false;
    }
    u8 fs = st->sys->cfg.frameskip;
    if (fs == 0u) {
        st->skip_counter = 0u;
        return false;
    }
    if (fs == 0xFFu) {
        /* Auto frameskip: skip display pass if previous frame exceeded ~18.5 ms */
        u64 now = plat_time_us();
        u64 dt = (st->last_present_us != 0ULL) ? (now - st->last_present_us) : 0ULL;
        st->last_present_us = now;
        if (dt > 18500ULL && st->skip_counter == 0u) {
            st->skip_counter = 1u;
            return true;
        }
        st->skip_counter = 0u;
        return false;
    }
    u32 period = (u32)(fs <= 3u ? fs : 3u) + 1u;
    st->skip_counter = (st->skip_counter + 1u) % period;
    return st->skip_counter != 0u;
}

static void log_batch_diagnostics(const GpuHwState *st);

static void hw_present(void *self, const GpuDisplay *d) {
    GpuHwState *st = (GpuHwState *)self;
    if (!st) {
        return;
    }
    ++st->n_presents;
#if defined(GPU_HW_DIAG_FILE)
    if ((st->n_presents % 60u) == 0u) {
        log_batch_diagnostics(st);
    }
#endif

    u16 vx   = (d && d->vram_x < GPU_HW_VRAM_W) ? d->vram_x : 0u;
    u16 vy   = (d && d->vram_y < GPU_HW_VRAM_H) ? d->vram_y : 0u;
    u16 hres = (d && d->hres > 0u) ? d->hres : 320u;
    u16 vres = (d && d->vres > 0u) ? d->vres : 240u;
    if (vx + hres > GPU_HW_VRAM_W) hres = (u16)(GPU_HW_VRAM_W - vx);
    if (vy + vres > GPU_HW_VRAM_H) vres = (u16)(GPU_HW_VRAM_H - vy);

    bool is24  = (d && d->color24 != 0u);
    bool blank = (d && d->blank != 0u) || hres == 0u || vres == 0u;

    if (!blank && !is24) {
        flush_cpu_dirty_if_needed(st, vx, vy, hres, vres);
    }
    flush_batch(st);

    bool skip_disp = should_skip_display(st);
    if (skip_disp) {
        if (st->in_frame) {
            C3D_FrameEnd(0);
            st->in_frame = false;
            st->on_vram_rt = false;
        }
        st->cur_ring = (st->cur_ring + 1u) % GPU_HW_VTX_RING_COUNT;
        st->ring_pos = 0u;
        st->batch_first = 0u;
        st->batch_count = 0u;
        st->has_batch_key = false;
        return;
    }

    if (!st->in_frame) {
        begin_frame(st, C3D_FRAME_SYNCDRAW);
    }

    C3D_RenderTargetClear(st->top_rt, C3D_CLEAR_COLOR, 0x000000FFu, 0u);
    C3D_FrameDrawOn(st->top_rt);
    st->on_vram_rt = false;

    if (!blank && st->ring_pos + 6u <= GPU_HW_VTX_RING_VERTS) {
        configure_common_pipeline(st);
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0u, 0u, 240u, 400u);
        C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0x00);
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        set_opaque_blend_state();
        configure_tev_mode(st, HW_TEX_RAW);

        C3D_Mtx proj_top;
        Mtx_OrthoTilt(&proj_top, 0.0f, 400.0f, 240.0f, 0.0f, -1.0f, 1.0f, true);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, st->u_proj, &proj_top);

        GPU_TEXTURE_FILTER_PARAM filt =
            (st->sys && st->sys->cfg.linear_filter) ? GPU_LINEAR : GPU_NEAREST;

        s16 dx0 = 40, dy0 = 0, dx1 = 360, dy1 = 240;
        compute_display_quad(st->sys ? &st->sys->cfg : NULL, hres, vres, &dx0, &dy0, &dx1, &dy1);

        GpuHwVertex *dv = &st->vbo_ring[st->cur_ring][st->ring_pos];
        if (is24 && st->sys && st->sys->gpu.vram) {
            if (!st->disp24_tex_ready) {
                if (C3D_TexInit(&st->disp24_tex, GPU_HW_VRAM_W, GPU_HW_VRAM_H, GPU_RGBA8)) {
                    C3D_TexSetWrap(&st->disp24_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
                    st->disp24_tex_ready = true;
                }
            }
            if (st->disp24_tex_ready) {
                gpu_morton_swizzle_vram_24bpp_8888((u32 *)st->disp24_tex.data,
                                                   GPU_HW_VRAM_W, GPU_HW_VRAM_H,
                                                   st->sys->gpu.vram, vx, vy, hres, vres);
                GSPGPU_FlushDataCache(st->disp24_tex.data,
                                      GPU_HW_VRAM_W * GPU_HW_VRAM_H * sizeof(u32));
                C3D_TexSetFilter(&st->disp24_tex, filt, filt);
                C3D_TexBind(0, &st->disp24_tex);
                set_uv_transform(st, 1.0f / 1024.0f, 1.0f / 512.0f, true);

                s16 u0 = 0, v0 = 0, u1 = (s16)hres, v1 = (s16)vres;
                write_hw_vtx(&dv[0], dx0, dy0, u0, v0, 255, 255, 255, 255);
                write_hw_vtx(&dv[1], dx1, dy0, u1, v0, 255, 255, 255, 255);
                write_hw_vtx(&dv[2], dx0, dy1, u0, v1, 255, 255, 255, 255);
                write_hw_vtx(&dv[3], dx1, dy0, u1, v0, 255, 255, 255, 255);
                write_hw_vtx(&dv[4], dx1, dy1, u1, v1, 255, 255, 255, 255);
                write_hw_vtx(&dv[5], dx0, dy1, u0, v1, 255, 255, 255, 255);

                GSPGPU_FlushDataCache(dv, 6u * sizeof(GpuHwVertex));
                C3D_DrawArrays(GPU_TRIANGLES, (int)st->ring_pos, 6);
                st->ring_pos += 6u;
                st->n_draw_calls++;
            if (st->sys) st->sys->stats.gpu_batches++;
            }
        } else {
            /*
             * vram_tex was rendered with Mtx_Ortho(0, 1024, 512, 0):
             * PSX y = 0 sits at NDC y = +1 (t = 1.0 in PICA200 texture space), and
             * PSX y = 512 sits at NDC y = -1 (t = 0.0), so t = 1.0 - v / 512.0 (flip_v = true).
             */
            C3D_TexSetFilter(&st->vram_tex, filt, filt);
            C3D_TexBind(0, &st->vram_tex);
            set_uv_transform(st, 1.0f / 1024.0f, 1.0f / 512.0f, true);

            s16 u0 = (s16)vx, v0 = (s16)vy;
            s16 u1 = (s16)(vx + hres), v1 = (s16)(vy + vres);
            write_hw_vtx(&dv[0], dx0, dy0, u0, v0, 255, 255, 255, 255);
            write_hw_vtx(&dv[1], dx1, dy0, u1, v0, 255, 255, 255, 255);
            write_hw_vtx(&dv[2], dx0, dy1, u0, v1, 255, 255, 255, 255);
            write_hw_vtx(&dv[3], dx1, dy0, u1, v0, 255, 255, 255, 255);
            write_hw_vtx(&dv[4], dx1, dy1, u1, v1, 255, 255, 255, 255);
            write_hw_vtx(&dv[5], dx0, dy1, u0, v1, 255, 255, 255, 255);

            GSPGPU_FlushDataCache(dv, 6u * sizeof(GpuHwVertex));
            C3D_DrawArrays(GPU_TRIANGLES, (int)st->ring_pos, 6);
            st->ring_pos += 6u;
            st->n_draw_calls++;
            if (st->sys) st->sys->stats.gpu_batches++;
        }
    }

    C3D_FrameEnd(0);
    st->in_frame = false;
    st->on_vram_rt = false;

    st->cur_ring = (st->cur_ring + 1u) % GPU_HW_VTX_RING_COUNT;
    st->ring_pos = 0u;
    st->batch_first = 0u;
    st->batch_count = 0u;
    st->has_batch_key = false;
}

static void log_batch_diagnostics(const GpuHwState *st) {
#if defined(GPU_HW_DIAG_FILE)
    FILE *df = fopen(GPU_HW_DIAG_FILE, "w");
    if (df) {
        fprintf(df, "presents=%u gpu_frames=%u draw_calls=%u scissor=%u alpha=%u abr=%u slot=%u other=%u "
                "cpu_uploads=%u readbacks=%u stalls=%u ring_wraps=%u tex_slots=%u rgba8=%d\n",
                (unsigned)st->n_presents, (unsigned)st->gpu_frame_id, (unsigned)st->n_draw_calls,
                (unsigned)st->brk_scissor, (unsigned)st->brk_alpha, (unsigned)st->brk_abr,
                (unsigned)st->brk_slot, (unsigned)st->brk_other,
                (unsigned)st->n_cpu_upload, (unsigned)st->n_readback, (unsigned)st->n_stall,
                (unsigned)st->n_ring_wrap, (unsigned)st->texcache.max_slots,
                (int)st->texcache.allow_rgba8);
        fclose(df);
    }
#endif
    plat_log("[gpu_citro3d] presents=%u gpu_frames=%u draw_calls=%u breaks{scissor=%u alpha=%u abr=%u "
             "slot=%u other=%u} cpu_uploads=%u readbacks=%u stalls=%u ring_wraps=%u tex_slots=%u rgba8=%d\n",
             (unsigned)st->n_presents, (unsigned)st->gpu_frame_id, (unsigned)st->n_draw_calls,
             (unsigned)st->brk_scissor, (unsigned)st->brk_alpha, (unsigned)st->brk_abr,
             (unsigned)st->brk_slot, (unsigned)st->brk_other,
             (unsigned)st->n_cpu_upload, (unsigned)st->n_readback, (unsigned)st->n_stall,
             (unsigned)st->n_ring_wrap, (unsigned)st->texcache.max_slots,
             (int)st->texcache.allow_rgba8);
}

static void hw_destroy(void *self) {
    GpuHwState *st = (GpuHwState *)self;
    if (!st) {
        return;
    }
    log_batch_diagnostics(st);
    if (st->in_frame) {
        flush_batch(st);
        C3D_FrameEnd(0);
        st->in_frame = false;
    }
    C3D_FrameSync();

    gpu_texcache_shutdown(&st->texcache);

    if (st->readback_buf) {
        linearFree(st->readback_buf);
        st->readback_buf = NULL;
    }
    if (st->disp24_tex_ready) {
        C3D_TexDelete(&st->disp24_tex);
        st->disp24_tex_ready = false;
    }
    if (st->white_tex_ready) {
        C3D_TexDelete(&st->white_tex);
        st->white_tex_ready = false;
    }
    if (st->upload_tex_ready) {
        C3D_TexDelete(&st->upload_tex);
        st->upload_tex_ready = false;
    }
    if (st->top_rt) {
        C3D_RenderTargetDelete(st->top_rt);
        st->top_rt = NULL;
    }
    if (st->vram_rt) {
        C3D_RenderTargetDelete(st->vram_rt);
        st->vram_rt = NULL;
    }
    if (st->vram_tex.data) {
        C3D_TexDelete(&st->vram_tex);
    }
    for (u32 i = 0; i < GPU_HW_VTX_RING_COUNT; ++i) {
        if (st->vbo_ring[i]) {
            linearFree(st->vbo_ring[i]);
            st->vbo_ring[i] = NULL;
        }
    }
    if (st->dvlb) {
        shaderProgramFree(&st->prog);
        DVLB_Free(st->dvlb);
        st->dvlb = NULL;
    }
    free(st->shbin_copy);
    st->shbin_copy = NULL;
    free(st);
}

static const GpuBackend k_gpu_hw_backend = {
    .name           = "citro3d",
    .reset          = hw_reset,
    .set_draw_env   = hw_set_draw_env,
    .draw           = hw_draw,
    .fill_rect      = hw_fill_rect,
    .vram_written   = hw_vram_written,
    .vram_copy      = hw_vram_copy,
    .sync_to_shadow = hw_sync_to_shadow,
    .present        = hw_present,
    .destroy        = hw_destroy,
};

PsxResult gpu_hw_backend_create(PsxSystem *sys, const GpuBackend **be, void **self) {
    if (!be || !self) {
        return PSX_ERR_BAD_ARG;
    }
    *be = NULL;
    *self = NULL;

    GpuHwState *st = (GpuHwState *)calloc(1, sizeof(GpuHwState));
    if (!st) {
        return PSX_ERR_NOMEM;
    }
    st->sys = sys;

    /* Copy shbin out of .rodata because DVLB_ParseFile relocates pointers in-place */
    u32 aligned_sz = (psx_gpu_shbin_size + 3u) & ~3u;
    st->shbin_copy = (u32 *)malloc(aligned_sz);
    if (!st->shbin_copy) {
        hw_destroy(st);
        return PSX_ERR_NOMEM;
    }
    memcpy(st->shbin_copy, psx_gpu_shbin, psx_gpu_shbin_size);

    st->dvlb = DVLB_ParseFile(st->shbin_copy, psx_gpu_shbin_size);
    if (!st->dvlb || st->dvlb->numDVLE == 0u) {
        hw_destroy(st);
        return PSX_ERR_FORMAT;
    }
    shaderProgramInit(&st->prog);
    shaderProgramSetVsh(&st->prog, &st->dvlb->DVLE[0]);

    st->u_proj     = shaderInstanceGetUniformLocation(st->prog.vertexShader, "proj");
    st->u_uv_scale = shaderInstanceGetUniformLocation(st->prog.vertexShader, "uv_scale");
    st->u_uv_bias  = shaderInstanceGetUniformLocation(st->prog.vertexShader, "uv_bias");

    AttrInfo_Init(&st->attr_info);
    AttrInfo_AddLoader(&st->attr_info, 0, GPU_SHORT, 2);
    AttrInfo_AddLoader(&st->attr_info, 1, GPU_SHORT, 2);
    AttrInfo_AddLoader(&st->attr_info, 2, GPU_UNSIGNED_BYTE, 4);
    st->cur_tev = 0xFFu;

    for (u32 i = 0; i < GPU_HW_VTX_RING_COUNT; ++i) {
        st->vbo_ring[i] = (GpuHwVertex *)linearMemAlign(
            GPU_HW_VTX_RING_VERTS * sizeof(GpuHwVertex), 0x80);
        if (!st->vbo_ring[i]) {
            hw_destroy(st);
            return PSX_ERR_NOMEM;
        }
    }

    if (!C3D_TexInitVRAM(&st->vram_tex, GPU_HW_VRAM_W, GPU_HW_VRAM_H, GPU_RGBA5551)) {
        hw_destroy(st);
        return PSX_ERR_NOMEM;
    }
    C3D_TexSetWrap(&st->vram_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexSetFilter(&st->vram_tex, GPU_NEAREST, GPU_NEAREST);

    bool small_mem = (sys != NULL) && sys->cfg.small_mem;
    if (!small_mem) {
        st->vram_rt = C3D_RenderTargetCreateFromTex(&st->vram_tex, GPU_TEXFACE_2D, 0,
                                                    GPU_RB_DEPTH24_STENCIL8);
        if (st->vram_rt != NULL) {
            st->has_stencil = true;
        }
    }
    if (st->vram_rt == NULL) {
        st->vram_rt = C3D_RenderTargetCreateFromTex(&st->vram_tex, GPU_TEXFACE_2D, 0, -1);
        st->has_stencil = false;
    }
    if (st->vram_rt == NULL) {
        hw_destroy(st);
        return PSX_ERR_NOMEM;
    }

    /* 8x8 opaque white texture bound by untextured-only batches */
    if (!C3D_TexInit(&st->white_tex, 8u, 8u, GPU_RGBA5551)) {
        hw_destroy(st);
        return PSX_ERR_NOMEM;
    }
    st->white_tex_ready = true;
    memset(st->white_tex.data, 0xFF, 8u * 8u * sizeof(u16));
    GSPGPU_FlushDataCache(st->white_tex.data, 8u * 8u * sizeof(u16));
    C3D_TexSetWrap(&st->white_tex, GPU_REPEAT, GPU_REPEAT);
    C3D_TexSetFilter(&st->white_tex, GPU_NEAREST, GPU_NEAREST);

    st->top_rt = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, -1);
    if (!st->top_rt) {
        hw_destroy(st);
        return PSX_ERR_NOMEM;
    }
    C3D_RenderTargetSetOutput(st->top_rt, GFX_TOP, GFX_LEFT, TOP_DISPLAY_TRANSFER_FLAGS);

    /* Unrotated 1024x512 PSX VRAM projection: z=0 maps to o_pos.z = -0.5 in [-1.0, 0.0] */
    Mtx_Ortho(&st->proj_vram, 0.0f, 1024.0f, 512.0f, 0.0f, -1.0f, 1.0f, true);

    /*
     * Old 3DS (small_mem): RGBA5551-only pages (no RGBA8 MIXED split) and a 20-slot cap keep
     * the texture cache at <= 2.5 MiB of LINEAR RAM; readback_buf / disp24_tex stay lazy.
     */
    if (!gpu_texcache_init(&st->texcache, !small_mem,
                           small_mem ? GPU_HW_TEXCACHE_SLOTS_SMALL : GPU_HW_TEXCACHE_SLOTS)) {
        hw_destroy(st);
        return PSX_ERR_NOMEM;
    }

    st->cur_env.clip_x1 = (s16)(GPU_HW_VRAM_W - 1u);
    st->cur_env.clip_y1 = (s16)(GPU_HW_VRAM_H - 1u);
    if (sys != NULL && sys->gpu.vram != NULL) {
        mark_cpu_dirty(st, 0u, 0u, GPU_HW_VRAM_W, GPU_HW_VRAM_H);
    }

    *be = &k_gpu_hw_backend;
    *self = st;
    return PSX_OK;
}
