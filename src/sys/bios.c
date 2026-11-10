#include "psx3ds/system.h"

#include <strings.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Embedded 512 KiB OpenBIOS (src/sys/openbios_blob.S) */
extern const u8  psx_openbios_blob[];
extern const u32 psx_openbios_blob_size;

const u8 *psx_builtin_openbios(u32 *out_size) {
    if (out_size) {
        *out_size = psx_openbios_blob_size;
    }
    return psx_openbios_blob;
}

static bool bios_name_is_candidate(const char *name) {
    char low[64];
    size_t n = strlen(name);
    if (n < 5u || n >= sizeof(low)) {
        return false;
    }
    for (size_t i = 0; i <= n; i++) {
        low[i] = (char)tolower((unsigned char)name[i]);
    }
    const char *ext = strrchr(low, '.');
    if (!ext || (strcmp(ext, ".bin") != 0 && strcmp(ext, ".rom") != 0)) {
        return false;
    }
    return strncmp(low, "scph", 4) == 0 || strncmp(low, "psxonpsp", 8) == 0 ||
           strncmp(low, "bios", 4) == 0 || strncmp(low, "ps1_rom", 7) == 0 ||
           strcmp(low, "openbios.bin") == 0;
}

/* Prefer Sony retail images (best compatibility), then other dumps, then OpenBIOS */
static int bios_name_rank(const char *name) {
    if (strncasecmp(name, "scph", 4) == 0) {
        return 0;
    }
    if (strncasecmp(name, "openbios", 8) == 0) {
        return 2;
    }
    return 1;
}

bool psx_bios_find_external(char *path_out, u32 path_cap, char *badge_out, u32 badge_cap) {
    static const char *const k_dirs[] = {
#if PSX3DS_PLATFORM_3DS
        "sdmc:/psx3ds/bios", "sdmc:/psx", "sdmc:/roms/psx", "sdmc:/bios", "sdmc:/",
#else
        ".", "bios",
#endif
    };
    char (*names)[256] = (char (*)[256])malloc(256u * 256u);
    if (!names) {
        return false;
    }
    int best_rank = 99;
    char best_path[256] = { 0 };
    char best_name[64] = { 0 };
    for (size_t d = 0; d < sizeof(k_dirs) / sizeof(k_dirs[0]) && best_rank > 0; d++) {
        u32 n = plat_listdir(k_dirs[d], names, 256u);
        for (u32 i = 0; i < n; i++) {
            if (!bios_name_is_candidate(names[i])) {
                continue;
            }
            int rank = bios_name_rank(names[i]);
            if (rank >= best_rank) {
                continue;
            }
            char full[256];
            size_t dl = strlen(k_dirs[d]);
            snprintf(full, sizeof(full), "%s%s%.200s", k_dirs[d],
                     (dl > 0u && k_dirs[d][dl - 1u] == '/') ? "" : "/", names[i]);
            PlatFile *f = plat_fopen(full, "rb");
            if (!f) {
                continue;
            }
            u64 fsz = plat_fsize(f);
            plat_fclose(f);
            if (fsz != (u64)PSX_BIOS_SIZE) {
                continue;
            }
            best_rank = rank;
            snprintf(best_path, sizeof(best_path), "%s", full);
            snprintf(best_name, sizeof(best_name), "%.63s", names[i]);
        }
    }
    free(names);
    if (best_rank == 99) {
        return false;
    }
    if (path_out && path_cap > 0u) {
        snprintf(path_out, path_cap, "%s", best_path);
    }
    if (badge_out && badge_cap > 0u) {
        u32 k = 0u;
        for (; best_name[k] != '\0' && best_name[k] != '.' && k + 1u < badge_cap && k < 10u; k++) {
            badge_out[k] = (char)toupper((unsigned char)best_name[k]);
        }
        badge_out[k] = '\0';
    }
    return true;
}

#define PSX_TTY_BUF_CAP (64u * 1024u)

