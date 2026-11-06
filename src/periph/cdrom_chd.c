/*
 * cdrom_chd.c — CHD v1–v5 Compressed Disc Reader & Multi-Hunk LRU Cache
 *
 * Supports all 4 CHD v5 CD codecs (cdzl Deflate, cdlz LZMA, cdzs Zstandard,
 * cdfl FLAC CD-DA + Deflate subcode), legacy v1–v4 CD/GD codecs, and raw hunks:
 *   - Parses CHT2, CHTR, CHGD, CHCD, and CHGT track metadata with exact
 *     CD_TRACK_PADDING (4-frame) CHD hunk alignment remapping.
 *   - Handles virtual pregaps, in-CHD ('V'-prefixed) pregaps, pad frames,
 *     postgaps, raw 2352B Mode 1/Mode 2 sectors, cooked 2048B/2324B/2336B
 *     header synthesis, and big-endian -> little-endian 16-bit PCM CD-DA swap
 *     (compiled to ARMv6K REV16 2-samples-per-word SWAR loop).
 *   - 8-slot LRU decoded hunk cache (~156 KB on N3DS/Host, 4-slot ~78 KB on
 *     O3DS small_mem) + libchdr compressed SD readahead window, serialized via
 *     CdImage::io_lock / g_cd_io_lock so Core 2 async 128-sector readahead
 *     decompresses each 8-sector hunk once off Core 0.
 */

#include "periph_internal.h"

#include <libchdr/cdrom.h>
#include <libchdr/chd.h>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define CHD_CACHE_SLOTS_LARGE   8u
#define CHD_CACHE_SLOTS_SMALL   4u
#define CHD_RA_BUDGET_LARGE     (128u * 1024u)
#define CHD_RA_BUDGET_SMALL     (64u * 1024u)
#define CHD_INVALID_HUNK        0xFFFFFFFFu

static const u8 k_chd_magic[8] = { 'M', 'C', 'o', 'm', 'p', 'r', 'H', 'D' };

/* --------------------------------------------------------------------------
 * PeriphFile -> libchdr core_file_callbacks adapter (avoids redundant seeks)
 * -------------------------------------------------------------------------- */
typedef struct CdChdIoFile {
    PeriphFile pf;
    u64        size;
    u64        pos;
    bool       pos_valid;
} CdChdIoFile;

static uint64_t chd_io_fsize(void *argp) {
    CdChdIoFile *io = (CdChdIoFile *)argp;
    if (!io || !periph_is_open(&io->pf)) {
        return (uint64_t)-1;
    }
    return (uint64_t)io->size;
}

static size_t chd_io_fread(void *ptr, size_t size, size_t nmemb, void *argp) {
    CdChdIoFile *io = (CdChdIoFile *)argp;
    if (!io || !ptr || size == 0u || nmemb == 0u || !periph_is_open(&io->pf)) {
        return 0u;
    }
    if (nmemb > ((size_t)-1) / size) {
        return 0u;
    }
    size_t total = size * nmemb;
    size_t done  = 0u;
    u8    *dst   = (u8 *)ptr;
    while (done < total) {
        size_t rem   = total - done;
        u32    chunk = (rem > 0x40000000u) ? 0x40000000u : (u32)rem;
        u32    got   = periph_fread(&io->pf, dst + done, chunk);
        done += (size_t)got;
        io->pos += (u64)got;
        if (got < chunk) {
            io->pos_valid = false;
            break;
        }
    }
    return done / size;
}

static int chd_io_fseek(void *argp, int64_t offset, int whence) {
    CdChdIoFile *io = (CdChdIoFile *)argp;
    if (!io || !periph_is_open(&io->pf)) {
        return -1;
    }
    s64 base = 0;
    if (whence == SEEK_SET) {
        base = 0;
    } else if (whence == SEEK_CUR) {
        if (!io->pos_valid) {
            return -1;
        }
        base = (s64)io->pos;
    } else if (whence == SEEK_END) {
        base = (s64)io->size;
    } else {
        return -1;
    }
    if ((offset > 0 && base > INT64_MAX - offset) ||
        (offset < 0 && base < INT64_MIN - offset)) {
        return -1;
    }
    s64 target = base + offset;
    if (target < 0) {
        return -1;
    }
    if (io->pos_valid && (u64)target == io->pos) {
        return 0;
    }
    if (!periph_fseek(&io->pf, (u64)target)) {
        io->pos_valid = false;
        return -1;
    }
    io->pos       = (u64)target;
    io->pos_valid = true;
    return 0;
}

