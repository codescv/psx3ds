/*
 * Exhaustive ABI, struct-offset, endianness, SPSC, and Arena contract test.
 * Compiled on ALL three targets (T1 3DS, T2 ARM11 Linux QEMU, T3 x86_64 Host).
 */

/* Include all 26 shared contract headers to verify standalone & combined compilability */
#include "psx3ds/arena.h"
#include "psx3ds/bus.h"
#include "psx3ds/cdrom.h"
#include "psx3ds/config.h"
#include "psx3ds/cpu.h"
#include "psx3ds/cpu_layout.h"
#include "psx3ds/dma.h"
#include "psx3ds/gpu.h"
#include "psx3ds/gpu_hw.h"
#include "psx3ds/gpu_soft.h"
#include "psx3ds/gte.h"
#include "psx3ds/hw_regs.h"
#include "psx3ds/irq.h"
#include "psx3ds/jit.h"
#include "psx3ds/jit_mem.h"
#include "psx3ds/mdec.h"
#include "psx3ds/platform.h"
#include "psx3ds/scheduler.h"
#include "psx3ds/sio.h"
#include "psx3ds/spsc.h"
#include "psx3ds/spu.h"
#include "psx3ds/state.h"
#include "psx3ds/stats.h"
#include "psx3ds/system.h"
#include "psx3ds/timers.h"
#include "psx3ds/types.h"

#include <stdio.h>
#include <string.h>

PSX_SPSC_DECLARE(TestSpscRing, u32, 8u)

#define CHECK_TRUE(cond, msg)                                                       \
    do {                                                                            \
        if (!(cond)) {                                                              \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, (msg));       \
            return 1;                                                               \
        }                                                                           \
    } while (0)

