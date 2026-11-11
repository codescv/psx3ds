/*
 * Unit tests for Slice C GP0/GP1 front-end, FIFO state machine, GPUSTAT, GPUINFO,
 * texture-disable latch, polyline termination, and dual GpuBackend dispatch.
 */
#include "psx3ds/arena.h"
#include "psx3ds/gpu.h"
#include "psx3ds/system.h"
#include "psx3ds/timers.h"

#include <stdio.h>
#include <string.h>

static PsxSystem s_sys;

#define CHECK_TRUE(cond, msg)                                                       \
    do {                                                                            \
        if (!(cond)) {                                                              \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, (msg));       \
            return 1;                                                               \
        }                                                                           \
    } while (0)

#define CHECK_EQ_U32(actual, expected, msg)                                         \
    do {                                                                            \
        u32 _a = (u32)(actual);                                                     \
        u32 _e = (u32)(expected);                                                   \
        if (_a != _e) {                                                             \
            fprintf(stderr, "FAIL [%s:%d]: %s (got 0x%08X, expected 0x%08X)\n",     \
                    __FILE__, __LINE__, (msg), (unsigned)_a, (unsigned)_e);         \
            return 1;                                                               \
        }                                                                           \
    } while (0)

/* --------------------------------------------------------------------------
 * Mock GpuBackend to verify dual-dispatch invariants (#35 contract with Slice D)
 * -------------------------------------------------------------------------- */
typedef struct MockBackendState {
    u32      reset_calls;
    u32      env_calls;
    u32      draw_calls;
    u32      fill_calls;
    u32      written_calls;
    u32      copy_calls;
    u32      sync_calls;
    u32      present_calls;
    GpuPrim  last_prims[4];
    u16      last_fill_x, last_fill_y, last_fill_w, last_fill_h;
    u16      sync_x, sync_y, sync_w, sync_h;
    PsxGpu  *gpu;
} MockBackendState;

static void mock_reset(void *self) {
    ((MockBackendState *)self)->reset_calls++;
}
static void mock_set_env(void *self, const GpuDrawEnv *env) {
    (void)env;
    ((MockBackendState *)self)->env_calls++;
}
static void mock_draw(void *self, const GpuPrim *p, const GpuDrawEnv *env) {
    (void)env;
    MockBackendState *m = (MockBackendState *)self;
    if (m->draw_calls < 4u) {
        m->last_prims[m->draw_calls] = *p;
    }
    m->draw_calls++;
}
static void mock_fill(void *self, u16 x, u16 y, u16 w, u16 h, u32 bgr24) {
    (void)bgr24;
    MockBackendState *m = (MockBackendState *)self;
    m->fill_calls++;
    m->last_fill_x = x;
    m->last_fill_y = y;
    m->last_fill_w = w;
    m->last_fill_h = h;
}
static void mock_written(void *self, u16 x, u16 y, u16 w, u16 h) {
    (void)x; (void)y; (void)w; (void)h;
    ((MockBackendState *)self)->written_calls++;
}
static void mock_copy(void *self, u16 sx, u16 sy, u16 dx, u16 dy, u16 w, u16 h) {
    (void)sx; (void)sy; (void)dx; (void)dy; (void)w; (void)h;
    ((MockBackendState *)self)->copy_calls++;
}
static void mock_sync(void *self, u16 x, u16 y, u16 w, u16 h) {
    MockBackendState *m = (MockBackendState *)self;
    m->sync_calls++;
    m->sync_x = x;
    m->sync_y = y;
    m->sync_w = w;
    m->sync_h = h;
    /* Simulate HW backend flushing 0x5678 into shadow VRAM on sync_to_shadow */
    if (m->gpu && m->gpu->vram && w > 0u && h > 0u) {
        m->gpu->vram[(y & 0x1FFu) * PSX_VRAM_W + (x & 0x3FFu)] = 0x5678u;
    }
}
static void mock_present(void *self, const GpuDisplay *d) {
    (void)d;
    ((MockBackendState *)self)->present_calls++;
}

