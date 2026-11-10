/*
 * ISO9660 CD-ROM boot regression tests (v0.2.2).
 *
 * Builds real PS1-style discs (PVD @ LBA 16, path tables, SYSTEM.CNF, PS-X EXE) as
 * .iso / .bin+.cue / uncompressed CHD v5 and verifies:
 *   1. SYSTEM.CNF parsing edge cases ("cdrom:\", "cdrom:", "cdrom0:", spaces/tabs,
 *      lower case, sub-directories, ";1", trailing args, PSX.EXE fallback, non-ISO).
 *   2. HLE BIOS: psx_load_cd() direct-boots the disc EXE; the game renders (prims > 0).
 *   3. HLE kernel cdrom: file I/O (B0:32 open / B0:34 read / B0:33 lseek / B0:36 close).
 *   4. Built-in OpenBIOS: native CD boot through the CD-ROM controller (Setloc/ReadN/
 *      DMA3, CD Rd > EXE sectors) of SYSTEM.CNF -> EXE from .chd and .cue/.bin, with
 *      the game rendering; fast boot (0x80030000 sideload); the TCB/EVENT-less
 *      SYSTEM.CNF OpenBIOS quirk; and the embedded 512 KiB OpenBIOS image itself.
 *
 * Set PSX3DS_KEEP_TEST_DISCS=1 to keep /tmp/psx3ds_test_cdboot_* images (used by the
 * 3DS azahar-headless end-to-end run).
 */
#include "psx3ds/system.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !PSX3DS_PLATFORM_3DS
#include "cdboot_disc.h"
#endif

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, (msg));       \
            return 1;                                                               \
        }                                                                           \
    } while (0)

#define DISC_DIR "/tmp/psx3ds_test_cdboot_"

#if !PSX3DS_PLATFORM_3DS
static PsxSystem g_sys;

static void resolve_rom_path(char *out, size_t cap, const char *rel_under_roms) {
    const char *env_roms = getenv("PSX3DS_ROMS_DIR");
    if (env_roms && env_roms[0] != '\0') {
        snprintf(out, cap, "%s/%s", env_roms, rel_under_roms);
        return;
    }
    const char *f = __FILE__;
    const char *slash = strrchr(f, '/');
    if (slash && (size_t)(slash - f) < 400u) {
        snprintf(out, cap, "%.*s/../roms/%s", (int)(slash - f), f, rel_under_roms);
        return;
    }
    snprintf(out, cap, "tests/roms/%s", rel_under_roms);
}

typedef struct DiscFiles {
    char iso[128];
    char bin[128];
    char cue[128];
    char chd[128];
} DiscFiles;

static bool make_disc(const char *tag, const TestDiscOpts *o, DiscFiles *df) {
    snprintf(df->iso, sizeof(df->iso), DISC_DIR "%s.iso", tag);
    snprintf(df->bin, sizeof(df->bin), DISC_DIR "%s.bin", tag);
    snprintf(df->cue, sizeof(df->cue), DISC_DIR "%s.cue", tag);
    snprintf(df->chd, sizeof(df->chd), DISC_DIR "%s.chd", tag);
    u32 ns = 0u;
    u8 *iso = tdisc_build_iso(o, &ns);
    if (!iso) {
        return false;
    }
    bool ok = tdisc_write_iso(df->iso, iso, ns) && tdisc_write_bin_cue(df->bin, df->cue, iso, ns, o) &&
              tdisc_write_chd(df->chd, iso, ns, o);
    free(iso);
    return ok;
}

static void remove_disc(const DiscFiles *df) {
    const char *keep = getenv("PSX3DS_KEEP_TEST_DISCS");
    if (keep && keep[0] == '1') {
        return;
    }
    (void)remove(df->iso);
    (void)remove(df->bin);
    (void)remove(df->cue);
    (void)remove(df->chd);
}

static PsxResult boot_system(const char *bios, bool fast_boot) {
    PsxConfig cfg;
    psx_config_defaults(&cfg, true);
    cfg.gpu_backend = GPU_BACKEND_SOFT;
    cfg.cd_fast_boot = fast_boot;
    if (!bios) {
        cfg.hle_bios = true;
    } else {
        snprintf(cfg.bios_path, sizeof(cfg.bios_path), "%s", bios);
    }
    return psx_system_init(&g_sys, &cfg);
}

