#ifndef PSX3DS_SPU_H
#define PSX3DS_SPU_H

#include "psx3ds/hw_regs.h"
#include "psx3ds/types.h"

typedef struct SpuVoice {
    u32 start_addr, repeat_addr, cur_addr;
    u16 vol_l, vol_r, pitch, adsr_lo, adsr_hi, adsr_vol;
    s16 hist[2];
    s16 ring[32];
    u32 counter;
    u8  adsr_phase, block_flags, keyed, reverb_on, noise_on, pmon;
    s32 env_level;
} SpuVoice;

typedef struct SpuCmd {
    u32 tick;
    u16 reg;
    u16 val;
} SpuCmd;

typedef struct PsxSpu {
    u8      *ram;         /* 512 KiB SPU RAM */
    u16      regs[0x200]; /* raw register image 0x1F801C00..0x1F801FFF */
    SpuVoice v[24];
    u32      spucnt, spustat, irq_addr, xfer_addr, xfer_ctrl, noise_lfsr, kon, koff, endx;
    s32      reverb_regs[32];
    u32      reverb_pos;
    s16     *reverb_buf;
    u16      fifo[32];
    u8       fifo_n;
    s16     *out_ring;
    u32      out_w, out_r, out_cap;
    SpuCmd  *cmd_ring;
    alignas(32) u32 cmd_head;
    alignas(32) u32 cmd_tail;
    s16      xa_ring[2][4096];
    u32      xa_w, xa_r;
    bool     async_mode, irq_enabled;
    u32      batch_tick;
} PsxSpu;

void spu_init(PsxSystem *sys);
void spu_reset(PsxSystem *sys);
void spu_state(PsxSystem *sys, StateIO *st);
void spu_shutdown(PsxSystem *sys);

void spu_render_batch(PsxSystem *sys, u32 frames);
void spu_push_xa(PsxSystem *sys, const s16 *stereo, u32 frames);

void spu_dma_write_words(PsxSystem *sys, const u32 *src, u32 n);
u32  spu_dma_read_words(PsxSystem *sys, u32 *dst, u32 n);

#endif /* PSX3DS_SPU_H */
