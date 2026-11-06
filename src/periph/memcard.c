#include "periph_internal.h"

#include <string.h>

#define MC_SECTORS     1024u
#define MC_SECTOR_SIZE 128u
#define MC_STAGE_SLOTS 4u

typedef struct MemcardStageSlot {
    const PsxMemcard *owner;
    u8                buf[MC_SECTOR_SIZE];
} MemcardStageSlot;

static MemcardStageSlot g_mc_stage[MC_STAGE_SLOTS];

static u8 *mc_get_stage_buf(const PsxMemcard *mc) {
    for (u32 i = 0u; i < MC_STAGE_SLOTS; i++) {
        if (g_mc_stage[i].owner == mc) {
            return g_mc_stage[i].buf;
        }
    }
    for (u32 i = 0u; i < MC_STAGE_SLOTS; i++) {
        if (g_mc_stage[i].owner == NULL) {
            g_mc_stage[i].owner = mc;
            return g_mc_stage[i].buf;
        }
    }
    g_mc_stage[0].owner = mc;
    return g_mc_stage[0].buf;
}

static u8 mc_xor_127(const u8 *frame) {
    u8 x = 0u;
    for (u32 i = 0u; i < 127u; i++) {
        x ^= frame[i];
    }
    return x;
}

void memcard_format_fresh(u8 *data_128k) {
    if (!data_128k) {
        return;
    }
    memset(data_128k, 0, PSX_MEMCARD_BYTES);

    /* Frame 0: Header frame ('M', 'C', XOR = 0x0E) */
    data_128k[0]    = (u8)'M';
    data_128k[1]    = (u8)'C';
    data_128k[0x7F] = mc_xor_127(data_128k);

    /* Frames 1..15: 15 free directory entries (0xA0 = free formatted block, link = 0xFFFF) */
    for (u32 f = 1u; f <= 15u; f++) {
        u8 *dir   = data_128k + f * MC_SECTOR_SIZE;
        dir[0]    = 0xA0u;
        dir[8]    = 0xFFu;
        dir[9]    = 0xFFu;
        dir[0x7F] = mc_xor_127(dir);
    }

    /* Frames 16..35: 20 broken-sector table frames (0xFFFFFFFF = none) */
    for (u32 f = 16u; f <= 35u; f++) {
        u8 *brk   = data_128k + f * MC_SECTOR_SIZE;
        brk[0]    = 0xFFu;
        brk[1]    = 0xFFu;
        brk[2]    = 0xFFu;
        brk[3]    = 0xFFu;
        brk[8]    = 0xFFu;
        brk[9]    = 0xFFu;
        brk[0x7F] = mc_xor_127(brk);
    }

    /* Frames 36..62 remain zeroed; Frame 63: write-test frame (copy of Frame 0) */
    u8 *wt   = data_128k + 63u * MC_SECTOR_SIZE;
    wt[0]    = (u8)'M';
    wt[1]    = (u8)'C';
    wt[0x7F] = mc_xor_127(wt);
}

void memcard_reset_transfer(PsxMemcard *mc) {
    if (!mc) {
        return;
    }
    mc->state     = 0u;
    mc->cmd       = 0u;
    mc->sector_hi = 0u;
    mc->sector_lo = 0u;
    mc->checksum  = 0u;
    mc->pos       = 0u;
}

bool memcard_load_file(PsxMemcard *mc) {
    if (!mc || !mc->data || mc->path[0] == '\0') {
        return false;
    }
    PeriphFile f = { NULL, NULL };
    if (!periph_fopen(&f, mc->path, "rb")) {
        return false;
    }
    u64 sz = periph_fsize(&f);
    /*
     * Support raw 128 KiB (.mcd/.mcr/.srm = 131,072 B) as well as DexDrive
     * .gme images (3904-byte header + 131,072 B payload = 134,976 B).
     */
    if (sz == PSX_MEMCARD_BYTES + 3904u) {
        if (!periph_fseek(&f, 3904u)) {
            periph_fclose(&f);
            return false;
        }
    } else if (sz < PSX_MEMCARD_BYTES) {
        periph_fclose(&f);
        return false;
    }

    u32 got = periph_fread(&f, mc->data, PSX_MEMCARD_BYTES);
    periph_fclose(&f);
    if (got != PSX_MEMCARD_BYTES) {
        return false;
    }
    mc->dirty = false;
    mc->flag  = 0x08u;
    return true;
}

