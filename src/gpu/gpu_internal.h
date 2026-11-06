#ifndef PSX3DS_GPU_INTERNAL_H
#define PSX3DS_GPU_INTERNAL_H

#include "psx3ds/config.h"
#include "psx3ds/gpu.h"
#include "psx3ds/gpu_soft.h"
#include "psx3ds/system.h"

/* --------------------------------------------------------------------------
 * Lookup table: 15-bit PSX 0BGR555 (0..32767) -> 0x00BBGGRR (r5<<3 | g5<<11 | b5<<19)
 * Initialized idempotently by gpu_soft_span_init_tables().
 * -------------------------------------------------------------------------- */
extern u32 g_gpu_rgb555_to_888[32768];

void gpu_soft_span_init_tables(void);

/* --------------------------------------------------------------------------
 * Shadow VRAM operations (src/gpu/gpu_vram.c)
 * -------------------------------------------------------------------------- */
void gpu_vram_clear(u16 *vram);
void gpu_vram_fill_rect(u16 *vram, u16 x, u16 y, u16 w, u16 h, u32 bgr24);
void gpu_vram_copy_rect(u16 *vram, u16 sx, u16 sy, u16 dx, u16 dy,
                        u16 w, u16 h, u8 check_mask, u8 set_mask);

static inline void gpu_vram_write_pixel_masked(u16 *vram, u32 x, u32 y,
                                               u16 pix, u8 check_mask, u8 set_mask) {
    u16 *dst = &vram[((y & 0x1FFu) * PSX_VRAM_W) + (x & 0x3FFu)];
    if (check_mask && (*dst & 0x8000u)) {
        return;
    }
    *dst = (u16)(pix | (set_mask ? 0x8000u : 0u));
}

static inline u16 gpu_vram_read_pixel(const u16 *vram, u32 x, u32 y) {
    return vram[((y & 0x1FFu) * PSX_VRAM_W) + (x & 0x3FFu)];
}

/* Extract active display region into staging buffer (or zero-copy pointer) and present */
void gpu_vram_present(const u16 *vram, u16 *staging_buf, const GpuDisplay *disp);

/* Explicit extraction helpers for unit tests */
void gpu_vram_extract_rgb555(const u16 *vram, u16 vx, u16 vy, u16 w, u16 h,
                             bool blank, u16 *out_rgb555, u16 out_pitch_px);
void gpu_vram_extract_rgb888(const u16 *vram, u16 vx, u16 vy, u16 w, u16 h,
                             bool blank, u8 *out_rgb888, u32 out_row_bytes);

/* --------------------------------------------------------------------------
 * Software backend internal hooks (src/gpu/gpu_soft.c)
 * -------------------------------------------------------------------------- */
bool gpu_soft_is_backend(const GpuBackend *be);
void gpu_soft_invalidate_clut(void *self);

/* --------------------------------------------------------------------------
 * Portable C Reference Spans (src/gpu/gpu_soft_span.c)
 * All span kernels write `count` pixels starting at `dst` (within a single scanline,
 * `check_mask == 0` fast paths). `mask_or` is either `0x0000` or `0x8000`.
 * -------------------------------------------------------------------------- */
void gpu_soft_span_fill16_c(u16 *dst, u16 val, u32 count);

void gpu_soft_span_flat_abr_c(u16 *dst, u32 count, u16 col15, u32 abr, u16 mask_or);

void gpu_soft_span_gouraud_opaque_c(u16 *dst, u32 count,
                                    s32 r_q16, s32 g_q16, s32 b_q16,
                                    s32 dr_q16, s32 dg_q16, s32 db_q16,
                                    u16 mask_or);

void gpu_soft_span_gouraud_abr_c(u16 *dst, u32 count,
                                 s32 r_q16, s32 g_q16, s32 b_q16,
                                 s32 dr_q16, s32 dg_q16, s32 db_q16,
                                 u32 abr, u16 mask_or);

