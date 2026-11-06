#ifndef PSX3DS_PERIPH_INTERNAL_H
#define PSX3DS_PERIPH_INTERNAL_H

#include "psx3ds/cdrom.h"
#include "psx3ds/config.h"
#include "psx3ds/dma.h"
#include "psx3ds/hw_regs.h"
#include "psx3ds/irq.h"
#include "psx3ds/mdec.h"
#include "psx3ds/platform.h"
#include "psx3ds/scheduler.h"
#include "psx3ds/sio.h"
#include "psx3ds/spsc.h"
#include "psx3ds/spu.h"
#include "psx3ds/system.h"
#include "psx3ds/types.h"

#include <stdio.h>

/* --------------------------------------------------------------------------
 * Portable file wrapper: uses plat_fopen when implemented, with seamless
 * fallback to standard C stdio so standalone slice unit tests work out of box.
 * -------------------------------------------------------------------------- */
typedef struct PeriphFile {
    PlatFile *pf;
    FILE     *cf;
} PeriphFile;

static inline bool periph_fopen(PeriphFile *f, const char *path, const char *mode) {
    if (!f || !path || !mode) {
        return false;
    }
    f->pf = plat_fopen(path, mode);
    if (f->pf) {
        f->cf = NULL;
        return true;
    }
    f->cf = fopen(path, mode);
    return f->cf != NULL;
}

static inline void periph_fclose(PeriphFile *f) {
    if (!f) {
        return;
    }
    if (f->pf) {
        plat_fclose(f->pf);
        f->pf = NULL;
    }
    if (f->cf) {
        fclose(f->cf);
        f->cf = NULL;
    }
}

static inline bool periph_is_open(const PeriphFile *f) {
    return f && (f->pf != NULL || f->cf != NULL);
}

static inline bool periph_fseek(PeriphFile *f, u64 off) {
    if (!f) {
        return false;
    }
    if (f->pf) {
        return plat_fseek(f->pf, off);
    }
    if (f->cf) {
        return fseek(f->cf, (long)off, SEEK_SET) == 0;
    }
    return false;
}

static inline u32 periph_fread(PeriphFile *f, void *dst, u32 bytes) {
    if (!f || !dst || bytes == 0u) {
        return 0u;
    }
    if (f->pf) {
        return plat_fread(f->pf, dst, bytes);
    }
    if (f->cf) {
        return (u32)fread(dst, 1u, (size_t)bytes, f->cf);
    }
    return 0u;
}

static inline u32 periph_fwrite(PeriphFile *f, const void *src, u32 bytes) {
    if (!f || !src || bytes == 0u) {
        return 0u;
    }
    if (f->pf) {
        return plat_fwrite(f->pf, src, bytes);
    }
    if (f->cf) {
        u32 n = (u32)fwrite(src, 1u, (size_t)bytes, f->cf);
        fflush(f->cf);
        return n;
    }
    return 0u;
}

static inline u64 periph_fsize(PeriphFile *f) {
    if (!f) {
        return 0u;
    }
    if (f->pf) {
        return plat_fsize(f->pf);
    }
    if (f->cf) {
        long cur = ftell(f->cf);
        if (fseek(f->cf, 0L, SEEK_END) != 0) {
            return 0u;
        }
        long end = ftell(f->cf);
        if (cur >= 0L) {
            (void)fseek(f->cf, cur, SEEK_SET);
        }
        return (end > 0L) ? (u64)end : 0u;
    }
    return 0u;
}

/* --------------------------------------------------------------------------
 * SPU internal interfaces (spu.c, spu_adpcm.c, spu_adpcm_armv6.S, spu_reverb.c, spu_xa.c)
 * -------------------------------------------------------------------------- */
#define PSX_SPU_CMD_CAP      4096u /* power-of-two SpuCmd SPSC ring capacity */
#define PSX_SPU_OUT_CAP      8192u /* stereo frames capacity in out_ring */
#define SPU_CMD_REG_ADVANCE  0xFFFFu

void spu_adpcm_decode_block_c(const u8 block[16], s16 out[28], s16 hist[2]);
#if PSX3DS_ARCH_ARMV6K
void spu_adpcm_decode_block_armv6(const u8 block[16], s16 out[28], s16 hist[2]);
#endif
void spu_adpcm_decode_block(const u8 block[16], s16 out[28], s16 hist[2]);

void spu_check_irq_addr(PsxSystem *sys, u32 byte_addr);
void spu_check_irq_range(PsxSystem *sys, u32 byte_addr, u32 len);

void spu_reverb_reset(PsxSpu *spu);
void spu_reverb_process(PsxSystem *sys, s32 in_l, s32 in_r, s32 *out_l, s32 *out_r);

void spu_xa_reset(void);
void spu_xa_decode_sector(PsxSystem *sys, const u8 sector_2352[2352], const u8 vol[4]);
void spu_cdda_push_sector(PsxSystem *sys, const u8 sector_2352[2352], const u8 vol[4]);

u32  spu_mmio_read(PsxSystem *sys, u32 addr, u32 width);
void spu_mmio_write(PsxSystem *sys, u32 addr, u32 val, u32 width);
void spu_event_cb(PsxSystem *sys, u32 late_cycles);

/* --------------------------------------------------------------------------
 * CD-ROM internal interfaces (cdrom.c, cdrom_image.c, cdrom_readahead.c)
 * -------------------------------------------------------------------------- */
#define PSX_CD_RA_SECTORS   128u
#define PSX_CD_MAX_TRACKS   100u
#define PSX_CD_MAX_FILES    16u