typedef struct RunResult {
    u32 frames;
    u32 prims;
    u32 cd_rd;
    u32 pc;
} RunResult;

/* Run until the game has drawn `want_prims` primitives (or max_frames). */
static RunResult run_until_prims(u32 max_frames, u32 want_prims) {
    RunResult r = { 0u, 0u, 0u, 0u };
    for (; r.frames < max_frames && r.prims < want_prims; r.frames++) {
        psx_run_frame(&g_sys);
        r.prims += g_sys.stats.gpu_prims;
    }
    r.cd_rd = g_sys.cdrom.sectors_read;
    r.pc = g_sys.cpu.pc;
    return r;
}

static bool ram_matches_exe(const u8 *exe, u32 exe_size) {
    PsxExeHeader h;
    if (psx_exe_parse_header(exe, exe_size, &h) != PSX_OK) {
        return false;
    }
    u32 n = h.t_size < 4096u ? h.t_size : 4096u;
    return memcmp(g_sys.bus.ram + (h.t_addr & PSX_RAM_MASK), exe + 0x800u, n) == 0;
}

/* ------------------------------------------------------------------------ */

typedef struct ParseCase {
    const char *tag;
    const char *cnf;
    const char *exe_name;
    const char *subdir;
    const char *want_boot;
} ParseCase;

static int test_system_cnf_parsing(const u8 *exe, u32 exe_size) {
    static const ParseCase k_cases[] = {
        { "p_std",   "BOOT = cdrom:\\GAME.EXE;1\r\nTCB = 4\r\nEVENT = 10\r\nSTACK = 801FFFF0\r\n",
          "GAME.EXE", NULL, "GAME.EXE" },
        { "p_sub",   "boot=cdrom:\\SLUS\\SLUS_000.01;1\n", "SLUS_000.01", "SLUS", "SLUS\\SLUS_000.01" },
        { "p_nosl",  "  BOOT\t=\tcdrom:game.exe  \n", "GAME.EXE", NULL, "GAME.EXE" },
        { "p_cd0",   "TCB=4\nBOOT = cdrom0:\\GAME.EXE;1 arg1 arg2\n", "GAME.EXE", NULL, "GAME.EXE" },
        { "p_fwd",   "BOOT = cdrom:/DATA/MAIN.EXE;1\n", "MAIN.EXE", "DATA", "DATA\\MAIN.EXE" },
        { "p_nocnf", NULL, "PSX.EXE", NULL, "PSX.EXE" },
        { "p_miss",  "BOOT = cdrom:\\NOPE.EXE;1\n", "PSX.EXE", NULL, "PSX.EXE" },
    };
    for (size_t i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
        const ParseCase *pc = &k_cases[i];
        TestDiscOpts o = { "PARSE_TEST", pc->cnf, pc->exe_name, pc->subdir, exe, exe_size };
        DiscFiles df;
        CHECK(make_disc(pc->tag, &o, &df), "build parse-case disc");
        CHECK(boot_system(NULL, false) == PSX_OK, "init HLE system");
        CHECK(psx_load_cd(&g_sys, df.iso) == PSX_OK, "psx_load_cd(.iso)");
        const PsxCdBootInfo *bi = &g_sys.bios.cdboot;
        if (!bi->valid || strcmp(bi->boot_path, pc->want_boot) != 0) {
            fprintf(stderr, "case %s: valid=%d boot='%s' want '%s'\n", pc->tag, (int)bi->valid,
                    bi->boot_path, pc->want_boot);
        }
        CHECK(bi->valid && g_sys.bios.cd_boot, "boot EXE located");
        CHECK(strcmp(bi->boot_path, pc->want_boot) == 0, "normalized BOOT path");
        CHECK(bi->has_pvd && strcmp(bi->volume, "PARSE_TEST") == 0, "PVD volume id");
        CHECK(bi->exe_lba == TDISC_LBA_EXE && bi->exe_size == exe_size, "boot EXE extent");
        CHECK(bi->has_system_cnf == (pc->cnf != NULL), "SYSTEM.CNF presence");
        CHECK(g_sys.cpu.pc == bi->hdr.pc0, "HLE direct boot jumped to pc0");
        CHECK(ram_matches_exe(exe, exe_size), "EXE payload injected into RAM");

        /* Path lookups are case-insensitive and accept device prefixes / ;1 suffixes */
        u32 lba = 0u, size = 0u;
        char q[160];
        snprintf(q, sizeof(q), "cdrom:\\%s%s%s;1", pc->subdir ? pc->subdir : "",
                 pc->subdir ? "\\" : "", pc->exe_name);
        for (char *p = q; *p; p++) {
            if (*p >= 'A' && *p <= 'Z') {
                *p = (char)(*p - 'A' + 'a');
            }
        }
        CHECK(psx_cdboot_find_file(&g_sys, q, &lba, &size) && lba == TDISC_LBA_EXE, "find_file");
        CHECK(!psx_cdboot_find_file(&g_sys, "cdrom:\\DOES\\NOT.EXIST", &lba, &size), "missing file");
        psx_system_shutdown(&g_sys);
        remove_disc(&df);
    }

    /* Non-ISO data (no PVD): mounts fine, but is not bootable */
    {
        u8 *blank = (u8 *)calloc(64u, TDISC_SEC);
        CHECK(blank != NULL, "alloc blank");
        CHECK(tdisc_write_iso(DISC_DIR "blank.iso", blank, 64u), "write blank iso");
        free(blank);
        CHECK(boot_system(NULL, false) == PSX_OK, "init HLE system");
        CHECK(psx_load_cd(&g_sys, DISC_DIR "blank.iso") == PSX_OK, "mount blank disc");
        CHECK(!g_sys.bios.cd_boot && !g_sys.bios.cdboot.has_pvd, "blank disc not bootable");
        psx_system_shutdown(&g_sys);
        (void)remove(DISC_DIR "blank.iso");
    }
    printf("  [ok] SYSTEM.CNF / ISO9660 parsing (7 variants + non-ISO)\n");
    return 0;
}

