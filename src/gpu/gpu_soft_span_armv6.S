/*
 * Strict ARMv6K SIMD Software Rasterizer Span Kernels (Nintendo 3DS ARM11 MPCore)
 *
 * Uses ONLY ARMv6K instructions:
 * - UADD16  : Dual 16-bit 8.8 UV coordinate stepping without cross-lane carry
 * - PKHBT   : Dual 16-bit RGB555 pixel packing for 32-bit word stores
 * - USAT    : Single-cycle signed->unsigned 5-bit channel saturation with barrel shift
 * - UHADD8  : Quad 8-bit unsigned halving add (PSX ABR 0: 0.5B + 0.5F)
 * - UQADD8  : Quad 8-bit unsigned saturating add (PSX ABR 1: B + F & ABR 3: B + 0.25F)
 * - UQSUB8  : Quad 8-bit unsigned saturating sub (PSX ABR 2: B - F)
 *
 * Strictly zero ARMv7 / Thumb-2 / NEON instructions.
 */
    .syntax unified
    .arch armv6k
    .arm
    .text

/* Helper macro: pack 0x00BBGGRR in \reg8 back to 15-bit 0BGR555 in \out15 using \t1, \t2
 * (\out15 may safely alias \reg8 because \t1 and \t2 are extracted first) */
.macro PACK_888_TO_555 out15, reg8, t1, t2
    and     \t1, \reg8, #0xF800
    and     \t2, \reg8, #0xF80000
    and     \out15, \reg8, #0xF8
    mov     \out15, \out15, lsr #3
    orr     \out15, \out15, \t1, lsr #6
    orr     \out15, \out15, \t2, lsr #9
.endm

/* Helper macro: ABR blend \b8 and \f8 into \out8 using mode \mode_reg (0..3) and scratch \tmp */
.macro BLEND_ABR_DYN out8, b8, f8, mode_reg, tmp
    cmp     \mode_reg, #1
    blo     10f
    beq     11f
    cmp     \mode_reg, #2
    beq     12f
    mov     \tmp, \f8, lsr #2
    uqadd8  \out8, \b8, \tmp
    b       13f
10:
    uhadd8  \out8, \b8, \f8
    b       13f
11:
    uqadd8  \out8, \b8, \f8
    b       13f
12:
    uqsub8  \out8, \b8, \f8
13:
.endm

/* ==========================================================================
 * 1. void gpu_soft_span_fill16_armv6(u16 *dst, u16 val, u32 count)
 *    r0 = dst, r1 = val, r2 = count
 * ========================================================================== */
    .align 2
    .global gpu_soft_span_fill16_armv6
    .type gpu_soft_span_fill16_armv6, %function
gpu_soft_span_fill16_armv6:
    cmp     r2, #0
    bxeq    lr
    uxth    r1, r1
    tst     r0, #2
    strhne  r1, [r0], #2
    subne   r2, r2, #1
    cmp     r2, #0
    bxeq    lr

    pkhbt   r1, r1, r1, lsl #16
    cmp     r2, #8
    blo     .Lfill_pairs

    push    {r4, r5}
    mov     r3, r1
    mov     r4, r1
    mov     r5, r1
.Lfill_oct:
    subs    r2, r2, #8
    stmiahs r0!, {r1, r3, r4, r5}
    bhi     .Lfill_oct
    pop     {r4, r5}
    bxeq    lr
    add     r2, r2, #8

.Lfill_pairs:
    subs    r2, r2, #2
    strhs   r1, [r0], #4
    bhi     .Lfill_pairs
    bxeq    lr
    add     r2, r2, #2
    tst     r2, #1
    strhne  r1, [r0]
    bx      lr
    .size gpu_soft_span_fill16_armv6, . - gpu_soft_span_fill16_armv6

/* ==========================================================================
 * 2. void gpu_soft_span_flat_abr_armv6(u16 *dst, u32 count, u16 col15,
 *                                      u32 abr, u16 mask_or)
 *    r0 = dst, r1 = count, r2 = col15, r3 = abr, [sp+0] = mask_or
 * ========================================================================== */
    .align 2
    .global gpu_soft_span_flat_abr_armv6
    .type gpu_soft_span_flat_abr_armv6, %function
