#ifndef PSX3DS_CPU_H
#define PSX3DS_CPU_H

#include "psx3ds/cpu_layout.h"
#include "psx3ds/hw_regs.h"
#include "psx3ds/types.h"

/*
 * 8-byte pointer slot across ARM32 (sizeof(void*)==4) and x86_64 (sizeof(void*)==8).
 * On little-endian ARM32, `p` occupies offset +0 of the 8-byte union so
 * `ldr rX, [r11, #PSX_OFF_*]` loads the 32-bit pointer directly.
 */
typedef union PsxPtrSlot {
    void *p;
    u64   _pad;
} PsxPtrSlot;

_Static_assert(sizeof(PsxPtrSlot) == 8, "PsxPtrSlot must be 8 bytes on every ABI");

/*
 * R3000A + COP0 + COP2 (GTE) architectural & JIT-hot state (ARCHITECTURE.md §3.2).
 * All JIT-visible fields live in [0x000, 0x300) for single-insn [r11, #imm12] access.
 */
typedef struct PSX_ALIGNED(32) PsxCpu {
    u32 gpr[32];

    u32 pc;
    u32 next_pc;
    u32 hi;
    u32 lo;

    s32 cycles_left;
    s32 cycle_deficit;

    u32 irq_pending;
    u32 in_delay_slot;
    u32 load_delay_reg;
    u32 load_delay_val;

    u32 jit_abort;
    u32 exit_reason;
    u32 branch_target;
    u32 branch_taken;
    u32 idle_skip;

    s32 slice_len;

    u32 cop0[16];
    u32 gte_d[32];
    u32 gte_c[32];

    PsxPtrSlot fastmem_r;
    PsxPtrSlot fastmem_w;
    PsxPtrSlot ibtc;
    PsxPtrSlot blocktab_ram;
    PsxPtrSlot blocktab_bios;
    PsxPtrSlot ram_base;
    PsxPtrSlot scratch_base;
    PsxPtrSlot sys;

    u32 in_jit;
    u32 abort_badvaddr;
    u32 gte_scratch[8];
    u32 icache_dirty;
    u32 psexe_pending;

    u8  _hot_pad[PSX_OFF_HOT_END - 0x270];

    /* ---- cold (>= 0x300) ---- */
    u64 total_cycles;
    u32 cache_ctrl;
    u32 exc_pending;
    u32 icache_tag_dummy[256];
} PsxCpu;

/* Exhaustive compile-time verification of all 33 hot layout offsets */
PSX_ASSERT_OFF(PsxCpu, gpr,            PSX_OFF_GPR);
PSX_ASSERT_OFF(PsxCpu, pc,             PSX_OFF_PC);
PSX_ASSERT_OFF(PsxCpu, next_pc,        PSX_OFF_NEXT_PC);
PSX_ASSERT_OFF(PsxCpu, hi,             PSX_OFF_HI);
PSX_ASSERT_OFF(PsxCpu, lo,             PSX_OFF_LO);
PSX_ASSERT_OFF(PsxCpu, cycles_left,    PSX_OFF_CYCLES_LEFT);
PSX_ASSERT_OFF(PsxCpu, cycle_deficit,  PSX_OFF_CYCLE_DEFICIT);
PSX_ASSERT_OFF(PsxCpu, irq_pending,    PSX_OFF_IRQ_PENDING);
PSX_ASSERT_OFF(PsxCpu, in_delay_slot,  PSX_OFF_IN_DELAY_SLOT);
PSX_ASSERT_OFF(PsxCpu, load_delay_reg, PSX_OFF_LOAD_DELAY_REG);
PSX_ASSERT_OFF(PsxCpu, load_delay_val, PSX_OFF_LOAD_DELAY_VAL);
PSX_ASSERT_OFF(PsxCpu, jit_abort,      PSX_OFF_JIT_ABORT);
PSX_ASSERT_OFF(PsxCpu, exit_reason,    PSX_OFF_EXIT_REASON);
PSX_ASSERT_OFF(PsxCpu, branch_target,  PSX_OFF_BRANCH_TARGET);
PSX_ASSERT_OFF(PsxCpu, branch_taken,   PSX_OFF_BRANCH_TAKEN);
PSX_ASSERT_OFF(PsxCpu, idle_skip,      PSX_OFF_IDLE_SKIP);
PSX_ASSERT_OFF(PsxCpu, slice_len,      PSX_OFF_SLICE_LEN);
PSX_ASSERT_OFF(PsxCpu, cop0,           PSX_OFF_COP0);
PSX_ASSERT_OFF(PsxCpu, gte_d,          PSX_OFF_GTE_D);
PSX_ASSERT_OFF(PsxCpu, gte_c,          PSX_OFF_GTE_C);
PSX_ASSERT_OFF(PsxCpu, fastmem_r,      PSX_OFF_FASTMEM_R);
PSX_ASSERT_OFF(PsxCpu, fastmem_w,      PSX_OFF_FASTMEM_W);
PSX_ASSERT_OFF(PsxCpu, ibtc,           PSX_OFF_IBTC);
PSX_ASSERT_OFF(PsxCpu, blocktab_ram,   PSX_OFF_BLOCKTAB_RAM);
PSX_ASSERT_OFF(PsxCpu, blocktab_bios,  PSX_OFF_BLOCKTAB_BIOS);
PSX_ASSERT_OFF(PsxCpu, ram_base,       PSX_OFF_RAM_BASE);
PSX_ASSERT_OFF(PsxCpu, scratch_base,   PSX_OFF_SCRATCH_BASE);
PSX_ASSERT_OFF(PsxCpu, sys,            PSX_OFF_SYS);
PSX_ASSERT_OFF(PsxCpu, in_jit,         PSX_OFF_IN_JIT);
PSX_ASSERT_OFF(PsxCpu, abort_badvaddr, PSX_OFF_ABORT_BADVADDR);
PSX_ASSERT_OFF(PsxCpu, gte_scratch,    PSX_OFF_GTE_SCRATCH);
PSX_ASSERT_OFF(PsxCpu, icache_dirty,   PSX_OFF_ICACHE_DIRTY);
PSX_ASSERT_OFF(PsxCpu, psexe_pending,  PSX_OFF_PSEXE_PENDING);
_Static_assert(offsetof(PsxCpu, total_cycles) >= PSX_OFF_HOT_END, "hot region overflow");
_Static_assert(offsetof(PsxCpu, total_cycles) == PSX_OFF_HOT_END, "unexpected padding at hot/cold boundary");

/* Lifecycle & Interpreter / COP0 API (Slice A) */
void cpu_init(PsxSystem *sys);
void cpu_reset(PsxSystem *sys);
void cpu_state(PsxSystem *sys, StateIO *st);
void cpu_shutdown(PsxSystem *sys);

void cpu_run_interp(PsxSystem *sys);
void cpu_step_one(PsxSystem *sys);
void cpu_step_until_clean(PsxSystem *sys);

void cpu_raise_exception(PsxSystem *sys, u32 code, u32 pc, bool bd,
                         u32 branch_target, u32 badvaddr, u32 ce);
void cpu_set_sr(PsxSystem *sys, u32 v);
void cpu_set_cause(PsxSystem *sys, u32 v);
void cpu_check_irq(PsxSystem *sys);
void cpu_dispatch_irq(PsxSystem *sys);
void cpu_request_exit(PsxSystem *sys);

#endif /* PSX3DS_CPU_H */