static int hle_call(u32 vec, u32 fn, u32 a0, u32 a1, u32 a2) {
    g_sys.cpu.gpr[9] = fn;
    g_sys.cpu.gpr[4] = a0;
    g_sys.cpu.gpr[5] = a1;
    g_sys.cpu.gpr[6] = a2;
    g_sys.cpu.gpr[31] = 0x80001000u;
    (void)hle_bios_dispatch(&g_sys, vec);
    return (int)g_sys.cpu.gpr[2];
}

static int test_hle_file_io(const DiscFiles *df, const char *cnf) {
    CHECK(boot_system(NULL, false) == PSX_OK, "init HLE system");
    CHECK(psx_load_cd(&g_sys, df->chd) == PSX_OK, "mount chd");
    const u32 name = 0x80100000u, buf = 0x80101000u;
    const char *path = "cdrom:\\system.cnf;1";
    memcpy(g_sys.bus.ram + (name & PSX_RAM_MASK), path, strlen(path) + 1u);
    int fd = hle_call(0xB0u, 0x32u, name, 1u, 0u);
    CHECK(fd >= 3, "B0:32 open(cdrom:\\SYSTEM.CNF;1)");
    int got = hle_call(0xB0u, 0x34u, (u32)fd, buf, 2048u);
    CHECK(got == (int)strlen(cnf), "B0:34 read returns file size");
    CHECK(memcmp(g_sys.bus.ram + (buf & PSX_RAM_MASK), cnf, strlen(cnf)) == 0, "read content");
    CHECK(hle_call(0xB0u, 0x33u, (u32)fd, 7u, 0u) == 7, "B0:33 lseek SET");
    CHECK(hle_call(0xB0u, 0x34u, (u32)fd, buf, 5u) == 5, "read after seek");
    CHECK(memcmp(g_sys.bus.ram + (buf & PSX_RAM_MASK), cnf + 7, 5u) == 0, "seeked content");
    CHECK(hle_call(0xB0u, 0x36u, (u32)fd, 0u, 0u) == fd, "B0:36 close");
    CHECK(hle_call(0xB0u, 0x34u, (u32)fd, buf, 4u) == -1, "read on closed fd fails");
    memcpy(g_sys.bus.ram + (name & PSX_RAM_MASK), "cdrom:\\NOPE;1", 14u);
    CHECK(hle_call(0xA0u, 0x00u, name, 1u, 0u) == -1, "A0:00 open missing file");
    psx_system_shutdown(&g_sys);
    printf("  [ok] HLE kernel cdrom: open/read/lseek/close\n");
    return 0;
}

