#include "psx3ds/cpu_layout.h"

    .syntax unified
    .arch armv6k
    .arm
    .text
    .align 2

    .global jit_enter
    .type jit_enter, %function
jit_enter:
    push    {r4-r12, lr}
    mov     r11, r0
    ldr     r9,  [r11, #PSX_OFF_FASTMEM_R]
    ldr     r10, [r11, #PSX_OFF_CYCLES_LEFT]
    ldr     r0,  [r11, #PSX_OFF_PC]
    b       jit_exit
    .size jit_enter, . - jit_enter

    .global jit_dispatch
    .type jit_dispatch, %function
jit_dispatch:
    .global jit_dispatch_slow
    .type jit_dispatch_slow, %function
jit_dispatch_slow:
    .global jit_compile_stub
    .type jit_compile_stub, %function
jit_compile_stub:
    .global jit_bios_vec_stub
    .type jit_bios_vec_stub, %function
jit_bios_vec_stub:
    .global jit_psexe_stub
    .type jit_psexe_stub, %function
jit_psexe_stub:
    str     r0, [r11, #PSX_OFF_PC]
    b       jit_exit

    .global jit_exit
    .type jit_exit, %function
jit_exit:
    str     r10, [r11, #PSX_OFF_CYCLES_LEFT]
    mov     r0,  #PSX_EXIT_EVENTS
    str     r0,  [r11, #PSX_OFF_EXIT_REASON]
    pop     {r4-r12, pc}
    .size jit_exit, . - jit_exit

#if defined(__linux__) && defined(__ELF__)
    .section .note.GNU-stack,"",%progbits
#endif