static const GpuBackend s_mock_be = {
    .name           = "mock",
    .reset          = mock_reset,
    .set_draw_env   = mock_set_env,
    .draw           = mock_draw,
    .fill_rect      = mock_fill,
    .vram_written   = mock_written,
    .vram_copy      = mock_copy,
    .sync_to_shadow = mock_sync,
    .present        = mock_present,
    .destroy        = NULL
};

static int test_reset_and_gp1_registers(PsxSystem *sys) {
    gpu_reset(sys);

    /* Power-on / GP1(00h) NTSC GPUSTAT == 0x14802000 */
    CHECK_EQ_U32(gpu_stat(sys), 0x14802000u, "NTSC power-on GPUSTAT == 0x14802000");
    CHECK_TRUE(sys->gpu.disp.hres == 256u && sys->gpu.disp.vres == 240u, "default 256x240");
    CHECK_TRUE(sys->gpu.disp.blank == 1u, "default blank == 1");

    /* IRQ1 GP0(1Fh) & GP1(02h) acknowledge */
    gpu_gp0_write(sys, 0x1F000000u);
    CHECK_TRUE((gpu_stat(sys) & (1u << 24)) != 0u, "GP0(1Fh) sets GPUSTAT.24");
    gpu_gp1_write(sys, 0x02000000u);
    CHECK_TRUE((gpu_stat(sys) & (1u << 24)) == 0u, "GP1(02h) clears GPUSTAT.24");

    /* Display Blank GP1(03h) */
    gpu_gp1_write(sys, 0x03000000u);
    CHECK_TRUE((gpu_stat(sys) & (1u << 23)) == 0u && sys->gpu.disp.blank == 0u,
               "GP1(03h)=0 unblanks display");
    gpu_gp1_write(sys, 0x03000001u);
    CHECK_TRUE((gpu_stat(sys) & (1u << 23)) != 0u && sys->gpu.disp.blank == 1u,
               "GP1(03h)=1 blanks display");

    /* DMA Direction GP1(04h) & GPUSTAT.25 DMA request bit */
    gpu_gp1_write(sys, 0x04000000u); /* Off */
    CHECK_EQ_U32((gpu_stat(sys) >> 29) & 3u, 0u, "dma_dir == 0");
    CHECK_EQ_U32((gpu_stat(sys) >> 25) & 1u, 0u, "dma_req == 0 when dma_dir == 0");

    gpu_gp1_write(sys, 0x04000001u); /* FIFO */
    CHECK_EQ_U32((gpu_stat(sys) >> 29) & 3u, 1u, "dma_dir == 1");
    CHECK_EQ_U32((gpu_stat(sys) >> 25) & 1u, 1u, "dma_req == 1 when FIFO not full");

    gpu_gp1_write(sys, 0x04000002u); /* CPU -> GP0 */
    CHECK_EQ_U32((gpu_stat(sys) >> 29) & 3u, 2u, "dma_dir == 2");
    CHECK_EQ_U32((gpu_stat(sys) >> 25) & 1u, 1u, "dma_req == rdy_dma (1) when idle");

    gpu_gp1_write(sys, 0x04000003u); /* GPUREAD -> CPU */
    CHECK_EQ_U32((gpu_stat(sys) >> 29) & 3u, 3u, "dma_dir == 3");
    CHECK_EQ_U32((gpu_stat(sys) >> 25) & 1u, 0u, "dma_req == rdy_vram (0) when not in C0h");

    /* Display Area Start GP1(05h) — X forced even (& 0x3FE) */
    gpu_gp1_write(sys, 0x05000000u | 321u | (123u << 10));
    CHECK_EQ_U32(sys->gpu.disp.vram_x, 320u, "GP1(05h) forces even vram_x");
    CHECK_EQ_U32(sys->gpu.disp.vram_y, 123u, "GP1(05h) vram_y");

    /* H/V Display Range GP1(06h / 07h) */
    gpu_gp1_write(sys, 0x06000000u | 0x260u | (0xC60u << 12));
    CHECK_TRUE(sys->gpu.disp.x1 == 0x260u && sys->gpu.disp.x2 == 0xC60u, "GP1(06h) HRange");
    gpu_gp1_write(sys, 0x07000000u | 0x018u | (0x108u << 10));
    CHECK_TRUE(sys->gpu.disp.y1 == 0x018u && sys->gpu.disp.y2 == 0x108u, "GP1(07h) VRange");

    /* All GP1(08h) resolutions & mode flags */
    static const struct { u32 mode; u16 w; u16 h; } modes[] = {
        { 0x00u, 256u, 240u },
        { 0x01u, 320u, 240u },
        { 0x02u, 512u, 240u },
        { 0x03u, 640u, 240u },
        { 0x40u, 368u, 240u },
        { 0x04u, 256u, 240u }, /* vres_b=1 WITHOUT interlace stays 240 lines! */
        { 0x24u, 256u, 480u }, /* interlace=1 + vres_b=1 -> 480i */
        { 0x27u, 640u, 480u },
    };
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        gpu_gp1_write(sys, 0x08000000u | modes[i].mode);
        CHECK_EQ_U32(sys->gpu.disp.hres, modes[i].w, "GP1(08h) hres");
        CHECK_EQ_U32(sys->gpu.disp.vres, modes[i].h, "GP1(08h) vres");
    }

    /* PAL + 24bpp + Reverseflag bits */
    gpu_gp1_write(sys, 0x08000000u | (1u << 3) | (1u << 4) | (1u << 7));
    u32 st = gpu_stat(sys);
    CHECK_TRUE(sys->gpu.disp.pal == 1u && ((st >> 20) & 1u) == 1u, "PAL bit 20");
    CHECK_TRUE(sys->gpu.disp.color24 == 1u && ((st >> 21) & 1u) == 1u, "24bpp bit 21");
    CHECK_TRUE(((st >> 14) & 1u) == 1u, "Reverseflag bit 14");

    /* Scanline toggle Bit 31 & Interlace Field Bit 13 */
    gpu_gp1_write(sys, 0x08000001u); /* Progressive 320x240 */
    sys->timers.in_vblank = false;
    sys->timers.scanline  = 0u;
    st = gpu_stat(sys);
    CHECK_TRUE(((st >> 13) & 1u) == 1u && ((st >> 31) & 1u) == 0u,
               "240p even scanline: bit13=1, bit31=0");

    sys->timers.scanline = 1u;
    st = gpu_stat(sys);
    CHECK_TRUE(((st >> 13) & 1u) == 1u && ((st >> 31) & 1u) == 1u,
               "240p odd scanline: bit13=1, bit31=1");

    sys->timers.in_vblank = true;
    st = gpu_stat(sys);
    CHECK_TRUE(((st >> 31) & 1u) == 0u, "240p in_vblank forces bit31=0");

    /* 480i interlaced: bit 13 and bit 31 track odd/even field during active video */
    gpu_gp1_write(sys, 0x08000027u); /* 640x480i */
    sys->timers.in_vblank = false;
    sys->gpu.frame = 0u;
    st = gpu_stat(sys);
    CHECK_TRUE(((st >> 13) & 1u) == 0u && ((st >> 31) & 1u) == 0u,
               "480i even field: bit13=0, bit31=0");
    sys->gpu.frame = 1u;
    st = gpu_stat(sys);
    CHECK_TRUE(((st >> 13) & 1u) == 1u && ((st >> 31) & 1u) == 1u,
               "480i odd field: bit13=1, bit31=1");
    sys->timers.in_vblank = true;
    st = gpu_stat(sys);
    CHECK_TRUE(((st >> 13) & 1u) == 1u && ((st >> 31) & 1u) == 0u,
               "480i in_vblank: bit13=1, bit31=0");

    sys->timers.in_vblank = false;
    sys->timers.scanline  = 0u;
    return 0;
}