static int test_hle_boot(const DiscFiles *df, const char *what, u32 exe_secs) {
    CHECK(boot_system(NULL, false) == PSX_OK, "init HLE system");
    CHECK(psx_load_cd(&g_sys, df->chd) == PSX_OK, "psx_load_cd(.chd) in HLE mode");
    CHECK(g_sys.bios.hle && g_sys.bios.cd_boot, "HLE CD boot armed");
    RunResult r = run_until_prims(240u, 30u);
    printf("  [ok] HLE BIOS  + .chd %-10s: %3u frames, prims=%u, CD Rd=%u, pc=0x%08X\n", what,
           (unsigned)r.frames, (unsigned)r.prims, (unsigned)r.cd_rd, (unsigned)r.pc);
    CHECK(r.prims >= 30u, "HLE CD boot renders GPU primitives");
    CHECK(r.cd_rd >= exe_secs, "HLE CD boot read the EXE from the disc");
    psx_system_shutdown(&g_sys);
    return 0;
}

static int test_openbios_boot(const char *disc, const char *what, bool fast, u32 exe_secs,
                              u32 max_frames) {
    CHECK(boot_system(PSX_BIOS_BUILTIN_NAME, fast) == PSX_OK, "init with built-in OpenBIOS");
    CHECK(g_sys.bios.loaded && !g_sys.bios.hle, "OpenBIOS loaded");
    CHECK(psx_load_cd(&g_sys, disc) == PSX_OK, "psx_load_cd under OpenBIOS");
    CHECK(g_sys.bios.cd_boot, "disc bootable");
    bool native = g_sys.bios.cdboot_skip_shell;
    RunResult r = run_until_prims(max_frames, 30u);
    printf("  [ok] OpenBIOS + %-15s: %3u frames, prims=%u, CD Rd=%u, pc=0x%08X (%s)\n", what,
           (unsigned)r.frames, (unsigned)r.prims, (unsigned)r.cd_rd, (unsigned)r.pc,
           native ? "native BIOS CD boot" : "0x80030000 sideload");
    CHECK(r.prims >= 30u, "OpenBIOS CD boot renders GPU primitives");
    if (native) {
        /* probe reads ~10 sectors directly; the BIOS must read the EXE via the controller */
        CHECK(r.cd_rd >= exe_secs + 10u, "OpenBIOS read the EXE through the CD-ROM controller");
        CHECK(!g_sys.bios.cdboot_watch, "native boot completed before the watchdog");
    }
    psx_system_shutdown(&g_sys);
    return 0;
}

static int test_builtin_openbios_image(void) {
    u32 sz = 0u;
    const u8 *blob = psx_builtin_openbios(&sz);
    CHECK(blob != NULL && sz == PSX_BIOS_SIZE, "embedded OpenBIOS is 512 KiB");
    bool tag = false;
    for (u32 i = 0; i + 8u <= sz && !tag; i++) {
        tag = memcmp(blob + i, "OpenBios", 8u) == 0;
    }
    CHECK(tag, "embedded image is OpenBIOS");
    char path[512];
    resolve_rom_path(path, sizeof(path), "bios/openbios.bin");
    u32 fsz = 0u;
    u8 *file = tdisc_read_file(path, &fsz);
    if (file) {
        CHECK(fsz == sz && memcmp(file, blob, sz) == 0, "embedded image == tests/roms/bios/openbios.bin");
        free(file);
    }
    CHECK(boot_system(PSX_BIOS_BUILTIN_NAME, false) == PSX_OK, "init builtin");
    CHECK(memcmp(g_sys.bus.bios, blob, sz) == 0 && strcmp(g_sys.cfg.bios_path, "builtin") == 0,
          "psx_load_bios(builtin) maps the embedded image");
    psx_system_shutdown(&g_sys);
    printf("  [ok] built-in OpenBIOS image (524288 B) + psx_load_bios(\"builtin\")\n");
    return 0;
}
#endif