static u8 bios_read_guest_u8(const PsxSystem *sys, u32 vaddr) {
    u32 phys = vaddr & 0x1FFFFFFFu;
    if (phys < PSX_RAM_MIRROR_END && sys->bus.ram) {
        return sys->bus.ram[phys & PSX_RAM_MASK];
    }
    if (phys >= PSX_SCRATCH_PHYS && phys < (PSX_SCRATCH_PHYS + PSX_SCRATCH_SIZE) &&
        vaddr < 0xA0000000u && sys->bus.scratch_page) {
        return sys->bus.scratch_page[phys & PSX_SCRATCH_MASK];
    }
    if (phys >= PSX_BIOS_PHYS && phys < (PSX_BIOS_PHYS + PSX_BIOS_SIZE) && sys->bus.bios) {
        return sys->bus.bios[phys & PSX_BIOS_MASK];
    }
    return 0u;
}

void bios_init(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    PsxBios *b = &sys->bios;
    memset(b, 0, sizeof(*b));
    if (sys->arena.base) {
        b->tty_buf = (char *)arena_alloc(&sys->arena, PSX_TTY_BUF_CAP, 32u);
        b->tty_buf_cap = PSX_TTY_BUF_CAP;
        b->bios_rom_backup = (u8 *)arena_alloc(&sys->arena, PSX_BIOS_SIZE, 32u);
    }
}

void bios_tty_clear(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    PsxBios *b = &sys->bios;
    b->tty_len = 0u;
    b->tty_line[0] = '\0';
    b->tty_buf_len = 0u;
    if (b->tty_buf && b->tty_buf_cap > 0u) {
        b->tty_buf[0] = '\0';
    }
    b->until_matched = false;
    b->tty_outer_sp = 0u;
}

void bios_set_tty_callback(PsxSystem *sys, PsxTtyCallback cb, void *user) {
    if (!sys) {
        return;
    }
    sys->bios.tty_cb = cb;
    sys->bios.tty_cb_user = user;
}

void bios_set_stop_pattern(PsxSystem *sys, const char *substr) {
    if (!sys) {
        return;
    }
    PsxBios *b = &sys->bios;
    b->until_matched = false;
    if (!substr || substr[0] == '\0') {
        b->until_pattern[0] = '\0';
        b->until_pattern_len = 0u;
        return;
    }
    size_t len = strlen(substr);
    if (len >= sizeof(b->until_pattern)) {
        len = sizeof(b->until_pattern) - 1u;
    }
    memcpy(b->until_pattern, substr, len);
    b->until_pattern[len] = '\0';
    b->until_pattern_len = (u32)len;
}

void bios_tty_putc(PsxSystem *sys, char c) {
    if (!sys || c == '\0' || c == '\r') {
        return;
    }
    PsxBios *b = &sys->bios;

    /* 1. Streaming callback (e.g. --tty-out - live stdout) */
    if (b->tty_cb) {
        b->tty_cb(&c, 1u, b->tty_cb_user);
    }

    /* 2. Full transcript buffer (64 KiB, drops oldest half if ever full) */
    if (b->tty_buf && b->tty_buf_cap > 1u) {
        if (b->tty_buf_len + 1u >= b->tty_buf_cap) {
            u32 keep = b->tty_buf_cap / 2u;
            memmove(b->tty_buf, b->tty_buf + (b->tty_buf_len - keep), keep);
            b->tty_buf_len = keep;
        }
        b->tty_buf[b->tty_buf_len++] = c;
        b->tty_buf[b->tty_buf_len] = '\0';
    }

    /* 3. Rolling current line buffer */
    if (c == '\n') {
        b->tty_line[b->tty_len < sizeof(b->tty_line) ? b->tty_len : (sizeof(b->tty_line) - 1u)] = '\0';
        b->tty_len = 0u;
    } else {
        if (b->tty_len + 1u < sizeof(b->tty_line)) {
            b->tty_line[b->tty_len++] = c;
            b->tty_line[b->tty_len] = '\0';
        } else {
            memmove(b->tty_line, b->tty_line + 1, sizeof(b->tty_line) - 2u);
            b->tty_line[sizeof(b->tty_line) - 2u] = c;
            b->tty_line[sizeof(b->tty_line) - 1u] = '\0';
        }
    }

    /* 4. O(1)-amortized instant substring stop check (--stop-on-tty / --until-printf) */
    if (!b->until_matched && b->until_pattern_len > 0u) {
        u32 plen = b->until_pattern_len;
        if (b->tty_buf && b->tty_buf_len >= plen) {
            if (b->tty_buf[b->tty_buf_len - 1u] == b->until_pattern[plen - 1u] &&
                memcmp(b->tty_buf + (b->tty_buf_len - plen), b->until_pattern, plen) == 0) {
                b->until_matched = true;
                sys->quit = true;
                cpu_request_exit(sys);
            }
        } else if (strstr(b->tty_line, b->until_pattern) != NULL) {
            b->until_matched = true;
            sys->quit = true;
            cpu_request_exit(sys);
        }
    }
}

