#include "psx3ds/gpu_hw.h"
#include "psx_gpu_shbin.h"

PsxResult gpu_hw_backend_create(PsxSystem *sys, const GpuBackend **be, void **self) {
    (void)sys;
    (void)psx_gpu_shbin;
    (void)psx_gpu_shbin_size;
    if (be) {
        *be = NULL;
    }
    if (self) {
        *self = NULL;
    }
    return PSX_ERR_NOT_IMPL;
}
