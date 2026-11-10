/*
 * ISO9660 CD-ROM boot support (v0.2.2).
 *
 * Reads 2048-byte user-data sectors straight from the mounted CdImage (CHD / CUE+BIN /
 * ISO), locates the Primary Volume Descriptor at LBA 16, walks the directory tree,
 * parses SYSTEM.CNF "BOOT = cdrom:\PATH\FILE.EXE;1" (falling back to PSX.EXE) and
 * loads the boot PS-X EXE into RAM.
 *
 * Used by:
 *   - psx_load_cd() in HLE BIOS mode (direct boot: there is no BIOS to read the disc),
 *   - the 0x80030000 shell-entry hook when a real BIOS / OpenBIOS is active and the
 *     user asked for fast boot (or the native BIOS CD boot never happened),
 *   - the HLE kernel's cdrom: file I/O (open/read/lseek/close, Load/LoadExec).
 */
#include "../periph/periph_internal.h"
#include "psx3ds/system.h"

#include <ctype.h>
#include <string.h>

#define CDB_SECTOR      2048u
#define CDB_PVD_LBA     16u
#define CDB_MAX_DEPTH   8u
#define CDB_MAX_DIR_SEC 64u
#define PSX_CDBOOT_WATCHDOG_FRAMES 1800u /* 30 s: >= 1.5 MiB EXE at 2x speed */ /* directories larger than 128 KiB are not plausible on PS1 */

static u32 cdb_le32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static bool cdb_disc_ready(const PsxSystem *sys) {
    return sys && sys->cdrom.img && sys->cdrom.img->loaded && sys->cdrom.img->total_lba > 0u;
}

/* Read one 2048-byte user-data sector (Mode 1 / Mode 2 Form 1). */
static bool cdb_read_sector(PsxSystem *sys, u32 lba, u8 out[CDB_SECTOR]) {
    if (!cdb_disc_ready(sys)) {
        return false;
    }
    CdImage *img = sys->cdrom.img;
    const CdTrack *t = cd_image_find_track(img, lba);
    if (t && t->mode == CD_TRACK_AUDIO_2352) {
        return false;
    }
    u8 raw[2352];
    if (!cd_image_read_sector(img, lba, raw)) {
        return false;
    }
    u32 off = (raw[15] == 0x01u) ? 16u : 24u;
    memcpy(out, raw + off, CDB_SECTOR);
    sys->cdrom.sectors_read++;
    return true;
}

bool psx_cdboot_read_data(PsxSystem *sys, u32 lba, u32 offset, u8 *dst, u32 len) {
    if (!dst && len > 0u) {
        return false;
    }
    u8 sec[CDB_SECTOR];
    u32 cur = lba + offset / CDB_SECTOR;
    u32 in_off = offset % CDB_SECTOR;
    u32 done = 0u;
    while (done < len) {
        if (!cdb_read_sector(sys, cur, sec)) {
            return false;
        }
        u32 n = CDB_SECTOR - in_off;
        if (n > len - done) {
            n = len - done;
        }
        memcpy(dst + done, sec + in_off, n);
        done += n;
        in_off = 0u;
        cur++;
    }
    return true;
}

/* Compare an ISO9660 identifier ("NAME.EXT;1", "NAME.;1") against a wanted component. */
static bool cdb_name_match(const u8 *id, u32 id_len, const char *want, u32 want_len) {
    /* Strip ";version" and a trailing '.' (files without extension are stored "NAME.") */
    u32 n = id_len;
    for (u32 i = 0; i < id_len; i++) {
        if (id[i] == ';') {
            n = i;
            break;
        }
    }
    if (n > 0u && id[n - 1u] == '.') {
        n--;
    }
    u32 w = want_len;
    if (w > 0u && want[w - 1u] == '.') {
        w--;
    }
    if (n != w) {
        return false;
    }
    for (u32 i = 0; i < n; i++) {
        if (toupper((unsigned char)id[i]) != toupper((unsigned char)want[i])) {
            return false;
        }
    }
    return true;
}

