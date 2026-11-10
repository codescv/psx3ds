#ifndef PSX3DS_CTR_HEAP_H
#define PSX3DS_CTR_HEAP_H

/*
 * Nintendo 3DS startup heap split (normal heap vs. linear heap), v0.2.1+.
 *
 * Pure, platform-independent arithmetic so it is unit-tested on all three tiers
 * (T3 x86_64, T2 ARM11 QEMU, T1 devkitARM) and used verbatim by the
 * `__system_allocateHeaps` override in src/platform/3ds/main_3ds.c.
 *
 * Background (v0.2.0 New 3DS startup crash, svcBreak(USERBREAK_PANIC)):
 *   v0.2.0 set `__ctru_linear_heap_size = 20 MiB` and left `__ctru_heap_size = 0`.
 *   libctru then takes its *uncapped* branch: heap = remaining - linear. On a New 3DS
 *   (124 MiB RESLIMIT_COMMIT, ~122.58 MiB remaining) that is 102.58 MiB (0x06694000),
 *   which overflows the Kernel11 application heap VA window
 *   [OS_HEAP_AREA_BEGIN 0x08000000, OS_HEAP_AREA_END 0x0E000000) = 96 MiB, so
 *   svcControlMemory(MEMOP_ALLOC) fails and libctru panics before main().
 *
 * Policy:
 *   remaining = (commit_limit - commit_current) rounded down to 4 KiB
 *   normal heap = min(remaining - 20 MiB, 80 MiB)      (never touches 0x0E000000)
 *   linear heap = remaining - normal heap               (>= 20 MiB; capped to its VA window)
 *
 *   Old 3DS  64 MiB  (.cia/.cci/.3dsx): heap ~42.5 MiB, linear 20 MiB
 *   Old 3DS  80 MiB  (.3dsx ext. mode): heap ~58.5 MiB, linear 20 MiB
 *   New 3DS 124 MiB  (.cia/.cci/.3dsx): heap   80 MiB, linear ~42.5 MiB
 *   New 3DS 178 MiB  (.3dsx ext. mode): heap   80 MiB, linear ~96.5 MiB
 *
 *   Tiny envelopes (< 44 MiB remaining, e.g. applet-mode launchers) fall back to a
 *   2:1 normal:linear split instead of panicking.
 */

#include "psx3ds/types.h"

#include <stdbool.h>

#define PSX_CTR_MIB(x)                  ((u32)(x) * 1024u * 1024u)
#define PSX_CTR_PAGE_BYTES              0x1000u

/* Kernel11 application heap VA window (libctru os.h OS_HEAP_AREA_BEGIN/END). */
#define PSX_CTR_HEAP_AREA_BEGIN         0x08000000u
#define PSX_CTR_HEAP_AREA_END           0x0E000000u
#define PSX_CTR_HEAP_AREA_BYTES         (PSX_CTR_HEAP_AREA_END - PSX_CTR_HEAP_AREA_BEGIN)

/* Linear heap VA window: 128 MiB on pre-8.x kernels (0x14000000..0x1C000000). */
#define PSX_CTR_LINEAR_AREA_BYTES       0x08000000u

/* Normal-heap ceiling: 16 MiB below the 96 MiB VA window for safety margin. */
#define PSX_CTR_HEAP_MAX_BYTES          PSX_CTR_MIB(80)
/* Minimum normal heap guaranteed on every supported O3DS/N3DS configuration. */
#define PSX_CTR_HEAP_MIN_BYTES          PSX_CTR_MIB(36)
/* Linear heap floor: citro3d command buffers + VBOs + GSP framebuffers + ndsp. */
#define PSX_CTR_LINEAR_MIN_BYTES        PSX_CTR_MIB(20)
/* Below this normal heap the emulator should use its Old-3DS (small_mem) profile. */
#define PSX_CTR_HEAP_FULL_PROFILE_BYTES PSX_CTR_MIB(64)
/* Remaining-commit threshold below which the 2:1 fallback split is used. */
#define PSX_CTR_SPLIT_FALLBACK_BYTES    (PSX_CTR_LINEAR_MIN_BYTES + PSX_CTR_MIB(24))

typedef struct PsxCtrHeapSplit {
    u32 remaining;   /* page-aligned commit available to this process */
    u32 heap_size;   /* normal heap (svcControlMemory MEMOP_ALLOC @ 0x08000000) */
    u32 linear_size; /* linear heap (svcControlMemory MEMOP_ALLOC_LINEAR) */
} PsxCtrHeapSplit;

static inline u32 psx_ctr_page_floor(u32 v) {
    return v & ~(PSX_CTR_PAGE_BYTES - 1u);
}

/*
 * Compute the heap split from RESLIMIT_COMMIT limit/current values.
 * Returns false (out zeroed) if there is not enough commit for both heaps.
 */
static inline bool psx_ctr_compute_heap_split(s64 commit_limit, s64 commit_current,
                                              PsxCtrHeapSplit *out) {
    if (!out) {
        return false;
    }
    out->remaining = 0u;
    out->heap_size = 0u;
    out->linear_size = 0u;

    if (commit_limit <= commit_current || commit_current < 0) {
        return false;
    }
    s64 avail = commit_limit - commit_current;
    if (avail > (s64)0xFFFFF000u) {
        avail = (s64)0xFFFFF000u;
    }
    u32 remaining = psx_ctr_page_floor((u32)avail);
    if (remaining < 2u * PSX_CTR_PAGE_BYTES) {
        return false;
    }

    u32 heap;
    if (remaining >= PSX_CTR_SPLIT_FALLBACK_BYTES) {
        heap = remaining - PSX_CTR_LINEAR_MIN_BYTES;
    } else {
        heap = psx_ctr_page_floor((remaining / 3u) * 2u);
    }
    if (heap > PSX_CTR_HEAP_MAX_BYTES) {
        heap = PSX_CTR_HEAP_MAX_BYTES;
    }
    if (heap == 0u) {
        heap = PSX_CTR_PAGE_BYTES;
    }

    u32 linear = remaining - heap;
    if (linear > PSX_CTR_LINEAR_AREA_BYTES) {
        linear = PSX_CTR_LINEAR_AREA_BYTES;
    }

    out->remaining = remaining;
    out->heap_size = heap;
    out->linear_size = linear;
    return linear > 0u;
}

/* True if a normal heap of `heap_size` bytes at 0x08000000 stays inside the VA window. */
static inline bool psx_ctr_heap_fits_va(u32 heap_size) {
    return heap_size <= PSX_CTR_HEAP_AREA_BYTES;
}

#endif /* PSX3DS_CTR_HEAP_H */