static int chd_io_fclose(void *argp) {
    CdChdIoFile *io = (CdChdIoFile *)argp;
    if (io) {
        periph_fclose(&io->pf);
        free(io);
    }
    return 0;
}

static const core_file_callbacks k_chd_io_cbs = {
    chd_io_fsize,
    chd_io_fread,
    chd_io_fclose,
    chd_io_fseek
};

/* --------------------------------------------------------------------------
 * Per-track CHD metadata & Multi-Hunk LRU state
 * -------------------------------------------------------------------------- */
typedef struct CdChdTrackMeta {
    u32  chd_frame_ofs; /* First frame of this track inside CHD hunk stream */
    u32  chd_frames;    /* Stored frames in CHD before 4-frame pad */
    u32  pad_frames;    /* Virtual zero/pad frames after active frames (GD-ROM) */
    u32  postgap;       /* Virtual postgap frames after track */
    u8   chd_type;      /* libchdr CD_TRACK_* for active track data */
    u8   pg_chd_type;   /* libchdr CD_TRACK_* for in-CHD ('V') pregap */
    bool pg_in_chd;     /* true when PREGAP > 0 and PGTYPE starts with 'V' */
    bool swap_audio;    /* true for MAME big-endian CD-DA -> PS1 little-endian */
} CdChdTrackMeta;

struct CdChdState {
    chd_file      *chd;
    u32            hunk_bytes;
    u32            frames_per_hunk;
    u32            total_hunks;
    u32            slot_count;
    u32            slot_hunk[CHD_CACHE_SLOTS_LARGE];
    u64            slot_stamp[CHD_CACHE_SLOTS_LARGE];
    u64            lru_clock;
    u64            cache_hits;
    u64            cache_misses;
    u8            *cache_buf; /* slot_count * hunk_bytes */
    CdChdTrackMeta trk[PSX_CD_MAX_TRACKS];
};

bool cd_chd_has_magic(const char *path) {
    if (!path || path[0] == '\0') {
        return false;
    }
    PeriphFile pf = { 0 };
    if (!periph_fopen(&pf, path, "rb")) {
        return false;
    }
    u8 magic[8] = { 0 };
    u32 got = periph_fread(&pf, magic, 8u);
    periph_fclose(&pf);
    return got == 8u && memcmp(magic, k_chd_magic, 8u) == 0;
}

void cd_chd_close_state(CdChdState *st) {
    if (!st) {
        return;
    }
    if (st->chd) {
        chd_close(st->chd);
        st->chd = NULL;
    }
    free(st->cache_buf);
    st->cache_buf = NULL;
    free(st);
}

void cd_chd_reset_cache_stats(CdImage *img) {
    if (!img || !img->chd) {
        return;
    }
    img->chd->cache_hits   = 0u;
    img->chd->cache_misses = 0u;
}

void cd_chd_get_cache_stats(const CdImage *img, u64 *out_hits, u64 *out_misses, u32 *out_slots) {
    u64 hits   = 0u;
    u64 misses = 0u;
    u32 slots  = 0u;
    if (img && img->loaded && img->chd) {
        hits   = img->chd->cache_hits;
        misses = img->chd->cache_misses;
        slots  = img->chd->slot_count;
    }
    if (out_hits) {
        *out_hits = hits;
    }
    if (out_misses) {
        *out_misses = misses;
    }
    if (out_slots) {
        *out_slots = slots;
    }
}

/* --------------------------------------------------------------------------
 * CHD track type mapping & safe key:value metadata parser
 * -------------------------------------------------------------------------- */
static bool chd_str_ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static bool chd_key_match(const char *tok, const char *key_colon, const char **out_val) {
    size_t klen = strlen(key_colon);
    for (size_t i = 0u; i < klen; i++) {
        if (toupper((unsigned char)tok[i]) != (unsigned char)key_colon[i]) {
            return false;
        }
    }
    *out_val = tok + klen;
    return true;
}

static bool chd_parse_u32_strict(const char *s, u32 *out) {
    if (!s || *s == '\0') {
        return false;
    }
    u64 acc = 0u;
    for (const char *p = s; *p != '\0'; p++) {
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
        acc = acc * 10u + (u64)(*p - '0');
        if (acc > 0x7FFFFFFFu) {
            return false;
        }
    }
    *out = (u32)acc;
    return true;
}

