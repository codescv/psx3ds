#include "periph_internal.h"

#include <ctype.h>
#include <string.h>

u8 cd_bin_to_bcd(u8 v) {
    if (v > 99u) {
        v %= 100u;
    }
    return (u8)(((v / 10u) << 4) | (v % 10u));
}

u8 cd_bcd_to_bin(u8 bcd, bool *ok) {
    u8 hi = (bcd >> 4) & 0x0Fu;
    u8 lo = bcd & 0x0Fu;
    if (hi > 9u || lo > 9u) {
        if (ok) {
            *ok = false;
        }
        return 0u;
    }
    if (ok) {
        *ok = true;
    }
    return (u8)(hi * 10u + lo);
}

void cd_lba_to_msf(u32 lba, u8 *mm, u8 *ss, u8 *ff) {
    u32 total = lba + 150u;
    if (mm) {
        *mm = (u8)(total / (75u * 60u));
    }
    if (ss) {
        *ss = (u8)((total / 75u) % 60u);
    }
    if (ff) {
        *ff = (u8)(total % 75u);
    }
}

u32 cd_msf_to_lba(u8 mm, u8 ss, u8 ff) {
    u32 total = (u32)mm * 60u * 75u + (u32)ss * 75u + (u32)ff;
    return (total >= 150u) ? (total - 150u) : 0u;
}

static void cd_io_lock(CdImage *img) {
    while (__atomic_test_and_set(&img->io_lock, __ATOMIC_ACQUIRE)) {
        plat_yield();
    }
}

static void cd_io_unlock(CdImage *img) {
    __atomic_clear(&img->io_lock, __ATOMIC_RELEASE);
}

void cd_image_init(CdImage *img) {
    if (!img) {
        return;
    }
    memset(img, 0, sizeof(*img));
    img->region_char = 'A';
}

void cd_image_close(CdImage *img) {
    if (!img) {
        return;
    }
    cd_io_lock(img);
    CdChdState *old_chd = img->chd;
    img->chd    = NULL;
    img->loaded = false;
    for (u32 i = 0u; i < img->file_count && i < PSX_CD_MAX_FILES; i++) {
        periph_fclose(&img->files[i]);
    }
    bool small_mem    = img->small_mem;
    bool motor_on     = img->motor_on;
    bool header_valid = img->header_valid;
    bool bfrd         = img->bfrd;
    cd_io_unlock(img);

    if (old_chd) {
        cd_chd_close_state(old_chd);
    }

    memset(img, 0, sizeof(*img));
    img->region_char  = 'A';
    img->small_mem    = small_mem;
    img->motor_on     = motor_on;
    img->header_valid = header_valid;
    img->bfrd         = bfrd;
}

static bool cd_str_ieq_n(const char *a, const char *b, size_t n) {
    for (size_t i = 0u; i < n; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
            return false;
        }
        if (a[i] == '\0') {
            break;
        }
    }
    return true;
}

static bool cd_has_ext(const char *path, const char *ext) {
    size_t plen = strlen(path);
    size_t elen = strlen(ext);
    if (plen < elen) {
        return false;
    }
    return cd_str_ieq_n(path + (plen - elen), ext, elen);
}

static bool cd_parse_msf_str(const char *s, u32 *out_frames) {
    unsigned mm = 0u, ss = 0u, ff = 0u;
    if (sscanf(s, "%u:%u:%u", &mm, &ss, &ff) != 3) {
        return false;
    }
    if (ss >= 60u || ff >= 75u) {
        return false;
    }
    *out_frames = (u32)(mm * 60u * 75u + ss * 75u + ff);
    return true;
}

static void cd_resolve_rel_path(const char *cue_path, const char *rel, char out[256]) {
    if (rel[0] == '/' || (rel[0] != '\0' && rel[1] == ':')) {
        snprintf(out, 256u, "%s", rel);
        return;
    }
    const char *slash1 = strrchr(cue_path, '/');
    const char *slash2 = strrchr(cue_path, '\\');
    const char *slash  = (slash2 && (!slash1 || slash2 > slash1)) ? slash2 : slash1;
    if (!slash) {
        snprintf(out, 256u, "%s", rel);
        return;
    }
    size_t dir_len = (size_t)(slash - cue_path + 1);
    if (dir_len >= 255u) {
        dir_len = 255u;
    }
    memcpy(out, cue_path, dir_len);
    out[dir_len] = '\0';
    snprintf(out + dir_len, 256u - dir_len, "%s", rel);
}

