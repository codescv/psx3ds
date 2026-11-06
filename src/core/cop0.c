#include "psx3ds/cpu.h"

void cpu_raise_exception(PsxSystem *sys, u32 code, u32 pc, bool bd,
                         u32 branch_target, u32 badvaddr, u32 ce) {
    (void)sys;
    (void)code;
    (void)pc;
    (void)bd;
    (void)branch_target;
    (void)badvaddr;
    (void)ce;
}

void cpu_set_sr(PsxSystem *sys, u32 v) {
    (void)sys;
    (void)v;
}

void cpu_set_cause(PsxSystem *sys, u32 v) {
    (void)sys;
    (void)v;
}

void cpu_check_irq(PsxSystem *sys) {
    (void)sys;
}

void cpu_dispatch_irq(PsxSystem *sys) {
    (void)sys;
}
