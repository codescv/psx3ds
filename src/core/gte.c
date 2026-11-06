#include "psx3ds/gte.h"

void gte_init(PsxSystem *sys) {
    (void)sys;
}

void gte_reset(PsxSystem *sys) {
    (void)sys;
}

void gte_state(PsxSystem *sys, StateIO *st) {
    (void)sys;
    (void)st;
}

void gte_shutdown(PsxSystem *sys) {
    (void)sys;
}

void gte_exec_c(PsxCpu *cpu, u32 opcode) {
    (void)cpu;
    (void)opcode;
}

u32 gte_read_data(PsxCpu *cpu, u32 reg) {
    (void)cpu;
    (void)reg;
    return 0u;
}

void gte_write_data(PsxCpu *cpu, u32 reg, u32 val) {
    (void)cpu;
    (void)reg;
    (void)val;
}

u32 gte_read_ctrl(const PsxCpu *cpu, u32 reg) {
    (void)cpu;
    (void)reg;
    return 0u;
}

void gte_write_ctrl(PsxCpu *cpu, u32 reg, u32 val) {
    (void)cpu;
    (void)reg;
    (void)val;
}

u32 gte_div_unr(u32 h, u32 sz3, bool *overflow) {
    (void)h;
    (void)sz3;
    if (overflow) {
        *overflow = false;
    }
    return 0u;
}