static bool cdb_find_in_dir(PsxSystem *sys, u32 dir_lba, u32 dir_size, const char *name,
                            u32 name_len, u32 *out_lba, u32 *out_size, bool *out_is_dir) {
    u8 sec[CDB_SECTOR];
    u32 nsec = (dir_size + CDB_SECTOR - 1u) / CDB_SECTOR;
    if (nsec == 0u) {
        nsec = 1u;
    }
    if (nsec > CDB_MAX_DIR_SEC) {
        nsec = CDB_MAX_DIR_SEC;
    }
    for (u32 s = 0; s < nsec; s++) {
        if (!cdb_read_sector(sys, dir_lba + s, sec)) {
            return false;
        }
        u32 pos = 0u;
        while (pos + 33u <= CDB_SECTOR) {
            u32 rec_len = sec[pos];
            if (rec_len == 0u) {
                break; /* records never straddle sectors: rest of sector is padding */
            }
            if (rec_len < 33u || pos + rec_len > CDB_SECTOR) {
                break;
            }
            u32 id_len = sec[pos + 32u];
            if (33u + id_len <= rec_len) {
                const u8 *id = &sec[pos + 33u];
                bool special = (id_len == 1u && (id[0] == 0x00u || id[0] == 0x01u));
                if (!special && cdb_name_match(id, id_len, name, name_len)) {
                    *out_lba = cdb_le32(&sec[pos + 2u]);
                    *out_size = cdb_le32(&sec[pos + 10u]);
                    *out_is_dir = (sec[pos + 25u] & 0x02u) != 0u;
                    return true;
                }
            }
            pos += rec_len;
        }
    }
    return false;
}

static bool cdb_read_root(PsxSystem *sys, u32 *root_lba, u32 *root_size, char *volume_out) {
    u8 pvd[CDB_SECTOR];
    if (!cdb_read_sector(sys, CDB_PVD_LBA, pvd)) {
        return false;
    }
    if (pvd[0] != 0x01u || memcmp(&pvd[1], "CD001", 5) != 0) {
        return false;
    }
    if (volume_out) {
        u32 n = 32u;
        memcpy(volume_out, &pvd[40], 32u);
        while (n > 0u && (volume_out[n - 1u] == ' ' || volume_out[n - 1u] == '\0')) {
            n--;
        }
        volume_out[n] = '\0';
        for (u32 i = 0; i < n; i++) {
            if ((unsigned char)volume_out[i] < 0x20u || (unsigned char)volume_out[i] > 0x7Eu) {
                volume_out[i] = '?';
            }
        }
    }
    *root_lba = cdb_le32(&pvd[156u + 2u]);
    *root_size = cdb_le32(&pvd[156u + 10u]);
    return *root_lba != 0u;
}

/*
 * Normalize "cdrom:\DIR\FILE.EXE;1" / "cdrom:FILE.EXE" / "cdrom0:\X" / "/DIR/FILE" to
 * "DIR\FILE.EXE" (no device, no leading separator, no ";version", no surrounding spaces).
 */
static void cdb_normalize_path(const char *in, char *out, u32 cap) {
    if (cap == 0u) {
        return;
    }
    out[0] = '\0';
    if (!in) {
        return;
    }
    while (*in == ' ' || *in == '\t') {
        in++;
    }
    const char *colon = strchr(in, ':');
    if (colon && (colon - in) <= 8) {
        in = colon + 1;
    }
    while (*in == '\\' || *in == '/' || *in == ' ') {
        in++;
    }
    u32 n = 0u;
    for (; *in != '\0' && n + 1u < cap; in++) {
        char ch = *in;
        if (ch == ';' || ch == '\r' || ch == '\n' || ch == '\t' || ch == ' ') {
            break;
        }
        if (ch == '/') {
            ch = '\\';
        }
        if (ch == '\\' && n > 0u && out[n - 1u] == '\\') {
            continue;
        }
        out[n++] = (char)toupper((unsigned char)ch);
    }
    while (n > 0u && out[n - 1u] == '\\') {
        n--;
    }
    out[n] = '\0';
}

bool psx_cdboot_find_file(PsxSystem *sys, const char *path, u32 *out_lba, u32 *out_size) {
    if (!cdb_disc_ready(sys) || !path) {
        return false;
    }
    char norm[128];
    cdb_normalize_path(path, norm, sizeof(norm));
    if (norm[0] == '\0') {
        return false;
    }
    u32 lba = 0u;
    u32 size = 0u;
    if (!cdb_read_root(sys, &lba, &size, NULL)) {
        return false;
    }
    const char *p = norm;
    for (u32 depth = 0; depth < CDB_MAX_DEPTH; depth++) {
        const char *sep = strchr(p, '\\');
        u32 comp_len = sep ? (u32)(sep - p) : (u32)strlen(p);
        bool is_dir = false;
        u32 nlba = 0u;
        u32 nsize = 0u;
        if (!cdb_find_in_dir(sys, lba, size, p, comp_len, &nlba, &nsize, &is_dir)) {
            return false;
        }
        if (!sep) {
            if (is_dir) {
                return false;
            }
            if (out_lba) {
                *out_lba = nlba;
            }
            if (out_size) {
                *out_size = nsize;
            }
            return true;
        }
        if (!is_dir) {
            return false;
        }
        lba = nlba;
        size = nsize;
        p = sep + 1;
    }
    return false;
}