static u32 cd_sector_size_for_mode(CdTrackMode m) {
    switch (m) {
    case CD_TRACK_MODE1_2048:
        return 2048u;
    case CD_TRACK_MODE2_2336:
        return 2336u;
    case CD_TRACK_MODE2_2352:
    case CD_TRACK_MODE1_2352:
    case CD_TRACK_AUDIO_2352:
    default:
        return 2352u;
    }
}

static void cd_detect_region(CdImage *img) {
    img->region_char = 'A';
    if (!img->loaded || img->total_lba < 5u) {
        return;
    }
    u8 sec[2352];
    for (u32 lba = 4u; lba < 12u && lba < img->total_lba; lba++) {
        if (!cd_image_read_sector(img, lba, sec)) {
            continue;
        }
        /* Search payload for Sony Computer Entertainment license text or SCEx */
        for (u32 i = 16u; i + 36u <= 2352u; i++) {
            if (memcmp(&sec[i], "Sony Computer Entertainment", 27u) == 0) {
                const char *tail = (const char *)&sec[i + 27u];
                for (u32 j = 0u; j < 20u && i + 27u + j + 4u <= 2352u; j++) {
                    if (cd_str_ieq_n(&tail[j], "Euro", 4u)) {
                        img->region_char = 'E';
                        return;
                    }
                    if (cd_str_ieq_n(&tail[j], "Inc", 3u)) {
                        img->region_char = 'I';
                        return;
                    }
                    if (cd_str_ieq_n(&tail[j], "Amer", 4u)) {
                        img->region_char = 'A';
                        return;
                    }
                }
            }
            if (sec[i] == 'S' && sec[i + 1u] == 'C' && sec[i + 2u] == 'E') {
                u8 c = sec[i + 3u];
                if (c == 'A' || c == 'E' || c == 'I') {
                    img->region_char = c;
                    return;
                }
            }
        }
    }
}

