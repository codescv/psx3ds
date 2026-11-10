/*
 * Minimal PS1 ISO9660 test-disc builder shared by CD-boot unit tests (v0.2.2).
 *
 * Builds an ISO9660 image (PVD @ LBA 16, path tables, root + optional sub-directory,
 * optional SYSTEM.CNF, boot PS-X EXE) and writes it as:
 *   - 2048-byte .iso
 *   - 2352-byte raw Mode 2 Form 1 .bin + .cue
 *   - uncompressed CHD v5 (MODE2_RAW, CHT2 metadata) readable by libchdr
 *
 * Header-only (static functions) so it is linked only into the tests that include it.
 */
#ifndef PSX3DS_TEST_CDBOOT_DISC_H
#define PSX3DS_TEST_CDBOOT_DISC_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psx3ds/types.h"

#define TDISC_SEC        2048u
#define TDISC_RAW        2352u
#define TDISC_LBA_PVD    16u
#define TDISC_LBA_TERM   17u
#define TDISC_LBA_LPATH  18u
#define TDISC_LBA_MPATH  19u
#define TDISC_LBA_ROOT   20u
#define TDISC_LBA_SUBDIR 21u
#define TDISC_LBA_CNF    22u
#define TDISC_LBA_EXE    23u
#define TDISC_TAIL_PAD   64u

typedef struct TestDiscOpts {
    const char *volume;     /* PVD volume id */
    const char *system_cnf; /* SYSTEM.CNF text, or NULL for none */
    const char *exe_name;   /* ISO name without ";1", e.g. "CUBE.EXE" or "PSX.EXE" */
    const char *subdir;     /* NULL: exe in root; else directory name holding the exe */
    const u8   *exe;
    u32         exe_size;
} TestDiscOpts;

static void tdisc_both16(u8 *p, u16 v) {
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 8); p[3] = (u8)v;
}
static void tdisc_both32(u8 *p, u32 v) {
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
    p[4] = (u8)(v >> 24); p[5] = (u8)(v >> 16); p[6] = (u8)(v >> 8); p[7] = (u8)v;
}
static void tdisc_le32(u8 *p, u32 v) {
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}
static void tdisc_be32(u8 *p, u32 v) {
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v;
}
static void tdisc_be16(u8 *p, u16 v) {
    p[0] = (u8)(v >> 8); p[1] = (u8)v;
}
static void tdisc_be64(u8 *p, u64 v) {
    for (int i = 0; i < 8; i++) {
        p[i] = (u8)(v >> (56 - 8 * i));
    }
}
static void tdisc_pad_str(u8 *dst, const char *s, u32 n) {
    memset(dst, ' ', n);
    if (s) {
        size_t l = strlen(s);
        memcpy(dst, s, l < n ? l : n);
    }
}

/* Appends one directory record; returns its length. */
static u32 tdisc_dirrec(u8 *dst, const char *id, u32 id_len, u32 lba, u32 size, bool is_dir) {
    u32 len = 33u + id_len;
    if (len & 1u) {
        len++;
    }
    memset(dst, 0, len);
    dst[0] = (u8)len;
    tdisc_both32(&dst[2], lba);
    tdisc_both32(&dst[10], size);
    dst[18] = 98; dst[19] = 1; dst[20] = 1; /* 1998-01-01 */
    dst[25] = is_dir ? 0x02u : 0x00u;
    tdisc_both16(&dst[28], 1u);
    dst[32] = (u8)id_len;
    memcpy(&dst[33], id, id_len);
    return len;
}

static u32 tdisc_sectors_for(u32 bytes) {
    return bytes == 0u ? 1u : (bytes + TDISC_SEC - 1u) / TDISC_SEC;
}