static int test_env_and_gpuinfo(PsxSystem *sys) {
    gpu_reset(sys);

    /* E1 Draw Mode */
    gpu_gp0_write(sys, 0xE10007FFu); /* texpage=0x1FF, dither=1, draw_to_display=1 */
    CHECK_EQ_U32(gpu_stat(sys) & 0x7FFu, 0x7FFu, "E1 updates GPUSTAT bits 0..10");
    CHECK_TRUE(sys->gpu.env.dither == 1u && sys->gpu.env.draw_to_display == 1u, "E1 env fields");

    /* E2 Texture Window (20 bits) */
    gpu_gp0_write(sys, 0xE20ABCDEu);
    /* E3 Draw Area Top-Left (X=0x123, Y=0x0AF) */
    gpu_gp0_write(sys, 0xE3000000u | 0x123u | (0x0AFu << 10));
    /* E4 Draw Area Bottom-Right (X=0x2BC, Y=0x1DF) */
    gpu_gp0_write(sys, 0xE4000000u | 0x2BCu | (0x1DFu << 10));
    /* E5 Draw Offset (X = -17 -> 0x7EF, Y = +42 -> 0x02A) */
    u32 off_raw = (0x7EFu) | (0x02Au << 11);
    gpu_gp0_write(sys, 0xE5000000u | off_raw);
    CHECK_TRUE(sys->gpu.env.off_x == -17 && sys->gpu.env.off_y == 42,
               "E5 sign-extends 11-bit signed offsets");

    /* E6 Mask Bit */
    gpu_gp0_write(sys, 0xE6000003u);
    CHECK_EQ_U32((gpu_stat(sys) >> 11) & 3u, 3u, "E6 sets GPUSTAT bits 11..12");

    /* GP1(10h) GetGPUInfo on v2 208-pin GPU */
    gpu_gp1_write(sys, 0x10000002u);
    CHECK_EQ_U32(gpu_read(sys), 0x0ABCDEu, "GPUINFO(2) returns 20-bit TexWindow");

    /* Unassigned index (e.g. 0, 1, 6, 9..15) MUST leave GPUREAD latch unchanged */
    gpu_gp1_write(sys, 0x10000000u);
    CHECK_EQ_U32(gpu_read(sys), 0x0ABCDEu, "GPUINFO(0) leaves GPUREAD unchanged");
    gpu_gp1_write(sys, 0x10000006u);
    CHECK_EQ_U32(gpu_read(sys), 0x0ABCDEu, "GPUINFO(6) leaves GPUREAD unchanged");

    gpu_gp1_write(sys, 0x10000003u);
    CHECK_EQ_U32(gpu_read(sys), 0x123u | (0x0AFu << 10), "GPUINFO(3) DrawArea TL");

    gpu_gp1_write(sys, 0x10000004u);
    CHECK_EQ_U32(gpu_read(sys), 0x2BCu | (0x1DFu << 10), "GPUINFO(4) DrawArea BR");

    gpu_gp1_write(sys, 0x10000005u);
    CHECK_EQ_U32(gpu_read(sys), off_raw, "GPUINFO(5) DrawOffset 22-bit");

    gpu_gp1_write(sys, 0x10000007u);
    CHECK_EQ_U32(gpu_read(sys), 2u, "GPUINFO(7) GPU version == 2");

    gpu_gp1_write(sys, 0x10000008u);
    CHECK_EQ_U32(gpu_read(sys), 0u, "GPUINFO(8) == 0");

    /* Mirrored GP1 command range 0x11..0x1F also decodes GPUINFO */
    gpu_gp1_write(sys, 0x1F000007u);
    CHECK_EQ_U32(gpu_read(sys), 2u, "GP1(1Fh) mirrors GP1(10h)");

    return 0;
}