static PsxResult cd_open_cue(CdImage *img, const char *cue_path) {
    PeriphFile cf = { 0 };
    if (!periph_fopen(&cf, cue_path, "rb")) {
        return PSX_ERR_IO;
    }
    u64 csize = periph_fsize(&cf);
    if (csize == 0u || csize > 65535u) {
        periph_fclose(&cf);
        return PSX_ERR_FORMAT;
    }

    char cue_buf[16384];
    u32 to_read = (csize < sizeof(cue_buf) - 1u) ? (u32)csize : (u32)(sizeof(cue_buf) - 1u);
    u32 got = periph_fread(&cf, cue_buf, to_read);
    periph_fclose(&cf);
    cue_buf[got] = '\0';

    s32 cur_file = -1;
    s32 cur_trk  = -1;
    u32 pending_pregap = 0u;
    u32 track_file_frames[PSX_CD_MAX_TRACKS];
    u32 track_pregap[PSX_CD_MAX_TRACKS];
    bool track_has_idx0[PSX_CD_MAX_TRACKS];
    u32 track_idx0_frames[PSX_CD_MAX_TRACKS];
    memset(track_file_frames, 0, sizeof(track_file_frames));
    memset(track_pregap, 0, sizeof(track_pregap));
    memset(track_has_idx0, 0, sizeof(track_has_idx0));
    memset(track_idx0_frames, 0, sizeof(track_idx0_frames));

    char *cursor = cue_buf;
    while (*cursor != '\0') {
        char *line = cursor;
        char *nl   = strchr(cursor, '\n');
        if (nl) {
            *nl    = '\0';
            cursor = nl + 1;
        } else {
            cursor += strlen(cursor);
        }

        while (*line != '\0' && isspace((unsigned char)*line)) {
            line++;
        }
        size_t llen = strlen(line);
        while (llen > 0u && isspace((unsigned char)line[llen - 1u])) {
            line[--llen] = '\0';
        }
        if (llen == 0u) {
            continue;
        }

        if (cd_str_ieq_n(line, "FILE", 4u) && isspace((unsigned char)line[4])) {
            const char *p = line + 4;
            while (*p && isspace((unsigned char)*p)) {
                p++;
            }
            char fname[256] = { 0 };
            if (*p == '"') {
                p++;
                const char *q = strchr(p, '"');
                if (!q) {
                    cd_image_close(img);
                    return PSX_ERR_FORMAT;
                }
                size_t flen = (size_t)(q - p);
                if (flen >= sizeof(fname)) {
                    flen = sizeof(fname) - 1u;
                }
                memcpy(fname, p, flen);
                fname[flen] = '\0';
            } else {
                const char *q = p;
                while (*q && !isspace((unsigned char)*q)) {
                    q++;
                }
                size_t flen = (size_t)(q - p);
                if (flen >= sizeof(fname)) {
                    flen = sizeof(fname) - 1u;
                }
                memcpy(fname, p, flen);
                fname[flen] = '\0';
            }

            if (img->file_count >= PSX_CD_MAX_FILES) {
                cd_image_close(img);
                return PSX_ERR_FORMAT;
            }
            u32 slot = img->file_count;
            cd_resolve_rel_path(cue_path, fname, img->file_paths[slot]);
            if (!periph_fopen(&img->files[slot], img->file_paths[slot], "rb")) {
                cd_image_close(img);
                return PSX_ERR_IO;
            }
            cur_file = (s32)slot;
            img->file_count++;
        } else if (cd_str_ieq_n(line, "TRACK", 5u) && isspace((unsigned char)line[5])) {
            if (cur_file < 0 || img->track_count >= PSX_CD_MAX_TRACKS - 1u) {
                cd_image_close(img);
                return PSX_ERR_FORMAT;
            }
            unsigned tno = 0u;
            char mode_str[64] = { 0 };
            if (sscanf(line + 5, "%u %63s", &tno, mode_str) != 2 || tno == 0u || tno > 99u) {
                cd_image_close(img);
                return PSX_ERR_FORMAT;
            }
            CdTrackMode tm = CD_TRACK_MODE2_2352;
            if (cd_str_ieq_n(mode_str, "MODE2/2352", 10u)) {
                tm = CD_TRACK_MODE2_2352;
            } else if (cd_str_ieq_n(mode_str, "MODE1/2352", 10u)) {
                tm = CD_TRACK_MODE1_2352;
            } else if (cd_str_ieq_n(mode_str, "MODE1/2048", 10u) ||
                       cd_str_ieq_n(mode_str, "MODE2/2048", 10u)) {
                tm = CD_TRACK_MODE1_2048;
            } else if (cd_str_ieq_n(mode_str, "MODE2/2336", 10u)) {
                tm = CD_TRACK_MODE2_2336;
            } else if (cd_str_ieq_n(mode_str, "AUDIO", 5u)) {
                tm = CD_TRACK_AUDIO_2352;
            }

            cur_trk = (s32)img->track_count++;
            CdTrack *t     = &img->tracks[cur_trk];
            t->track_no    = (u8)tno;
            t->mode        = (u8)tm;
            t->file_slot   = (u8)cur_file;
            t->sector_size = cd_sector_size_for_mode(tm);
            pending_pregap = 0u;
        } else if (cd_str_ieq_n(line, "PREGAP", 6u) && isspace((unsigned char)line[6])) {
            if (cur_trk >= 0) {
                const char *p = line + 6;
                while (*p && isspace((unsigned char)*p)) {
                    p++;
                }
                u32 pg = 0u;
                if (cd_parse_msf_str(p, &pg)) {
                    pending_pregap = pg;
                    track_pregap[cur_trk] = pg;
                }
            }
        } else if (cd_str_ieq_n(line, "INDEX", 5u) && isspace((unsigned char)line[5])) {
            if (cur_trk >= 0) {
                unsigned idx_no = 0u;
                char msf_str[32] = { 0 };
                if (sscanf(line + 5, "%u %31s", &idx_no, msf_str) == 2) {
                    u32 frames = 0u;
                    if (cd_parse_msf_str(msf_str, &frames)) {
                        if (idx_no == 0u) {
                            track_has_idx0[cur_trk]    = true;
                            track_idx0_frames[cur_trk] = frames;
                        } else if (idx_no == 1u) {
                            track_file_frames[cur_trk] = frames;
                        }
                    }
                }
            }
        }
    }
    (void)pending_pregap;

    if (img->track_count == 0u) {
        cd_image_close(img);
        return PSX_ERR_FORMAT;
    }

    /* Compute per-track file offsets, lengths, and monotonically increasing disc LBAs */
    u32 running_lba   = 0u;
    u32 accum_pregap  = 0u;
    for (u32 i = 0u; i < img->track_count; i++) {
        CdTrack *t = &img->tracks[i];
        u64 fsz    = periph_fsize(&img->files[t->file_slot]);
        u64 total_file_sectors = fsz / (u64)t->sector_size;

        u32 idx1_in_file = track_file_frames[i];
        t->file_offset   = (u64)idx1_in_file * (u64)t->sector_size;

        u32 end_in_file;
        if (i + 1u < img->track_count && img->tracks[i + 1u].file_slot == t->file_slot) {
            /* Next track shares same file: ends at next track's INDEX 01 (or INDEX 00? Data stays contiguous to INDEX 01) */
            end_in_file = track_file_frames[i + 1u];
        } else {
            end_in_file = (u32)total_file_sectors;
        }
        t->length_lba = (end_in_file > idx1_in_file) ? (end_in_file - idx1_in_file) : 0u;

        if (i == 0u) {
            t->start_lba  = idx1_in_file;
            t->index0_lba = 0u;
            running_lba   = t->start_lba + t->length_lba;
        } else if (img->tracks[i - 1u].file_slot == t->file_slot) {
            accum_pregap += track_pregap[i];
            t->start_lba  = idx1_in_file + accum_pregap;
            t->index0_lba = track_has_idx0[i] ? (track_idx0_frames[i] + accum_pregap) : t->start_lba;
            running_lba   = t->start_lba + t->length_lba;
        } else {
            /* Separate file per track */
            u32 in_file_pregap = idx1_in_file;
            u32 virt_pregap    = track_pregap[i];
            t->index0_lba      = running_lba;
            t->start_lba       = running_lba + virt_pregap + in_file_pregap;
            running_lba        = t->start_lba + t->length_lba;
        }
    }

    img->first_track = img->tracks[0].track_no;
    img->last_track  = img->tracks[img->track_count - 1u].track_no;
    img->total_lba   = running_lba;
    img->loaded      = (img->total_lba > 0u);
    if (!img->loaded) {
        cd_image_close(img);
        return PSX_ERR_FORMAT;
    }

    cd_detect_region(img);
    return PSX_OK;
}

