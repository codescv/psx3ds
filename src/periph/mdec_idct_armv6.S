/*
 * ARM11 MPCore (ARMv6K) SIMD 2-pass 8x8 MDEC IDCT kernel.
 *
 * Strictly ARMv6K instructions only (SMUAD, SMLAD, SSAT, PKHBT).
 * Matches portable C mdec_idct_c() bit-for-bit across all 64 outputs.
 *
 * void mdec_idct_armv6(s16 blk[64], const s16 scale[64]);
 *   r0 = s16 blk[64]        (in/out, 2-byte aligned safe)
 *   r1 = const s16 scale[64] (in, 2-byte aligned safe)
 */

    .syntax unified
    .arch armv6k
    .arm
    .text
    .align 2
    .global mdec_idct_armv6
    .type mdec_idct_armv6, %function

mdec_idct_armv6:
    push    {r4-r11, lr}
    sub     sp, sp, #388            /* 36 + 388 = 424 bytes (8-byte AAPCS aligned) */
    str     r0, [sp, #384]          /* save blk pointer */

    /*
     * Step 1: Transpose blk[z*8 + c] -> blk_T[c*8 + z] at [sp, #0..127]
     *         and scale[z*8 + c]     -> scale_T[c*8 + z] at [sp, #128..255]
     */
    mov     r2, #0                  /* c = 0..7 */
.Ltranspose_loop:
    add     r3, r0, r2, lsl #1      /* &blk[0*8 + c] */
    add     r12, sp, r2, lsl #4     /* &blk_T[c*8 + 0] */

    ldrh    r4, [r3, #0]
    ldrh    r5, [r3, #16]
    ldrh    r6, [r3, #32]
    ldrh    r7, [r3, #48]
    ldrh    r8, [r3, #64]
    ldrh    r9, [r3, #80]
    ldrh    r10, [r3, #96]
    ldrh    r11, [r3, #112]
    pkhbt   r4, r4, r5, lsl #16
    pkhbt   r5, r6, r7, lsl #16
    pkhbt   r6, r8, r9, lsl #16
    pkhbt   r7, r10, r11, lsl #16
    stmia   r12, {r4, r5, r6, r7}

    add     r3, r1, r2, lsl #1      /* &scale[0*8 + c] */
    add     r12, r12, #128          /* &scale_T[c*8 + 0] */

    ldrh    r4, [r3, #0]
    ldrh    r5, [r3, #16]
    ldrh    r6, [r3, #32]
    ldrh    r7, [r3, #48]
    ldrh    r8, [r3, #64]
    ldrh    r9, [r3, #80]
    ldrh    r10, [r3, #96]
    ldrh    r11, [r3, #112]
    pkhbt   r4, r4, r5, lsl #16
    pkhbt   r5, r6, r7, lsl #16
    pkhbt   r6, r8, r9, lsl #16
    pkhbt   r7, r10, r11, lsl #16
    stmia   r12, {r4, r5, r6, r7}

    add     r2, r2, #1
    cmp     r2, #8
    bne     .Ltranspose_loop

    /*
     * Step 2 (Pass 1):
     *   tmp[y*8 + x] = (sum_{z=0..7} blk_T[x*8 + z] * scale_T[y*8 + z] + 0x8000) >> 16
     */
    mov     r0, #0                  /* x = 0..7 */
.Lpass1_x:
    add     r3, sp, r0, lsl #4      /* &blk_T[x*8] */
    ldmia   r3, {r4, r5, r6, r7}

    add     r1, sp, #128            /* &scale_T[0] */
    add     r2, sp, #256            /* &tmp[0] */
    add     r2, r2, r0, lsl #1      /* &tmp[0*8 + x] */
    mov     r12, #8                 /* y count = 8 */

.Lpass1_y:
    ldmia   r1!, {r8, r9, r10, r11}
    smuad   lr, r4, r8
    smlad   lr, r5, r9, lr
    smlad   lr, r6, r10, lr
    smlad   lr, r7, r11, lr
    add     lr, lr, #0x8000
    mov     lr, lr, asr #16
    strh    lr, [r2], #16           /* tmp[y*8 + x], advance y */
    subs    r12, r12, #1
    bne     .Lpass1_y

    add     r0, r0, #1
    cmp     r0, #8
    bne     .Lpass1_x

    /*
     * Step 3 (Pass 2):
     *   blk[y*8 + x] = clamp_s8((sum_{z=0..7} tmp[y*8 + z] * scale_T[x*8 + z] + 0x8000) >> 16)
     */
    ldr     r0, [sp, #384]          /* restore blk output pointer */
    mov     r3, #0                  /* y = 0..7 */
.Lpass2_y:
    add     r2, sp, #256
    add     r2, r2, r3, lsl #4      /* &tmp[y*8] */
    ldmia   r2, {r4, r5, r6, r7}

    add     r1, sp, #128            /* &scale_T[0] */
    mov     r12, #8                 /* x count = 8 */

.Lpass2_x:
    ldmia   r1!, {r8, r9, r10, r11}
    smuad   lr, r4, r8
    smlad   lr, r5, r9, lr
    smlad   lr, r6, r10, lr
    smlad   lr, r7, r11, lr
    add     lr, lr, #0x8000
    mov     lr, lr, asr #16
    ssat    lr, #8, lr              /* clamp to [-128, +127] */
    strh    lr, [r0], #2
    subs    r12, r12, #1
    bne     .Lpass2_x

    add     r3, r3, #1
    cmp     r3, #8
    bne     .Lpass2_y

    add     sp, sp, #388
    pop     {r4-r11, pc}

    .size mdec_idct_armv6, .-mdec_idct_armv6

#if defined(__linux__) && defined(__ELF__)
    .section .note.GNU-stack,"",%progbits
#endif
