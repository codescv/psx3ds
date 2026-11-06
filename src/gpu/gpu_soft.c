#include "psx3ds/gpu_soft.h"

PsxResult gpu_soft_backend_create(PsxSystem *sys, const GpuBackend **be, void **self) {
    (void)sys;
    if (be) {
        *be = NULL;
    }
    if (self) {
        *self = NULL;
    }
    return PSX_ERR_NOT_IMPL;
}