gpu_soft_span_flat_abr_armv6:
    cmp     r1, #0
    bxeq    lr
    push    {r4, r5, r6, r7, r8, r9, r10, lr}
    ldrh    r4, [sp, #32]                   /* r4 = mask_or */
    ldr     r5, .Llut_flat_abr              /* r5 = g_gpu_rgb555_to_888 */
    mov     r2, r2, lsl #17
    mov     r2, r2, lsr #17
    ldr     r2, [r5, r2, lsl #2]            /* r2 = f8 */
    and     r3, r3, #3

    /* Align dst to 4-byte boundary for dual-pixel LDR/PKHBT/STR loop */
    tst     r0, #2
    beq     .Lflat_aligned
    ldrh    r6, [r0]
    bic     r6, r6, #0x8000
    ldr     r6, [r5, r6, lsl #2]
    BLEND_ABR_DYN r6, r6, r2, r3, r7
    PACK_888_TO_555 r6, r6, r7, r8
    orr     r6, r6, r4
    strh    r6, [r0], #2
    subs    r1, r1, #1
    beq     .Lflat_done

.Lflat_aligned:
    cmp     r3, #1
    blo     .Lflat_abr0_pair
    beq     .Lflat_abr1_pair
    cmp     r3, #2
    beq     .Lflat_abr2_pair

    /* ABR 3: B + 0.25F (pre-shift f8 >>= 2 once outside loop!) */
    mov     r2, r2, lsr #2
.Lflat_abr3_pair:
    subs    r1, r1, #2
    blo     .Lflat_abr3_tail
    ldr     r6, [r0]
    mov     r7, r6, lsl #17
    mov     r7, r7, lsr #17
    ldr     r7, [r5, r7, lsl #2]
    bic     r8, r6, #0x80000000
    mov     r8, r8, lsr #16
    ldr     r8, [r5, r8, lsl #2]
    uqadd8  r7, r7, r2
    uqadd8  r8, r8, r2
    PACK_888_TO_555 r7, r7, r9, r10
    PACK_888_TO_555 r8, r8, r9, r10
    orr     r7, r7, r4
    orr     r8, r8, r4
    pkhbt   r7, r7, r8, lsl #16
    str     r7, [r0], #4
    bhi     .Lflat_abr3_pair
    b       .Lflat_done
.Lflat_abr3_tail:
    adds    r1, r1, #2
    beq     .Lflat_done
    ldrh    r6, [r0]
    bic     r6, r6, #0x8000
    ldr     r6, [r5, r6, lsl #2]
    uqadd8  r6, r6, r2
    PACK_888_TO_555 r6, r6, r9, r10
    orr     r6, r6, r4
    strh    r6, [r0]
    b       .Lflat_done

    /* ABR 0: 0.5B + 0.5F via UHADD8 */
.Lflat_abr0_pair:
    subs    r1, r1, #2
    blo     .Lflat_abr0_tail
    ldr     r6, [r0]
    mov     r7, r6, lsl #17
    mov     r7, r7, lsr #17
    ldr     r7, [r5, r7, lsl #2]
    bic     r8, r6, #0x80000000
    mov     r8, r8, lsr #16
    ldr     r8, [r5, r8, lsl #2]
    uhadd8  r7, r7, r2
    uhadd8  r8, r8, r2
    PACK_888_TO_555 r7, r7, r9, r10
    PACK_888_TO_555 r8, r8, r9, r10
    orr     r7, r7, r4
    orr     r8, r8, r4
    pkhbt   r7, r7, r8, lsl #16
    str     r7, [r0], #4
    bhi     .Lflat_abr0_pair
    b       .Lflat_done
.Lflat_abr0_tail:
    adds    r1, r1, #2
    beq     .Lflat_done
    ldrh    r6, [r0]
    bic     r6, r6, #0x8000
    ldr     r6, [r5, r6, lsl #2]
    uhadd8  r6, r6, r2
    PACK_888_TO_555 r6, r6, r9, r10
    orr     r6, r6, r4
    strh    r6, [r0]
    b       .Lflat_done

    /* ABR 1: 1.0B + 1.0F via UQADD8 */
.Lflat_abr1_pair:
    subs    r1, r1, #2
    blo     .Lflat_abr1_tail
    ldr     r6, [r0]
    mov     r7, r6, lsl #17
    mov     r7, r7, lsr #17
    ldr     r7, [r5, r7, lsl #2]
    bic     r8, r6, #0x80000000
    mov     r8, r8, lsr #16
    ldr     r8, [r5, r8, lsl #2]
    uqadd8  r7, r7, r2
    uqadd8  r8, r8, r2
    PACK_888_TO_555 r7, r7, r9, r10
    PACK_888_TO_555 r8, r8, r9, r10
    orr     r7, r7, r4
    orr     r8, r8, r4
    pkhbt   r7, r7, r8, lsl #16
    str     r7, [r0], #4
    bhi     .Lflat_abr1_pair
    b       .Lflat_done
.Lflat_abr1_tail:
    adds    r1, r1, #2
    beq     .Lflat_done
    ldrh    r6, [r0]
    bic     r6, r6, #0x8000
    ldr     r6, [r5, r6, lsl #2]
    uqadd8  r6, r6, r2
    PACK_888_TO_555 r6, r6, r9, r10
    orr     r6, r6, r4
    strh    r6, [r0]
    b       .Lflat_done

    /* ABR 2: 1.0B - 1.0F via UQSUB8 */
.Lflat_abr2_pair:
    subs    r1, r1, #2
    blo     .Lflat_abr2_tail
    ldr     r6, [r0]
    mov     r7, r6, lsl #17
    mov     r7, r7, lsr #17
    ldr     r7, [r5, r7, lsl #2]
    bic     r8, r6, #0x80000000
    mov     r8, r8, lsr #16
    ldr     r8, [r5, r8, lsl #2]
    uqsub8  r7, r7, r2
    uqsub8  r8, r8, r2
    PACK_888_TO_555 r7, r7, r9, r10
    PACK_888_TO_555 r8, r8, r9, r10
    orr     r7, r7, r4
    orr     r8, r8, r4
    pkhbt   r7, r7, r8, lsl #16
    str     r7, [r0], #4
    bhi     .Lflat_abr2_pair
    b       .Lflat_done
.Lflat_abr2_tail:
    adds    r1, r1, #2
    beq     .Lflat_done
    ldrh    r6, [r0]
    bic     r6, r6, #0x8000
    ldr     r6, [r5, r6, lsl #2]
    uqsub8  r6, r6, r2
    PACK_888_TO_555 r6, r6, r9, r10
    orr     r6, r6, r4
    strh    r6, [r0]

.Lflat_done:
    pop     {r4, r5, r6, r7, r8, r9, r10, pc}
    .align 2
.Llut_flat_abr:
    .word   g_gpu_rgb555_to_888
    .size gpu_soft_span_flat_abr_armv6, . - gpu_soft_span_flat_abr_armv6

/* ==========================================================================
 * 3. void gpu_soft_span_gouraud_opaque_armv6(u16 *dst, u32 count,
 *        s32 r_q16, s32 g_q16, s32 b_q16, s32 dr_q16, s32 dg_q16, s32 db_q16,
 *        u16 mask_or)
 *    r0=dst, r1=count, r2=r, r3=g, [sp+0]=b, [sp+4]=dr, [sp+8]=dg, [sp+12]=db, [sp+16]=mask_or
 * ========================================================================== */
    .align 2
    .global gpu_soft_span_gouraud_opaque_armv6
    .type gpu_soft_span_gouraud_opaque_armv6, %function
gpu_soft_span_gouraud_opaque_armv6:
    cmp     r1, #0
    bxeq    lr
    push    {r4, r5, r6, r7, r8, r9, r10, lr}
    ldr     r4, [sp, #32]                   /* b_q16 */
    ldr     r5, [sp, #36]                   /* dr_q16 */
    ldr     r6, [sp, #40]                   /* dg_q16 */
    ldr     r7, [sp, #44]                   /* db_q16 */
    ldrh    r8, [sp, #48]                   /* mask_or */

    tst     r0, #2
    beq     .Lgour_aligned
    usat    r9,  #5, r2, asr #19
    usat    r10, #5, r3, asr #19
    usat    r12, #5, r4, asr #19
    add     r2, r2, r5
    add     r3, r3, r6
    add     r4, r4, r7
    orr     r9, r9, r8
    orr     r9, r9, r10, lsl #5
    orr     r9, r9, r12, lsl #10
    strh    r9, [r0], #2
    subs    r1, r1, #1
    beq     .Lgour_done

.Lgour_aligned:
    subs    r1, r1, #2
    blo     .Lgour_tail
.Lgour_pair_loop:
    usat    r9,  #5, r2, asr #19
    usat    r10, #5, r3, asr #19
    usat    r12, #5, r4, asr #19
    add     r2, r2, r5
    add     r3, r3, r6
    add     r4, r4, r7
    orr     r9, r9, r8
    orr     r9, r9, r10, lsl #5
    orr     r9, r9, r12, lsl #10

    usat    r10, #5, r2, asr #19
    usat    r12, #5, r3, asr #19
    usat    lr,  #5, r4, asr #19
    add     r2, r2, r5
    add     r3, r3, r6
    add     r4, r4, r7
    orr     r10, r10, r8
    orr     r10, r10, r12, lsl #5
    orr     r10, r10, lr, lsl #10

    pkhbt   r9, r9, r10, lsl #16
    str     r9, [r0], #4
    subs    r1, r1, #2
    bhs     .Lgour_pair_loop

.Lgour_tail:
    adds    r1, r1, #2
    beq     .Lgour_done
    usat    r9,  #5, r2, asr #19
    usat    r10, #5, r3, asr #19
    usat    r12, #5, r4, asr #19
    orr     r9, r9, r8
    orr     r9, r9, r10, lsl #5
    orr     r9, r9, r12, lsl #10
    strh    r9, [r0]

.Lgour_done:
    pop     {r4, r5, r6, r7, r8, r9, r10, pc}
    .size gpu_soft_span_gouraud_opaque_armv6, . - gpu_soft_span_gouraud_opaque_armv6

/* ==========================================================================
 * 4. void gpu_soft_span_gouraud_abr_armv6(u16 *dst, u32 count,
 *        s32 r_q16, s32 g_q16, s32 b_q16, s32 dr_q16, s32 dg_q16, s32 db_q16,
 *        u32 abr, u16 mask_or)
 * ========================================================================== */
    .align 2
    .global gpu_soft_span_gouraud_abr_armv6
    .type gpu_soft_span_gouraud_abr_armv6, %function
gpu_soft_span_gouraud_abr_armv6:
    cmp     r1, #0
    bxeq    lr
    push    {r4, r5, r6, r7, r8, r9, r10, r11, r12, lr}
    ldr     r4, [sp, #40]                   /* b_q16 */
    ldr     r5, [sp, #44]                   /* dr_q16 */
    ldr     r6, [sp, #48]                   /* dg_q16 */
    ldr     r7, [sp, #52]                   /* db_q16 */
    ldr     r8, [sp, #56]                   /* abr */
    ldrh    r9, [sp, #60]                   /* mask_or */
    and     r8, r8, #3
    ldr     r10, .Llut_gour_abr

.Lgour_abr_loop:
    usat    r11, #5, r2, asr #19
    usat    r12, #5, r3, asr #19
    usat    lr,  #5, r4, asr #19
    add     r2, r2, r5
    add     r3, r3, r6
    add     r4, r4, r7

    mov     r11, r11, lsl #3
    orr     r11, r11, r12, lsl #11
    orr     r11, r11, lr, lsl #19           /* r11 = f8 */

    ldrh    r12, [r0]
    bic     r12, r12, #0x8000
    ldr     r12, [r10, r12, lsl #2]         /* r12 = b8 */

    BLEND_ABR_DYN r12, r12, r11, r8, lr
    PACK_888_TO_555 r12, r12, r11, lr
    orr     r12, r12, r9
    strh    r12, [r0], #2
    subs    r1, r1, #1
    bne     .Lgour_abr_loop

    pop     {r4, r5, r6, r7, r8, r9, r10, r11, r12, pc}
    .align 2
.Llut_gour_abr:
    .word   g_gpu_rgb555_to_888
    .size gpu_soft_span_gouraud_abr_armv6, . - gpu_soft_span_gouraud_abr_armv6

/* ==========================================================================
 * 4b. void gpu_soft_span_gouraud_dither_opaque_armv6(u16 *dst, u32 count,
 *        s32 r_q16, s32 g_q16, s32 b_q16, s32 dr_q16, s32 dg_q16, s32 db_q16,
 *        u32 dither_pack, u16 mask_or)
 *    r0=dst, r1=count, r2=r, r3=g, [sp+0]=b, [sp+4]=dr, [sp+8]=dg, [sp+12]=db,
 *    [sp+16]=dither_pack, [sp+20]=mask_or
 *
 *    Per pixel (bit-identical to gpu_soft_span_general):
 *      d  = SXTB(pack); pack = ROR(pack, 8)
 *      c8 = USAT8(c_q16 ASR 16); c5 = USAT5((c8 + d) ASR 3)
 *    Dual-pixel PKHBT + 32-bit STR on word-aligned destinations.
 * ========================================================================== */
/* GDITH_PIX out, t: compute one dithered 15-bit pixel into \out (clobbers \t),
 * consuming r10 = d and stepping r2..r4 by r5..r7 */
.macro GDITH_PIX out, t
    usat    \out, #8, r2, asr #16
    usat    \t,   #8, r3, asr #16
    add     \out, \out, r10
    add     \t,   \t,   r10
    usat    \out, #5, \out, asr #3
    usat    \t,   #5, \t,   asr #3
    orr     \out, \out, \t, lsl #5
    usat    \t,   #8, r4, asr #16
    add     r2, r2, r5
    add     \t,   \t,   r10
    add     r3, r3, r6
    usat    \t,   #5, \t,   asr #3
    add     r4, r4, r7
    orr     \out, \out, \t, lsl #10
.endm

    .align 2
    .global gpu_soft_span_gouraud_dither_opaque_armv6
    .type gpu_soft_span_gouraud_dither_opaque_armv6, %function
gpu_soft_span_gouraud_dither_opaque_armv6:
    cmp     r1, #0
    bxeq    lr
    push    {r4, r5, r6, r7, r8, r9, r10, r11, lr}
    ldr     r4, [sp, #36]                   /* b_q16 */
    ldr     r5, [sp, #40]                   /* dr_q16 */
    ldr     r6, [sp, #44]                   /* dg_q16 */
    ldr     r7, [sp, #48]                   /* db_q16 */
    ldr     r8, [sp, #52]                   /* dither_pack */
    ldrh    r9, [sp, #56]                   /* mask_or */
    orr     r9, r9, r9, lsl #16             /* mask_or in both halfwords */

    tst     r0, #2
    beq     .Lgdith_aligned
    sxtb    r10, r8
    mov     r8, r8, ror #8
    GDITH_PIX r11, r12
    orr     r11, r11, r9
    strh    r11, [r0], #2
    subs    r1, r1, #1
    beq     .Lgdith_done

.Lgdith_aligned:
    subs    r1, r1, #2
    blo     .Lgdith_tail
.Lgdith_pair_loop:
    sxtb    r10, r8
    mov     r8, r8, ror #8
    GDITH_PIX r11, r12
    sxtb    r10, r8
    mov     r8, r8, ror #8
    GDITH_PIX r12, lr
    pkhbt   r11, r11, r12, lsl #16
    orr     r11, r11, r9
    str     r11, [r0], #4
    subs    r1, r1, #2
    bhs     .Lgdith_pair_loop

.Lgdith_tail:
    adds    r1, r1, #2
    beq     .Lgdith_done
    sxtb    r10, r8
    GDITH_PIX r11, r12
    orr     r11, r11, r9
    strh    r11, [r0]

.Lgdith_done:
    pop     {r4, r5, r6, r7, r8, r9, r10, r11, pc}
    .size gpu_soft_span_gouraud_dither_opaque_armv6, . - gpu_soft_span_gouraud_dither_opaque_armv6

/* ==========================================================================
 * 5. Textured Raw Opaque Spans (UADD16 8.8 UV stepping)
 * ========================================================================== */
    .align 2
    .global gpu_soft_span_tex4_raw_opaque_armv6
    .type gpu_soft_span_tex4_raw_opaque_armv6, %function
gpu_soft_span_tex4_raw_opaque_armv6:
    cmp     r1, #0
    bxeq    lr
    push    {r4, r5, r6, r7, r8, lr}
    ldr     r4, [sp, #24]                   /* uv_pack */
    ldr     r5, [sp, #28]                   /* duv_pack */
    ldrh    r6, [sp, #32]                   /* mask_or */
.Ltex4_raw_loop:
    mov     r7, r4, lsr #8
    and     r7, r7, #0xFF                   /* u */
    mov     r8, r4, lsr #24                 /* v */
    uadd16  r4, r4, r5                      /* dual 8.8 UV step */
    add     r8, r2, r8, lsl #11
    bic     r12, r7, #3
    add     r8, r8, r12, lsr #1
    ldrh    r8, [r8]
    and     r7, r7, #3
    mov     r7, r7, lsl #2
    mov     r8, r8, lsr r7
    and     r8, r8, #0xF
    add     r8, r3, r8, lsl #1
    ldrh    r8, [r8]
    cmp     r8, #0
    orrne   r8, r8, r6
    strhne  r8, [r0]
    add     r0, r0, #2
    subs    r1, r1, #1
    bne     .Ltex4_raw_loop
    pop     {r4, r5, r6, r7, r8, pc}
    .size gpu_soft_span_tex4_raw_opaque_armv6, . - gpu_soft_span_tex4_raw_opaque_armv6

    .align 2
    .global gpu_soft_span_tex8_raw_opaque_armv6
    .type gpu_soft_span_tex8_raw_opaque_armv6, %function
gpu_soft_span_tex8_raw_opaque_armv6:
    cmp     r1, #0
    bxeq    lr
    push    {r4, r5, r6, r7, r8, lr}
    ldr     r4, [sp, #24]                   /* uv_pack */
    ldr     r5, [sp, #28]                   /* duv_pack */
    ldrh    r6, [sp, #32]                   /* mask_or */
.Ltex8_raw_loop:
    mov     r7, r4, lsr #8
    and     r7, r7, #0xFF
    mov     r8, r4, lsr #24
    uadd16  r4, r4, r5
    add     r8, r2, r8, lsl #11
    ldrb    r8, [r8, r7]
    add     r8, r3, r8, lsl #1
    ldrh    r8, [r8]
    cmp     r8, #0
    orrne   r8, r8, r6
    strhne  r8, [r0]
    add     r0, r0, #2
    subs    r1, r1, #1
    bne     .Ltex8_raw_loop
    pop     {r4, r5, r6, r7, r8, pc}
    .size gpu_soft_span_tex8_raw_opaque_armv6, . - gpu_soft_span_tex8_raw_opaque_armv6

    .align 2
    .global gpu_soft_span_tex16_raw_opaque_armv6
    .type gpu_soft_span_tex16_raw_opaque_armv6, %function
gpu_soft_span_tex16_raw_opaque_armv6:
    /* r0=dst, r1=count, r2=tex_base, r3=uv_pack, [sp+0]=duv_pack, [sp+4]=mask_or */
    cmp     r1, #0
    bxeq    lr
    push    {r4, r5, r6, r7, r8, lr}
    ldr     r4, [sp, #24]                   /* duv_pack */
    ldrh    r5, [sp, #28]                   /* mask_or */
.Ltex16_raw_loop:
    mov     r6, r3, lsr #8
    and     r6, r6, #0xFF
    mov     r7, r3, lsr #24
    uadd16  r3, r3, r4
    add     r7, r2, r7, lsl #11
    add     r7, r7, r6, lsl #1
    ldrh    r8, [r7]
    cmp     r8, #0
    orrne   r8, r8, r5
    strhne  r8, [r0]
    add     r0, r0, #2
    subs    r1, r1, #1
    bne     .Ltex16_raw_loop
    pop     {r4, r5, r6, r7, r8, pc}
    .size gpu_soft_span_tex16_raw_opaque_armv6, . - gpu_soft_span_tex16_raw_opaque_armv6

/* ==========================================================================
 * 6. Textured Flat-Modulated Opaque Spans (UADD16 + USAT #5, ASR #7)
 * ========================================================================== */
.macro MODULATE_TEXEL_OPAQUE tex, cr, cg, cb, mask_or, out, tmp
    and     \out, \tex, #0x1F
    mul     \out, \cr, \out
    usat    \out, #5, \out, asr #7

    mov     \tmp, \tex, lsr #5
    and     \tmp, \tmp, #0x1F
    mul     \tmp, \cg, \tmp
    usat    \tmp, #5, \tmp, asr #7
    orr     \out, \out, \tmp, lsl #5

    mov     \tmp, \tex, lsr #10
    and     \tmp, \tmp, #0x1F
    mul     \tmp, \cb, \tmp
    usat    \tmp, #5, \tmp, asr #7
    orr     \out, \out, \tmp, lsl #10

    and     \tmp, \tex, #0x8000
    orr     \out, \out, \tmp
    orr     \out, \out, \mask_or
.endm

    .align 2
    .global gpu_soft_span_tex4_mod_flat_opaque_armv6
    .type gpu_soft_span_tex4_mod_flat_opaque_armv6, %function
gpu_soft_span_tex4_mod_flat_opaque_armv6:
    cmp     r1, #0
    bxeq    lr
    push    {r4, r5, r6, r7, r8, r9, r10, r11}
    ldr     r4, [sp, #32]                   /* uv_pack */
    ldr     r5, [sp, #36]                   /* duv_pack */
    ldr     r6, [sp, #40]                   /* rgb_mod */
    ldrh    r7, [sp, #44]                   /* mask_or */
    and     r8, r6, #0xFF
    mov     r9, r6, lsr #8
    and     r9, r9, #0xFF
    mov     r10, r6, lsr #16
    and     r10, r10, #0xFF
.Ltex4_mod_loop:
    mov     r6, r4, lsr #8
    and     r6, r6, #0xFF
    mov     r11, r4, lsr #24
    uadd16  r4, r4, r5
    add     r11, r2, r11, lsl #11
    bic     r12, r6, #3
    add     r11, r11, r12, lsr #1
    ldrh    r12, [r11]
    and     r6, r6, #3
    mov     r6, r6, lsl #2
    mov     r12, r12, lsr r6
    and     r12, r12, #0xF
    add     r12, r3, r12, lsl #1
    ldrh    r12, [r12]
    cmp     r12, #0
    beq     .Ltex4_mod_skip
    MODULATE_TEXEL_OPAQUE r12, r8, r9, r10, r7, r6, r11
    strh    r6, [r0]
.Ltex4_mod_skip:
    add     r0, r0, #2
    subs    r1, r1, #1
    bne     .Ltex4_mod_loop
    pop     {r4, r5, r6, r7, r8, r9, r10, r11}
    bx      lr
    .size gpu_soft_span_tex4_mod_flat_opaque_armv6, . - gpu_soft_span_tex4_mod_flat_opaque_armv6

    .align 2
    .global gpu_soft_span_tex8_mod_flat_opaque_armv6
    .type gpu_soft_span_tex8_mod_flat_opaque_armv6, %function
gpu_soft_span_tex8_mod_flat_opaque_armv6:
    cmp     r1, #0
    bxeq    lr
    push    {r4, r5, r6, r7, r8, r9, r10, r11}
    ldr     r4, [sp, #32]
    ldr     r5, [sp, #36]
    ldr     r6, [sp, #40]
    ldrh    r7, [sp, #44]
    and     r8, r6, #0xFF
    mov     r9, r6, lsr #8
    and     r9, r9, #0xFF
    mov     r10, r6, lsr #16
    and     r10, r10, #0xFF
.Ltex8_mod_loop:
    mov     r6, r4, lsr #8
    and     r6, r6, #0xFF
    mov     r11, r4, lsr #24
    uadd16  r4, r4, r5
    add     r11, r2, r11, lsl #11
    ldrb    r12, [r11, r6]
    add     r12, r3, r12, lsl #1
    ldrh    r12, [r12]
    cmp     r12, #0
    beq     .Ltex8_mod_skip
    MODULATE_TEXEL_OPAQUE r12, r8, r9, r10, r7, r6, r11
    strh    r6, [r0]
.Ltex8_mod_skip:
    add     r0, r0, #2
    subs    r1, r1, #1
    bne     .Ltex8_mod_loop
    pop     {r4, r5, r6, r7, r8, r9, r10, r11}
    bx      lr
    .size gpu_soft_span_tex8_mod_flat_opaque_armv6, . - gpu_soft_span_tex8_mod_flat_opaque_armv6

    .align 2
    .global gpu_soft_span_tex16_mod_flat_opaque_armv6
    .type gpu_soft_span_tex16_mod_flat_opaque_armv6, %function
gpu_soft_span_tex16_mod_flat_opaque_armv6:
    /* r0=dst, r1=count, r2=tex_base, r3=uv_pack, [sp+0]=duv, [sp+4]=rgb_mod, [sp+8]=mask_or */
    cmp     r1, #0
    bxeq    lr
    push    {r4, r5, r6, r7, r8, r9, r10, r11}
    ldr     r4, [sp, #32]                   /* duv_pack */
    ldr     r5, [sp, #36]                   /* rgb_mod */
    ldrh    r7, [sp, #40]                   /* mask_or */
    and     r8, r5, #0xFF
    mov     r9, r5, lsr #8
    and     r9, r9, #0xFF
    mov     r10, r5, lsr #16
    and     r10, r10, #0xFF
.Ltex16_mod_loop:
    mov     r6, r3, lsr #8
    and     r6, r6, #0xFF
    mov     r11, r3, lsr #24
    uadd16  r3, r3, r4
    add     r11, r2, r11, lsl #11
    add     r11, r11, r6, lsl #1
    ldrh    r12, [r11]
    cmp     r12, #0
    beq     .Ltex16_mod_skip
    MODULATE_TEXEL_OPAQUE r12, r8, r9, r10, r7, r6, r11
    strh    r6, [r0]
.Ltex16_mod_skip:
    add     r0, r0, #2
    subs    r1, r1, #1
    bne     .Ltex16_mod_loop
    pop     {r4, r5, r6, r7, r8, r9, r10, r11}
    bx      lr
    .size gpu_soft_span_tex16_mod_flat_opaque_armv6, . - gpu_soft_span_tex16_mod_flat_opaque_armv6

/* ==========================================================================
 * 7. Textured Flat-Modulated ABR Spans (Per-texel STP gated!)
 * ========================================================================== */
    .align 2
    .global gpu_soft_span_tex4_mod_flat_abr_armv6
    .type gpu_soft_span_tex4_mod_flat_abr_armv6, %function
gpu_soft_span_tex4_mod_flat_abr_armv6:
    /* r0=dst, r1=count, r2=tex_base, r3=clut, [sp+0]=uv, [sp+4]=duv, [sp+8]=mod, [sp+12]=abr, [sp+16]=mask_or */
    cmp     r1, #0
    bxeq    lr
    push    {r4, r5, r6, r7, r8, r9, r10, r11, r12, lr}
    ldr     r4, [sp, #40]                   /* uv_pack */
    ldr     r5, [sp, #44]                   /* duv_pack */
    ldr     r6, [sp, #48]                   /* rgb_mod */
    ldr     r7, [sp, #52]                   /* abr */
    and     r7, r7, #3
    and     r8, r6, #0xFF
    mov     r9, r6, lsr #8
    and     r9, r9, #0xFF
    mov     r10, r6, lsr #16
    and     r10, r10, #0xFF
.Ltex4_abr_loop:
    mov     r6, r4, lsr #8
    and     r6, r6, #0xFF
    mov     r11, r4, lsr #24
    uadd16  r4, r4, r5
    add     r11, r2, r11, lsl #11
    bic     r12, r6, #3
    add     r11, r11, r12, lsr #1
    ldrh    r12, [r11]
    and     r6, r6, #3
    mov     r6, r6, lsl #2
    mov     r12, r12, lsr r6
    and     r12, r12, #0xF
    add     r12, r3, r12, lsl #1
    ldrh    r12, [r12]
    cmp     r12, #0
    beq     .Ltex4_abr_skip

    and     r6, r12, #0x1F
    mul     r6, r8, r6
    usat    r6, #5, r6, asr #7              /* r5 */
    mov     r11, r12, lsr #5
    and     r11, r11, #0x1F
    mul     r11, r9, r11
    usat    r11, #5, r11, asr #7            /* g5 */
    mov     lr, r12, lsr #10
    and     lr, lr, #0x1F
    mul     lr, r10, lr
    usat    lr, #5, lr, asr #7              /* b5 */

    tst     r12, #0x8000
    bne     .Ltex4_abr_do_blend
    ldrh    r12, [sp, #56]                  /* mask_or */
    orr     r6, r6, r11, lsl #5
    orr     r6, r6, lr, lsl #10
    orr     r6, r6, r12
    strh    r6, [r0]
    b       .Ltex4_abr_skip

.Ltex4_abr_do_blend:
    mov     r6, r6, lsl #3
    orr     r6, r6, r11, lsl #11
    orr     r6, r6, lr, lsl #19             /* r6 = f8 */
    ldrh    r11, [r0]
    bic     r11, r11, #0x8000
    ldr     r12, .Llut_tex_abr
    ldr     r11, [r12, r11, lsl #2]         /* r11 = b8 */
    BLEND_ABR_DYN r11, r11, r6, r7, lr
    PACK_888_TO_555 r11, r11, r6, lr
    orr     r11, r11, #0x8000               /* stp == 0x8000 implies bit 15 = 1 */
    strh    r11, [r0]

.Ltex4_abr_skip:
    add     r0, r0, #2
    subs    r1, r1, #1
    bne     .Ltex4_abr_loop
    pop     {r4, r5, r6, r7, r8, r9, r10, r11, r12, pc}
    .size gpu_soft_span_tex4_mod_flat_abr_armv6, . - gpu_soft_span_tex4_mod_flat_abr_armv6

    .align 2
    .global gpu_soft_span_tex8_mod_flat_abr_armv6
    .type gpu_soft_span_tex8_mod_flat_abr_armv6, %function
gpu_soft_span_tex8_mod_flat_abr_armv6:
    cmp     r1, #0
    bxeq    lr
    push    {r4, r5, r6, r7, r8, r9, r10, r11, r12, lr}
    ldr     r4, [sp, #40]
    ldr     r5, [sp, #44]
    ldr     r6, [sp, #48]
    ldr     r7, [sp, #52]
    and     r7, r7, #3
    and     r8, r6, #0xFF
    mov     r9, r6, lsr #8
    and     r9, r9, #0xFF
    mov     r10, r6, lsr #16
    and     r10, r10, #0xFF
.Ltex8_abr_loop:
    mov     r6, r4, lsr #8
    and     r6, r6, #0xFF
    mov     r11, r4, lsr #24
    uadd16  r4, r4, r5
    add     r11, r2, r11, lsl #11
    ldrb    r12, [r11, r6]
    add     r12, r3, r12, lsl #1
    ldrh    r12, [r12]
    cmp     r12, #0
    beq     .Ltex8_abr_skip

    and     r6, r12, #0x1F
    mul     r6, r8, r6
    usat    r6, #5, r6, asr #7
    mov     r11, r12, lsr #5
    and     r11, r11, #0x1F
    mul     r11, r9, r11
    usat    r11, #5, r11, asr #7
    mov     lr, r12, lsr #10
    and     lr, lr, #0x1F
    mul     lr, r10, lr
    usat    lr, #5, lr, asr #7

    tst     r12, #0x8000
    bne     .Ltex8_abr_do_blend
    ldrh    r12, [sp, #56]
    orr     r6, r6, r11, lsl #5
    orr     r6, r6, lr, lsl #10
    orr     r6, r6, r12
    strh    r6, [r0]
    b       .Ltex8_abr_skip

.Ltex8_abr_do_blend:
    mov     r6, r6, lsl #3
    orr     r6, r6, r11, lsl #11
    orr     r6, r6, lr, lsl #19
    ldrh    r11, [r0]
    bic     r11, r11, #0x8000
    ldr     r12, .Llut_tex_abr
    ldr     r11, [r12, r11, lsl #2]
    BLEND_ABR_DYN r11, r11, r6, r7, lr
    PACK_888_TO_555 r11, r11, r6, lr
    orr     r11, r11, #0x8000
    strh    r11, [r0]

.Ltex8_abr_skip:
    add     r0, r0, #2
    subs    r1, r1, #1
    bne     .Ltex8_abr_loop
    pop     {r4, r5, r6, r7, r8, r9, r10, r11, r12, pc}
    .size gpu_soft_span_tex8_mod_flat_abr_armv6, . - gpu_soft_span_tex8_mod_flat_abr_armv6

    .align 2
    .global gpu_soft_span_tex16_mod_flat_abr_armv6
    .type gpu_soft_span_tex16_mod_flat_abr_armv6, %function
gpu_soft_span_tex16_mod_flat_abr_armv6:
    /* r0=dst, r1=count, r2=tex_base, r3=uv_pack, [sp+0]=duv, [sp+4]=mod, [sp+8]=abr, [sp+12]=mask_or */
    cmp     r1, #0
    bxeq    lr
    push    {r4, r5, r6, r7, r8, r9, r10, r11, r12, lr}
    ldr     r4, [sp, #40]                   /* duv_pack */
    ldr     r5, [sp, #44]                   /* rgb_mod */
    ldr     r7, [sp, #48]                   /* abr */
    and     r7, r7, #3
    and     r8, r5, #0xFF
    mov     r9, r5, lsr #8
    and     r9, r9, #0xFF
    mov     r10, r5, lsr #16
    and     r10, r10, #0xFF
.Ltex16_abr_loop:
    mov     r6, r3, lsr #8
    and     r6, r6, #0xFF
    mov     r11, r3, lsr #24
    uadd16  r3, r3, r4
    add     r11, r2, r11, lsl #11
    add     r11, r11, r6, lsl #1
    ldrh    r12, [r11]
    cmp     r12, #0
    beq     .Ltex16_abr_skip

    and     r6, r12, #0x1F
    mul     r6, r8, r6
    usat    r6, #5, r6, asr #7
    mov     r11, r12, lsr #5
    and     r11, r11, #0x1F
    mul     r11, r9, r11
    usat    r11, #5, r11, asr #7
    mov     lr, r12, lsr #10
    and     lr, lr, #0x1F
    mul     lr, r10, lr
    usat    lr, #5, lr, asr #7

    tst     r12, #0x8000
    bne     .Ltex16_abr_do_blend
    ldrh    r12, [sp, #52]
    orr     r6, r6, r11, lsl #5
    orr     r6, r6, lr, lsl #10
    orr     r6, r6, r12
    strh    r6, [r0]
    b       .Ltex16_abr_skip

.Ltex16_abr_do_blend:
    mov     r6, r6, lsl #3
    orr     r6, r6, r11, lsl #11
    orr     r6, r6, lr, lsl #19
    ldrh    r11, [r0]
    bic     r11, r11, #0x8000
    ldr     r12, .Llut_tex_abr
    ldr     r11, [r12, r11, lsl #2]
    BLEND_ABR_DYN r11, r11, r6, r7, lr
    PACK_888_TO_555 r11, r11, r6, lr
    orr     r11, r11, #0x8000
    strh    r11, [r0]

.Ltex16_abr_skip:
    add     r0, r0, #2
    subs    r1, r1, #1
    bne     .Ltex16_abr_loop
    pop     {r4, r5, r6, r7, r8, r9, r10, r11, r12, pc}

    .align 2
.Llut_tex_abr:
    .word   g_gpu_rgb555_to_888
    .size gpu_soft_span_tex16_mod_flat_abr_armv6, . - gpu_soft_span_tex16_mod_flat_abr_armv6

#if defined(__linux__) && defined(__ELF__)
    .section .note.GNU-stack,"",%progbits
#endif