static PsxResult cd_open_raw(CdImage *img, const char *path) {
    snprintf(img->file_paths[0], sizeof(img->file_paths[0]), "%s", path);
    if (!periph_fopen(&img->files[0], path, "rb")) {
        return PSX_ERR_IO;
    }
    img->file_count = 1u;

    u64 fsz = periph_fsize(&img->files[0]);
    if (fsz < 2048u) {
        cd_image_close(img);
        return PSX_ERR_FORMAT;
    }

    u8 hdr[16] = { 0 };
    (void)periph_fseek(&img->files[0], 0u);
    u32 got = periph_fread(&img->files[0], hdr, 16u);

    static const u8 k_sync[12] = {
        0x00u, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
        0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0x00u
    };

    CdTrackMode mode = CD_TRACK_MODE2_2352;
    u32 sector_size  = 2352u;

    if (got == 16u && memcmp(hdr, k_sync, 12u) == 0 && fsz >= 2352u) {
        mode        = (hdr[15] == 1u) ? CD_TRACK_MODE1_2352 : CD_TRACK_MODE2_2352;
        sector_size = 2352u;
    } else {
        /* Check ISO9660 PVD magic "CD001" at 16 * 2048 + 1 or exact 2048-byte multiple */
        bool is_iso = false;
        if (fsz >= 17u * 2048u) {
            u8 pvd[6] = { 0 };
            if (periph_fseek(&img->files[0], 16u * 2048u) &&
                periph_fread(&img->files[0], pvd, 6u) == 6u &&
                memcmp(&pvd[1], "CD001", 5u) == 0) {
                is_iso = true;
            }
        }
        if (is_iso || (cd_has_ext(path, ".iso") && (fsz % 2048u) == 0u) ||
            ((fsz % 2048u) == 0u && (fsz % 2352u) != 0u)) {
            mode        = CD_TRACK_MODE1_2048;
            sector_size = 2048u;
        } else if ((fsz % 2352u) == 0u) {
            mode        = CD_TRACK_MODE2_2352;
            sector_size = 2352u;
        } else if ((fsz % 2336u) == 0u) {
            mode        = CD_TRACK_MODE2_2336;
            sector_size = 2336u;
        } else if ((fsz % 2048u) == 0u) {
            mode        = CD_TRACK_MODE1_2048;
            sector_size = 2048u;
        } else {
            cd_image_close(img);
            return PSX_ERR_FORMAT;
        }
    }

    CdTrack *t     = &img->tracks[0];
    t->track_no    = 1u;
    t->mode        = (u8)mode;
    t->file_slot   = 0u;
    t->start_lba   = 0u;
    t->index0_lba  = 0u;
    t->sector_size = sector_size;
    t->file_offset = 0u;
    t->length_lba  = (u32)(fsz / (u64)sector_size);

    img->track_count = 1u;
    img->first_track = 1u;
    img->last_track  = 1u;
    img->total_lba   = t->length_lba;
    img->loaded      = (img->total_lba > 0u);

    cd_detect_region(img);
    return PSX_OK;
}

