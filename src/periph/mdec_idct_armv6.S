    .syntax unified
    .arch armv6k
    .arm
    .text
    .align 2

    .global mdec_idct_armv6
    .type mdec_idct_armv6, %function
mdec_idct_armv6:
    bx      lr
    .size mdec_idct_armv6, . - mdec_idct_armv6

#if defined(__linux__) && defined(__ELF__)
    .section .note.GNU-stack,"",%progbits
#endif
