#ifndef PSX3DS_GTE_H
#define PSX3DS_GTE_H

#include "psx3ds/types.h"

/*
 * COP2 Geometry Transformation Engine (GTE) C reference & shared UNR table
 * (ARCHITECTURE.md §3.8, §5.11). Architectural registers live in cpu.gte_d[32]
 * and cpu.gte_c[32].
 */
typedef struct PsxGte {
    u32 unr_table[257];
} PsxGte;

extern const u8 gte_unr_table[257];

void gte_init(PsxSystem *sys);
void gte_reset(PsxSystem *sys);
void gte_state(PsxSystem *sys, StateIO *st);
void gte_shutdown(PsxSystem *sys);

/* Reference COP2 instruction executor (also used as JIT bounce target: r0=cpu, r1=opcode) */
void gte_exec_c(PsxCpu *cpu, u32 opcode);

/* Register transfer helpers implementing SXYP/IRGB/ORGB/LZCS/FLAG/sign-extend quirks */
u32  gte_read_data(PsxCpu *cpu, u32 reg);
void gte_write_data(PsxCpu *cpu, u32 reg, u32 val);
u32  gte_read_ctrl(const PsxCpu *cpu, u32 reg);
void gte_write_ctrl(PsxCpu *cpu, u32 reg, u32 val);

/* Unsigned Newton-Raphson reciprocal division (sets *overflow = true if h >= sz3 * 2) */
u32  gte_div_unr(u32 h, u32 sz3, bool *overflow);

#endif /* PSX3DS_GTE_H */