static int test_gp0_e1_texture_disable_quirk(PsxSystem *sys) {
    gpu_reset(sys);
    gpu_gp0_write(sys, 0xE3000000u);
    gpu_gp0_write(sys, 0xE4000000u | 63u | (63u << 10));

    /* Without GP1(09h)=1, E1 bit 11 is ignored */
    gpu_gp0_write(sys, 0xE1000800u);
    CHECK_EQ_U32((gpu_stat(sys) >> 15) & 1u, 0u, "E1.bit11 ignored when GP1(09h)==0");

    /* Enable allow-tex-disable via GP1(09h)=1, then set E1.bit11=1 */
    gpu_gp1_write(sys, 0x09000001u);
    gpu_gp0_write(sys, 0xE1000800u);
    CHECK_EQ_U32((gpu_stat(sys) >> 15) & 1u, 1u, "GPUSTAT.15 set when GP1(09h)==1 && E1.bit11==1");

    /* Clearing GP1(09h)=0 MUST NOT immediately clear GPUSTAT.15 (gp0-e1.exe rule!) */
    gpu_gp1_write(sys, 0x09000000u);
    CHECK_EQ_U32((gpu_stat(sys) >> 15) & 1u, 1u, "GP1(09h)=0 preserves latched GPUSTAT.15");

    /* Next E1 (even with bit11=1!) sees tex_disable==0 and clears GPUSTAT.15 */
    gpu_gp0_write(sys, 0xE1000800u);
    CHECK_EQ_U32((gpu_stat(sys) >> 15) & 1u, 0u, "Subsequent E1 clears GPUSTAT.15 when GP1(09h)==0");

    /* Textured polygon with tpage.bit11==1 while GP1(09h)==1 disables texturing and draws solid color */
    gpu_gp1_write(sys, 0x09000001u);
    /* Write 0x0000 (transparent) at texpage (256, 0) so textured draw would skip pixels */
    sys->gpu.vram[256] = 0x0000u;
    /* GP0(24h): flat modulated textured triangle, BGR=0x0000FF (red=31 -> 0x001F), tpage=(4 | (2<<7) | (1<<11)) */
    u32 tpage = 4u | (2u << 7) | (1u << 11);
    gpu_gp0_write(sys, 0x240000FFu);
    gpu_gp0_write(sys, (0u)  | (0u << 16));
    gpu_gp0_write(sys, 0x0000u);
    gpu_gp0_write(sys, (8u)  | (0u << 16));
    gpu_gp0_write(sys, 0x0000u | (tpage << 16));
    gpu_gp0_write(sys, (0u)  | (8u << 16));
    gpu_gp0_write(sys, 0x0000u);

    CHECK_EQ_U32((gpu_stat(sys) >> 15) & 1u, 1u, "Textured poly tpage.bit11 sets GPUSTAT.15");
    CHECK_EQ_U32(sys->gpu.vram[1 * PSX_VRAM_W + 1], 0x001Fu,
                 "Texture-disabled textured poly draws solid vertex color");
    return 0;
}

