#ifndef PSX3DS_DMA_H
#define PSX3DS_DMA_H

#include "psx3ds/hw_regs.h"
#include "psx3ds/types.h"

typedef struct DmaChannel {
    u32 madr;
    u32 bcr;
    u32 chcr;
} DmaChannel;

typedef struct PsxDma {
    DmaChannel ch[7];
    u32        dpcr;
    u32        dicr;
    u8         running_mask;
} PsxDma;

void dma_init(PsxSystem *sys);
void dma_reset(PsxSystem *sys);
void dma_state(PsxSystem *sys, StateIO *st);
void dma_shutdown(PsxSystem *sys);

/* Device DMA hooks (declared here, implemented by device owners in slices C & E) */
u32  mdec_dma_in_words(PsxSystem *sys, const u32 *src, u32 n);
u32  mdec_dma_out_words(PsxSystem *sys, u32 *dst, u32 n);
u32  mdec_dma_out_blocks_ready(const PsxSystem *sys, u32 block_words);

/*
 * Device-side DREQ edge: a peripheral whose DMA request just went active (e.g. MDEC
 * produced output for an armed DMA1) calls this to wake the DMA engine on the next tick.
 * Safe to call from inside a DMA transfer callback (never re-enters dma_step_channel).
 */
void dma_request_wakeup(PsxSystem *sys, u32 ch);
void gpu_dma_write_words(PsxSystem *sys, const u32 *src, u32 n);
u32  gpu_dma_read_words(PsxSystem *sys, u32 *dst, u32 n);
u32  cdrom_dma_read_words(PsxSystem *sys, u32 *dst, u32 n);
void spu_dma_write_words(PsxSystem *sys, const u32 *src, u32 n);
u32  spu_dma_read_words(PsxSystem *sys, u32 *dst, u32 n);

#endif /* PSX3DS_DMA_H */
