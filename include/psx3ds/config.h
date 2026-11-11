#ifndef PSX3DS_CONFIG_H
#define PSX3DS_CONFIG_H

#include "psx3ds/types.h"

/* --------------------------------------------------------------------------
 * Semantic version constants (v0.2.6)
 * -------------------------------------------------------------------------- */
#define PSX3DS_VERSION_MAJOR 0
#define PSX3DS_VERSION_MINOR 2
#define PSX3DS_VERSION_PATCH 6
#define PSX3DS_VERSION_STR   "0.2.6"

/* --------------------------------------------------------------------------
 * Compile-time platform & architecture detection (single source of truth)
 * -------------------------------------------------------------------------- */
#if defined(__3DS__)
#  define PSX3DS_PLATFORM_3DS  1
#  define PSX3DS_PLATFORM_HOST 0
#else
#  define PSX3DS_PLATFORM_3DS  0
#  define PSX3DS_PLATFORM_HOST 1
#endif

#if defined(__arm__) && (defined(__ARM_ARCH_6K__) || defined(__ARM_ARCH_6ZK__) || \
                         (defined(__ARM_ARCH) && __ARM_ARCH == 6))
#  define PSX3DS_ARCH_ARMV6K 1
#  if !defined(PSX3DS_DISABLE_JIT)
#    define PSX3DS_HAVE_JIT 1
#  else
#    define PSX3DS_HAVE_JIT 0
#  endif
#else
#  define PSX3DS_ARCH_ARMV6K 0
#  define PSX3DS_HAVE_JIT    0
#endif

#if PSX3DS_PLATFORM_3DS && !defined(PSX3DS_DISABLE_HW_GPU)
#  define PSX3DS_HAVE_CITRO3D 1
#else
#  define PSX3DS_HAVE_CITRO3D 0
#endif

_Static_assert(sizeof(void *) == 4 || !PSX3DS_HAVE_JIT, "arm_jit is 32-bit only");

#ifndef PSX3DS_CYCLE_MULT_DEFAULT
#  define PSX3DS_CYCLE_MULT_DEFAULT 448
#endif

/* --------------------------------------------------------------------------
 * Runtime configuration (populated by CLI flags or 3DS menu before init)
 * -------------------------------------------------------------------------- */
typedef enum PsxCpuEngine {
    CPU_AUTO = 0,
    CPU_INTERP,
    CPU_CACHED,
    CPU_JIT
} PsxCpuEngine;

typedef enum PsxGpuBackendKind {
    GPU_BACKEND_SOFT = 0,
    GPU_BACKEND_HW
} PsxGpuBackendKind;

typedef enum PsxRegion {
    PSX_REGION_AUTO = 0,
    PSX_REGION_NTSC_U,
    PSX_REGION_NTSC_J,
    PSX_REGION_PAL
} PsxRegion;

typedef enum PsxVideoScale {
    PSX_SCALE_1X1 = 0,
    PSX_SCALE_STRETCH,
    PSX_SCALE_FIT_4_3
} PsxVideoScale;

typedef struct PsxConfig {
    PsxCpuEngine      cpu_engine;
    PsxGpuBackendKind gpu_backend;
    PsxRegion         region;

    u16 cycle_mult;      /* 8.8 fixed-point, default PSX3DS_CYCLE_MULT_DEFAULT (448) */
    u8  cycles_per_mem;  /* extra cycles per load/store, default 0 */
    u8  frameskip;       /* 0..3, 0xFF = auto */
    u8  scale;           /* PsxVideoScale (0 = 1:1, 1 = stretch, 2 = 4:3-fit) */
    bool linear_filter;  /* top-screen linear filtering toggle */

    bool jit_ibtc;
    bool jit_link;
    bool jit_idle_skip;
    bool jit_check_align;
    bool jit_check_kuc;
    bool jit_inline_gte;
    bool jit_allow_hle_exec;
    bool isc_swap_table;

    bool strict_ram_size;
    bool gpu_timing;
    bool spu_async;
    bool spu_reverb;
    bool small_mem;
    bool hle_bios;
    bool dither_hw;
    bool cd_fast_boot; /* real BIOS: skip native CD boot, sideload SYSTEM.CNF EXE at 0x80030000 */

    u32 smc_trap_budget;          /* default UINT32_MAX (adaptive VERIFY disabled) */
    u32 jit_code_bytes;
    u32 jit_max_blocks;
    u32 dma_ll_max_nodes_per_tick;
    u32 dma_cycles_per_word;

    char bios_path[256];
    char memcard_path[2][256];
} PsxConfig;

void psx_config_defaults(PsxConfig *cfg, bool is_n3ds);

#endif /* PSX3DS_CONFIG_H */