/* Returns malloc'd 2048-byte-sector image; caller frees. */
static u8 *tdisc_build_iso(const TestDiscOpts *o, u32 *out_sectors) {
    u32 exe_secs = tdisc_sectors_for(o->exe_size);
    u32 total = TDISC_LBA_EXE + exe_secs + TDISC_TAIL_PAD;
    u8 *img = (u8 *)calloc(total, TDISC_SEC);
    if (!img) {
        return NULL;
    }
    /* LBA 4: Sony license string (region detection reads "SCEA") */
    const char *lic = "          Licensed  by          Sony Computer Entertainment Amer  ica ";
    memcpy(img + 4u * TDISC_SEC, lic, strlen(lic));

    /* Directories */
    u8 *root = img + TDISC_LBA_ROOT * TDISC_SEC;
    u32 rp = 0u;
    rp += tdisc_dirrec(root + rp, "\0", 1u, TDISC_LBA_ROOT, TDISC_SEC, true);
    rp += tdisc_dirrec(root + rp, "\1", 1u, TDISC_LBA_ROOT, TDISC_SEC, true);
    char idbuf[64];
    u32 cnf_len = o->system_cnf ? (u32)strlen(o->system_cnf) : 0u;
    /* ISO9660 requires sorted identifiers: emit in name order */
    const char *names[3];
    int kinds[3]; /* 0 = SYSTEM.CNF, 1 = exe, 2 = subdir */
    int nent = 0;
    if (o->system_cnf) { names[nent] = "SYSTEM.CNF"; kinds[nent++] = 0; }
    if (o->subdir)     { names[nent] = o->subdir;    kinds[nent++] = 2; }
    else               { names[nent] = o->exe_name;  kinds[nent++] = 1; }
    for (int a = 0; a < nent; a++) {
        for (int b = a + 1; b < nent; b++) {
            if (strcmp(names[b], names[a]) < 0) {
                const char *tn = names[a]; names[a] = names[b]; names[b] = tn;
                int tk = kinds[a]; kinds[a] = kinds[b]; kinds[b] = tk;
            }
        }
    }
    for (int e = 0; e < nent; e++) {
        if (kinds[e] == 0) {
            snprintf(idbuf, sizeof(idbuf), "SYSTEM.CNF;1");
            rp += tdisc_dirrec(root + rp, idbuf, (u32)strlen(idbuf), TDISC_LBA_CNF, cnf_len, false);
        } else if (kinds[e] == 1) {
            snprintf(idbuf, sizeof(idbuf), "%s;1", o->exe_name);
            rp += tdisc_dirrec(root + rp, idbuf, (u32)strlen(idbuf), TDISC_LBA_EXE, o->exe_size, false);
        } else {
            rp += tdisc_dirrec(root + rp, o->subdir, (u32)strlen(o->subdir), TDISC_LBA_SUBDIR,
                               TDISC_SEC, true);
        }
    }
    if (o->subdir) {
        u8 *sd = img + TDISC_LBA_SUBDIR * TDISC_SEC;
        u32 sp = 0u;
        sp += tdisc_dirrec(sd + sp, "\0", 1u, TDISC_LBA_SUBDIR, TDISC_SEC, true);
        sp += tdisc_dirrec(sd + sp, "\1", 1u, TDISC_LBA_ROOT, TDISC_SEC, true);
        snprintf(idbuf, sizeof(idbuf), "%s;1", o->exe_name);
        sp += tdisc_dirrec(sd + sp, idbuf, (u32)strlen(idbuf), TDISC_LBA_EXE, o->exe_size, false);
    }

    /* Path tables (L = little-endian @18, M = big-endian @19) */
    u8 *lpt = img + TDISC_LBA_LPATH * TDISC_SEC;
    u8 *mpt = img + TDISC_LBA_MPATH * TDISC_SEC;
    u32 pt = 0u;
    lpt[0] = 1; mpt[0] = 1;
    tdisc_le32(&lpt[2], TDISC_LBA_ROOT); tdisc_be32(&mpt[2], TDISC_LBA_ROOT);
    lpt[6] = 1; lpt[7] = 0; tdisc_be16(&mpt[6], 1u);
    pt = 10u;
    if (o->subdir) {
        u32 nl = (u32)strlen(o->subdir);
        lpt[pt] = (u8)nl; mpt[pt] = (u8)nl;
        tdisc_le32(&lpt[pt + 2u], TDISC_LBA_SUBDIR); tdisc_be32(&mpt[pt + 2u], TDISC_LBA_SUBDIR);
        lpt[pt + 6u] = 1; lpt[pt + 7u] = 0; tdisc_be16(&mpt[pt + 6u], 1u);
        memcpy(&lpt[pt + 8u], o->subdir, nl); memcpy(&mpt[pt + 8u], o->subdir, nl);
        pt += 8u + nl + (nl & 1u);
    }

    /* PVD */
    u8 *pvd = img + TDISC_LBA_PVD * TDISC_SEC;
    pvd[0] = 1; memcpy(&pvd[1], "CD001", 5); pvd[6] = 1;
    tdisc_pad_str(&pvd[8], "PLAYSTATION", 32u);
    tdisc_pad_str(&pvd[40], o->volume ? o->volume : "PSX3DS_TEST", 32u);
    tdisc_both32(&pvd[80], total);
    tdisc_both16(&pvd[120], 1u);
    tdisc_both16(&pvd[124], 1u);
    tdisc_both16(&pvd[128], (u16)TDISC_SEC);
    tdisc_both32(&pvd[132], pt);
    tdisc_le32(&pvd[140], TDISC_LBA_LPATH);
    tdisc_be32(&pvd[148], TDISC_LBA_MPATH);
    (void)tdisc_dirrec(&pvd[156], "\0", 1u, TDISC_LBA_ROOT, TDISC_SEC, true);
    tdisc_pad_str(&pvd[190], "", 128u);
    tdisc_pad_str(&pvd[318], "PSX3DS", 128u);
    tdisc_pad_str(&pvd[446], "PSX3DS", 128u);
    tdisc_pad_str(&pvd[574], "PLAYSTATION", 128u);
    tdisc_pad_str(&pvd[702], "", 37u * 3u);
    for (u32 d = 0; d < 4u; d++) {
        memset(&pvd[813u + d * 17u], '0', 16u);
    }
    pvd[881] = 1;

    /* Terminator */
    u8 *term = img + TDISC_LBA_TERM * TDISC_SEC;
    term[0] = 0xFF; memcpy(&term[1], "CD001", 5); term[6] = 1;

    if (o->system_cnf) {
        memcpy(img + TDISC_LBA_CNF * TDISC_SEC, o->system_cnf, cnf_len);
    }
    if (o->exe && o->exe_size) {
        memcpy(img + TDISC_LBA_EXE * TDISC_SEC, o->exe, o->exe_size);
    }
    *out_sectors = total;
    return img;
}