static int test_fifo_and_polylines(PsxSystem *sys) {
    gpu_reset(sys);
    gpu_gp0_write(sys, 0xE3000000u);
    gpu_gp0_write(sys, 0xE4000000u | 127u | (127u << 10));

    /* Mid-command GPUSTAT.26 == 0, GPUSTAT.28 == 1; GP1(01h) aborts cleanly */
    gpu_gp0_write(sys, 0x2000FF00u); /* 4-word flat triangle */
    CHECK_EQ_U32((gpu_stat(sys) >> 26) & 1u, 0u, "GPUSTAT.26 == 0 while collecting GP0 params");
    CHECK_EQ_U32((gpu_stat(sys) >> 28) & 1u, 1u, "GPUSTAT.28 == 1 while collecting GP0 params");
    gpu_gp1_write(sys, 0x01000000u); /* Reset command buffer */
    CHECK_EQ_U32((gpu_stat(sys) >> 26) & 1u, 1u, "GP1(01h) restores GP0_IDLE & GPUSTAT.26 == 1");

    /* Monochrome Polyline GP0(48h): 3 initial words, then vertices until (w & 0xF000F000) == 0x50005000 */
    gpu_gp0_write(sys, 0x480000FFu); /* Red polyline */
    gpu_gp0_write(sys, (10u) | (10u << 16));
    gpu_gp0_write(sys, (20u) | (10u << 16));
    CHECK_EQ_U32((gpu_stat(sys) >> 26) & 1u, 0u, "GPUSTAT.26 == 0 during active polyline");
    gpu_gp0_write(sys, (20u) | (20u << 16));
    CHECK_EQ_U32((gpu_stat(sys) >> 26) & 1u, 0u, "GPUSTAT.26 still 0 after 2nd polyline segment");
    gpu_gp0_write(sys, 0x55555555u); /* Terminator */
    CHECK_EQ_U32((gpu_stat(sys) >> 26) & 1u, 1u, "0x55555555 terminates polyline -> GPUSTAT.26 == 1");
    CHECK_EQ_U32(sys->gpu.vram[10 * PSX_VRAM_W + 15], 0x001Fu, "Polyline seg 1 drawn");
    CHECK_EQ_U32(sys->gpu.vram[15 * PSX_VRAM_W + 20], 0x001Fu, "Polyline seg 2 drawn");

    /* Gouraud Polyline GP0(58h): 0x50005000 in a vertex COORDINATE slot must NOT terminate! */
    gpu_gp0_write(sys, 0x5800FF00u);         /* C0 (Green) */
    gpu_gp0_write(sys, (30u) | (30u << 16)); /* V0 */
    gpu_gp0_write(sys, 0x0000FF00u);         /* C1 (Green) */
    gpu_gp0_write(sys, (40u) | (30u << 16)); /* V1 */
    CHECK_EQ_U32((gpu_stat(sys) >> 26) & 1u, 0u, "Gouraud polyline active");

    gpu_gp0_write(sys, 0x0000FF00u);         /* C2 (color slot, upper nibble 0 != 5) */
    /* Coordinate slot with upper nibbles 0x5...5... (low 11 bits: X=40, Y=40): MUST NOT terminate! */
    gpu_gp0_write(sys, 0x50005000u | 40u | (40u << 16));
    CHECK_EQ_U32((gpu_stat(sys) >> 26) & 1u, 0u,
                 "0x50005000 in Gouraud coordinate slot must NOT terminate polyline");
    /* Next word IS a color/terminator slot -> 0x50005000 terminates! */
    gpu_gp0_write(sys, 0x50005000u);
    CHECK_EQ_U32((gpu_stat(sys) >> 26) & 1u, 1u,
                 "0x50005000 in Gouraud color slot terminates polyline");
    CHECK_EQ_U32(sys->gpu.vram[35 * PSX_VRAM_W + 40], (31u << 5),
                 "Gouraud polyline segment with 0x50005000 coord word rendered");

    return 0;
}