/*
 * Affine non-windowed textured spans using 8.8 dual-packed UV (`uv = (v_q8 << 16) | u_q8`,
 * stepped via `UADD16` on ARMv6K so U and V wrap independently mod 256 in one instruction).
 * `tex_page` points to `&vram[ty_base * 1024 + tx_base]`; row stride is 1024 `u16`s.
 * Wait: note PSX 4bpp/8bpp/15bpp texpage X wrap! Inside a texpage starting at `(tx_base, ty_base)`:
 * - 4bpp: `u` in `0..255` reads `vram[((ty_base + v) & 0x1FF)*1024 + ((tx_base + (u >> 2)) & 0x3FF)]`
 *   Wait: when `tx_base <= 1024 - 64` (all 4bpp pages! `15*64 = 960 <= 960`, `960 + 64 = 1024`!),
 *   `tx_base + (u >> 2)` NEVER wraps past 1023!
 * - 8bpp: when `tx_base <= 1024 - 128` (`tx_base <= 896`), `tx_base + (u >> 1)` NEVER wraps!
 * - 15bpp: when `tx_base <= 1024 - 256` (`tx_base <= 768`), `tx_base + u` NEVER wraps!
 *   And `ty_base` is ALWAYS `0` or `256`, so `ty_base + v` (`v` in `0..255`) NEVER wraps past 511!
 */
void gpu_soft_span_tex4_raw_opaque_c(u16 *dst, u32 count, const u16 *tex_base,
                                     const u16 *clut, u32 uv_pack, u32 duv_pack, u16 mask_or);
void gpu_soft_span_tex8_raw_opaque_c(u16 *dst, u32 count, const u16 *tex_base,
                                     const u16 *clut, u32 uv_pack, u32 duv_pack, u16 mask_or);
void gpu_soft_span_tex16_raw_opaque_c(u16 *dst, u32 count, const u16 *tex_base,
                                      u32 uv_pack, u32 duv_pack, u16 mask_or);

void gpu_soft_span_tex4_mod_flat_opaque_c(u16 *dst, u32 count, const u16 *tex_base,
                                          const u16 *clut, u32 uv_pack, u32 duv_pack,
                                          u32 rgb_mod, u16 mask_or);
void gpu_soft_span_tex8_mod_flat_opaque_c(u16 *dst, u32 count, const u16 *tex_base,
                                          const u16 *clut, u32 uv_pack, u32 duv_pack,
                                          u32 rgb_mod, u16 mask_or);
void gpu_soft_span_tex16_mod_flat_opaque_c(u16 *dst, u32 count, const u16 *tex_base,
                                           u32 uv_pack, u32 duv_pack,
                                           u32 rgb_mod, u16 mask_or);

void gpu_soft_span_tex4_mod_flat_abr_c(u16 *dst, u32 count, const u16 *tex_base,
                                       const u16 *clut, u32 uv_pack, u32 duv_pack,
                                       u32 rgb_mod, u32 abr, u16 mask_or);
void gpu_soft_span_tex8_mod_flat_abr_c(u16 *dst, u32 count, const u16 *tex_base,
                                       const u16 *clut, u32 uv_pack, u32 duv_pack,
                                       u32 rgb_mod, u32 abr, u16 mask_or);
void gpu_soft_span_tex16_mod_flat_abr_c(u16 *dst, u32 count, const u16 *tex_base,
                                        u32 uv_pack, u32 duv_pack,
                                        u32 rgb_mod, u32 abr, u16 mask_or);

/* --------------------------------------------------------------------------
 * ARMv6K SIMD Spans (src/gpu/gpu_soft_span_armv6.S — strictly ARMv6K!)
 * -------------------------------------------------------------------------- */
#if PSX3DS_ARCH_ARMV6K
void gpu_soft_span_fill16_armv6(u16 *dst, u16 val, u32 count);
void gpu_soft_span_flat_abr_armv6(u16 *dst, u32 count, u16 col15, u32 abr, u16 mask_or);

void gpu_soft_span_gouraud_opaque_armv6(u16 *dst, u32 count,
                                        s32 r_q16, s32 g_q16, s32 b_q16,
                                        s32 dr_q16, s32 dg_q16, s32 db_q16,
                                        u16 mask_or);

void gpu_soft_span_gouraud_abr_armv6(u16 *dst, u32 count,
                                     s32 r_q16, s32 g_q16, s32 b_q16,
                                     s32 dr_q16, s32 dg_q16, s32 db_q16,
                                     u32 abr, u16 mask_or);

void gpu_soft_span_tex4_raw_opaque_armv6(u16 *dst, u32 count, const u16 *tex_base,
                                         const u16 *clut, u32 uv_pack, u32 duv_pack, u16 mask_or);
