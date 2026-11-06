#include "psx3ds/system.h"

bool hle_bios_dispatch_stub(PsxSystem *sys, u32 vector_pc);

bool hle_bios_dispatch_stub(PsxSystem *sys, u32 vector_pc) {
    (void)sys;
    (void)vector_pc;
    return false;
}
