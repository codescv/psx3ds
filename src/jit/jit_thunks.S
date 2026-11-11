#include "psx3ds/cpu_layout.h"

    .syntax unified
    .arch armv6k
    .fpu vfpv2
    .arm
    .text
    .align 2

/* ============================================================================
 * ARM11v6K Dynarec Dispatcher, Exit Trampolines & UNR Leaf (ARCHITECTURE.md §5)
 *
 * Register contract inside JIT frame (after jit_enter push):
 *   r11 = PsxCpu *
 *   r9  = cpu->fastmem_r.p
 *   r10 = cpu->cycles_left (signed s32)
 *   r4-r8, r12, lr = 7 intra-block MIPS GPR cache slots (dead at block edges)
 *   sp  = 8-byte aligned throughout (10 regs = 40 bytes pushed by jit_enter)
 * ============================================================================ */

    .global jit_enter
    .type jit_enter, %function
jit_enter:
    push    {r4-r12, lr}                @ 10 regs (40 B) -> preserves 8-byte SP alignment
    mov     r11, r0                     @ r11 = PsxCpu *
    str     sp,  [r11, #PSX_OFF_JIT_SP] @ v0.2.3: SP guard, restored by every jit_exit* path
    ldr     r9,  [r11, #PSX_OFF_FASTMEM_R]
    ldr     r10, [r11, #PSX_OFF_CYCLES_LEFT]
    ldr     r2,  [r11, #PSX_OFF_BLOCKTAB_RAM]
    cmp     r2,  #0
    beq     .Lexit_no_injit             @ guard uninitialized PsxCpu (e.g. test_layout)
    mov     r2,  #1
    str     r2,  [r11, #PSX_OFF_IN_JIT]
    ldr     r0,  [r11, #PSX_OFF_PC]
    b       jit_dispatch
    .size jit_enter, . - jit_enter

/* ----------------------------------------------------------------------------
 * O(1) Direct-Table Block Dispatcher (r0 = target guest virtual PC)
 * ---------------------------------------------------------------------------- */
    .global jit_dispatch
    .type jit_dispatch, %function
jit_dispatch:
    str     r0,  [r11, #PSX_OFF_PC]
    cmp     r10, #0
    ble     jit_exit
    @ 1. Reject misaligned PC before any table load (prevents misaligned LDR PC)
    tst     r0,  #3
    bne     jit_dispatch_slow
    @ 2. User-mode (SR.KUc == bit 1) kernel address (>= 0x80000000) -> AdEL
    ldr     r2,  [r11, #PSX_OFF_COP0_SR]
    tst     r2,  #2
    beq     1f
    tst     r0,  #0x80000000
    bne     jit_dispatch_slow
1:
    @ 3. Validate segment: KUSEG (top3=0) or KSEG0/KSEG1 (top3=4,5)
    lsr     r3,  r0, #29
    cmp     r3,  #0
    beq     2f
    bic     r3,  r3, #1
    cmp     r3,  #4
    bne     jit_dispatch_slow
2:
    @ 4. Primary 2 MiB RAM fastpath (r2[1:0]==0 so r2 == BLOCKTAB_RAM_IDX*4)
    bic     r2,  r0, #0xE0000000
    cmp     r2,  #0x00200000
    ldrlo   r3,  [r11, #PSX_OFF_BLOCKTAB_RAM]
    ldrlo   pc,  [r3, r2]
    @ 5. 512 KiB BIOS fastpath (0x1FC00000 = 0x7F ror 10, 0x00080000 = 0x02 ror 14)
    sub     r2,  r2, #0x1FC00000
    cmp     r2,  #0x00080000
    ldrlo   r3,  [r11, #PSX_OFF_BLOCKTAB_BIOS]
    ldrlo   pc,  [r3, r2]
    b       jit_dispatch_slow
    .size jit_dispatch, . - jit_dispatch

/* ----------------------------------------------------------------------------
 * Slow-Path Dispatcher, Lazy Compiler, & Pinned BIOS/PS-EXE Vector Stubs
 * ---------------------------------------------------------------------------- */
    .global jit_dispatch_slow
    .type jit_dispatch_slow, %function
jit_dispatch_slow:
    str     r0,  [r11, #PSX_OFF_PC]
    str     r10, [r11, #PSX_OFF_CYCLES_LEFT]
    mov     r1,  r0
    mov     r0,  r11
    bl      jit_lookup_slow
    mov     r3,  r0
    ldr     r0,  [r11, #PSX_OFF_PC]
    ldr     r10, [r11, #PSX_OFF_CYCLES_LEFT]
    ldr     r9,  [r11, #PSX_OFF_FASTMEM_R]
    bx      r3
    .size jit_dispatch_slow, . - jit_dispatch_slow

    .global jit_compile_stub
    .type jit_compile_stub, %function
jit_compile_stub:
    str     r0,  [r11, #PSX_OFF_PC]
    cmp     r10, #0
    ble     jit_exit
    str     r10, [r11, #PSX_OFF_CYCLES_LEFT]
    mov     r0,  r11
    bl      jit_compile_at_pc
    mov     r3,  r0
    ldr     r0,  [r11, #PSX_OFF_PC]
    ldr     r10, [r11, #PSX_OFF_CYCLES_LEFT]
    ldr     r9,  [r11, #PSX_OFF_FASTMEM_R]
    bx      r3
    .size jit_compile_stub, . - jit_compile_stub

    .global jit_bios_vec_stub
    .type jit_bios_vec_stub, %function
jit_bios_vec_stub:
    str     r0,  [r11, #PSX_OFF_PC]
    cmp     r10, #0
    ble     jit_exit
    str     r10, [r11, #PSX_OFF_CYCLES_LEFT]
    mov     r1,  r0
    mov     r0,  r11
    bl      jit_bios_vec_helper
    mov     r3,  r0
    ldr     r0,  [r11, #PSX_OFF_PC]
    ldr     r10, [r11, #PSX_OFF_CYCLES_LEFT]
    ldr     r9,  [r11, #PSX_OFF_FASTMEM_R]
    bx      r3
    .size jit_bios_vec_stub, . - jit_bios_vec_stub

    .global jit_psexe_stub
    .type jit_psexe_stub, %function
jit_psexe_stub:
    str     r0,  [r11, #PSX_OFF_PC]
    cmp     r10, #0
    ble     jit_exit
    str     r10, [r11, #PSX_OFF_CYCLES_LEFT]
    mov     r1,  r0
    mov     r0,  r11
    bl      jit_psexe_helper
    mov     r3,  r0
    ldr     r0,  [r11, #PSX_OFF_PC]
    ldr     r10, [r11, #PSX_OFF_CYCLES_LEFT]
    ldr     r9,  [r11, #PSX_OFF_FASTMEM_R]
    bx      r3
    .size jit_psexe_stub, . - jit_psexe_stub

    .global jit_resume_trampoline
    .type jit_resume_trampoline, %function
jit_resume_trampoline:
    ldr     r0,  [r11, #PSX_OFF_PC]
    b       jit_dispatch
    .size jit_resume_trampoline, . - jit_resume_trampoline

/* ----------------------------------------------------------------------------
 * Slice Exit Trampolines
 * ---------------------------------------------------------------------------- */
    .global jit_exit
    .type jit_exit, %function
jit_exit:
    mov     r2,  #0
    str     r2,  [r11, #PSX_OFF_IN_JIT]
.Lexit_no_injit:
    str     r10, [r11, #PSX_OFF_CYCLES_LEFT]
    mov     r0,  #PSX_EXIT_EVENTS
    str     r0,  [r11, #PSX_OFF_EXIT_REASON]
    ldr     sp,  [r11, #PSX_OFF_JIT_SP] @ v0.2.3: exact SP saved by jit_enter
    pop     {r4-r12, pc}
    .size jit_exit, . - jit_exit

    .global jit_exit_step_one
    .type jit_exit_step_one, %function
jit_exit_step_one:
    mov     r2,  #0
    str     r2,  [r11, #PSX_OFF_IN_JIT]
    str     r10, [r11, #PSX_OFF_CYCLES_LEFT]
    mov     r0,  #PSX_EXIT_STEP_ONE
    str     r0,  [r11, #PSX_OFF_EXIT_REASON]
    ldr     sp,  [r11, #PSX_OFF_JIT_SP] @ v0.2.3: exact SP saved by jit_enter
    pop     {r4-r12, pc}
    .size jit_exit_step_one, . - jit_exit_step_one

    .global jit_exit_with_reason
    .type jit_exit_with_reason, %function
jit_exit_with_reason:
    mov     r2,  #0
    str     r2,  [r11, #PSX_OFF_IN_JIT]
    str     r10, [r11, #PSX_OFF_CYCLES_LEFT]
    ldr     r0,  [r11, #PSX_OFF_EXIT_REASON]
    ldr     sp,  [r11, #PSX_OFF_JIT_SP] @ v0.2.3: exact SP saved by jit_enter
    pop     {r4-r12, pc}
    .size jit_exit_with_reason, . - jit_exit_with_reason

    .global jit_raise_exception_exit
    .type jit_raise_exception_exit, %function
jit_raise_exception_exit:
    @ Entered from block cold exc_stub / abort_stub with:
    @   r1 = exception code (PSX_EXC_*)
    @   r2 = fault instruction PC
    @   r3 = packed flags (bit0 = bd, bit1 = branch_taken, bits9:8 = ce)
    mov     r0,  #0
    str     r0,  [r11, #PSX_OFF_IN_JIT]
    str     r10, [r11, #PSX_OFF_CYCLES_LEFT]
    mov     r0,  r11
    bl      jit_handle_exception_thunk
    ldr     r10, [r11, #PSX_OFF_CYCLES_LEFT]
    mov     r0,  #PSX_EXIT_EXCEPTION
    str     r0,  [r11, #PSX_OFF_EXIT_REASON]
    ldr     sp,  [r11, #PSX_OFF_JIT_SP] @ v0.2.3: exact SP saved by jit_enter
    pop     {r4-r12, pc}
    .size jit_raise_exception_exit, . - jit_raise_exception_exit

/* ----------------------------------------------------------------------------
 * Exact PS1 GTE UNR Reciprocal Division Leaf (ARM11v6K, zero stack usage)
 * Prototype: u64 jit_gte_div_unr_asm(u32 h, u32 sz3, const u8 *unr_table)
 * Returns:   r0 = reciprocal (0..0x1FFFF), r1 = overflow flag (0 or 1)
 * Clobbers:  r0, r1, r2, r3, r12 only
 * ---------------------------------------------------------------------------- */
    .global jit_gte_div_unr_asm
    .type jit_gte_div_unr_asm, %function
jit_gte_div_unr_asm:
    uxth    r0,  r0                     @ h &= 0xFFFF
    uxth    r1,  r1                     @ sz3 &= 0xFFFF
    cmp     r0,  r1, lsl #1             @ h >= sz3 * 2 (also catches sz3 == 0)
    bhs     .Lunr_ovf

    clz     r3,  r1                     @ 16..31
    sub     r3,  r3, #16                @ z = clz(sz3) - 16 (0..15)
    lsl     r0,  r0, r3                 @ n = h << z
    lsl     r1,  r1, r3                 @ d = sz3 << z (0x8000..0xFFFF)

    bic     r3,  r1, #0x8000            @ d & 0x7FFF
    add     r3,  r3, #0x40              @ (d & 0x7FFF) + 0x40
    ldrb    r2,  [r2, r3, lsr #7]       @ unr_table[idx] (idx in 0..256)
    add     r2,  r2, #0x100
    add     r2,  r2, #1                 @ u = unr_table[idx] + 0x101

    mul     r3,  r1, r2                 @ d * u (Rd=r3 != Rm=r1)
    mov     r12, #0x02000000
    add     r12, r12, #0x80             @ 0x02000080
    sub     r3,  r12, r3
    asr     r3,  r3, #8                 @ d1 = (0x02000080 - d * u) >> 8

    mul     r1,  r3, r2                 @ d1 * u (Rd=r1 != Rm=r3)
    add     r1,  r1, #0x80
    asr     r1,  r1, #8                 @ d2 = (0x80 + d1 * u) >> 8

    umull   r2,  r3, r0, r1             @ r3:r2 = (u64)n * (u64)d2 (RdLo,RdHi != Rm)
    adds    r2,  r2, #0x8000
    adc     r3,  r3, #0
    lsr     r0,  r2, #16
    orr     r0,  r0, r3, lsl #16        @ (n * d2 + 0x8000) >> 16

    mov     r2,  #0x20000
    sub     r2,  r2, #1                 @ 0x1FFFF
    cmp     r0,  r2
    movhi   r0,  r2                     @ min(0x1FFFF, res)
    mov     r1,  #0                     @ overflow = 0
    bx      lr

.Lunr_ovf:
    mov     r0,  #0x20000
    sub     r0,  r0, #1                 @ 0x1FFFF
    mov     r1,  #1                     @ overflow = 1
    bx      lr
    .size jit_gte_div_unr_asm, . - jit_gte_div_unr_asm

#if defined(__linux__) && defined(__ELF__)
    .section .note.GNU-stack,"",%progbits
#endif