void bios_reset(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    PsxBios *b = &sys->bios;
    b->tty_len = 0u;
    b->tty_line[0] = '\0';
    b->tty_outer_sp = 0u;
    b->until_matched = false;
    b->hle_heap_ptr = 0x80180000u;
    memset(b->hle_events, 0, sizeof(b->hle_events));
    b->hle_hook_entry_int = 0u;
    b->hle_in_intr_hook = false;
    b->hle_pad_auto_ack = 0u;
    b->hle_rcnt_auto_ack = 0u;
    b->hle_pad_buf1_addr = 0u;
    b->hle_pad_buf2_addr = 0u;
    b->hle_pad_dest_addr = 0u;

    if (b->loaded && b->bios_rom_backup && sys->bus.bios) {
        memcpy(sys->bus.bios, b->bios_rom_backup, PSX_BIOS_SIZE);
    } else if (b->hle && sys->bus.bios) {
        hle_bios_install_rom_stub(sys);
        hle_bios_install_ram_vectors(sys);
    }

    memset(b->hle_files, 0, sizeof(b->hle_files));
    b->cdboot_pending = false;
    b->cdboot_skip_shell = false;
    b->cdboot_watch = false;
    if (b->exe_path[0] != '\0') {
        if (b->hle) {
            (void)psx_exe_inject_now(sys);
        } else {
            b->psexe_pending = true;
            sys->cpu.psexe_pending = 1u;
        }
    } else {
        b->psexe_pending = false;
        sys->cpu.psexe_pending = 0u;
        if (b->cd_boot && b->cdboot.valid) {
            if (b->hle) {
                (void)psx_cdboot_inject(sys, &b->cdboot);
            } else {
                psx_cdboot_arm_bios(sys);
            }
        }
    }
}

void bios_state(PsxSystem *sys, StateIO *st) {
    if (!sys || !st) {
        return;
    }
    PsxBios *b = &sys->bios;
    state_bool(st, &b->loaded);
    state_bool(st, &b->hle);
    state_u32(st, &b->exe_pc);
    state_u32(st, &b->exe_gp);
    state_u32(st, &b->exe_sp);
    state_bytes(st, b->exe_path, (u32)sizeof(b->exe_path));
    state_bool(st, &b->psexe_pending);
    state_u32(st, &b->hle_heap_ptr);
    state_bytes(st, b->hle_events, (u32)sizeof(b->hle_events));
    state_u32(st, &b->hle_hook_entry_int);
    state_bytes(st, b->hle_saved_gpr, (u32)sizeof(b->hle_saved_gpr));
    state_u32(st, &b->hle_saved_hi);
    state_u32(st, &b->hle_saved_lo);
    state_u32(st, &b->hle_saved_epc);
    state_u32(st, &b->hle_saved_sr);
    state_bool(st, &b->hle_in_intr_hook);
    state_u16(st, &b->hle_pad_auto_ack);
    state_u16(st, &b->hle_rcnt_auto_ack);
    state_u32(st, &b->hle_pad_buf1_addr);
    state_u32(st, &b->hle_pad_buf2_addr);
    state_u32(st, &b->tty_outer_sp);
    if (!st->writing) {
        sys->cpu.psexe_pending = b->psexe_pending ? 1u : 0u;
    }
}

void bios_shutdown(PsxSystem *sys) {
    (void)sys;
}

static PlatFile *bios_open_candidate(const char *path, char *resolved_out, size_t resolved_cap) {
    if (!path || path[0] == '\0') {
        return NULL;
    }
    PlatFile *f = plat_fopen(path, "rb");
    if (f) {
        if (resolved_out && resolved_cap > 0u) {
            size_t n = strlen(path);
            if (n >= resolved_cap) {
                n = resolved_cap - 1u;
            }
            memcpy(resolved_out, path, n);
            resolved_out[n] = '\0';
        }
        return f;
    }
    return NULL;
}

