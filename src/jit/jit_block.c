#include "psx3ds/jit.h"

void jit_init(PsxSystem *sys) {
    (void)sys;
}

void jit_reset(PsxSystem *sys) {
    (void)sys;
}

void jit_state(PsxSystem *sys, StateIO *st) {
    (void)sys;
    (void)st;
}

void jit_shutdown(PsxSystem *sys) {
    (void)sys;
}

void jit_invalidate_page(PsxSystem *sys, u32 phys_page) {
    (void)sys;
    (void)phys_page;
}

void jit_invalidate_range(PsxSystem *sys, u32 phys, u32 bytes) {
    (void)sys;
    (void)phys;
    (void)bytes;
}

void jit_flush_all(PsxSystem *sys) {
    (void)sys;
}

#if !PSX3DS_HAVE_JIT
char jit_dispatch[1];
char jit_dispatch_slow[1];
char jit_compile_stub[1];
char jit_bios_vec_stub[1];
char jit_psexe_stub[1];
char jit_exit[1];

void jit_run(PsxSystem *sys) {
    cpu_cached_run(sys);
}

void *jit_lookup_slow(PsxCpu *cpu, u32 pc) {
    (void)cpu;
    (void)pc;
    return (void *)jit_exit;
}

void *jit_compile_at_pc(PsxCpu *cpu) {
    (void)cpu;
    return (void *)jit_exit;
}

int jit_enter(PsxCpu *cpu) {
    (void)cpu;
    return PSX_EXIT_EVENTS;
}
#endif
