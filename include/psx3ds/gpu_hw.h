#ifndef PSX3DS_GPU_HW_H
#define PSX3DS_GPU_HW_H

#include "psx3ds/config.h"
#include "psx3ds/gpu.h"

/* --------------------------------------------------------------------------
 * Constants & Portable Types (shared across T1 3DS, T2 ARM11, T3 Host tests)
 * -------------------------------------------------------------------------- */
#define GPU_HW_VRAM_W            1024u
#define GPU_HW_VRAM_H            512u
#define GPU_HW_TEXPAGE_DIM       256u
#define GPU_HW_TEXPAGE_PIXELS    (GPU_HW_TEXPAGE_DIM * GPU_HW_TEXPAGE_DIM)
#define GPU_HW_TEXCACHE_SLOTS    64u
#define GPU_HW_VTX_RING_COUNT    2u
#define GPU_HW_VTX_RING_VERTS    4096u

/* 64x32 grid of 16x16 VRAM cells (col bitmask fits in u64, row bitmask in u32) */
#define GPU_HW_CELL_SHIFT        4u
#define GPU_HW_CELL_SIZE         (1u << GPU_HW_CELL_SHIFT)
#define GPU_HW_CELL_COLS         (GPU_HW_VRAM_W >> GPU_HW_CELL_SHIFT) /* 64 */
#define GPU_HW_CELL_ROWS         (GPU_HW_VRAM_H >> GPU_HW_CELL_SHIFT) /* 32 */

typedef enum GpuTexAlphaClass {
    GPU_TEX_ALPHA_ALL_OPAQUE = 0, /* No non-zero texel has STP=1 (RGBA5551, 1 pass) */
    GPU_TEX_ALPHA_ALL_STP    = 1, /* Every non-zero texel has STP=1 (RGBA5551, 1 pass) */
    GPU_TEX_ALPHA_MIXED      = 2  /* Both STP=0 non-zero and STP=1 texels present (RGBA8, 2-pass) */
} GpuTexAlphaClass;

typedef struct GpuTexWindowKey {
    u8 mask_x;
    u8 mask_y;
    u8 off_x;
    u8 off_y;
} GpuTexWindowKey;

/* Per-8x8-tile STP presence bitmasks for a 256x256 page (bit tx of row ty) */
typedef struct GpuTexTileMasks {
    u32 stp0[32]; /* tile holds >= 1 non-zero STP=0 texel */
    u32 stp1[32]; /* tile holds >= 1 STP=1 texel (incl. 0x8000) */
} GpuTexTileMasks;

typedef struct GpuTexSlot {
    bool             valid;
    bool             texel_dirty;
    bool             clut_dirty;
    bool             has_clut;
    bool             is_rgba8;
    u8               tex_depth;   /* 0 = 4bpp, 1 = 8bpp, 2 = 15bpp */
    u8               alpha_class; /* GpuTexAlphaClass */
    u16              texpage_key; /* normalized texpage bits [8:7, 4:0] */
    u16              clut_attr;   /* normalized clut bits [14:0] (0 for 15bpp) */
    GpuTexWindowKey  tw;          /* non-zero only when pre-wrapped for tiled window */
    u32              clut_hash;   /* 32-bit FNV-1a of CLUT words (0 for 15bpp) */
    u32              last_used;   /* monotonic LRU timestamp */
    u32              decode_gen;  /* incremented every time this slot re-decodes texels */

    u64              tp_col_mask;
    u32              tp_row_mask;
    u64              clut_col_mask;
    u32              clut_row_bit;

    u32              last_frame;  /* GPU frame id that last referenced this slot (0 = never) */
    GpuTexTileMasks  tiles;       /* per-8x8-tile STP=0 / STP=1 presence */

    void            *pixels;      /* Morton-tiled pixel buffer (u16 RGBA5551 or u32 RGBA8) */
    u32              pixel_bytes;
    void            *hw_tex;      /* C3D_Tex* on 3DS (NULL on T2/T3 host builds) */
} GpuTexSlot;