PsxResult psx_load_bios(PsxSystem *sys, const char *path) {
    if (!sys || !sys->bus.bios) {
        return PSX_ERR_BAD_ARG;
    }
    PsxBios *b = &sys->bios;

    /* NULL / empty / "hle" activates standalone BIOS-less HLE mode */
    if (!path || path[0] == '\0' || strcmp(path, "hle") == 0 || strcmp(path, "HLE") == 0) {
        b->loaded = false;
        b->hle = true;
        sys->cfg.hle_bios = true;
        hle_bios_install_rom_stub(sys);
        hle_bios_install_ram_vectors(sys);
        if (sys->bus.fm_r && sys->bus.fm_w) {
            fastmem_build(&sys->bus);
        }
        jit_flush_all(sys);
        if (b->psexe_pending || b->exe_path[0] != '\0') {
            return psx_exe_inject_now(sys);
        }
        return PSX_OK;
    }

    PlatFile *f = NULL;
    char resolved[256] = { 0 };
    bool use_builtin = (strcmp(path, PSX_BIOS_BUILTIN_NAME) == 0 ||
                        strcmp(path, "openbios") == 0 || strcmp(path, "OPENBIOS") == 0);

    if (!use_builtin && strcmp(path, "auto") == 0) {
#if PSX3DS_PLATFORM_HOST
        const char *env_bios = getenv("PSX3DS_OPENBIOS");
        if (env_bios && env_bios[0] != '\0') {
            f = bios_open_candidate(env_bios, resolved, sizeof(resolved));
        }
#endif
        if (!f) {
            char found[256];
            if (psx_bios_find_external(found, sizeof(found), NULL, 0u)) {
                f = bios_open_candidate(found, resolved, sizeof(resolved));
            }
        }
        if (!f) {
            /* No external 512 KiB BIOS on SD/disk: the embedded OpenBIOS is always there */
            use_builtin = true;
        }
    } else if (!use_builtin) {
        f = bios_open_candidate(path, resolved, sizeof(resolved));
        if (!f) {
            plat_log("[bios] cannot open '%s'\n", path);
            return PSX_ERR_IO;
        }
    }

    if (use_builtin) {
        u32 bsz = 0u;
        const u8 *blob = psx_builtin_openbios(&bsz);
        if (!blob || bsz != PSX_BIOS_SIZE) {
            plat_log("[bios] built-in OpenBIOS missing (size=%u)\n", (unsigned)bsz);
            return PSX_ERR_FORMAT;
        }
        memcpy(sys->bus.bios, blob, PSX_BIOS_SIZE);
        snprintf(resolved, sizeof(resolved), "%s", PSX_BIOS_BUILTIN_NAME);
        plat_log("[bios] Built-in OPENBIOS (512 KiB embedded)\n");
    } else {
        u64 sz = plat_fsize(f);
        if (sz != (u64)PSX_BIOS_SIZE) {
            plat_fclose(f);
            plat_log("[bios] '%s' is %llu bytes (need 524288)\n", resolved,
                     (unsigned long long)sz);
            return PSX_ERR_FORMAT;
        }
        u32 got = plat_fread(f, sys->bus.bios, PSX_BIOS_SIZE);
        plat_fclose(f);
        if (got != PSX_BIOS_SIZE) {
            plat_log("[bios] short read on '%s'\n", resolved);
            return PSX_ERR_IO;
        }
        plat_log("[bios] external BIOS %s\n", resolved);
    }

    if (b->bios_rom_backup) {
        memcpy(b->bios_rom_backup, sys->bus.bios, PSX_BIOS_SIZE);
    }
    if (resolved[0] != '\0') {
        size_t n = strlen(resolved);
        if (n >= sizeof(sys->cfg.bios_path)) {
            n = sizeof(sys->cfg.bios_path) - 1u;
        }
        memcpy(sys->cfg.bios_path, resolved, n);
        sys->cfg.bios_path[n] = '\0';
    }

    b->loaded = true;
    b->hle = false;
    sys->cfg.hle_bios = false;
    if (sys->bus.fm_r && sys->bus.fm_w) {
        fastmem_build(&sys->bus);
    }
    jit_flush_all(sys);
    return PSX_OK;
}