static u8 tdisc_bcd(u32 v) {
    return (u8)(((v / 10u) << 4) | (v % 10u));
}

/* 2048 -> 2352 raw Mode 2 Form 1 sector (sync + BCD MSF header + subheader). */
static void tdisc_raw_sector(const u8 *user, u32 lba, bool eof, u8 out[TDISC_RAW]) {
    memset(out, 0, TDISC_RAW);
    memset(&out[1], 0xFF, 10u);
    u32 a = lba + 150u;
    out[12] = tdisc_bcd(a / (75u * 60u));
    out[13] = tdisc_bcd((a / 75u) % 60u);
    out[14] = tdisc_bcd(a % 75u);
    out[15] = 0x02u;
    u8 sub = eof ? 0x89u : 0x08u;
    out[18] = sub;
    out[22] = sub;
    memcpy(&out[24], user, TDISC_SEC);
}

static bool tdisc_is_eof_lba(u32 lba, const TestDiscOpts *o) {
    if (lba == TDISC_LBA_TERM) {
        return true;
    }
    u32 exe_last = TDISC_LBA_EXE + tdisc_sectors_for(o->exe_size) - 1u;
    return lba == TDISC_LBA_CNF || lba == exe_last || lba == TDISC_LBA_ROOT;
}

static bool tdisc_write_iso(const char *path, const u8 *iso, u32 nsec) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return false;
    }
    bool ok = fwrite(iso, TDISC_SEC, nsec, fp) == nsec;
    fclose(fp);
    return ok;
}

static bool tdisc_write_bin_cue(const char *bin_path, const char *cue_path, const u8 *iso,
                                u32 nsec, const TestDiscOpts *o) {
    FILE *fp = fopen(bin_path, "wb");
    if (!fp) {
        return false;
    }
    u8 raw[TDISC_RAW];
    bool ok = true;
    for (u32 l = 0; l < nsec && ok; l++) {
        tdisc_raw_sector(iso + (size_t)l * TDISC_SEC, l, tdisc_is_eof_lba(l, o), raw);
        ok = fwrite(raw, 1u, TDISC_RAW, fp) == TDISC_RAW;
    }
    fclose(fp);
    if (!ok) {
        return false;
    }
    const char *base = strrchr(bin_path, '/');
    base = base ? base + 1 : bin_path;
    FILE *cf = fopen(cue_path, "wb");
    if (!cf) {
        return false;
    }
    fprintf(cf, "FILE \"%s\" BINARY\r\n  TRACK 01 MODE2/2352\r\n    INDEX 01 00:00:00\r\n", base);
    fclose(cf);
    return true;
}

