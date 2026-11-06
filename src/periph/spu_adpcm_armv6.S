/*
 * ARM11 MPCore (ARMv6K) SIMD PS-ADPCM 16-byte -> 28-sample block decoder.
 * Bit-for-bit identical to spu_adpcm_decode_block_c().
 * Strict ARMv6K ISA: uses SMLAD, SSAT, PKHBT; zero ARMv7 instructions.
 */
    .syntax unified
    .arm
    .arch armv6k
    .text
    .align 2

/*
 * void spu_adpcm_decode_block_armv6(const u8 block[16], s16 out[28], s16 hist[2]);
 *   r0 = block
 *   r1 = out
 *   r2 = hist
 */
    .global spu_adpcm_decode_block_armv6
    .type spu_adpcm_decode_block_armv6, %function
spu_adpcm_decode_block_armv6:
    push    {r4-r10, lr}

    /* Parse header byte block[0]: shift = hdr & 0xF (>12 -> 9), filter = (hdr >> 4) & 7 */
    ldrb    r3, [r0], #2            /* skip block[1] flags; r0 now at block[2] */
    and     r4, r3, #0x0F           /* r4 = shift */
    cmp     r4, #12
    movgt   r4, #9
    mov     r3, r3, lsr #4
    and     r3, r3, #0x07           /* r3 = filter (0..7) */

    adr     r5, .Ladpcm_coeffs
    ldr     r5, [r5, r3, lsl #2]    /* r5 = [f1:16 | f0:16] */

    /* Load hist[0] (low 16) and hist[1] (high 16) safely for 2-byte alignment */
    ldrh    r6, [r2, #0]
    ldrh    r7, [r2, #2]
    orr     r6, r6, r7, lsl #16     /* r6 = [hist1:16 | hist0:16] */

    mov     r7, #32                 /* rounding constant +32 */
    mov     r8, #14                 /* 14 data bytes = 28 nibbles */

.Ladpcm_byte_loop:
    ldrb    r9, [r0], #1

    /* Low nibble */
    mov     r10, r9, lsl #28
    mov     r10, r10, asr #16
    mov     r10, r10, asr r4        /* s0 = ((s16)(nib_lo << 12)) >> shift */
    smlad   r3, r6, r5, r7          /* r3 = h0*f0 + h1*f1 + 32 */
    add     r3, r10, r3, asr #6
    ssat    r3, #16, r3
    pkhbt   r6, r3, r6, lsl #16
    strh    r3, [r1], #2

    /* High nibble */
    bic     r10, r9, #0x0F
    mov     r10, r10, lsl #24
    mov     r10, r10, asr #16
    mov     r10, r10, asr r4        /* s1 = ((s16)(nib_hi << 12)) >> shift */
    smlad   r3, r6, r5, r7          /* r3 = h0*f0 + h1*f1 + 32 */
    add     r3, r10, r3, asr #6
    ssat    r3, #16, r3
    pkhbt   r6, r3, r6, lsl #16
    strh    r3, [r1], #2

    subs    r8, r8, #1
    bne     .Ladpcm_byte_loop

    /* Store updated hist[0], hist[1] */
    strh    r6, [r2, #0]
    mov     r6, r6, lsr #16
    strh    r6, [r2, #2]

    pop     {r4-r10, pc}

    .align 2
.Ladpcm_coeffs:
    .word   0x00000000              /* filter 0: f0 =   0, f1 =   0 */
    .word   0x0000003C              /* filter 1: f0 =  60, f1 =   0 */
    .word   0xFFCC0073              /* filter 2: f0 = 115, f1 = -52 */
    .word   0xFFC90062              /* filter 3: f0 =  98, f1 = -55 */
    .word   0xFFC4007A              /* filter 4: f0 = 122, f1 = -60 */
    .word   0x00000000              /* filter 5 (invalid -> 0, 0)   */
    .word   0x00000000              /* filter 6 (invalid -> 0, 0)   */
    .word   0x00000000              /* filter 7 (invalid -> 0, 0)   */
    .size spu_adpcm_decode_block_armv6, .-spu_adpcm_decode_block_armv6

#if defined(__linux__) && defined(__ELF__)
    .section .note.GNU-stack,"",%progbits
#endif
