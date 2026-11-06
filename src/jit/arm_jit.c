#include "arm_emit.h"

void jit_run(PsxSystem *sys) {
    (void)sys;
}

PSX_USED void *jit_lookup_slow(PsxCpu *cpu, u32 pc) {
    (void)cpu;
    (void)pc;
    return (void *)jit_exit;
}

PSX_USED void *jit_compile_at_pc(PsxCpu *cpu) {
    (void)cpu;
    return (void *)jit_exit;
}
