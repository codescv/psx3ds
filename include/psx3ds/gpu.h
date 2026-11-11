#ifndef PSX3DS_GPU_H
#define PSX3DS_GPU_H

#include "psx3ds/hw_regs.h"
#include "psx3ds/stats.h"
#include "psx3ds/types.h"

typedef struct GpuVertex {
    s16 x, y;
    u8  r, g, b, _pad;
    u8  u, v;
    u16 _pad2;
} GpuVertex;

_Static_assert(sizeof(GpuVertex) == 12, "GpuVertex must be 12 bytes");

typedef enum GpuPrimKind {
    PRIM_TRI = 0,
    PRIM_QUAD,
    PRIM_LINE,
    PRIM_POLYLINE_SEG,
    PRIM_RECT
} GpuPrimKind;

enum GpuPrimFlags {
    PF_GOURAUD     = 1,
    PF_TEXTURED    = 2,
    PF_RAW_TEX     = 4,
    PF_SEMI        = 8,
    PF_RECT_FLIP_X = 16,
    PF_RECT_FLIP_Y = 32
};

typedef struct GpuPrim {
    u8        kind, flags, abr, tex_depth;
    u16       texpage;
    u16       clut;
    u16       w, h;
    GpuVertex v[4];
} GpuPrim;

typedef struct GpuDrawEnv {
    s16 clip_x0, clip_y0, clip_x1, clip_y1;
    s16 off_x, off_y;
    u8  tw_mask_x, tw_mask_y, tw_off_x, tw_off_y;
    u8  set_mask, check_mask, dither, draw_to_display;
    u16 texpage_e1;
} GpuDrawEnv;

typedef struct GpuDisplay {
    u16 vram_x, vram_y;
    u16 hres, vres;
    u8  interlace, color24, blank, pal;
    u16 x1, x2, y1, y2;
} GpuDisplay;

typedef struct GpuBackend {
    const char *name;
    void (*reset)(void *self);
    void (*set_draw_env)(void *self, const GpuDrawEnv *env);
    void (*draw)(void *self, const GpuPrim *p, const GpuDrawEnv *env);
    void (*fill_rect)(void *self, u16 x, u16 y, u16 w, u16 h, u32 bgr24);
    void (*vram_written)(void *self, u16 x, u16 y, u16 w, u16 h);
    void (*vram_copy)(void *self, u16 sx, u16 sy, u16 dx, u16 dy, u16 w, u16 h);
    void (*sync_to_shadow)(void *self, u16 x, u16 y, u16 w, u16 h);
    void (*present)(void *self, const GpuDisplay *d);
    void (*destroy)(void *self);
} GpuBackend;

typedef struct PsxGpu {
    u16       *vram; /* 1024x512 16-bit shadow VRAM */
    u32        gpustat;
    GpuDrawEnv env;
    GpuDisplay disp;

    enum {
        GP0_IDLE = 0,
        GP0_COLLECT,
        GP0_POLYLINE,
        GP0_IMG_LOAD,
        GP0_IMG_STORE
    } st;
    u32  fifo[16];
    u8   fifo_n, fifo_need;
    u8   cmd;

    u16  xfer_x, xfer_y, xfer_w, xfer_h, xfer_cx, xfer_cy;
    u32  xfer_words_left;
    u32  gpuread_latch;
    bool gpuread_valid;

    u32  irq1;
    u8   dma_dir;
    u8   tex_disable;

    const GpuBackend *be;
    void             *be_self;

    u32       frame;
    PsxStats *stats;
} PsxGpu;

void gpu_init(PsxSystem *sys);
void gpu_reset(PsxSystem *sys);
void gpu_state(PsxSystem *sys, StateIO *st);
void gpu_shutdown(PsxSystem *sys);

void gpu_gp0_write(PsxSystem *sys, u32 word);
void gpu_dma_write_words(PsxSystem *sys, const u32 *src, u32 n);
u32  gpu_dma_read_words(PsxSystem *sys, u32 *dst, u32 n);
void gpu_gp1_write(PsxSystem *sys, u32 word);
u32  gpu_read(PsxSystem *sys);
u32  gpu_stat(PsxSystem *sys);

/*
 * GP0(80h) VRAM->VRAM copy that cannot change VRAM: source == destination (modulo the
 * 1024x512 wrap) with mask-bit test and set both disabled. Backends and gpu.c skip it.
 */
static inline bool gpu_vram_copy_is_noop(u16 sx, u16 sy, u16 dx, u16 dy, u8 check_mask,
                                         u8 set_mask) {
    return ((sx & 0x3FFu) == (dx & 0x3FFu)) && ((sy & 0x1FFu) == (dy & 0x1FFu)) &&
           check_mask == 0u && set_mask == 0u;
}

#endif /* PSX3DS_GPU_H */
