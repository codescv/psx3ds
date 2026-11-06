#include "psx3ds/gpu_hw.h"

/*
 * On non-3DS builds (T2 ARM11 Linux QEMU, T3 x86_64 host ASan+UBSan) or when
 * PSX3DS_ENABLE_HW_GPU is OFF, CMake compiles only gpu_hw_stub.c from src/gpu_hw/.
 * Including the pure-C Morton swizzler and 64-slot CLUT+TexPage cache here makes
 * them testable across all 3 verification tiers without modifying CMakeLists.txt.
 */
#include "gpu_morton.c"
#include "gpu_texcache.c"

PsxResult gpu_hw_backend_create(PsxSystem *sys, const GpuBackend **be, void **self) {
    (void)sys;
    if (be) {
        *be = NULL;
    }
    if (self) {
        *self = NULL;
    }
    return PSX_ERR_NOT_IMPL;
}