bool memcard_flush_file(PsxMemcard *mc) {
    if (!mc || !mc->data || mc->path[0] == '\0') {
        return false;
    }
    PeriphFile f = { NULL, NULL };
    if (!periph_fopen(&f, mc->path, "wb")) {
        return false;
    }
    u32 put = periph_fwrite(&f, mc->data, PSX_MEMCARD_BYTES);
    periph_fclose(&f);
    if (put != PSX_MEMCARD_BYTES) {
        return false;
    }
    mc->dirty = false;
    return true;
}

void memcard_init_slot(PsxMemcard *mc, u8 *backing_128k, const char *path) {
    if (!mc) {
        return;
    }
    memset(mc, 0, sizeof(*mc));
    mc->data    = backing_128k;
    mc->present = (backing_128k != NULL);
    mc->flag    = 0x08u; /* bit 3 = directory unread after power-on / card insert */
    mc->dirty   = false;

    if (path && path[0] != '\0') {
        size_t len = strlen(path);
        if (len >= sizeof(mc->path)) {
            len = sizeof(mc->path) - 1u;
        }
        memcpy(mc->path, path, len);
        mc->path[len] = '\0';
    }

    (void)mc_get_stage_buf(mc);

    if (mc->data) {
        if (mc->path[0] == '\0' || !memcard_load_file(mc)) {
            memcard_format_fresh(mc->data);
            mc->dirty = false;
            mc->flag  = 0x08u;
        }
    }
}