static int test_dual_backend_contract(PsxSystem *sys) {
    gpu_reset(sys);
    const GpuBackend *orig_be   = sys->gpu.be;
    void             *orig_self = sys->gpu.be_self;

    MockBackendState mock;
    memset(&mock, 0, sizeof(mock));
    mock.gpu = &sys->gpu;

    sys->gpu.be      = &s_mock_be;
    sys->gpu.be_self = &mock;

    /* Set draw offset (+100, -50) via E5 and texpage via E1 */
    gpu_gp0_write(sys, 0xE1000025u); /* abr=1, tex_depth=0, tx=5 */
    gpu_gp0_write(sys, 0xE5000000u | (100u & 0x7FFu) | (((u32)(-50) & 0x7FFu) << 11));
    CHECK_EQ_U32(mock.env_calls, 2u, "E1 and E5 notify be->set_draw_env");

    /* Emit untextured semi-transparent quad GP0(2Ah): ONE PRIM_QUAD with offset pre-applied! */
    gpu_gp0_write(sys, 0x2A112233u);
    gpu_gp0_write(sys, (10u) | (60u << 16));
    gpu_gp0_write(sys, (20u) | (60u << 16));
    gpu_gp0_write(sys, (10u) | (70u << 16));
    gpu_gp0_write(sys, (20u) | (70u << 16));

    CHECK_EQ_U32(mock.draw_calls, 1u, "Quad submitted as a single PRIM_QUAD draw call");
    CHECK_TRUE(mock.last_prims[0].kind == PRIM_QUAD, "Quad primitive kind == PRIM_QUAD");
    CHECK_TRUE(mock.last_prims[0].v[0].x == 110 && mock.last_prims[0].v[0].y == 10,
               "Backend receives pre-offset vertex coordinates (v0)");
    CHECK_TRUE(mock.last_prims[0].v[3].x == 120 && mock.last_prims[0].v[3].y == 20,
               "Backend receives pre-offset vertex coordinates (v3, PSX Z-order)");
    CHECK_TRUE(mock.last_prims[0].abr == 1u && (mock.last_prims[0].flags & PF_SEMI) != 0u,
               "Untextured semi-transparent prim carries E1 abr");

    /* Quad whose (1,2,3) half exceeds |dx| > 1023 -> only the (0,1,2) half survives as PRIM_TRI */
    gpu_gp0_write(sys, 0xE5000000u); /* offset (0, 0) */
    mock.draw_calls = 0u;
    gpu_gp0_write(sys, 0x28FFFFFFu);
    gpu_gp0_write(sys, (0u)   | (0u << 16));
    gpu_gp0_write(sys, (10u)  | (0u << 16));
    gpu_gp0_write(sys, (0u)   | (10u << 16));
    gpu_gp0_write(sys, ((u32)(-1020) & 0xFFFFu) | (10u << 16)); /* v3.x = -1020 -> dx(v1,v3)=1030 */
    CHECK_EQ_U32(mock.draw_calls, 1u, "Half-culled quad emits exactly one primitive");
    CHECK_TRUE(mock.last_prims[0].kind == PRIM_TRI &&
               mock.last_prims[0].v[0].x == 0 && mock.last_prims[0].v[1].x == 10,
               "Surviving (0,1,2) half submitted as PRIM_TRI");

    /* Quad where both halves are culled -> zero primitives */
    mock.draw_calls = 0u;
    gpu_gp0_write(sys, 0x28FFFFFFu);
    gpu_gp0_write(sys, ((u32)(-1000) & 0xFFFFu) | (0u << 16));
    gpu_gp0_write(sys, (1000u) | (0u << 16));
    gpu_gp0_write(sys, ((u32)(-1000) & 0xFFFFu) | (10u << 16));
    gpu_gp0_write(sys, (5u)    | (10u << 16));
    CHECK_EQ_U32(mock.draw_calls, 0u, "Fully culled quad emits zero primitives");

    /* C0h VRAM->CPU read MUST call sync_to_shadow BEFORE reading shadow VRAM */
    gpu_gp0_write(sys, 0xC0000000u);
    gpu_gp0_write(sys, (5u) | (6u << 16));
    gpu_gp0_write(sys, (2u) | (1u << 16));
    CHECK_EQ_U32(mock.sync_calls, 1u, "GP0(C0h) calls be->sync_to_shadow before readout");
    u32 read_word = gpu_read(sys);
    CHECK_EQ_U32(read_word & 0xFFFFu, 0x5678u, "GP0(C0h) reads synced shadow VRAM word");

    /* Restore software backend */
    sys->gpu.be      = orig_be;
    sys->gpu.be_self = orig_self;
    return 0;
}