static bool cdb_cnf_has_key(const char *txt, u32 len, const char *key) {
    u32 kl = (u32)strlen(key);
    u32 i = 0u;
    while (i < len) {
        while (i < len && (txt[i] == ' ' || txt[i] == '\t')) {
            i++;
        }
        if (i + kl <= len) {
            u32 k = 0u;
            while (k < kl && toupper((unsigned char)txt[i + k]) == key[k]) {
                k++;
            }
            if (k == kl) {
                u32 j = i + kl;
                while (j < len && (txt[j] == ' ' || txt[j] == '\t')) {
                    j++;
                }
                if (j < len && txt[j] == '=') {
                    return true;
                }
            }
        }
        while (i < len && txt[i] != '\n') {
            i++;
        }
        i++;
    }
    return false;
}

/* Extract the BOOT= value from SYSTEM.CNF text (case-insensitive key, any spacing). */
static bool cdb_parse_system_cnf(const char *txt, u32 len, char *out, u32 cap) {
    u32 i = 0u;
    while (i < len) {
        /* start of line: skip blanks */
        while (i < len && (txt[i] == ' ' || txt[i] == '\t')) {
            i++;
        }
        if (i + 4u <= len && toupper((unsigned char)txt[i]) == 'B' &&
            toupper((unsigned char)txt[i + 1u]) == 'O' &&
            toupper((unsigned char)txt[i + 2u]) == 'O' &&
            toupper((unsigned char)txt[i + 3u]) == 'T') {
            u32 j = i + 4u;
            /* "BOOT2" (PS2) is not ours; require blanks or '=' next */
            while (j < len && (txt[j] == ' ' || txt[j] == '\t')) {
                j++;
            }
            if (j < len && txt[j] == '=') {
                j++;
                while (j < len && (txt[j] == ' ' || txt[j] == '\t')) {
                    j++;
                }
                u32 n = 0u;
                while (j < len && n + 1u < cap && txt[j] != '\r' && txt[j] != '\n' &&
                       txt[j] != '\0' && txt[j] != ' ' && txt[j] != '\t') {
                    out[n++] = txt[j++];
                }
                out[n] = '\0';
                return n > 0u;
            }
        }
        while (i < len && txt[i] != '\n') {
            i++;
        }
        i++;
    }
    return false;
}