u8 memcard_transfer_byte(PsxMemcard *mc, u8 seq, u8 tx, bool *more) {
    if (more) {
        *more = false;
    }
    if (!mc || !mc->present || !mc->data) {
        return 0xFFu;
    }

    if (seq == 0u) {
        memcard_reset_transfer(mc);
        if (tx != 0x81u) {
            return 0xFFu;
        }
        mc->state = 1u;
        if (more) {
            *more = true;
        }
        return 0xFFu;
    }

    if (mc->state == 0u) {
        return 0xFFu;
    }

    if (seq == 1u) {
        u8 flg = mc->flag;
        if (tx == 0x52u || tx == 0x57u || tx == 0x53u) {
            mc->cmd = tx;
            if (more) {
                *more = true;
            }
        } else {
            mc->state = 0u;
        }
        return flg;
    }

    switch (mc->cmd) {
    case 0x52u: /* 'R' - Read 128-byte sector (seq 0..139) */ {
        u32 sec = ((u32)mc->sector_hi << 8) | (u32)mc->sector_lo;
        switch (seq) {
        case 2u:
            if (more) {
                *more = true;
            }
            return 0x5Au;
        case 3u:
            if (more) {
                *more = true;
            }
            return 0x5Du;
        case 4u:
            mc->sector_hi = tx;
            if (more) {
                *more = true;
            }
            return 0x00u;
        case 5u:
            mc->sector_lo = tx;
            mc->checksum  = (u8)(mc->sector_hi ^ mc->sector_lo);
            if (more) {
                *more = true;
            }
            return mc->sector_hi;
        case 6u:
            if (more) {
                *more = true;
            }
            return 0x5Cu;
        case 7u:
            if (more) {
                *more = true;
            }
            return 0x5Du;
        case 8u:
            if (sec >= MC_SECTORS) {
                if (more) {
                    *more = true;
                }
                return 0xFFu;
            }
            if (more) {
                *more = true;
            }
            return mc->sector_hi;
        case 9u:
            if (sec >= MC_SECTORS) {
                mc->state = 0u;
                return 0xFFu;
            }
            if (more) {
                *more = true;
            }
            return mc->sector_lo;
        default:
            if (sec >= MC_SECTORS) {
                mc->state = 0u;
                return 0xFFu;
            }
            if (seq >= 10u && seq <= 137u) {
                u32 idx = (u32)seq - 10u;
                u8  b   = mc->data[sec * MC_SECTOR_SIZE + idx];
                mc->checksum ^= b;
                if (more) {
                    *more = true;
                }
                return b;
            }
            if (seq == 138u) {
                if (more) {
                    *more = true;
                }
                return mc->checksum;
            }
            if (seq == 139u) {
                mc->state = 0u;
                /* Final byte: *more remains false (no /ACK) */
                return (u8)'G'; /* 0x47 */
            }
            mc->state = 0u;
            return 0xFFu;
        }
    }

    case 0x57u: /* 'W' - Write 128-byte sector (seq 0..137) */ {
        u8 *stage = mc_get_stage_buf(mc);
        switch (seq) {
        case 2u:
            if (more) {
                *more = true;
            }
            return 0x5Au;
        case 3u:
            if (more) {
                *more = true;
            }
            return 0x5Du;
        case 4u:
            mc->sector_hi = tx;
            mc->checksum  = tx;
            if (more) {
                *more = true;
            }
            return 0x00u;
        case 5u:
            mc->sector_lo = tx;
            mc->checksum ^= tx;
            mc->pos       = tx; /* echo byte for seq 6 */
            if (more) {
                *more = true;
            }
            return mc->sector_hi;
        default:
            if (seq >= 6u && seq <= 133u) {
                u32 idx  = (u32)seq - 6u;
                u8  prev = (u8)mc->pos;
                stage[idx]   = tx;
                mc->checksum ^= tx;
                mc->pos      = tx;
                if (more) {
                    *more = true;
                }
                return prev;
            }
            if (seq == 134u) {
                u8  prev = (u8)mc->pos;
                u32 sec  = ((u32)mc->sector_hi << 8) | (u32)mc->sector_lo;
                u8  end_status;
                if (sec >= MC_SECTORS) {
                    end_status = 0xFFu; /* Bad sector */
                } else if (tx != mc->checksum) {
                    end_status = (u8)'N'; /* 0x4E: Bad XOR checksum, sector untouched */
                } else {
                    memcpy(&mc->data[sec * MC_SECTOR_SIZE], stage, MC_SECTOR_SIZE);
                    mc->dirty = true;
                    mc->flag &= (u8)~0x08u; /* Clear unread-directory bit after write */
                    end_status = (u8)'G';   /* 0x47: Good */
                }
                mc->pos = end_status;
                if (more) {
                    *more = true;
                }
                return prev;
            }
            if (seq == 135u) {
                if (more) {
                    *more = true;
                }
                return 0x5Cu;
            }
            if (seq == 136u) {
                if (more) {
                    *more = true;
                }
                return 0x5Du;
            }
            if (seq == 137u) {
                u8 end_status = (u8)mc->pos;
                mc->state     = 0u;
                /* Final byte: *more remains false (no /ACK) */
                return end_status;
            }
            mc->state = 0u;
            return 0xFFu;
        }
    }

    case 0x53u: /* 'S' - Get Memory Card ID (seq 0..9) */ {
        static const u8 k_id_reply[8] = {
            0x5Au, 0x5Du, 0x5Cu, 0x5Du, 0x04u, 0x00u, 0x00u, 0x80u
        };
        if (seq >= 2u && seq <= 9u) {
            u8 b = k_id_reply[seq - 2u];
            if (seq < 9u) {
                if (more) {
                    *more = true;
                }
            } else {
                mc->state = 0u;
            }
            return b;
        }
        mc->state = 0u;
        return 0xFFu;
    }

    default:
        mc->state = 0u;
        return 0xFFu;
    }
}
