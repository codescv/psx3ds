#include "periph_internal.h"
#include "psx3ds/arena.h"

#include <string.h>

#define CD_RA_LBA_MASK   0x00FFFFFFu
#define CD_RA_EPOCH_STEP 0x01000000u

/*
 * cd_image_read_sector() is an fseek+fread pair on the shared image FILE*: the Core 2
 * producer and the Core 0 miss path must never interleave inside it.
 */
static u32 g_cd_io_lock = 0u;

static bool cd_read_sector_serialized(CdImage *img, u32 lba, u8 out_2352[2352]) {
    u32 spins = 0u;
    while (__atomic_exchange_n(&g_cd_io_lock, 1u, __ATOMIC_ACQUIRE) != 0u) {
        if (++spins >= 32u) {
            plat_yield();
            spins = 0u;
        }
    }
    bool ok = cd_image_read_sector(img, lba, out_2352);
    __atomic_store_n(&g_cd_io_lock, 0u, __ATOMIC_RELEASE);
    return ok;
}

void cdrom_ra_init(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    CdReadahead *ra = &sys->cdrom.ra;
    ra->cap = PSX_CD_RA_SECTORS;
    ra->buf = (u8 (*)[2352])arena_alloc(&sys->arena, PSX_CD_RA_SECTORS * 2352u, 32u);
    ra->lba = (u32 *)arena_alloc(&sys->arena, PSX_CD_RA_SECTORS * (u32)sizeof(u32), 32u);
    cdrom_ra_reset(sys);
}

void cdrom_ra_reset(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    CdReadahead *ra = &sys->cdrom.ra;
    spsc_index_reset(&ra->head, &ra->tail);
    __atomic_store_n(&ra->want_lba, CD_RA_EPOCH_STEP | 0u, __ATOMIC_RELEASE);
    if (sys->cdrom.img) {
        sys->cdrom.img->ra_prod_lba = 0u;
    }
}

/* Consumer (Core 0): request producer to re-anchor readahead at target_lba */
void cdrom_ra_seek(PsxSystem *sys, u32 target_lba) {
    if (!sys) {
        return;
    }
    CdReadahead *ra = &sys->cdrom.ra;

    /* Check if target_lba is already at the front or within current SPSC window */
    u32 slot = 0u;
    while (spsc_begin_pop(&ra->head, &ra->tail, ra->cap, &slot)) {
        u32 cached = ra->lba[slot];
        if (cached == target_lba) {
            return;
        }
        if (cached < target_lba && (target_lba - cached) < (ra->cap >> 1)) {
            spsc_commit_pop(&ra->tail);
            continue;
        }
        /* Flush stale ring entries from consumer side */
        spsc_commit_pop(&ra->tail);
    }

    u32 prev  = __atomic_load_n(&ra->want_lba, __ATOMIC_RELAXED);
    u32 epoch = (prev + CD_RA_EPOCH_STEP) & ~CD_RA_LBA_MASK;
    if (epoch == 0u) {
        epoch = CD_RA_EPOCH_STEP;
    }
    __atomic_store_n(&ra->want_lba, epoch | (target_lba & CD_RA_LBA_MASK), __ATOMIC_RELEASE);
}

/* Producer (Core 2 worker or explicit test pump): fill up to max_sectors into SPSC ring */
u32 cdrom_ra_pump(PsxSystem *sys, u32 max_sectors) {
    if (!sys || max_sectors == 0u) {
        return 0u;
    }
    CdImage     *img = sys->cdrom.img;
    CdReadahead *ra  = &sys->cdrom.ra;
    if (!img || !img->loaded || !ra->buf || !ra->lba || ra->cap == 0u) {
        return 0u;
    }

    u32 pumped = 0u;
    while (pumped < max_sectors) {
        u32 want = __atomic_load_n(&ra->want_lba, __ATOMIC_ACQUIRE);
        if ((want & ~CD_RA_LBA_MASK) != (img->ra_prod_lba & ~CD_RA_LBA_MASK)) {
            img->ra_prod_lba = want;
        }

        u32 lba = img->ra_prod_lba & CD_RA_LBA_MASK;
        if (lba >= img->total_lba) {
            break;
        }

        u32 slot = 0u;
        if (!spsc_begin_push(&ra->head, &ra->tail, ra->cap, &slot)) {
            break;
        }

        if (!cd_read_sector_serialized(img, lba, ra->buf[slot])) {
            break;
        }
        ra->lba[slot] = lba;
        spsc_commit_push(&ra->head);

        img->ra_prod_lba = (img->ra_prod_lba & ~CD_RA_LBA_MASK) | ((lba + 1u) & CD_RA_LBA_MASK);
        pumped++;
    }
    return pumped;
}

/* Consumer (Core 0): zero-stall sector read with SPSC readahead hit/miss tracking */
bool cdrom_ra_read_sector(PsxSystem *sys, u32 target_lba, u8 out_2352[2352]) {
    if (!sys || !out_2352) {
        return false;
    }
    CdImage     *img = sys->cdrom.img;
    CdReadahead *ra  = &sys->cdrom.ra;
    if (!img || !img->loaded) {
        memset(out_2352, 0, 2352u);
        return false;
    }

    if (ra->buf && ra->lba && ra->cap > 0u) {
        u32 slot = 0u;
        while (spsc_begin_pop(&ra->head, &ra->tail, ra->cap, &slot)) {
            u32 cached = ra->lba[slot];
            if (cached == target_lba) {
                memcpy(out_2352, ra->buf[slot], 2352u);
                spsc_commit_pop(&ra->tail);
                sys->stats.cd_ra_hits++;
                return true;
            }
            /* Discard skipped or stale cached sector */
            spsc_commit_pop(&ra->tail);
        }
    }

    /* Cache miss: read target_lba directly and re-seed readahead at target_lba + 1 */
    sys->stats.cd_ra_misses++;
    bool ok = cd_read_sector_serialized(img, target_lba, out_2352);
    if (ok && target_lba + 1u < img->total_lba) {
        cdrom_ra_seek(sys, target_lba + 1u);
    }
    return ok;
}

/*
 * Consumer (Core 0 SeekL header latch): read target_lba without popping it from the SPSC
 * readahead ring (so a following ReadN at target_lba stays a cache hit) and serialize any
 * underlying FILE* seek+read against Core 2 cdrom_ra_pump().
 */
bool cdrom_ra_peek_or_read_sector(PsxSystem *sys, u32 target_lba, u8 out_2352[2352]) {
    if (!sys || !out_2352) {
        return false;
    }
    CdImage     *img = sys->cdrom.img;
    CdReadahead *ra  = &sys->cdrom.ra;
    if (!img || !img->loaded) {
        memset(out_2352, 0, 2352u);
        return false;
    }

    u32 slot = 0u;
    if (ra->buf && ra->lba && ra->cap > 0u &&
        spsc_begin_pop(&ra->head, &ra->tail, ra->cap, &slot) &&
        ra->lba[slot] == target_lba) {
        memcpy(out_2352, ra->buf[slot], 2352u);
        return true;
    }
    return cd_read_sector_serialized(img, target_lba, out_2352);
}

void cdrom_readahead_thread(void *arg) {
    PsxSystem *sys = (PsxSystem *)arg;
    if (!sys) {
        return;
    }
    while (!__atomic_load_n(&sys->quit, __ATOMIC_ACQUIRE)) {
        u32 n = cdrom_ra_pump(sys, 8u);
        if (n == 0u) {
            plat_sleep_us(2000u);
        } else {
            plat_sleep_us(500u);
        }
    }
}