#define CHECK_OFF(field, macro_val)                                                 \
    do {                                                                            \
        size_t actual = offsetof(PsxCpu, field);                                    \
        size_t expect = (size_t)(macro_val);                                        \
        printf("OFFSET %s = 0x%03zx (expected 0x%03zx)\n",                          \
               #macro_val, actual, expect);                                         \
        if (actual != expect) {                                                     \
            fprintf(stderr, "FAIL: offsetof(PsxCpu, %s) == 0x%zx != %s (0x%zx)\n",  \
                    #field, actual, #macro_val, expect);                            \
            return 1;                                                               \
        }                                                                           \
    } while (0)

int main(void) {
    /* 1. Endianness & fixed-width types */
    union {
        u32 u;
        u8  b[4];
    } endian_u = { .u = 0x01020304u };
    CHECK_TRUE(endian_u.b[0] == 0x04u && endian_u.b[1] == 0x03u &&
               endian_u.b[2] == 0x02u && endian_u.b[3] == 0x01u,
               "target must be little-endian");

    CHECK_TRUE(sizeof(u8) == 1 && sizeof(u16) == 2 && sizeof(u32) == 4 && sizeof(u64) == 8,
               "unsigned fixed-width sizes");
    CHECK_TRUE(sizeof(s8) == 1 && sizeof(s16) == 2 && sizeof(s32) == 4 && sizeof(s64) == 8,
               "signed fixed-width sizes");
    CHECK_TRUE(sizeof(PsxPtrSlot) == 8, "sizeof(PsxPtrSlot) == 8");
    CHECK_TRUE(offsetof(PsxPtrSlot, p) == 0, "PsxPtrSlot.p at byte +0");
    CHECK_TRUE(sizeof(JitBlock) == 32, "sizeof(JitBlock) == 32");
    CHECK_TRUE(sizeof(GpuVertex) == 12, "sizeof(GpuVertex) == 12");
    CHECK_TRUE(EV_COUNT <= 16, "EV_COUNT <= 16");

    /* Verify PsxPtrSlot low-word aliasing used by ARM32 `ldr rX, [r11, #PSX_OFF_*]` */
    int dummy_target = 42;
    PsxPtrSlot slot = { ._pad = 0u };
    slot.p = &dummy_target;
    void *loaded_ptr = NULL;
    memcpy(&loaded_ptr, &slot, sizeof(void *));
    CHECK_TRUE(loaded_ptr == &dummy_target, "PsxPtrSlot low-address pointer aliasing");

    /* 2. Exhaustive PsxCpu hot-region byte offsets (all 33 fields + hot/cold boundary) */
    CHECK_OFF(gpr,            PSX_OFF_GPR);
    CHECK_OFF(pc,             PSX_OFF_PC);
    CHECK_OFF(next_pc,        PSX_OFF_NEXT_PC);
    CHECK_OFF(hi,             PSX_OFF_HI);
    CHECK_OFF(lo,             PSX_OFF_LO);
    CHECK_OFF(cycles_left,    PSX_OFF_CYCLES_LEFT);
    CHECK_OFF(cycle_deficit,  PSX_OFF_CYCLE_DEFICIT);
    CHECK_OFF(irq_pending,    PSX_OFF_IRQ_PENDING);
    CHECK_OFF(in_delay_slot,  PSX_OFF_IN_DELAY_SLOT);
    CHECK_OFF(load_delay_reg, PSX_OFF_LOAD_DELAY_REG);
    CHECK_OFF(load_delay_val, PSX_OFF_LOAD_DELAY_VAL);
    CHECK_OFF(jit_abort,      PSX_OFF_JIT_ABORT);
    CHECK_OFF(exit_reason,    PSX_OFF_EXIT_REASON);
    CHECK_OFF(branch_target,  PSX_OFF_BRANCH_TARGET);
    CHECK_OFF(branch_taken,   PSX_OFF_BRANCH_TAKEN);
    CHECK_OFF(idle_skip,      PSX_OFF_IDLE_SKIP);
    CHECK_OFF(slice_len,      PSX_OFF_SLICE_LEN);
    CHECK_OFF(cop0,           PSX_OFF_COP0);
    CHECK_OFF(gte_d,          PSX_OFF_GTE_D);
    CHECK_OFF(gte_c,          PSX_OFF_GTE_C);
    CHECK_OFF(fastmem_r,      PSX_OFF_FASTMEM_R);
    CHECK_OFF(fastmem_w,      PSX_OFF_FASTMEM_W);
    CHECK_OFF(ibtc,           PSX_OFF_IBTC);
    CHECK_OFF(blocktab_ram,   PSX_OFF_BLOCKTAB_RAM);
    CHECK_OFF(blocktab_bios,  PSX_OFF_BLOCKTAB_BIOS);
    CHECK_OFF(ram_base,       PSX_OFF_RAM_BASE);
    CHECK_OFF(scratch_base,   PSX_OFF_SCRATCH_BASE);
    CHECK_OFF(sys,            PSX_OFF_SYS);
    CHECK_OFF(in_jit,         PSX_OFF_IN_JIT);
    CHECK_OFF(abort_badvaddr, PSX_OFF_ABORT_BADVADDR);
    CHECK_OFF(gte_scratch,    PSX_OFF_GTE_SCRATCH);
    CHECK_OFF(icache_dirty,   PSX_OFF_ICACHE_DIRTY);
    CHECK_OFF(psexe_pending,  PSX_OFF_PSEXE_PENDING);
    CHECK_OFF(jit_saved_sp,   PSX_OFF_JIT_SP);
    CHECK_OFF(total_cycles,   PSX_OFF_HOT_END);
    CHECK_TRUE(PSX_OFF_JIT_SP == 0x270, "PSX_OFF_JIT_SP == 0x270 (v0.2.3 SP guard)");
    CHECK_TRUE(offsetof(PsxCpu, _hot_pad) + sizeof(((PsxCpu *)0)->_hot_pad) == PSX_OFF_HOT_END,
               "hot region ends exactly at PSX_OFF_HOT_END");

    CHECK_TRUE(offsetof(PsxCpu, cop0) + COP0_SR * 4u == PSX_OFF_COP0_SR, "COP0 SR offset");
    CHECK_TRUE(offsetof(PsxCpu, cop0) + COP0_CAUSE * 4u == PSX_OFF_COP0_CAUSE, "COP0 CAUSE offset");
    CHECK_TRUE(offsetof(PsxCpu, cop0) + COP0_EPC * 4u == PSX_OFF_COP0_EPC, "COP0 EPC offset");
    CHECK_TRUE((offsetof(PsxSystem, cpu) % 32u) == 0u, "PsxSystem.cpu 32-byte alignment");

    /* 3. Tick wrap-around helper sanity */
    CHECK_TRUE(tick_before(0xFFFFFFF0u, 0x00000010u), "tick_before across 2^32 wrap");
    CHECK_TRUE(!tick_before(0x00000010u, 0xFFFFFFF0u), "tick_before inverse across wrap");

    /* 4. Arena bump allocator sanity */
    PsxArena arena = { 0 };
    CHECK_TRUE(arena_init(&arena, 64u * 1024u) == PSX_OK, "arena_init 64 KiB");
    CHECK_TRUE(((uintptr_t)arena.base & 4095u) == 0u, "arena base 4 KiB aligned");

    u8 *p1 = (u8 *)arena_alloc(&arena, 13u, 16u);
    u8 *p2 = (u8 *)arena_alloc(&arena, 256u, 32u);
    u8 *p3 = (u8 *)arena_alloc(&arena, 4096u, 4096u);
    CHECK_TRUE(((uintptr_t)p1 & 15u) == 0u, "arena 16B align");
    CHECK_TRUE(((uintptr_t)p2 & 31u) == 0u && p2 >= p1 + 13u, "arena 32B align + non-overlap");
    CHECK_TRUE(((uintptr_t)p3 & 4095u) == 0u && p3 >= p2 + 256u, "arena 4096B align + non-overlap");
    p1[0] = 0xAAu;
    arena_reset(&arena);
    CHECK_TRUE(arena.used == 0u && p1[0] == 0u, "arena_reset zeroes used span");
    arena_destroy(&arena);
    CHECK_TRUE(arena.base == NULL && arena.size == 0u, "arena_destroy");

    /* 5. Lock-free SPSC ring sanity (incl. ARMv6K CP15 barrier codegen on T1/T2) */
    TestSpscRing ring;
    TestSpscRing_init(&ring);
    CHECK_TRUE(offsetof(TestSpscRing, tail) - offsetof(TestSpscRing, head) >= PSX_CACHELINE,
               "SPSC head/tail cacheline separation");
    CHECK_TRUE(TestSpscRing_empty(&ring), "SPSC initially empty");

    for (int round = 0; round < 3; round++) {
        for (u32 i = 0; i < 8u; i++) {
            CHECK_TRUE(TestSpscRing_push(&ring, (u32)(round * 100) + i), "SPSC push");
        }
        CHECK_TRUE(TestSpscRing_full(&ring), "SPSC full at capacity");
        CHECK_TRUE(!TestSpscRing_push(&ring, 999u), "SPSC rejects push when full");

        for (u32 i = 0; i < 8u; i++) {
            u32 out = 0u;
            CHECK_TRUE(TestSpscRing_pop(&ring, &out), "SPSC pop");
            CHECK_TRUE(out == (u32)(round * 100) + i, "SPSC FIFO order across wrap");
        }
        CHECK_TRUE(TestSpscRing_empty(&ring), "SPSC empty after drain");
        u32 dummy = 0u;
        CHECK_TRUE(!TestSpscRing_pop(&ring, &dummy), "SPSC rejects pop when empty");
    }

    /* 6. ARMv6K assembly thunk smoke check (executes jit_thunks.S under QEMU arm11mpcore) */
#if PSX3DS_HAVE_JIT
    PsxCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.pc = PSX_RESET_PC;
    cpu.cycles_left = 1234;
    cpu.exit_reason = 0xDEADBEEFu;
    int rc = jit_enter(&cpu);
    CHECK_TRUE(rc == PSX_EXIT_EVENTS && cpu.exit_reason == PSX_EXIT_EVENTS &&
               cpu.cycles_left == 1234,
               "jit_enter -> jit_exit round-trip");
#endif

    printf("test_layout PASS (arch_armv6k=%d have_jit=%d platform_3ds=%d ptr=%zu)\n",
           PSX3DS_ARCH_ARMV6K, PSX3DS_HAVE_JIT, PSX3DS_PLATFORM_3DS, sizeof(void *));
    return 0;
}