void gpu_soft_span_tex8_raw_opaque_armv6(u16 *dst, u32 count, const u16 *tex_base,
                                         const u16 *clut, u32 uv_pack, u32 duv_pack, u16 mask_or);
void gpu_soft_span_tex16_raw_opaque_armv6(u16 *dst, u32 count, const u16 *tex_base,
                                          u32 uv_pack, u32 duv_pack, u16 mask_or);

void gpu_soft_span_tex4_mod_flat_opaque_armv6(u16 *dst, u32 count, const u16 *tex_base,
                                              const u16 *clut, u32 uv_pack, u32 duv_pack,
                                              u32 rgb_mod, u16 mask_or);
void gpu_soft_span_tex8_mod_flat_opaque_armv6(u16 *dst, u32 count, const u16 *tex_base,
                                              const u16 *clut, u32 uv_pack, u32 duv_pack,
                                              u32 rgb_mod, u16 mask_or);
void gpu_soft_span_tex16_mod_flat_opaque_armv6(u16 *dst, u32 count, const u16 *tex_base,
                                               u32 uv_pack, u32 duv_pack,
                                               u32 rgb_mod, u16 mask_or);

void gpu_soft_span_tex4_mod_flat_abr_armv6(u16 *dst, u32 count, const u16 *tex_base,
                                           const u16 *clut, u32 uv_pack, u32 duv_pack,
                                           u32 rgb_mod, u32 abr, u16 mask_or);
void gpu_soft_span_tex8_mod_flat_abr_armv6(u16 *dst, u32 count, const u16 *tex_base,
                                           const u16 *clut, u32 uv_pack, u32 duv_pack,
                                           u32 rgb_mod, u32 abr, u16 mask_or);
void gpu_soft_span_tex16_mod_flat_abr_armv6(u16 *dst, u32 count, const u16 *tex_base,
                                            u32 uv_pack, u32 duv_pack,
                                            u32 rgb_mod, u32 abr, u16 mask_or);
#endif /* PSX3DS_ARCH_ARMV6K */

/* --------------------------------------------------------------------------
 * Active Target Dispatchers (ARMv6K on T1/T2, Portable C on T3)
 * -------------------------------------------------------------------------- */
#if PSX3DS_ARCH_ARMV6K
#  define gpu_soft_span_fill16                 gpu_soft_span_fill16_armv6
#  define gpu_soft_span_flat_abr               gpu_soft_span_flat_abr_armv6
#  define gpu_soft_span_gouraud_opaque         gpu_soft_span_gouraud_opaque_armv6
#  define gpu_soft_span_gouraud_abr            gpu_soft_span_gouraud_abr_armv6
#  define gpu_soft_span_tex4_raw_opaque        gpu_soft_span_tex4_raw_opaque_armv6
#  define gpu_soft_span_tex8_raw_opaque        gpu_soft_span_tex8_raw_opaque_armv6
#  define gpu_soft_span_tex16_raw_opaque       gpu_soft_span_tex16_raw_opaque_armv6
#  define gpu_soft_span_tex4_mod_flat_opaque   gpu_soft_span_tex4_mod_flat_opaque_armv6
#  define gpu_soft_span_tex8_mod_flat_opaque   gpu_soft_span_tex8_mod_flat_opaque_armv6
#  define gpu_soft_span_tex16_mod_flat_opaque  gpu_soft_span_tex16_mod_flat_opaque_armv6
#  define gpu_soft_span_tex4_mod_flat_abr      gpu_soft_span_tex4_mod_flat_abr_armv6
#  define gpu_soft_span_tex8_mod_flat_abr      gpu_soft_span_tex8_mod_flat_abr_armv6
#  define gpu_soft_span_tex16_mod_flat_abr     gpu_soft_span_tex16_mod_flat_abr_armv6
#else
#  define gpu_soft_span_fill16                 gpu_soft_span_fill16_c
#  define gpu_soft_span_flat_abr               gpu_soft_span_flat_abr_c
#  define gpu_soft_span_gouraud_opaque         gpu_soft_span_gouraud_opaque_c
#  define gpu_soft_span_gouraud_abr            gpu_soft_span_gouraud_abr_c
#  define gpu_soft_span_tex4_raw_opaque        gpu_soft_span_tex4_raw_opaque_c
#  define gpu_soft_span_tex8_raw_opaque        gpu_soft_span_tex8_raw_opaque_c
#  define gpu_soft_span_tex16_raw_opaque       gpu_soft_span_tex16_raw_opaque_c
#  define gpu_soft_span_tex4_mod_flat_opaque   gpu_soft_span_tex4_mod_flat_opaque_c
#  define gpu_soft_span_tex8_mod_flat_opaque   gpu_soft_span_tex8_mod_flat_opaque_c
#  define gpu_soft_span_tex16_mod_flat_opaque  gpu_soft_span_tex16_mod_flat_opaque_c
#  define gpu_soft_span_tex4_mod_flat_abr      gpu_soft_span_tex4_mod_flat_abr_c
#  define gpu_soft_span_tex8_mod_flat_abr      gpu_soft_span_tex8_mod_flat_abr_c
#  define gpu_soft_span_tex16_mod_flat_abr     gpu_soft_span_tex16_mod_flat_abr_c
#endif