PsxResult psx_cdboot_probe(PsxSystem *sys, PsxCdBootInfo *out) {
    if (!sys || !out) {
        return PSX_ERR_BAD_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (!cdb_disc_ready(sys)) {
        return PSX_ERR_IO;
    }
    u32 root_lba = 0u;
    u32 root_size = 0u;
    if (!cdb_read_root(sys, &root_lba, &root_size, out->volume)) {
        plat_log("[cdboot] no ISO9660 PVD at LBA 16 (audio CD or unsupported image)\n");
        return PSX_ERR_FORMAT;
    }
    out->has_pvd = true;

    char boot_raw[128] = { 0 };
    u32 cnf_lba = 0u;
    u32 cnf_size = 0u;
    if (psx_cdboot_find_file(sys, "SYSTEM.CNF", &cnf_lba, &cnf_size)) {
        out->has_system_cnf = true;
        char txt[CDB_SECTOR + 1u];
        u32 rd = cnf_size < CDB_SECTOR ? cnf_size : CDB_SECTOR;
        if (rd > 0u && psx_cdboot_read_data(sys, cnf_lba, 0u, (u8 *)txt, rd)) {
            txt[rd] = '\0';
            (void)cdb_parse_system_cnf(txt, rd, boot_raw, sizeof(boot_raw));
            out->cnf_kernel_cfg = cdb_cnf_has_key(txt, rd, "TCB") && cdb_cnf_has_key(txt, rd, "EVENT");
        }
        plat_log("[cdboot] SYSTEM.CNF lba=%u size=%u BOOT='%s'\n", (unsigned)cnf_lba,
                 (unsigned)cnf_size, boot_raw);
    } else {
        plat_log("[cdboot] SYSTEM.CNF not found, trying PSX.EXE\n");
    }

    if (boot_raw[0] != '\0') {
        cdb_normalize_path(boot_raw, out->boot_path, sizeof(out->boot_path));
    }
    bool found = out->boot_path[0] != '\0' &&
                 psx_cdboot_find_file(sys, out->boot_path, &out->exe_lba, &out->exe_size);
    if (!found) {
        if (out->boot_path[0] != '\0') {
            plat_log("[cdboot] boot file '%s' not found, trying PSX.EXE\n", out->boot_path);
        }
        snprintf(out->boot_path, sizeof(out->boot_path), "PSX.EXE");
        found = psx_cdboot_find_file(sys, out->boot_path, &out->exe_lba, &out->exe_size);
    }
    if (!found) {
        plat_log("[cdboot] ISO9660 volume='%s': no boot executable\n", out->volume);
        return PSX_ERR_FORMAT;
    }

    u8 hdr_raw[CDB_SECTOR];
    if (!psx_cdboot_read_data(sys, out->exe_lba, 0u, hdr_raw, CDB_SECTOR)) {
        plat_log("[cdboot] read error at boot exe lba=%u\n", (unsigned)out->exe_lba);
        return PSX_ERR_IO;
    }
    PsxResult prc = psx_exe_parse_header(hdr_raw, CDB_SECTOR, &out->hdr);
    if (prc != PSX_OK) {
        plat_log("[cdboot] '%s' is not a PS-X EXE\n", out->boot_path);
        return prc;
    }
    /* Load-complete signature: the last non-zero 16 bytes of the text (BIOS loads in order) */
    u32 t_bytes = out->hdr.t_size;
    if (out->exe_size > CDB_SECTOR && out->exe_size - CDB_SECTOR < t_bytes) {
        t_bytes = out->exe_size - CDB_SECTOR;
    }
    t_bytes &= ~15u;
    for (u32 back = 0u; back < 32u && t_bytes >= 16u; back++) {
        u32 sec_off = (t_bytes - 1u) & ~(CDB_SECTOR - 1u);
        u8 sec[CDB_SECTOR];
        if (!psx_cdboot_read_data(sys, out->exe_lba + 1u, sec_off, sec, t_bytes - sec_off)) {
            break;
        }
        bool found_sig = false;
        for (u32 off = t_bytes - sec_off; off >= 16u; off -= 16u) {
            const u8 *w = &sec[off - 16u];
            u32 any = 0u;
            for (u32 k = 0; k < 16u; k++) {
                any |= w[k];
            }
            if (any != 0u) {
                out->sig_off = sec_off + off - 16u;
                memcpy(out->sig, w, sizeof(out->sig));
                found_sig = true;
                break;
            }
        }
        if (found_sig || sec_off == 0u) {
            break;
        }
        t_bytes = sec_off;
    }
    out->valid = true;
    plat_log("[cdboot] ISO9660 volume='%s' boot='%s' lba=%u size=%u\n", out->volume,
             out->boot_path, (unsigned)out->exe_lba, (unsigned)out->exe_size);
    plat_log("[cdboot] pc0=0x%08X gp0=0x%08X t_addr=0x%08X t_size=0x%X\n",
             (unsigned)out->hdr.pc0, (unsigned)out->hdr.gp0, (unsigned)out->hdr.t_addr,
             (unsigned)out->hdr.t_size);
    return PSX_OK;
}

PsxResult psx_cdboot_load_exe(PsxSystem *sys, u32 exe_lba, u32 exe_size, PsxExeHeader *hdr_out) {
    if (!sys || !sys->bus.ram || !hdr_out) {
        return PSX_ERR_BAD_ARG;
    }
    u8 sec[CDB_SECTOR];
    if (!psx_cdboot_read_data(sys, exe_lba, 0u, sec, CDB_SECTOR)) {
        return PSX_ERR_IO;
    }
    PsxResult prc = psx_exe_parse_header(sec, CDB_SECTOR, hdr_out);
    if (prc != PSX_OK) {
        return prc;
    }
    const PsxExeHeader *h = hdr_out;
    if (h->b_addr != 0u && h->b_size != 0u && h->b_size <= PSX_RAM_SIZE) {
        psx_exe_write_ram(sys, h->b_addr & PSX_RAM_MASK, NULL, h->b_size);
    }
    u32 avail = exe_size > CDB_SECTOR ? exe_size - CDB_SECTOR : 0u;
    u32 to_read = h->t_size < avail ? h->t_size : avail;
    if (exe_size == 0u) {
        to_read = h->t_size; /* unknown size: trust the header */
    }
    u32 t_phys = h->t_addr & PSX_RAM_MASK;
    u32 done = 0u;
    u32 lba = exe_lba + 1u;
    while (done < to_read) {
        if (!cdb_read_sector(sys, lba, sec)) {
            plat_log("[cdboot] read error at lba=%u (%u/%u bytes)\n", (unsigned)lba,
                     (unsigned)done, (unsigned)to_read);
            return PSX_ERR_IO;
        }
        u32 n = to_read - done;
        if (n > CDB_SECTOR) {
            n = CDB_SECTOR;
        }
        psx_exe_write_ram(sys, t_phys + done, sec, n);
        done += n;
        lba++;
    }
    if (done < h->t_size) {
        psx_exe_write_ram(sys, t_phys + done, NULL, h->t_size - done);
    }
    return PSX_OK;
}

PsxResult psx_cdboot_inject(PsxSystem *sys, const PsxCdBootInfo *info) {
    if (!sys || !info || !info->valid) {
        return PSX_ERR_BAD_ARG;
    }
    PsxExeHeader hdr;
    PsxResult rc = psx_cdboot_load_exe(sys, info->exe_lba, info->exe_size, &hdr);
    if (rc != PSX_OK) {
        return rc;
    }
    u32 sp = (hdr.s_addr != 0u) ? (hdr.s_addr + hdr.s_size) : 0x801FFF00u;
    psx_exe_apply_entry(sys, &hdr, sp);
    plat_log("[cdboot] injected '%s' -> pc=0x%08X sp=0x%08X (%s)\n", info->boot_path,
             (unsigned)hdr.pc0, (unsigned)sp, sys->bios.hle ? "HLE direct" : "BIOS sideload");
    return PSX_OK;
}

static bool cdb_bios_is_openbios(const PsxSystem *sys) {
    if (!sys->bus.bios) {
        return false;
    }
    static const char k_tag[] = "OpenBios";
    const u32 n = (u32)sizeof(k_tag) - 1u;
    for (u32 i = 0; i + n <= PSX_BIOS_SIZE; i++) {
        if (sys->bus.bios[i] == 'O' && memcmp(&sys->bus.bios[i], k_tag, n) == 0) {
            return true;
        }
    }
    return false;
}

void psx_cdboot_arm_bios(PsxSystem *sys) {
    if (!sys || sys->bios.hle || !sys->bios.cd_boot || !sys->bios.cdboot.valid) {
        return;
    }
    PsxBios *b = &sys->bios;
    bool fast = sys->cfg.cd_fast_boot;
    if (!fast && b->cdboot.has_system_cnf && !b->cdboot.cnf_kernel_cfg && cdb_bios_is_openbios(sys)) {
        /* OpenBIOS configures 0 TCBs/EvCBs when SYSTEM.CNF omits TCB/EVENT and then hangs
         * opening the boot file; keep its default kernel config and sideload instead. */
        plat_log("[cdboot] SYSTEM.CNF lacks TCB/EVENT: OpenBIOS fast boot (sideload)\n");
        fast = true;
    }
    b->cdboot_pending = fast;
    b->cdboot_skip_shell = !fast;
    b->cdboot_watch = false;
    b->cdboot_watch_frames = 0u;
    sys->cpu.psexe_pending = 1u;
    jit_flush_all(sys);
}

void psx_cdboot_watchdog(PsxSystem *sys) {
    if (!sys || !sys->bios.cdboot_watch || !sys->bus.ram) {
        return;
    }
    PsxBios *b = &sys->bios;
    const PsxExeHeader *h = &b->cdboot.hdr;
    u32 n = (u32)sizeof(b->cdboot.sig);
    u32 t = (h->t_addr + b->cdboot.sig_off) & PSX_RAM_MASK;
    if (t + n <= PSX_RAM_SIZE && memcmp(sys->bus.ram + t, b->cdboot.sig, n) == 0) {
        b->cdboot_watch = false;
        plat_log("[cdboot] BIOS loaded %s natively after %u frames (CD Rd %u)\n",
                 b->cdboot.boot_path, (unsigned)b->cdboot_watch_frames,
                 (unsigned)sys->cdrom.sectors_read);
        return;
    }
    if (++b->cdboot_watch_frames >= PSX_CDBOOT_WATCHDOG_FRAMES) {
        b->cdboot_watch = false;
        plat_log("[cdboot] native BIOS CD boot stalled (%u frames, CD Rd %u, pc=0x%08X): "
                 "sideloading %s\n",
                 (unsigned)b->cdboot_watch_frames, (unsigned)sys->cdrom.sectors_read,
                 (unsigned)sys->cpu.pc, b->cdboot.boot_path);
        (void)psx_cdboot_inject(sys, &b->cdboot);
    }
}