int main(void) {
#if PSX3DS_PLATFORM_3DS
    return 0; /* cross-compiled on T1 for symbol / ABI validation */
#else
    printf("test_sys_cdboot: ISO9660 SYSTEM.CNF -> PS-X EXE CD boot\n");
    if (test_builtin_openbios_image() != 0) {
        return 1;
    }

    char cube_path[512], tri_path[512];
    resolve_rom_path(cube_path, sizeof(cube_path), "benchmarks/Cube.exe");
    resolve_rom_path(tri_path, sizeof(tri_path), "benchmarks/animated-triangle.exe");
    u32 cube_size = 0u, tri_size = 0u;
    u8 *cube = tdisc_read_file(cube_path, &cube_size);
    u8 *tri = tdisc_read_file(tri_path, &tri_size);
    CHECK(cube && tri, "load Cube.exe / animated-triangle.exe test ROMs");
    u32 cube_secs = tdisc_sectors_for(cube_size) - 1u;
    u32 tri_secs = tdisc_sectors_for(tri_size) - 1u;

    int rc = test_system_cnf_parsing(tri, tri_size);

    /* Cube.exe in the root (retail-style SYSTEM.CNF) */
    const char *cube_cnf = "BOOT = cdrom:\\CUBE.EXE;1\r\nTCB = 4\r\nEVENT = 10\r\nSTACK = 801FFFF0\r\n";
    TestDiscOpts cube_o = { "PSX3DS_CUBE", cube_cnf, "CUBE.EXE", NULL, cube, cube_size };
    DiscFiles cube_df;
    CHECK(make_disc("cube", &cube_o, &cube_df), "build Cube disc");

    /* animated-triangle in a sub-directory */
    const char *tri_cnf = "BOOT=cdrom:\\GAME\\TRI.EXE;1\nTCB=4\nEVENT=16\nSTACK=801FFF00\n";
    TestDiscOpts tri_o = { "PSX3DS_TRI", tri_cnf, "TRI.EXE", "GAME", tri, tri_size };
    DiscFiles tri_df;
    CHECK(make_disc("tri", &tri_o, &tri_df), "build triangle disc");

    /* SYSTEM.CNF without TCB/EVENT (OpenBIOS quirk -> automatic sideload) */
    TestDiscOpts bare_o = { "PSX3DS_BARE", "BOOT=cdrom:\\TRI.EXE;1\n", "TRI.EXE", NULL, tri, tri_size };
    DiscFiles bare_df;
    CHECK(make_disc("bare", &bare_o, &bare_df), "build bare-CNF disc");

    if (rc == 0) rc = test_hle_file_io(&tri_df, tri_cnf);
    if (rc == 0) rc = test_hle_boot(&cube_df, "Cube.exe", cube_secs);
    if (rc == 0) rc = test_hle_boot(&tri_df, "GAME\\TRI", tri_secs);
    if (rc == 0) rc = test_openbios_boot(cube_df.chd, ".chd Cube.exe", false, cube_secs, 900u);
    if (rc == 0) rc = test_openbios_boot(tri_df.cue, ".cue GAME\\TRI", false, tri_secs, 600u);
    if (rc == 0) rc = test_openbios_boot(tri_df.iso, ".iso fast boot", true, tri_secs, 600u);
    if (rc == 0) rc = test_openbios_boot(bare_df.chd, ".chd bare CNF", false, tri_secs, 600u);

    remove_disc(&cube_df);
    remove_disc(&tri_df);
    remove_disc(&bare_df);
    free(cube);
    free(tri);
    if (rc == 0) {
        printf("test_sys_cdboot: PASS\n");
    }
    return rc;
#endif
}
