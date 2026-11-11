#ifndef PSX3DS_MDEC_H
#define PSX3DS_MDEC_H

#include "psx3ds/hw_regs.h"
#include "psx3ds/types.h"

typedef struct PsxMdec {
    u32  status, control;
    u8   iq_y[64], iq_uv[64];
    s16  idct[64];
    u16  in_fifo[64];
    u32  in_r, in_w;
    u32  out_fifo[768];
    u32  out_r, out_w;
    s16  blocks[6][64];
    u32  cur_block, cur_coeff, words_left;
    u8   out_depth, out_signed, out_bit15;
    bool decoding;
} PsxMdec;

void mdec_init(PsxSystem *sys);
void mdec_reset(PsxSystem *sys);
void mdec_state(PsxSystem *sys, StateIO *st);
void mdec_shutdown(PsxSystem *sys);

u32  mdec_dma_in_words(PsxSystem *sys, const u32 *src, u32 n);
u32  mdec_dma_out_words(PsxSystem *sys, u32 *dst, u32 n);

/*
 * DMA1 (MDEC-Out, SyncMode 1) DREQ: number of `block_words`-sized blocks that may be
 * transferred right now. Returns 0 while the decoded-output FIFO is empty (or holds less
 * than one block while more output is still being decoded), so DMA1 stays armed instead
 * of draining zeros before DMA0 has delivered the compressed bitstream.
 */
u32  mdec_dma_out_blocks_ready(const PsxSystem *sys, u32 block_words);

#endif /* PSX3DS_MDEC_H */
