    .syntax unified
    .arch armv6k
    .arm
    .text
    .align 2

    .global spu_adpcm_decode_block_armv6
    .type spu_adpcm_decode_block_armv6, %function
spu_adpcm_decode_block_armv6:
    bx      lr
    .size spu_adpcm_decode_block_armv6, . - spu_adpcm_decode_block_armv6

#if defined(__linux__) && defined(__ELF__)
    .section .note.GNU-stack,"",%progbits
#endif