/* General fallback span processor (handles dither, check_mask, TexWindow, Gouraud+Tex, raw+ABR) */
typedef struct GpuSpanParams {
    u16       *dst_row;
    const u16 *vram;
    const u16 *clut;
    s32        x_start;
    s32        y;
    u32        count;

    s32        r_q16, g_q16, b_q16;
    s32        dr_q16, dg_q16, db_q16;

    s32        u_q16, v_q16;
    s32        du_q16, dv_q16;

    u16        tx_base;
    u16        ty_base;
    u8         tex_depth;
    u8         abr;
    u8         flags;      /* GpuPrimFlags */
    u8         dither;     /* 0 or 1 (already gated on prim type!) */
    u8         check_mask;
    u8         set_mask;
    u8         tw_and_x, tw_or_x;
    u8         tw_and_y, tw_or_y;
} GpuSpanParams;

void gpu_soft_span_general(const GpuSpanParams *sp);

/* Exact PSX single-pixel ABR blend helper (shared by general spans & lines) */
static inline u16 gpu_blend_abr15(u16 bg16, u16 fg16, u32 abr) {
    u32 b8 = g_gpu_rgb555_to_888[bg16 & 0x7FFFu];
    u32 f8 = g_gpu_rgb555_to_888[fg16 & 0x7FFFu];
    u32 out8;
    switch (abr & 3u) {
        case 0u: /* 0.5B + 0.5F */
            out8 = (b8 >> 1) + (f8 >> 1);
            break;
        case 1u: { /* 1.0B + 1.0F */
            u32 r = (b8 & 0xFFu) + (f8 & 0xFFu);
            u32 g = ((b8 >> 8) & 0xFFu) + ((f8 >> 8) & 0xFFu);
            u32 b = ((b8 >> 16) & 0xFFu) + ((f8 >> 16) & 0xFFu);
            if (r > 255u) r = 255u;
            if (g > 255u) g = 255u;
            if (b > 255u) b = 255u;
            out8 = r | (g << 8) | (b << 16);
            break;
        }
        case 2u: { /* 1.0B - 1.0F */
            s32 r = (s32)(b8 & 0xFFu) - (s32)(f8 & 0xFFu);
            s32 g = (s32)((b8 >> 8) & 0xFFu) - (s32)((f8 >> 8) & 0xFFu);
            s32 b = (s32)((b8 >> 16) & 0xFFu) - (s32)((f8 >> 16) & 0xFFu);
            if (r < 0) r = 0;
            if (g < 0) g = 0;
            if (b < 0) b = 0;
            out8 = (u32)r | ((u32)g << 8) | ((u32)b << 16);
            break;
        }
        default: { /* 1.0B + 0.25F */
            u32 fq = f8 >> 2;
            u32 r = (b8 & 0xFFu) + (fq & 0xFFu);
            u32 g = ((b8 >> 8) & 0xFFu) + ((fq >> 8) & 0xFFu);
            u32 b = ((b8 >> 16) & 0xFFu) + ((fq >> 16) & 0xFFu);
            if (r > 255u) r = 255u;
            if (g > 255u) g = 255u;
            if (b > 255u) b = 255u;
            out8 = r | (g << 8) | (b << 16);
            break;
        }
    }
    return (u16)(((out8 >> 3) & 0x1Fu) |
                 (((out8 >> 11) & 0x1Fu) << 5) |
                 (((out8 >> 19) & 0x1Fu) << 10));
}

#endif /* PSX3DS_GPU_INTERNAL_H */