/* Uncompressed CHD v5: 8 frames (2448 B = 2352 + 96 subcode) per 19584-byte hunk. */
static bool tdisc_write_chd(const char *path, const u8 *iso, u32 nsec, const TestDiscOpts *o) {
    const u32 unit = 2448u;
    const u32 hunk_bytes = unit * 8u;
    u32 frames_padded = (nsec + 3u) & ~3u; /* CD_TRACK_PADDING = 4 */
    u32 hunks = (frames_padded + 7u) / 8u;
    char meta[160];
    int ml = snprintf(meta, sizeof(meta),
                      "TRACK:1 TYPE:MODE2_RAW SUBTYPE:NONE FRAMES:%u PREGAP:0 PGTYPE:MODE1 "
                      "PGSUB:RW POSTGAP:0",
                      (unsigned)nsec);
    u32 meta_len = (u32)ml + 1u;
    u64 map_off = 124u;
    u64 meta_off = map_off + (u64)hunks * 4u;
    u64 first_hunk_unit = (meta_off + 16u + meta_len + hunk_bytes - 1u) / hunk_bytes;

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return false;
    }
    u8 hdr[124];
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, "MComprHD", 8u);
    tdisc_be32(&hdr[8], 124u);
    tdisc_be32(&hdr[12], 5u);
    tdisc_be64(&hdr[32], (u64)hunks * hunk_bytes);
    tdisc_be64(&hdr[40], map_off);
    tdisc_be64(&hdr[48], meta_off);
    tdisc_be32(&hdr[56], hunk_bytes);
    tdisc_be32(&hdr[60], unit);
    bool ok = fwrite(hdr, 1u, sizeof(hdr), fp) == sizeof(hdr);
    for (u32 h = 0; h < hunks && ok; h++) {
        u8 e[4];
        tdisc_be32(e, (u32)(first_hunk_unit + h));
        ok = fwrite(e, 1u, 4u, fp) == 4u;
    }
    u8 mh[16];
    memcpy(mh, "CHT2", 4u);
    mh[4] = 0x01u; /* CHD_MDFLAGS_CHECKSUM */
    mh[5] = (u8)(meta_len >> 16); mh[6] = (u8)(meta_len >> 8); mh[7] = (u8)meta_len;
    memset(&mh[8], 0, 8u);
    ok = ok && fwrite(mh, 1u, 16u, fp) == 16u && fwrite(meta, 1u, meta_len, fp) == meta_len;
    /* pad to first hunk */
    long pos = ftell(fp);
    while (ok && (u64)pos < first_hunk_unit * hunk_bytes) {
        ok = fputc(0, fp) != EOF;
        pos++;
    }
    u8 *hunk = (u8 *)malloc(hunk_bytes);
    if (!hunk) {
        fclose(fp);
        return false;
    }
    for (u32 h = 0; h < hunks && ok; h++) {
        memset(hunk, 0, hunk_bytes);
        for (u32 f = 0; f < 8u; f++) {
            u32 lba = h * 8u + f;
            if (lba < nsec) {
                tdisc_raw_sector(iso + (size_t)lba * TDISC_SEC, lba, tdisc_is_eof_lba(lba, o),
                                 hunk + f * unit);
            }
        }
        ok = fwrite(hunk, 1u, hunk_bytes, fp) == hunk_bytes;
    }
    free(hunk);
    fclose(fp);
    return ok;
}

static u8 *tdisc_read_file(const char *path, u32 *out_size) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n <= 0) {
        fclose(fp);
        return NULL;
    }
    u8 *buf = (u8 *)malloc((size_t)n);
    if (buf && fread(buf, 1u, (size_t)n, fp) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(fp);
    *out_size = (u32)n;
    return buf;
}

#endif /* PSX3DS_TEST_CDBOOT_DISC_H */