static bool chd_parse_track_type_str(const char *s, u8 *out_chd_type) {
    if (chd_str_ieq(s, "MODE1")) {
        *out_chd_type = (u8)CD_TRACK_MODE1;
        return true;
    }
    if (chd_str_ieq(s, "MODE1/2048")) {
        *out_chd_type = (u8)CD_TRACK_MODE1;
        return true;
    }
    if (chd_str_ieq(s, "MODE1_RAW") || chd_str_ieq(s, "MODE1/2352")) {
        *out_chd_type = (u8)CD_TRACK_MODE1_RAW;
        return true;
    }
    if (chd_str_ieq(s, "MODE2") || chd_str_ieq(s, "MODE2/2336")) {
        *out_chd_type = (u8)CD_TRACK_MODE2;
        return true;
    }
    if (chd_str_ieq(s, "MODE2_FORM1") || chd_str_ieq(s, "MODE2/2048")) {
        *out_chd_type = (u8)CD_TRACK_MODE2_FORM1;
        return true;
    }
    if (chd_str_ieq(s, "MODE2_FORM2") || chd_str_ieq(s, "MODE2/2324")) {
        *out_chd_type = (u8)CD_TRACK_MODE2_FORM2;
        return true;
    }
    if (chd_str_ieq(s, "MODE2_FORM_MIX")) {
        *out_chd_type = (u8)CD_TRACK_MODE2_FORM_MIX;
        return true;
    }
    if (chd_str_ieq(s, "MODE2_RAW") || chd_str_ieq(s, "MODE2/2352") || chd_str_ieq(s, "CDI/2352")) {
        *out_chd_type = (u8)CD_TRACK_MODE2_RAW;
        return true;
    }
    if (chd_str_ieq(s, "AUDIO")) {
        *out_chd_type = (u8)CD_TRACK_AUDIO;
        return true;
    }
    return false;
}

static CdTrackMode chd_to_psx_track_mode(u8 chd_type, u32 *out_sec_size) {
    switch (chd_type) {
    case CD_TRACK_MODE1:
        *out_sec_size = 2048u;
        return CD_TRACK_MODE1_2048;
    case CD_TRACK_MODE1_RAW:
        *out_sec_size = 2352u;
        return CD_TRACK_MODE1_2352;
    case CD_TRACK_MODE2_FORM1:
        *out_sec_size = 2048u;
        return CD_TRACK_MODE1_2048;
    case CD_TRACK_MODE2:
    case CD_TRACK_MODE2_FORM_MIX:
    case CD_TRACK_MODE2_FORM2:
        *out_sec_size = 2336u;
        return CD_TRACK_MODE2_2336;
    case CD_TRACK_AUDIO:
        *out_sec_size = 2352u;
        return CD_TRACK_AUDIO_2352;
    case CD_TRACK_MODE2_RAW:
    default:
        *out_sec_size = 2352u;
        return CD_TRACK_MODE2_2352;
    }
}

typedef struct ChdParsedTrack {
    u32  track_no;
    u32  frames;
    u32  pad;
    u32  pregap;
    u32  postgap;
    u32  extra_frames;
    u8   chd_type;
    u8   pg_chd_type;
    bool pg_in_chd;
    bool swap_audio;
} ChdParsedTrack;

/*
 * Tokenize CHTR / CHT2 / CHGD text metadata safely: MAME chdman emits empty
 * values like "PREGAP:0 PGTYPE: PGSUB:RW POSTGAP:0", which breaks naive
 * sscanf("%s") parsing. Splitting by whitespace handles empty KEY: tokens cleanly.
 */
