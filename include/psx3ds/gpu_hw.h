#ifndef PSX3DS_GPU_HW_H
#define PSX3DS_GPU_HW_H

#include "psx3ds/gpu.h"

/* Returns PSX_ERR_NOT_IMPL on non-3DS hosts (implemented in gpu_hw_stub.c) */
PsxResult gpu_hw_backend_create(PsxSystem *sys, const GpuBackend **be, void **self);

#endif /* PSX3DS_GPU_HW_H */
