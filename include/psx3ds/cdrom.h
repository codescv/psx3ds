#ifndef PSX3DS_CDROM_H
#define PSX3DS_CDROM_H

#include "psx3ds/hw_regs.h"
#include "psx3ds/types.h"

typedef struct CdImage CdImage;

typedef struct CdReadahead {
    u8  (*buf)[2352];
    u32  *lba;
    alignas(32) u32 head;
    alignas(32) u32 tail;
    u32   want_lba;
    u32   cap;
} CdReadahead;

typedef struct PsxCdrom {
    CdImage    *img;
    CdReadahead ra;
    u8          index, status, mode, irq_enable, irq_flag;
    u8          param_fifo[16], resp_fifo[16];
    u8          param_n, resp_r, resp_w;
    u8          sector[2352];
    u8          data_fifo[2352];
    u32         data_r, data_n;
    bool        data_ready;
    u8          pending_int[2], pending_resp[2][16], pending_n[2], pending_count;
    u32         seek_lba, cur_lba, setloc_lba;
    bool        setloc_pending, reading, playing, paused, muted, double_speed, xa_filter_on;
    u8          xa_file, xa_channel, cmd, state;
    u32         state_tick;
    u8          vol[4], vol_pending[4];
    u8          region;
} PsxCdrom;

void      cdrom_init(PsxSystem *sys);
void      cdrom_reset(PsxSystem *sys);
void      cdrom_state(PsxSystem *sys, StateIO *st);
void      cdrom_shutdown(PsxSystem *sys);

PsxResult cdrom_load(PsxSystem *sys, const char *path);
bool      cdrom_is_chd(const PsxSystem *sys);
void      cdrom_chd_cache_stats(const PsxSystem *sys, u64 *out_hits, u64 *out_misses, u32 *out_slots);
void      cdrom_readahead_thread(void *sys);
u32       cdrom_dma_read_words(PsxSystem *sys, u32 *dst, u32 n);

#endif /* PSX3DS_CDROM_H */