typedef enum CdTrackMode {
    CD_TRACK_MODE2_2352 = 0,
    CD_TRACK_MODE1_2352 = 1,
    CD_TRACK_MODE1_2048 = 2,
    CD_TRACK_MODE2_2336 = 3,
    CD_TRACK_AUDIO_2352 = 4
} CdTrackMode;

typedef struct CdTrack {
    u8          track_no;    /* 1..99 */
    u8          mode;        /* CdTrackMode */
    u8          file_slot;   /* index into files[] or path table */
    u8          pad0;
    u32         start_lba;   /* INDEX 01 LBA (0-based disc LBA, 00:02:00 = LBA 0) */
    u32         index0_lba;  /* INDEX 00 LBA */
    u32         length_lba;  /* sectors in track */
    u32         sector_size; /* 2352, 2048, or 2336 */
    u64         file_offset; /* byte offset of start_lba in backing file */
} CdTrack;

typedef struct CdChdState CdChdState;

struct CdImage {
    bool        loaded;
    bool        small_mem;
    u8          track_count;
    u8          first_track;
    u8          last_track;
    u8          region_char; /* 'A' (NTSC-U), 'E' (PAL), 'I' (NTSC-J) */
    u32         total_lba;
    u32         file_count;
    CdChdState *chd;         /* non-NULL when backed by CHD v1-v5 image */
    char        file_paths[PSX_CD_MAX_FILES][256];
    PeriphFile  files[PSX_CD_MAX_FILES];
    CdTrack     tracks[PSX_CD_MAX_TRACKS];
    /* Per-instance internal drive latches (serialized in cdrom_state) */
    bool        motor_on;
    bool        header_valid;
    bool        bfrd;
    u8          last_header[8];
    u32         pending_delay[2];
    alignas(32) u32 ra_prod_lba;
    alignas(32) u8  io_lock;
};

void      cd_image_init(CdImage *img);
void      cd_image_close(CdImage *img);
PsxResult cd_image_open(CdImage *img, const char *path);
bool      cd_image_read_sector(CdImage *img, u32 lba, u8 out_2352[2352]);
const CdTrack *cd_image_find_track(const CdImage *img, u32 lba);
void      cd_synth_sync_msf(u32 lba, u8 mode_byte, u8 out_2352[2352]);

bool      cd_chd_has_magic(const char *path);
PsxResult cd_chd_open(CdImage *img, const char *path);
void      cd_chd_close_state(CdChdState *st);
bool      cd_chd_read_sector_locked(CdImage *img, u32 lba, u8 out_2352[2352]);
void      cd_chd_reset_cache_stats(CdImage *img);
void      cd_chd_get_cache_stats(const CdImage *img, u64 *out_hits, u64 *out_misses, u32 *out_slots);

void cd_lba_to_msf(u32 lba, u8 *mm, u8 *ss, u8 *ff);
u32  cd_msf_to_lba(u8 mm, u8 ss, u8 ff);
u8   cd_bin_to_bcd(u8 v);
u8   cd_bcd_to_bin(u8 bcd, bool *ok);

void cdrom_ra_init(PsxSystem *sys);
void cdrom_ra_reset(PsxSystem *sys);
void cdrom_ra_seek(PsxSystem *sys, u32 target_lba);
bool cdrom_ra_read_sector(PsxSystem *sys, u32 target_lba, u8 out_2352[2352]);
bool cdrom_ra_peek_or_read_sector(PsxSystem *sys, u32 target_lba, u8 out_2352[2352]);
u32  cdrom_ra_pump(PsxSystem *sys, u32 max_sectors);

u32  cdrom_mmio_read(PsxSystem *sys, u32 addr, u32 width);
void cdrom_mmio_write(PsxSystem *sys, u32 addr, u32 val, u32 width);
void cdrom_event_cb(PsxSystem *sys, u32 late_cycles);

/* --------------------------------------------------------------------------
 * MDEC internal interfaces (mdec.c, mdec_idct_armv6.S)
 * -------------------------------------------------------------------------- */
void mdec_idct_c(s16 blk[64], const s16 scale[64]);
#if PSX3DS_ARCH_ARMV6K
void mdec_idct_armv6(s16 blk[64], const s16 scale[64]);
#endif
void mdec_idct(s16 blk[64], const s16 scale[64]);

u32  mdec_mmio_read(PsxSystem *sys, u32 addr, u32 width);
void mdec_mmio_write(PsxSystem *sys, u32 addr, u32 val, u32 width);
void mdec_event_cb(PsxSystem *sys, u32 late_cycles);

/* --------------------------------------------------------------------------
 * SIO0, Joypad & Memory Card internal interfaces (sio0.c, joypad.c, memcard.c)
 * -------------------------------------------------------------------------- */
void joypad_reset_port(PsxSystem *sys, u32 port);
u8   joypad_transfer_byte(PsxSystem *sys, u32 port, u8 seq, u8 tx, bool *more);

void memcard_init_slot(PsxMemcard *mc, u8 *backing_128k, const char *path);
void memcard_reset_transfer(PsxMemcard *mc);
void memcard_format_fresh(u8 *data_128k);
bool memcard_load_file(PsxMemcard *mc);
bool memcard_flush_file(PsxMemcard *mc);
u8   memcard_transfer_byte(PsxMemcard *mc, u8 seq, u8 tx, bool *more);

u32  sio0_mmio_read(PsxSystem *sys, u32 addr, u32 width);
void sio0_mmio_write(PsxSystem *sys, u32 addr, u32 val, u32 width);
void sio0_event_cb(PsxSystem *sys, u32 late_cycles);

#endif /* PSX3DS_PERIPH_INTERNAL_H */