typedef struct GpuTexCache {
    GpuTexSlot slots[GPU_HW_TEXCACHE_SLOTS];
    u32        lru_clock;
    u32        hits;
    u32        uploads;
    u32        max_slots;   /* active slot count (64, or fewer on Old 3DS small_mem) */
    u32        mru;         /* index of the most recently returned slot */
    u32        cur_frame;   /* in-flight GPU frame id (0 = no frame tracking) */
    bool       stalled;     /* last lookup failed: every slot is referenced by cur_frame */
    bool       allow_rgba8; /* true by default; false if small_mem forces RGBA5551 */
} GpuTexCache;

/* --------------------------------------------------------------------------
 * Morton Swizzling & PSX -> PICA200 Pixel Conversion (gpu_morton.c)
 * -------------------------------------------------------------------------- */
extern const u8 gpu_morton8x8_lut[8][8];

u32 gpu_morton_index(u32 x, u32 y, u32 width);

/* Textured conversion: PSX 0x0000 -> 0x0000 (A=0), non-zero (incl. 0x8000) -> R5G5B5A1 (A=1) */
static inline u16 gpu_psx_to_rgba5551(u16 psx) {
    if (psx == 0x0000u) {
        return 0x0000u;
    }
    u32 r5 = (u32)(psx & 0x001Fu);
    u32 g5 = (u32)((psx >> 5) & 0x001Fu);
    u32 b5 = (u32)((psx >> 10) & 0x001Fu);
    return (u16)((r5 << 11) | (g5 << 6) | (b5 << 1) | 1u);
}

/* Untextured / display VRAM conversion: always opaque A=1 (even for 0x0000 black) */
static inline u16 gpu_psx_to_rgba5551_opaque(u16 psx) {
    u32 r5 = (u32)(psx & 0x001Fu);
    u32 g5 = (u32)((psx >> 5) & 0x001Fu);
    u32 b5 = (u32)((psx >> 10) & 0x001Fu);
    return (u16)((r5 << 11) | (g5 << 6) | (b5 << 1) | 1u);
}

/* Reverse conversion for GPU -> shadow VRAM readback (RGBA5551 -> PSX ABGR1555) */
static inline u16 gpu_rgba5551_to_psx(u16 pica) {
    u32 r5 = (u32)((pica >> 11) & 0x1Fu);
    u32 g5 = (u32)((pica >> 6) & 0x1Fu);
    u32 b5 = (u32)((pica >> 1) & 0x1Fu);
    return (u16)((b5 << 10) | (g5 << 5) | r5);
}

/* MIXED STP conversion for GPU_RGBA8: A=0x00 (0x0000), A=0x80 (STP=1), A=0xFF (STP=0 non-zero) */
static inline u32 gpu_psx_to_rgba8888(u16 psx) {
    if (psx == 0x0000u) {
        return 0x00000000u;
    }
    u32 r5 = (u32)(psx & 0x001Fu);
    u32 g5 = (u32)((psx >> 5) & 0x001Fu);
    u32 b5 = (u32)((psx >> 10) & 0x001Fu);
    u32 r8 = (r5 << 3) | (r5 >> 2);
    u32 g8 = (g5 << 3) | (g5 >> 2);
    u32 b8 = (b5 << 3) | (b5 >> 2);
    u32 a8 = (psx & 0x8000u) ? 0x80u : 0xFFu;
    return (r8 << 24) | (g8 << 16) | (b8 << 8) | a8;
}

/* All decoders accept tm == NULL; when non-NULL they fill per-8x8-tile STP masks. */
GpuTexAlphaClass gpu_morton_decode_4bpp_5551(u16 *dst_morton, const u16 *vram,
                                             u16 tp_x, u16 tp_y, u16 clut_x, u16 clut_y,
                                             const GpuTexWindowKey *tw, GpuTexTileMasks *tm);
GpuTexAlphaClass gpu_morton_decode_8bpp_5551(u16 *dst_morton, const u16 *vram,
                                             u16 tp_x, u16 tp_y, u16 clut_x, u16 clut_y,
                                             const GpuTexWindowKey *tw, GpuTexTileMasks *tm);
GpuTexAlphaClass gpu_morton_decode_15bpp_5551(u16 *dst_morton, const u16 *vram,
                                              u16 tp_x, u16 tp_y,
                                              const GpuTexWindowKey *tw, GpuTexTileMasks *tm);

