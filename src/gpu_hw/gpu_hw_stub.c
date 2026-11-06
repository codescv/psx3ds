#include "psx3ds/gpu_hw.h"

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