static bool chd_parse_text_track_meta(const char *meta_str, ChdParsedTrack *out) {
    memset(out, 0, sizeof(*out));
    out->swap_audio = true;

    bool got_track  = false;
    bool got_type   = false;
    bool got_frames = false;
    char pgtype_buf[96] = { 0 };

    const char *p = meta_str;
    while (*p != '\0') {
        while (*p != '\0' && isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        char tok[96];
        size_t tlen = 0u;
        while (*p != '\0' && !isspace((unsigned char)*p)) {
            if (tlen + 1u < sizeof(tok)) {
                tok[tlen++] = *p;
            }
            p++;
        }
        tok[tlen] = '\0';

        const char *val = NULL;
        if (chd_key_match(tok, "TRACK:", &val)) {
            if (!chd_parse_u32_strict(val, &out->track_no) ||
                out->track_no == 0u || out->track_no > 99u) {
                return false;
            }
            got_track = true;
        } else if (chd_key_match(tok, "TYPE:", &val)) {
            if (!chd_parse_track_type_str(val, &out->chd_type)) {
                return false;
            }
            got_type = true;
        } else if (chd_key_match(tok, "FRAMES:", &val)) {
            if (!chd_parse_u32_strict(val, &out->frames)) {
                return false;
            }
            got_frames = true;
        } else if (chd_key_match(tok, "PAD:", &val)) {
            if (*val != '\0' && !chd_parse_u32_strict(val, &out->pad)) {
                return false;
            }
        } else if (chd_key_match(tok, "PREGAP:", &val)) {
            if (*val != '\0' && !chd_parse_u32_strict(val, &out->pregap)) {
                return false;
            }
        } else if (chd_key_match(tok, "PGTYPE:", &val)) {
            snprintf(pgtype_buf, sizeof(pgtype_buf), "%s", val);
        } else if (chd_key_match(tok, "POSTGAP:", &val)) {
            if (*val != '\0' && !chd_parse_u32_strict(val, &out->postgap)) {
                return false;
            }
        }
    }

    if (!got_track || !got_type || !got_frames) {
        return false;
    }

    u32 rem = out->frames % (u32)CD_TRACK_PADDING;
    out->extra_frames = (rem == 0u) ? 0u : ((u32)CD_TRACK_PADDING - rem);
    out->pg_chd_type  = out->chd_type;
    out->pg_in_chd    = false;

    if (out->pregap > 0u && (pgtype_buf[0] == 'V' || pgtype_buf[0] == 'v')) {
        out->pg_in_chd = true;
        u8 pgt = out->chd_type;
        if (chd_parse_track_type_str(pgtype_buf + 1, &pgt)) {
            out->pg_chd_type = pgt;
        }
    } else if (out->pregap > 0u && pgtype_buf[0] != '\0') {
        u8 pgt = out->chd_type;
        if (chd_parse_track_type_str(pgtype_buf, &pgt)) {
            out->pg_chd_type = pgt;
        }
    }
    return true;
}

static u32 chd_read_be32(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

static bool chd_append_track(CdImage *img, CdChdState *st, const ChdParsedTrack *pt,
                             u32 *io_log_lba, u32 *io_chd_ofs) {
    if (img->track_count >= PSX_CD_MAX_TRACKS - 1u) {
        return false;
    }
    u32 idx = img->track_count++;
    CdTrack        *t  = &img->tracks[idx];
    CdChdTrackMeta *mt = &st->trk[idx];

    u32 sec_size = 2352u;
    CdTrackMode psx_mode = chd_to_psx_track_mode(pt->chd_type, &sec_size);

    u32 log_lba = *io_log_lba;
    u32 chd_ofs = *io_chd_ofs;

    t->track_no    = (u8)pt->track_no;
    t->mode        = (u8)psx_mode;
    t->file_slot   = 0u;
    t->sector_size = sec_size;
    t->file_offset = (u64)chd_ofs * (u64)CD_FRAME_SIZE;

    mt->chd_frame_ofs = chd_ofs;
    mt->chd_frames    = pt->frames;
    mt->pad_frames    = pt->pad;
    mt->postgap       = pt->postgap;
    mt->chd_type      = pt->chd_type;
    mt->pg_chd_type   = pt->pg_chd_type;
    mt->pg_in_chd     = pt->pg_in_chd;
    mt->swap_audio    = pt->swap_audio;

    if (pt->pg_in_chd) {
        u32 pg = (pt->pregap <= pt->frames) ? pt->pregap : pt->frames;
        t->index0_lba = log_lba;
        t->start_lba  = log_lba + pg;
        t->length_lba = pt->frames - pg;
        log_lba += pt->frames + pt->pad + pt->postgap;
    } else {
        t->index0_lba = log_lba;
        log_lba += pt->pregap;
        t->start_lba  = log_lba;
        t->length_lba = pt->frames;
        log_lba += pt->frames + pt->pad + pt->postgap;
    }

    chd_ofs += pt->frames + pt->extra_frames;

    *io_log_lba = log_lba;
    *io_chd_ofs = chd_ofs;
    return true;
}

static bool chd_parse_legacy_binary_toc(CdImage *img, CdChdState *st,
                                        const u8 *buf, u32 len, u32 tag,
                                        u32 *io_log_lba, u32 *io_chd_ofs) {
    if (len < 28u) {
        return false;
    }
    u32 num_tracks = chd_read_be32(buf);
    if (num_tracks == 0u || num_tracks > CD_MAX_TRACKS) {
        return false;
    }
    u32 needed = 4u + num_tracks * 24u;
    if (len < needed) {
        return false;
    }
    /* Legacy CHGT metadata stores little-endian GD-ROM CDDA */
    bool swap_audio = (tag != GDROM_OLD_METADATA_TAG);
    if (len >= 4u + (u32)CD_MAX_TRACKS * 24u + 4u) {
        u32 flags = chd_read_be32(buf + 4u + (u32)CD_MAX_TRACKS * 24u);
        if ((flags & CD_FLAG_GDROMLE) != 0u) {
            swap_audio = false;
        }
    }

    for (u32 i = 0u; i < num_tracks; i++) {
        const u8 * entry = buf + 4u + i * 24u;
        u32 trk_type = chd_read_be32(entry + 0u);
        u32 frames   = chd_read_be32(entry + 16u);
        u32 extra    = chd_read_be32(entry + 20u);
        if (trk_type > (u32)CD_TRACK_AUDIO) {
            return false;
        }
        ChdParsedTrack pt;
        memset(&pt, 0, sizeof(pt));
        pt.track_no     = i + 1u;
        pt.frames       = frames;
        pt.extra_frames = extra;
        pt.chd_type     = (u8)trk_type;
        pt.pg_chd_type  = (u8)trk_type;
        pt.pg_in_chd    = false;
        pt.swap_audio   = swap_audio;
        if (!chd_append_track(img, st, &pt, io_log_lba, io_chd_ofs)) {
            return false;
        }
    }
    return true;
}

/* --------------------------------------------------------------------------
 * Open CHD v1–v5 disc image and populate CdImage::tracks[] + LRU cache
 * -------------------------------------------------------------------------- */
PsxResult cd_chd_open(CdImage *img, const char *path) {
    if (!img || !path || path[0] == '\0') {
        return PSX_ERR_BAD_ARG;
    }

    CdChdIoFile *io = (CdChdIoFile *)calloc(1u, sizeof(*io));
    if (!io) {
        return PSX_ERR_NOMEM;
    }
    if (!periph_fopen(&io->pf, path, "rb")) {
        free(io);
        return PSX_ERR_IO;
    }
    io->size = periph_fsize(&io->pf);
    if (io->size < (u64)CHD_V1_HEADER_SIZE) {
        periph_fclose(&io->pf);
        free(io);
        return PSX_ERR_FORMAT;
    }
    (void)periph_fseek(&io->pf, 0u);
    io->pos       = 0u;
    io->pos_valid = true;

    /* Ownership of `io` passes unconditionally to libchdr here */
    chd_file *chd = NULL;
    chd_error cerr = chd_open_core_file_callbacks(&k_chd_io_cbs, io, CHD_OPEN_READ, NULL, &chd);
    if (cerr != CHDERR_NONE || !chd) {
        if (cerr == CHDERR_OUT_OF_MEMORY) {
            return PSX_ERR_NOMEM;
        }
        if (cerr == CHDERR_FILE_NOT_FOUND || cerr == CHDERR_READ_ERROR) {
            return PSX_ERR_IO;
        }
        return PSX_ERR_FORMAT;
    }

    const chd_header *hdr = chd_get_header(chd);
    if (!hdr || hdr->hunkbytes == 0u || (hdr->hunkbytes % (u32)CD_FRAME_SIZE) != 0u) {
        chd_close(chd);
        return PSX_ERR_FORMAT;
    }
    if (hdr->version >= 5u && hdr->unitbytes != 0u && hdr->unitbytes != (u32)CD_FRAME_SIZE) {
        chd_close(chd);
        return PSX_ERR_FORMAT;
    }

    u32 total_hunks = (hdr->totalhunks != 0u) ? hdr->totalhunks : hdr->hunkcount;
    if (total_hunks == 0u && hdr->logicalbytes > 0u) {
        total_hunks = (u32)((hdr->logicalbytes + (u64)hdr->hunkbytes - 1u) / (u64)hdr->hunkbytes);
    }
    if (total_hunks == 0u) {
        chd_close(chd);
        return PSX_ERR_FORMAT;
    }

    CdChdState *st = (CdChdState *)calloc(1u, sizeof(*st));
    if (!st) {
        chd_close(chd);
        return PSX_ERR_NOMEM;
    }
    st->chd             = chd;
    st->hunk_bytes      = hdr->hunkbytes;
    st->frames_per_hunk = hdr->hunkbytes / (u32)CD_FRAME_SIZE;
    st->total_hunks     = total_hunks;
    st->slot_count      = img->small_mem ? CHD_CACHE_SLOTS_SMALL : CHD_CACHE_SLOTS_LARGE;

    size_t cache_bytes = (size_t)st->slot_count * (size_t)st->hunk_bytes;
    st->cache_buf = (u8 *)malloc(cache_bytes);
    if (!st->cache_buf) {
        cd_chd_close_state(st);
        return PSX_ERR_NOMEM;
    }
    for (u32 i = 0u; i < CHD_CACHE_SLOTS_LARGE; i++) {
        st->slot_hunk[i]  = CHD_INVALID_HUNK;
        st->slot_stamp[i] = 0u;
    }

    /* Enable compressed SD stream readahead inside libchdr (non-fatal if OOM) */
    size_t ra_budget = img->small_mem ? CHD_RA_BUDGET_SMALL : CHD_RA_BUDGET_LARGE;
    (void)chd_set_cache_budget(chd, ra_budget);

    /* Walk CHD metadata stream for CHT2 / CHTR / CHGD / CHCD / CHGT tags */
    u8  meta_buf[2560];
    u32 log_lba = 0u;
    u32 chd_ofs = 0u;

    for (u32 meta_idx = 0u; meta_idx < 256u; meta_idx++) {
        u32 res_len   = 0u;
        u32 res_tag   = 0u;
        u8  res_flags = 0u;
        cerr = chd_get_metadata(chd, CHDMETATAG_WILDCARD, meta_idx,
                                meta_buf, (u32)(sizeof(meta_buf) - 1u),
                                &res_len, &res_tag, &res_flags);
        if (cerr == CHDERR_METADATA_NOT_FOUND) {
            break;
        }
        if (cerr != CHDERR_NONE) {
            cd_chd_close_state(st);
            cd_image_close(img);
            return PSX_ERR_FORMAT;
        }

        if (res_tag == HARD_DISK_METADATA_TAG ||
            res_tag == AV_METADATA_TAG ||
            res_tag == DVD_METADATA_TAG) {
            cd_chd_close_state(st);
            cd_image_close(img);
            return PSX_ERR_FORMAT;
        }

        if (res_tag == CDROM_TRACK_METADATA2_TAG ||
            res_tag == CDROM_TRACK_METADATA_TAG ||
            res_tag == GDROM_TRACK_METADATA_TAG) {
            u32 copy_len = (res_len < sizeof(meta_buf) - 1u) ? res_len : (u32)(sizeof(meta_buf) - 1u);
            meta_buf[copy_len] = '\0';
            ChdParsedTrack pt;
            if (!chd_parse_text_track_meta((const char *)meta_buf, &pt) ||
                !chd_append_track(img, st, &pt, &log_lba, &chd_ofs)) {
                cd_chd_close_state(st);
                cd_image_close(img);
                return PSX_ERR_FORMAT;
            }
        } else if ((res_tag == CDROM_OLD_METADATA_TAG || res_tag == GDROM_OLD_METADATA_TAG) &&
                   img->track_count == 0u) {
            u32 copy_len = (res_len < sizeof(meta_buf)) ? res_len : (u32)sizeof(meta_buf);
            if (!chd_parse_legacy_binary_toc(img, st, meta_buf, copy_len, res_tag,
                                             &log_lba, &chd_ofs)) {
                cd_chd_close_state(st);
                cd_image_close(img);
                return PSX_ERR_FORMAT;
            }
        }
    }

    if (img->track_count == 0u || log_lba == 0u) {
        cd_chd_close_state(st);
        cd_image_close(img);
        return PSX_ERR_FORMAT;
    }

    /* Validate CHD has enough hunk frames for the declared tracks */
    u64 avail_frames = (u64)st->total_hunks * (u64)st->frames_per_hunk;
    if ((u64)chd_ofs > avail_frames) {
        cd_chd_close_state(st);
        cd_image_close(img);
        return PSX_ERR_FORMAT;
    }

    snprintf(img->file_paths[0], sizeof(img->file_paths[0]), "%s", path);
    img->first_track = img->tracks[0].track_no;
    img->last_track  = img->tracks[img->track_count - 1u].track_no;
    img->total_lba   = log_lba;
    img->file_count  = 0u; /* File handle owned inside st->chd */
    img->chd         = st;
    img->loaded      = true;
    return PSX_OK;
}

/* --------------------------------------------------------------------------
 * Decoded hunk LRU cache lookup & sector reconstruction / CD-DA byte swap
 * -------------------------------------------------------------------------- */
PSX_HOT static const u8 *chd_get_hunk_cached(CdChdState *st, u32 hunk_num) {
    if (PSX_UNLIKELY(hunk_num >= st->total_hunks)) {
        return NULL;
    }

    u32 slots = st->slot_count;
    for (u32 i = 0u; i < slots; i++) {
        if (st->slot_hunk[i] == hunk_num) {
            st->cache_hits++;
            st->slot_stamp[i] = ++st->lru_clock;
            return st->cache_buf + (size_t)i * (size_t)st->hunk_bytes;
        }
    }

    /* Miss: pick unused slot first, else evict minimum-stamp LRU slot */
    st->cache_misses++;
    u32 victim     = 0u;
    u64 best_stamp = st->slot_stamp[0];
    for (u32 i = 0u; i < slots; i++) {
        if (st->slot_hunk[i] == CHD_INVALID_HUNK) {
            victim = i;
            break;
        }
        if (st->slot_stamp[i] < best_stamp) {
            best_stamp = st->slot_stamp[i];
            victim     = i;
        }
    }

    u8 *slot_ptr = st->cache_buf + (size_t)victim * (size_t)st->hunk_bytes;
    chd_error err = chd_read(st->chd, hunk_num, slot_ptr);
    if (PSX_UNLIKELY(err != CHDERR_NONE)) {
        st->slot_hunk[victim]  = CHD_INVALID_HUNK;
        st->slot_stamp[victim] = 0u;
        return NULL;
    }

    st->slot_hunk[victim]  = hunk_num;
    st->slot_stamp[victim] = ++st->lru_clock;
    return slot_ptr;
}

/*
 * MAME/chdman stores CD-DA 16-bit PCM samples big-endian inside CHD hunks.
 * Swap 2 samples per 32-bit word (compiles to ARMv6K REV16 on 3DS / T2).
 */
PSX_HOT static void chd_copy_swap_cdda_2352(u8 dst[2352], const u8 src[2352]) {
    for (u32 off = 0u; off < 2352u; off += 4u) {
        u32 w;
        memcpy(&w, src + off, 4u);
#if PSX3DS_ARCH_ARMV6K && defined(__GNUC__)
        __asm__("rev16 %0, %1" : "=r"(w) : "r"(w));
#else
        w = ((w >> 8) & 0x00FF00FFu) | ((w & 0x00FF00FFu) << 8);
#endif
        memcpy(dst + off, &w, 4u);
    }
}

PSX_HOT static void chd_extract_sector_2352(u32 disc_lba, u8 chd_type, bool swap_audio,
                                            const u8 *frame_2448, u8 out_2352[2352]) {
    switch (chd_type) {
    case CD_TRACK_MODE1_RAW:
    case CD_TRACK_MODE2_RAW:
        memcpy(out_2352, frame_2448, 2352u);
        return;

    case CD_TRACK_AUDIO:
        if (PSX_LIKELY(swap_audio)) {
            chd_copy_swap_cdda_2352(out_2352, frame_2448);
        } else {
            memcpy(out_2352, frame_2448, 2352u);
        }
        return;

    case CD_TRACK_MODE1:
        /* Cooked 2048B Mode 1: synthesize 12B sync + 4B Mode 1 header (+16 payload) */
        cd_synth_sync_msf(disc_lba, 0x01u, out_2352);
        memcpy(&out_2352[16], frame_2448, 2048u);
        memset(&out_2352[2064], 0, 2352u - 2064u);
        return;

    case CD_TRACK_MODE2_FORM1:
        /* Cooked 2048B Mode 2 Form 1: sync + Mode 2 header + Form 1 subheader (+24 payload) */
        cd_synth_sync_msf(disc_lba, 0x02u, out_2352);
        memset(&out_2352[16], 0, 8u);
        out_2352[18] = 0x08u;
        out_2352[22] = 0x08u;
        memcpy(&out_2352[24], frame_2448, 2048u);
        memset(&out_2352[2072], 0, 2352u - 2072u);
        return;

    case CD_TRACK_MODE2:
    case CD_TRACK_MODE2_FORM_MIX:
        /* Cooked 2336B Mode 2 (subheader + payload + EDC/ECC): synthesize 16B sync+MSF+Mode2 */
        cd_synth_sync_msf(disc_lba, 0x02u, out_2352);
        memcpy(&out_2352[16], frame_2448, 2336u);
        return;

    case CD_TRACK_MODE2_FORM2:
        /* Cooked 2324B Mode 2 Form 2: sync + Mode 2 header + Form 2 subheader (+24 payload) */
        cd_synth_sync_msf(disc_lba, 0x02u, out_2352);
        memset(&out_2352[16], 0, 8u);
        out_2352[18] = 0x20u;
        out_2352[22] = 0x20u;
        memcpy(&out_2352[24], frame_2448, 2324u);
        memset(&out_2352[2348], 0, 4u);
        return;

    default:
        memcpy(out_2352, frame_2448, 2352u);
        return;
    }
}

static void chd_synth_zero_sector(u32 disc_lba, u8 chd_type, u8 out_2352[2352]) {
    memset(out_2352, 0, 2352u);
    if (chd_type != (u8)CD_TRACK_AUDIO) {
        u8 mode_byte = (chd_type == (u8)CD_TRACK_MODE1 || chd_type == (u8)CD_TRACK_MODE1_RAW)
                           ? 0x01u
                           : 0x02u;
        cd_synth_sync_msf(disc_lba, mode_byte, out_2352);
    }
}

PSX_HOT bool cd_chd_read_sector_locked(CdImage *img, u32 lba, u8 out_2352[2352]) {
    CdChdState *st = img->chd;
    if (PSX_UNLIKELY(!st || !st->chd || lba >= img->total_lba)) {
        memset(out_2352, 0, 2352u);
        return false;
    }

    const CdTrack *t = cd_image_find_track(img, lba);
    if (PSX_UNLIKELY(!t)) {
        memset(out_2352, 0, 2352u);
        return false;
    }
    u32 trk_idx = (u32)(t - img->tracks);
    if (PSX_UNLIKELY(trk_idx >= img->track_count)) {
        memset(out_2352, 0, 2352u);
        return false;
    }
    const CdChdTrackMeta *mt = &st->trk[trk_idx];

    u32 chd_frame = 0u;
    u8  eff_type  = mt->chd_type;

    if (lba < t->start_lba) {
        /* Pregap region [index0_lba, start_lba) */
        if (!mt->pg_in_chd || lba < t->index0_lba) {
            chd_synth_zero_sector(lba, mt->pg_chd_type, out_2352);
            return true;
        }
        u32 pg_rel = lba - t->index0_lba;
        if (pg_rel >= mt->chd_frames) {
            chd_synth_zero_sector(lba, mt->pg_chd_type, out_2352);
            return true;
        }
        chd_frame = mt->chd_frame_ofs + pg_rel;
        eff_type  = mt->pg_chd_type;
    } else {
        u32 rel = lba - t->start_lba;
        if (rel >= t->length_lba) {
            /* Virtual GD-ROM pad or Red Book postgap region */
            u32 tail_limit = t->length_lba + mt->pad_frames + mt->postgap;
            if (rel < tail_limit) {
                chd_synth_zero_sector(lba, mt->chd_type, out_2352);
                return true;
            }
            memset(out_2352, 0, 2352u);
            return false;
        }
        u32 in_trk_ofs = mt->pg_in_chd ? (t->start_lba - t->index0_lba) : 0u;
        chd_frame = mt->chd_frame_ofs + in_trk_ofs + rel;
        eff_type  = mt->chd_type;
    }

    u32 hunk_num      = chd_frame / st->frames_per_hunk;
    u32 frame_in_hunk = chd_frame % st->frames_per_hunk;
    const u8 *hunk    = chd_get_hunk_cached(st, hunk_num);
    if (PSX_UNLIKELY(!hunk)) {
        memset(out_2352, 0, 2352u);
        return false;
    }

    const u8 *frame_ptr = hunk + (size_t)frame_in_hunk * (size_t)CD_FRAME_SIZE;
    chd_extract_sector_2352(lba, eff_type, mt->swap_audio, frame_ptr, out_2352);
    return true;
}
