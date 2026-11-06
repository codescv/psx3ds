#include "psx3ds/system.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, (msg));       \
            return 1;                                                               \
        }                                                                           \
    } while (0)

#if !PSX3DS_PLATFORM_3DS
static void resolve_rom_path(char *out, size_t cap, const char *rel_under_roms) {
    const char *env_roms = getenv("PSX3DS_ROMS_DIR");
    if (env_roms && env_roms[0] != '\0') {
        snprintf(out, cap, "%s/%s", env_roms, rel_under_roms);
        return;
    }
    /* Derive <repo>/tests/roms from __FILE__ (.../tests/unit/test_sys_loader.c) */
    const char *f = __FILE__;
    const char *slash = strrchr(f, '/');
    if (slash) {
        size_t dir_len = (size_t)(slash - f);
        char unit_dir[512];
        if (dir_len < sizeof(unit_dir)) {
            memcpy(unit_dir, f, dir_len);
            unit_dir[dir_len] = '\0';
            snprintf(out, cap, "%s/../roms/%s", unit_dir, rel_under_roms);
            return;
        }
    }
    snprintf(out, cap, "tests/roms/%s", rel_under_roms);
}

static u32 g_cb_bytes = 0u;
static char g_cb_capture[512];

static void test_tty_cb(const char *data, u32 len, void *user) {
    int *tag = (int *)user;
    if (tag) {
        (*tag)++;
    }
    for (u32 i = 0u; i < len && g_cb_bytes + 1u < sizeof(g_cb_capture); i++) {
        g_cb_capture[g_cb_bytes++] = data[i];
        g_cb_capture[g_cb_bytes] = '\0';
    }
}

static void put_guest_u32_le(u8 *dst, u32 val) {
    dst[0] = (u8)(val & 0xFFu);
    dst[1] = (u8)((val >> 8) & 0xFFu);
    dst[2] = (u8)((val >> 16) & 0xFFu);
    dst[3] = (u8)((val >> 24) & 0xFFu);
}
#endif