bool bios_pc_hook(PsxSystem *sys, u32 pc) {
    if (!sys) {
        return false;
    }
    u32 phys = pc & 0x1FFFFFFFu;

    /* 1. Shell entry hook (0x80030000): sideload pending PS-X EXE / CD boot EXE */
    if (phys == 0x00030000u && (sys->bios.psexe_pending || sys->bios.cdboot_pending ||
                                sys->bios.cdboot_skip_shell ||
                                sys->cpu.psexe_pending != 0u)) {
        if (sys->bios.exe_path[0] != '\0') {
            return psx_exe_inject_now(sys) == PSX_OK;
        }
        if (sys->bios.cdboot_pending && sys->bios.cdboot.valid) {
            plat_log("[cdboot] BIOS reached shell entry 0x80030000: sideloading %s\n",
                     sys->bios.cdboot.boot_path);
            if (psx_cdboot_inject(sys, &sys->bios.cdboot) == PSX_OK) {
                return true;
            }
        }
        if (sys->bios.cdboot_skip_shell) {
            /* Native CD boot: skip the BIOS shell / boot animation by returning to the
             * kernel immediately (like a real shell that found a licensed disc). The BIOS
             * then reads SYSTEM.CNF through the CD-ROM controller and boots the EXE. */
            sys->bios.cdboot_skip_shell = false;
            sys->bios.cdboot_watch = true;
            sys->bios.cdboot_watch_frames = 0u;
            sys->bios.psexe_pending = false;
            sys->bios.cdboot_pending = false;
            sys->cpu.psexe_pending = 0u;
            plat_log("[cdboot] BIOS shell skipped at 0x80030000: native CD boot of %s\n",
                     sys->bios.cdboot.boot_path);
            PsxCpu *c = &sys->cpu;
            c->gpr[2] = 0u;
            c->in_delay_slot = 0u;
            c->branch_taken = 0u;
            c->pc = c->gpr[31];
            c->next_pc = c->pc + 4u;
            jit_flush_all(sys);
            cpu_request_exit(sys);
            return true;
        }
        sys->bios.psexe_pending = false;
        sys->bios.cdboot_pending = false;
        sys->cpu.psexe_pending = 0u;
        return false;
    }

    if (phys != 0xA0u && phys != 0xB0u && phys != 0xC0u) {
        return false;
    }

    /* 2. Standalone BIOS-less HLE mode handles A0/B0/C0 directly */
    if (sys->bios.hle) {
        return hle_bios_dispatch(sys, phys);
    }

    /* 3. Real BIOS / OpenBIOS mode: non-intrusive TTY stdout capture */
    PsxBios *b = &sys->bios;
    u32 fn = sys->cpu.gpr[9] & 0xFFu;
    u32 sp = sys->cpu.gpr[29];

    if (b->tty_outer_sp != 0u && sp >= b->tty_outer_sp) {
        b->tty_outer_sp = 0u;
    }

    if ((phys == 0xA0u && fn == 0x3Cu) || (phys == 0xB0u && fn == 0x3Du)) {
        /* putchar($a0) */
        if (b->tty_outer_sp == 0u) {
            bios_tty_putc(sys, (char)(sys->cpu.gpr[4] & 0xFFu));
        }
    } else if ((phys == 0xA0u && fn == 0x3Eu) || (phys == 0xB0u && fn == 0x3Fu)) {
        /* puts($a0) */
        if (b->tty_outer_sp == 0u) {
            u32 addr = sys->cpu.gpr[4];
            for (u32 i = 0; i < 4096u; i++) {
                u8 ch = bios_read_guest_u8(sys, addr + i);
                if (ch == 0u) {
                    break;
                }
                bios_tty_putc(sys, (char)ch);
            }
            bios_tty_putc(sys, '\n');
            b->tty_outer_sp = sp;
        }
    } else if (phys == 0xB0u && fn == 0x35u) {
        /* write(fd, buf, n) */
        u32 fd = sys->cpu.gpr[4];
        u32 buf_addr = sys->cpu.gpr[5];
        u32 count = sys->cpu.gpr[6];
        if ((fd == 1u || fd == 2u) && b->tty_outer_sp == 0u) {
            u32 limit = count > 65536u ? 65536u : count;
            for (u32 i = 0; i < limit; i++) {
                u8 ch = bios_read_guest_u8(sys, buf_addr + i);
                if (ch == 0u) {
                    continue;
                }
                bios_tty_putc(sys, (char)ch);
            }
            b->tty_outer_sp = sp;
        }
    }

    return false;
}