/*
 * v0.2.4 regression (Winning Eleven 3 freeze after the 480i Konami logo): with the real
 * timers_init() -> gpu_init() hook chain driving EV_HBLANK/EV_VBLANK, sys->gpu.frame must
 * advance exactly once per frame, so in 480i GPUSTAT bits 13 & 31 alternate 0,1,0,1 on
 * consecutive frames during active video, and Psy-Q VSync(0) (spin until bit 31 differs
 * from its pre-VBlank value) always terminates.
 */
static int test_480i_field_toggles_every_frame(PsxSystem *sys) {
    sched_init(&sys->sched);
    timers_init(sys);  /* EV_HBLANK = timers, EV_VBLANK = timers default (system order) */
    gpu_reset(sys);    /* GPU chains EV_HBLANK/EV_VBLANK on top of the timers */
    gpu_gp1_write(sys, 0x08000027u); /* 640x480i NTSC interlaced */
    CHECK_TRUE(sys->gpu.disp.interlace && sys->gpu.disp.vres == 480u, "480i display mode");

    sys->stats.frames = 0u;
    u32 frame0 = sys->gpu.frame;
    u32 prev_field = 2u;

    for (u32 f = 0u; f < 8u; f++) {
        /* Advance to active-video scanline 100 of the next frame */
        u32 guard = 0u;
        do {
            sys->sched.fn[EV_HBLANK](sys, 0u);
            guard++;
        } while ((sys->timers.scanline != 100u || sys->timers.in_vblank) && guard < 1000u);
        CHECK_TRUE(guard <= 263u, "reached scanline 100 within one NTSC frame");

        u32 st = gpu_stat(sys);
        u32 b13 = (st >> 13) & 1u;
        u32 b31 = (st >> 31) & 1u;
        CHECK_EQ_U32(sys->gpu.frame - frame0, f, "gpu.frame advances once per frame");
        CHECK_TRUE(b13 == b31, "480i active video: bit13 == bit31 (same field)");
        CHECK_TRUE(b31 == (sys->gpu.frame & 1u), "480i bit31 tracks field parity");
        if (prev_field != 2u) {
            CHECK_TRUE(b31 != prev_field, "480i GPUSTAT bit31 toggles on every consecutive frame");
        }
        prev_field = b31;

        /* Psy-Q VSync(0): latch bit31, wait for VBlank, then spin until bit31 changes */
        u32 s0 = b31;
        u32 frame_before = sys->gpu.frame;
        guard = 0u;
        while (!sys->timers.in_vblank && guard < 1000u) {
            sys->sched.fn[EV_HBLANK](sys, 0u);
            guard++;
        }
        CHECK_TRUE(sys->timers.in_vblank, "reached VBlank");
        CHECK_EQ_U32(sys->gpu.frame, frame_before + 1u, "exactly one frame++ at VBlank entry");
        guard = 0u;
        while (((gpu_stat(sys) >> 31) & 1u) == s0 && guard < 1000u) {
            sys->sched.fn[EV_HBLANK](sys, 0u);
            guard++;
        }
        CHECK_TRUE(guard <= 263u, "VSync(0) bit31 spin terminates within one frame");
        CHECK_EQ_U32(sys->gpu.frame, frame_before + 1u, "no second frame++ at scanline 0");
    }
    CHECK_EQ_U32(sys->gpu.frame - frame0, 8u, "8 frames -> gpu.frame advanced by exactly 8");
    CHECK_EQ_U32(sys->stats.frames, 0u, "VBlank callbacks never touch stats.frames");

    gpu_gp1_write(sys, 0x08000000u);
    sys->timers.in_vblank = false;
    sys->timers.scanline  = 0u;
    return 0;
}