PsxResult cd_image_open(CdImage *img, const char *path) {
    if (!img || !path || path[0] == '\0') {
        return PSX_ERR_BAD_ARG;
    }
    cd_image_close(img);
    if (cd_has_ext(path, ".chd") || (!cd_has_ext(path, ".cue") && cd_chd_has_magic(path))) {
        PsxResult r = cd_chd_open(img, path);
        if (r == PSX_OK) {
            cd_detect_region(img);
            cd_chd_reset_cache_stats(img);
        }
        return r;
    }
    if (cd_has_ext(path, ".cue")) {
        return cd_open_cue(img, path);
    }
    return cd_open_raw(img, path);
}

const CdTrack *cd_image_find_track(const CdImage *img, u32 lba) {
    if (!img || !img->loaded || img->track_count == 0u) {
        return NULL;
    }
    for (u32 i = img->track_count; i > 0u; i--) {
        const CdTrack *t = &img->tracks[i - 1u];
        u32 first = (t->index0_lba < t->start_lba) ? t->index0_lba : t->start_lba;
        if (lba >= first) {
            return t;
        }
    }
    return &img->tracks[0];
}

void cd_synth_sync_msf(u32 lba, u8 mode_byte, u8 out_2352[2352]) {
    out_2352[0] = 0x00u;
    memset(&out_2352[1], 0xFF, 10u);
    out_2352[11] = 0x00u;
    u8 mm = 0u, ss = 0u, ff = 0u;
    cd_lba_to_msf(lba, &mm, &ss, &ff);
    out_2352[12] = cd_bin_to_bcd(mm);
    out_2352[13] = cd_bin_to_bcd(ss);
    out_2352[14] = cd_bin_to_bcd(ff);
    out_2352[15] = mode_byte;
}

bool cd_image_read_sector(CdImage *img, u32 lba, u8 out_2352[2352]) {
    if (!img || !out_2352) {
        if (out_2352) {
            memset(out_2352, 0, 2352u);
        }
        return false;
    }

    cd_io_lock(img);
    if (!img->loaded || lba >= img->total_lba) {
        cd_io_unlock(img);
        memset(out_2352, 0, 2352u);
        return false;
    }
    if (img->chd) {
        bool ok = cd_chd_read_sector_locked(img, lba, out_2352);
        cd_io_unlock(img);
        return ok;
    }
    cd_io_unlock(img);

    const CdTrack *t = cd_image_find_track(img, lba);
    if (!t || t->file_slot >= img->file_count) {
        memset(out_2352, 0, 2352u);
        return false;
    }

    /* Pregap before track INDEX 01 */
    if (lba < t->start_lba) {
        memset(out_2352, 0, 2352u);
        if (t->mode != CD_TRACK_AUDIO_2352) {
            cd_synth_sync_msf(lba, 0x02u, out_2352);
        }
        return true;
    }

    u32 rel = lba - t->start_lba;
    if (rel >= t->length_lba) {
        memset(out_2352, 0, 2352u);
        return false;
    }

    u64 byte_off  = t->file_offset + (u64)rel * (u64)t->sector_size;
    PeriphFile *f = &img->files[t->file_slot];

    cd_io_lock(img);
    bool ok = false;
    if (t->sector_size == 2352u) {
        if (periph_fseek(f, byte_off) && periph_fread(f, out_2352, 2352u) == 2352u) {
            ok = true;
        }
    } else if (t->sector_size == 2048u) {
        memset(out_2352, 0, 2352u);
        cd_synth_sync_msf(lba, 0x02u, out_2352);
        /* Mode 2 Form 1 Data Subheader */
        out_2352[18] = 0x08u;
        out_2352[22] = 0x08u;
        if (periph_fseek(f, byte_off) && periph_fread(f, &out_2352[24], 2048u) == 2048u) {
            ok = true;
        }
    } else if (t->sector_size == 2336u) {
        memset(out_2352, 0, 2352u);
        cd_synth_sync_msf(lba, 0x02u, out_2352);
        if (periph_fseek(f, byte_off) && periph_fread(f, &out_2352[16], 2336u) == 2336u) {
            ok = true;
        }
    }
    cd_io_unlock(img);

    if (!ok) {
        memset(out_2352, 0, 2352u);
    }
    return ok;
}