GpuTexAlphaClass gpu_morton_decode_4bpp_8888(u32 *dst_morton, const u16 *vram,
                                             u16 tp_x, u16 tp_y, u16 clut_x, u16 clut_y,
                                             const GpuTexWindowKey *tw, GpuTexTileMasks *tm);
GpuTexAlphaClass gpu_morton_decode_8bpp_8888(u32 *dst_morton, const u16 *vram,
                                             u16 tp_x, u16 tp_y, u16 clut_x, u16 clut_y,
                                             const GpuTexWindowKey *tw, GpuTexTileMasks *tm);
GpuTexAlphaClass gpu_morton_decode_15bpp_8888(u32 *dst_morton, const u16 *vram,
                                              u16 tp_x, u16 tp_y,
                                              const GpuTexWindowKey *tw, GpuTexTileMasks *tm);

/*
 * Classify the inclusive texel box [u_lo..u_hi] x [v_lo..v_hi] of a decoded page:
 * ALL_OPAQUE (no STP=1 texel), ALL_STP (no non-zero STP=0 texel) or MIXED.
 * Spans >= 256 texels (or hi < lo) are treated as covering the whole axis.
 */
GpuTexAlphaClass gpu_tex_tiles_classify(const GpuTexTileMasks *tm,
                                        s32 u_lo, s32 v_lo, s32 u_hi, s32 v_hi);

/* Swizzle an arbitrary linear PSX VRAM subrect into a WxH RGBA5551 Morton texture (opaque A=1) */
void gpu_morton_swizzle_vram_rect_5551(u16 *dst_morton, u32 dst_w, u32 dst_h,
                                       const u16 *vram, u16 src_x, u16 src_y, u16 w, u16 h);

/* Swizzle a 24-bit RGB display region from PSX shadow VRAM into a WxH RGBA8 Morton texture */
void gpu_morton_swizzle_vram_24bpp_8888(u32 *dst_morton, u32 dst_w, u32 dst_h,
                                        const u16 *vram, u16 src_x, u16 src_y, u16 w_px, u16 h_px);

/* --------------------------------------------------------------------------
 * 64-Slot LRU CLUT + TexPage Cache (gpu_texcache.c)
 * -------------------------------------------------------------------------- */
u32  gpu_clut_hash_fnv1a(const u16 *vram, u16 clut_attr, u8 tex_depth);
void gpu_vram_rect_cell_masks(u16 x, u16 y, u16 w, u16 h, u64 *out_cols, u32 *out_rows);

/* Old 3DS (small_mem) slot cap: 20 x 128 KiB RGBA5551 pages = 2.5 MiB LINEAR RAM */
#define GPU_HW_TEXCACHE_SLOTS_SMALL 20u

/* max_slots is clamped to [1, GPU_HW_TEXCACHE_SLOTS] (0 selects the full 64) */
bool gpu_texcache_init(GpuTexCache *tc, bool allow_rgba8, u32 max_slots);

/*
 * In-flight tracking: slots referenced while tc->cur_frame == F are never re-decoded,
 * re-allocated or freed until cur_frame advances (the PICA200 has not consumed them yet).
 * When every active slot is in flight, lookup returns NULL and sets tc->stalled; the
 * backend must submit + wait for the GPU, advance cur_frame and retry.
 */
static inline void gpu_texcache_set_frame(GpuTexCache *tc, u32 frame_id) {
    tc->cur_frame = frame_id;
}
void gpu_texcache_reset(GpuTexCache *tc);
void gpu_texcache_shutdown(GpuTexCache *tc);
void gpu_texcache_invalidate_vram(GpuTexCache *tc, u16 x, u16 y, u16 w, u16 h);

const GpuTexSlot *gpu_texcache_lookup(GpuTexCache *tc, const u16 *vram,
                                      u16 texpage, u8 tex_depth, u16 clut,
                                      const GpuTexWindowKey *tw,
                                      PsxStats *stats, bool *out_hit);

/* --------------------------------------------------------------------------
 * Hardware Backend Entry Point (gpu_citro3d.c on 3DS, gpu_hw_stub.c on host)
 * -------------------------------------------------------------------------- */
PsxResult gpu_hw_backend_create(PsxSystem *sys, const GpuBackend **be, void **self);

#endif /* PSX3DS_GPU_HW_H */
