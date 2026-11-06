    .syntax unified
    .arch armv6k
    .arm
    .text
    .align 2

    .global gpu_soft_span_fill16_armv6
    .type gpu_soft_span_fill16_armv6, %function
gpu_soft_span_fill16_armv6:
    bx      lr
    .size gpu_soft_span_fill16_armv6, . - gpu_soft_span_fill16_armv6

#if defined(__linux__) && defined(__ELF__)
    .section .note.GNU-stack,"",%progbits
#endif
