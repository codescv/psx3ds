#include "psx3ds/system.h"

PsxResult psx_bench_run_stub(PsxSystem *sys, u32 frames);

PsxResult psx_bench_run_stub(PsxSystem *sys, u32 frames) {
    (void)sys;
    (void)frames;
    return PSX_ERR_NOT_IMPL;
}