int main(void) {
#if PSX3DS_PLATFORM_3DS
    /* Cross-compiled on T1 to validate symbols & ABI */
    return 0;
#else
    /* 1. Validate PS-X EXE parser against real test ROM headers & malformed inputs */
    const struct {
        const char *rel;
        u32         expect_pc0;
        u32         expect_t_addr;
    } k_exes[] = {
        { "cpu/psxtest_cpu.exe",     0x80010000u, 0x80010000u },
        { "benchmarks/PSXNICCC.exe", 0x80010000u, 0x80010000u },
        { "gte/psxtest_gte.exe",     0x8003F664u, 0x8003F008u }
    };

    for (size_t i = 0u; i < sizeof(k_exes) / sizeof(k_exes[0]); i++) {
        char path[512];
        resolve_rom_path(path, sizeof(path), k_exes[i].rel);
        PlatFile *pf = plat_fopen(path, "rb");
        CHECK(pf != NULL, "open real PS-X EXE");
        u64 fsz = plat_fsize(pf);
        CHECK(fsz >= 0x800u, "PS-X EXE >= 2048 bytes");
        u8 hdr_raw[0x800];
        CHECK(plat_fread(pf, hdr_raw, 0x800u) == 0x800u, "read 2KB PS-X EXE header");
        plat_fclose(pf);

        PsxExeHeader hdr;
        CHECK(psx_exe_parse_header(hdr_raw, (u32)fsz, &hdr) == PSX_OK, "parse valid PS-X EXE");
        CHECK(memcmp(hdr.magic, "PS-X EXE", 8u) == 0, "PS-X EXE magic");
        CHECK(hdr.pc0 == k_exes[i].expect_pc0, "expected pc0");
        CHECK(hdr.t_addr == k_exes[i].expect_t_addr, "expected t_addr");
        CHECK(hdr.t_size > 0u && (hdr.t_size <= fsz - 0x800u), "valid t_size");
    }

    u8 bad_hdr[0x800];
    memset(bad_hdr, 0, sizeof(bad_hdr));
    PsxExeHeader parsed;
    CHECK(psx_exe_parse_header(bad_hdr, 0x400u, &parsed) == PSX_ERR_FORMAT,
          "reject < 2KB PS-X EXE");
    CHECK(psx_exe_parse_header(bad_hdr, 0x800u, &parsed) == PSX_ERR_FORMAT,
          "reject bad magic");
    memcpy(bad_hdr, "PS-X EXE", 8u);
    put_guest_u32_le(&bad_hdr[0x10], 0x80010001u); /* misaligned pc0 */
    put_guest_u32_le(&bad_hdr[0x18], 0x80010000u);
    put_guest_u32_le(&bad_hdr[0x1C], 0x100u);
    CHECK(psx_exe_parse_header(bad_hdr, 0x900u, &parsed) == PSX_ERR_FORMAT,
          "reject misaligned pc0");

    /* 2. OpenBIOS loading + 0x80030000 shell-entry PS-EXE sideload + TTY hooks */
    char bios_path[512];
    resolve_rom_path(bios_path, sizeof(bios_path), "bios/openbios.bin");

    PsxConfig cfg;
    psx_config_defaults(&cfg, true);
    cfg.small_mem = true; /* Fast 26 MiB arena under ASan */
    cfg.hle_bios = false;
    snprintf(cfg.bios_path, sizeof(cfg.bios_path), "%.255s", bios_path);

    PsxSystem *sys = (PsxSystem *)calloc(1u, sizeof(*sys));
    CHECK(sys != NULL, "alloc PsxSystem");
    CHECK(psx_system_init(sys, &cfg) == PSX_OK, "psx_system_init with openbios.bin");
    CHECK(sys->bios.loaded && !sys->bios.hle, "openbios marked loaded");
    CHECK(sys->bus.bios != NULL && sys->bus.ram != NULL, "BIOS and RAM allocated");

    /* Verify non-zero MIPS boot instruction at 0xBFC00000 */
    u32 first_word = 0u;
    memcpy(&first_word, sys->bus.bios, sizeof(u32));
    CHECK(first_word != 0u, "openbios reset vector instruction non-zero");

    /* Queue psxtest_cpu.exe for 0x80030000 sideloading */
    char cpu_exe_path[512];
    resolve_rom_path(cpu_exe_path, sizeof(cpu_exe_path), "cpu/psxtest_cpu.exe");
    CHECK(psx_load_exe(sys, cpu_exe_path) == PSX_OK, "queue psxtest_cpu.exe");
    CHECK(sys->bios.psexe_pending && sys->cpu.psexe_pending == 1u, "psexe_pending armed");

    /* Trigger shell-entry hook at 0x80030000 */
    CHECK(bios_pc_hook(sys, PSX_SHELL_PC), "bios_pc_hook sideloads at 0x80030000");
    CHECK(!sys->bios.psexe_pending && sys->cpu.psexe_pending == 0u, "psexe_pending cleared");
    CHECK(sys->cpu.pc == 0x80010000u && sys->cpu.next_pc == 0x80010004u, "injected pc0");
    /* psxtest_cpu.exe has s_addr=0x801FFFF0, s_size=0 -> sp = 0x801FFFF0 */
    CHECK(sys->cpu.gpr[29] == 0x801FFFF0u && sys->cpu.gpr[30] == 0x801FFFF0u, "injected sp/fp");

    /* Verify first 64 bytes of RAM @ 0x00010000 match .exe offset 0x800 */
    PlatFile *ef = plat_fopen(cpu_exe_path, "rb");
    CHECK(ef && plat_fseek(ef, 0x800u), "seek exe payload");
    u8 expect_payload[64];
    CHECK(plat_fread(ef, expect_payload, sizeof(expect_payload)) == sizeof(expect_payload),
          "read exe payload");
    plat_fclose(ef);
    CHECK(memcmp(sys->bus.ram + 0x10000u, expect_payload, sizeof(expect_payload)) == 0,
          "PS-EXE payload copied into guest RAM");

    /* Test BIOS TTY capture (putchar, puts, write) + --stop-on-tty instant trigger */
    int cb_invocations = 0;
    g_cb_bytes = 0u;
    bios_tty_clear(sys);
    bios_set_tty_callback(sys, test_tty_cb, &cb_invocations);
    bios_set_stop_pattern(sys, "Tests Done!");

    /* A0:0x3C putchar('H'), B0:0x3D putchar('i'), putchar('\n') */
    sys->cpu.gpr[29] = 0x801FFF00u;
    sys->cpu.gpr[9] = 0x3Cu;
    sys->cpu.gpr[4] = (u32)'H';
    CHECK(!bios_pc_hook(sys, 0xA0u), "real BIOS TTY hook is non-consuming");
    sys->cpu.gpr[9] = 0x3Du;
    sys->cpu.gpr[4] = (u32)'i';
    (void)bios_pc_hook(sys, 0xB0u);
    sys->cpu.gpr[4] = (u32)'\n';
    (void)bios_pc_hook(sys, 0xB0u);

    /* A0:0x3E puts("All Tests Done!") in guest RAM */
    const char *msg = "All Tests Done!";
    memcpy(sys->bus.ram + 0x2000u, msg, strlen(msg) + 1u);
    sys->cpu.gpr[9] = 0x3Eu;
    sys->cpu.gpr[4] = 0x80002000u;
    (void)bios_pc_hook(sys, 0xA0u);

    CHECK(sys->bios.until_matched && sys->quit, "stop-on-tty matched substring immediately");
    CHECK(strstr(sys->bios.tty_buf, "Hi\nAll Tests Done!\n") != NULL, "TTY transcript matches");
    CHECK(cb_invocations > 0 && strcmp(g_cb_capture, sys->bios.tty_buf) == 0,
          "streaming TTY callback matches transcript");

    psx_system_shutdown(sys);

    /* 3. Standalone BIOS-less HLE mode: bootstrap, events, CriticalSection, printf */
    psx_config_defaults(&cfg, true);
    cfg.small_mem = true;
    cfg.hle_bios = true;
    CHECK(psx_system_init(sys, &cfg) == PSX_OK, "psx_system_init in HLE mode");
    CHECK(sys->bios.hle && !sys->bios.loaded, "HLE mode active");

    /* EnterCriticalSection (SYSCALL a0=1 -> Simulate at vector or direct COP0/syscall helper):
       Test HLE A0/B0/C0 vectors directly */
    sys->cpu.gpr[31] = 0x80012340u; /* $ra */

    /* B0:0x08 OpenEvent(class=0xF0000002, spec=2, mode=0x2000, func=0) */
    sys->cpu.gpr[9] = 0x08u;
    sys->cpu.gpr[4] = 0xF0000002u;
    sys->cpu.gpr[5] = 0x0002u;
    sys->cpu.gpr[6] = 0x2000u; /* NO_CALLBACK polling mode */
    sys->cpu.gpr[7] = 0u;
    CHECK(bios_pc_hook(sys, 0xB0u), "HLE OpenEvent handled");
    u32 ev_handle = sys->cpu.gpr[2];
    CHECK((ev_handle & 0xFFFFFF00u) == 0xF1000000u, "valid HLE event handle");

    /* B0:0x0C EnableEvent(ev_handle) */
    sys->cpu.gpr[9] = 0x0Cu;
    sys->cpu.gpr[4] = ev_handle;
    CHECK(bios_pc_hook(sys, 0xB0u) && sys->cpu.gpr[2] == 1u, "HLE EnableEvent");

    /* B0:0x0B TestEvent(ev_handle) -> initially 0 */
    sys->cpu.gpr[9] = 0x0Bu;
    sys->cpu.gpr[4] = ev_handle;
    CHECK(bios_pc_hook(sys, 0xB0u) && sys->cpu.gpr[2] == 0u, "HLE TestEvent initially 0");

    /* B0:0x07 DeliverEvent(class=0xF0000002, spec=2) -> marks delivered */
    sys->cpu.gpr[9] = 0x07u;
    sys->cpu.gpr[4] = 0xF0000002u;
    sys->cpu.gpr[5] = 0x0002u;
    CHECK(bios_pc_hook(sys, 0xB0u), "HLE DeliverEvent");

    /* B0:0x0B TestEvent(ev_handle) -> now 1, auto-clears to 0 */
    sys->cpu.gpr[9] = 0x0Bu;
    sys->cpu.gpr[4] = ev_handle;
    CHECK(bios_pc_hook(sys, 0xB0u) && sys->cpu.gpr[2] == 1u, "HLE TestEvent returns 1");
    sys->cpu.gpr[9] = 0x0Bu;
    sys->cpu.gpr[4] = ev_handle;
    CHECK(bios_pc_hook(sys, 0xB0u) && sys->cpu.gpr[2] == 0u, "HLE TestEvent auto-clears");

    /* A0:0x2B memset(0x80003000, 0x5A, 16) + A0:0x2A memcpy(0x80003100, 0x80003000, 16) */
    sys->cpu.gpr[9] = 0x2Bu;
    sys->cpu.gpr[4] = 0x80003000u;
    sys->cpu.gpr[5] = 0x5Au;
    sys->cpu.gpr[6] = 16u;
    CHECK(bios_pc_hook(sys, 0xA0u), "HLE memset");
    sys->cpu.gpr[9] = 0x2Au;
    sys->cpu.gpr[4] = 0x80003100u;
    sys->cpu.gpr[5] = 0x80003000u;
    sys->cpu.gpr[6] = 16u;
    CHECK(bios_pc_hook(sys, 0xA0u), "HLE memcpy");
    for (u32 i = 0u; i < 16u; i++) {
        CHECK(sys->bus.ram[0x3100u + i] == 0x5Au, "HLE memset+memcpy byte exact");
    }

    /* A0:0x3F printf("Score=%d Hex=%04X %s\n", -42, 0xBEEF, "OK") */
    bios_tty_clear(sys);
    const char *fmt = "Score=%d Hex=%04X %s\n";
    const char *ok_str = "OK";
    memcpy(sys->bus.ram + 0x4000u, fmt, strlen(fmt) + 1u);
    memcpy(sys->bus.ram + 0x4100u, ok_str, strlen(ok_str) + 1u);
    sys->cpu.gpr[9] = 0x3Fu;
    sys->cpu.gpr[4] = 0x80004000u;
    sys->cpu.gpr[5] = (u32)(-42);
    sys->cpu.gpr[6] = 0xBEEFu;
    sys->cpu.gpr[7] = 0x80004100u;
    CHECK(bios_pc_hook(sys, 0xA0u), "HLE printf");
    CHECK(strcmp(sys->bios.tty_buf, "Score=-42 Hex=BEEF OK\n") == 0,
          "HLE printf formatted string exact");

    psx_system_shutdown(sys);
    free(sys);
    printf("test_sys_loader PASS\n");
    return 0;
#endif
}