int main(void) {
    memset(&s_sys, 0, sizeof(s_sys));
    psx_config_defaults(&s_sys.cfg, false);
    s_sys.cfg.gpu_backend = GPU_BACKEND_SOFT;

    CHECK_TRUE(arena_init(&s_sys.arena, 4u * 1024u * 1024u) == PSX_OK, "arena_init 4 MiB");
    gpu_init(&s_sys);
    CHECK_TRUE(s_sys.gpu.vram != NULL && s_sys.gpu.be != NULL, "gpu_init allocated VRAM & soft backend");

    if (test_reset_and_gp1_registers(&s_sys) != 0) return 1;
    if (test_env_and_gpuinfo(&s_sys) != 0) return 1;
    if (test_gp0_e1_texture_disable_quirk(&s_sys) != 0) return 1;
    if (test_fifo_and_polylines(&s_sys) != 0) return 1;
    if (test_dual_backend_contract(&s_sys) != 0) return 1;
    if (test_480i_field_toggles_every_frame(&s_sys) != 0) return 1;

    /* Smoke test gpu_state save & load paths */
    StateIO st = { .writing = true, .error = false, .buf = NULL, .pos = 0u, .cap = 0u };
    gpu_state(&s_sys, &st);
    st.writing = false;
    gpu_state(&s_sys, &st);

    gpu_shutdown(&s_sys);
    arena_destroy(&s_sys.arena);
    printf("test_gpu_gp0_gp1 PASS\n");
    return 0;
}
