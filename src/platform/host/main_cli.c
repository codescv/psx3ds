#include "psx3ds/system.h"
#include "plat_host_internal.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char *prog) {
    fprintf(stderr,
            "psx3ds v" PSX3DS_VERSION_STR " — PlayStation 1 Emulator\n"
            "Usage: %s [options]\n"
            "  --version, -V             Print psx3ds version and exit\n"
            "  --bios <path>             Path to 512KB PS1 BIOS or openbios.bin (default: auto)\n"
            "  --hle                     Use built-in BIOS-less HLE bootstrap & kernel\n"
            "  --exe <path>              Load and execute PS-X EXE binary\n"
            "  --cd <path>               Mount CD-ROM image (.cue/.bin/.iso/.img)\n"
            "  --cpu <mode>              CPU engine: interp | cached | jit | auto (alias: --engine)\n"
            "  --gpu <mode>              GPU backend: soft | hw\n"
            "  --frames <N>              Run at most N emulated frames (default: 600)\n"
            "  --seconds <S>             Run at most S*60 emulated frames\n"
            "  --max-cycles <N>          Stop after N emulated CPU cycles\n"
            "  --tty-out <path|->        Stream BIOS/HLE TTY stdout to file or '-' for stdout\n"
            "  --stop-on-tty <substr>    Stop immediately once TTY output contains <substr> (alias: --until-printf)\n"
            "  --dump-ppm <path[:every]> Write final (and optional periodic) 24-bit PPM frame (alias: --dump-frame)\n"
            "  --dump-wav <path>         Write captured 44.1kHz stereo 16-bit WAV audio (alias: --dump-audio)\n"
            "  --savestate-test          Verify StateIO roundtrip + CRC32 tamper rejection mid-run\n"
            "  --trace <N>               Print up to N CPU state trace steps to stderr\n"
            "  --trace-hash <path|->     Write 64-bit architectural state hashes for trace_diff.py\n"
            "  --hash-every <cycles>     Cycle interval for --trace-hash (default: 10000)\n"
            "  --bench                   Run multi-engine benchmark suite (default for psx3ds-bench)\n"
            "  --bench-json <path|->     Write JSON telemetry / benchmark report (alias: --stats)\n"
            "  --pad-press <f[-end]:msk> Press pad buttons on frame f..end (hex mask or START+CROSS...)\n"
            "  --input <script.txt>      Load frame:mask input schedule file\n"
            "  --cycle-mult <N>          Override cycle multiplier (256 = 1.0x)\n"
            "  --small-mem               Test Old-3DS 26 MiB memory arena budget\n"
            "  --no-ibtc | --no-link | --no-idle-skip | --no-align-check | --no-hle-exec\n",
            prog);
}

static u64 fnv1a_u32(u64 h, u32 v) {
    for (int i = 0; i < 4; i++) {
        h ^= (u64)((v >> (i * 8)) & 0xFFu);
        h *= 1099511628211ULL;
    }
    return h;
}

static u64 hash_cpu_arch_state(const PsxCpu *c) {
    u64 h = 14695981039346656037ULL;
    for (int i = 0; i < 32; i++) {
        h = fnv1a_u32(h, c->gpr[i]);
    }
    h = fnv1a_u32(h, c->hi);
    h = fnv1a_u32(h, c->lo);
    h = fnv1a_u32(h, c->pc);
    h = fnv1a_u32(h, c->cop0[COP0_SR]);
    h = fnv1a_u32(h, c->cop0[COP0_CAUSE]);
    h = fnv1a_u32(h, c->cop0[COP0_EPC]);
    return h;
}

static bool parse_button_token(const char *tok, size_t len, u16 *out_bit) {
    static const struct {
        const char *name;
        u16         bit;
    } k_btns[] = {
        { "SELECT",   0x0001u }, { "L3",       0x0002u },
        { "R3",       0x0004u }, { "START",    0x0008u },
        { "UP",       0x0010u }, { "RIGHT",    0x0020u },
        { "DOWN",     0x0040u }, { "LEFT",     0x0080u },
        { "L2",       0x0100u }, { "R2",       0x0200u },
        { "L1",       0x0400u }, { "R1",       0x0800u },
        { "TRIANGLE", 0x1000u }, { "CIRCLE",   0x2000u },
        { "CROSS",    0x4000u }, { "SQUARE",   0x8000u },
        { "A",        0x2000u }, { "B",        0x4000u },
        { "X",        0x1000u }, { "Y",        0x8000u }
    };
    char upper[32];
    if (len == 0u || len >= sizeof(upper)) {
        return false;
    }
    for (size_t i = 0u; i < len; i++) {
        upper[i] = (char)toupper((unsigned char)tok[i]);
    }
    upper[len] = '\0';

    for (size_t i = 0u; i < sizeof(k_btns) / sizeof(k_btns[0]); i++) {
        if (strcmp(upper, k_btns[i].name) == 0) {
            *out_bit = k_btns[i].bit;
            return true;
        }
    }
    char *endp = NULL;
    unsigned long val = strtoul(upper, &endp, 0);
    if (endp && *endp == '\0' && val <= 0xFFFFUL) {
        *out_bit = (u16)val;
        return true;
    }
    return false;
}

static bool parse_pad_spec(const char *spec) {
    if (!spec) {
        return false;
    }
    while (*spec && isspace((unsigned char)*spec)) {
        spec++;
    }
    if (*spec == '\0' || *spec == '#') {
        return true;
    }
    const char *colon = strchr(spec, ':');
    if (!colon) {
        return false;
    }
    char range_buf[64];
    size_t rlen = (size_t)(colon - spec);
    if (rlen == 0u || rlen >= sizeof(range_buf)) {
        return false;
    }
    memcpy(range_buf, spec, rlen);
    range_buf[rlen] = '\0';

    u32 start_f = 0u;
    u32 end_f = 0u;
    char *dash = strchr(range_buf, '-');
    char *endp = NULL;
    if (dash) {
        *dash = '\0';
        start_f = (u32)strtoul(range_buf, &endp, 0);
        if (!endp || *endp != '\0') {
            return false;
        }
        end_f = (u32)strtoul(dash + 1, &endp, 0);
        if (!endp || *endp != '\0') {
            return false;
        }
    } else {
        start_f = (u32)strtoul(range_buf, &endp, 0);
        if (!endp || *endp != '\0') {
            return false;
        }
        end_f = start_f;
    }

    const char *p = colon + 1;
    u16 mask = 0u;
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == '+' || *p == '|' || *p == ',')) {
            p++;
        }
        if (*p == '\0' || *p == '#') {
            break;
        }
        const char *q = p;
        while (*q && !isspace((unsigned char)*q) && *q != '+' && *q != '|' && *q != ',' && *q != '#') {
            q++;
        }
        u16 bit = 0u;
        if (!parse_button_token(p, (size_t)(q - p), &bit)) {
            return false;
        }
        mask = (u16)(mask | bit);
        p = q;
        if (*p == '#') {
            break;
        }
    }
    return plat_host_input_add_event(start_f, end_f, mask);
}

static bool load_input_script(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return false;
    }
    char line[256];
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (!parse_pad_spec(line)) {
            fclose(fp);
            return false;
        }
    }
    fclose(fp);
    return true;
}

typedef struct CliTtySink {
    FILE *fp;
    bool  close_on_exit;
} CliTtySink;

static void cli_tty_callback(const char *data, u32 len, void *user) {
    CliTtySink *sink = (CliTtySink *)user;
    if (sink && sink->fp && data && len > 0u) {
        (void)fwrite(data, 1u, (size_t)len, sink->fp);
        fflush(sink->fp);
    }
}

static bool parse_cpu_engine(const char *s, PsxCpuEngine *out) {
    if (strcmp(s, "interp") == 0) {
        *out = CPU_INTERP;
        return true;
    }
    if (strcmp(s, "cached") == 0) {
        *out = CPU_CACHED;
        return true;
    }
    if (strcmp(s, "jit") == 0) {
        *out = CPU_JIT;
        return true;
    }
    if (strcmp(s, "auto") == 0) {
        *out = CPU_AUTO;
        return true;
    }
    return false;
}

static bool tty_contains_failure(const char *tty) {
    if (!tty) {
        return false;
    }
    return (strstr(tty, "FAIL") != NULL) ||
           (strstr(tty, "Fail") != NULL) ||
           (strstr(tty, "ERROR:") != NULL) ||
           (strstr(tty, "ASSERT") != NULL);
}

static PsxResult run_savestate_self_test(PsxSystem *sys) {
    StateIO measure = {
        .writing = true,
        .buf = NULL,
        .pos = 0u,
        .cap = UINT32_MAX,
        .error = false
    };
    PsxResult rc = psx_state_save(sys, &measure);
    if (rc != PSX_OK || measure.pos < 32u) {
        return rc != PSX_OK ? rc : PSX_ERR_FORMAT;
    }

    u32 sz = measure.pos;
    u8 *blob = (u8 *)malloc(sz);
    if (!blob) {
        return PSX_ERR_NOMEM;
    }

    StateIO save_st = {
        .writing = true,
        .buf = blob,
        .pos = 0u,
        .cap = sz,
        .error = false
    };
    rc = psx_state_save(sys, &save_st);
    if (rc != PSX_OK) {
        free(blob);
        return rc;
    }

    u32 saved_pc = sys->cpu.pc;
    u32 saved_sp = sys->cpu.gpr[29];

    /* 1. Verify CRC32 tamper rejection leaves live state uncorrupted */
    blob[sz - 1u] ^= 0x5Au;
    StateIO bad_st = {
        .writing = false,
        .buf = blob,
        .pos = 0u,
        .cap = sz,
        .error = false
    };
    if (psx_state_load(sys, &bad_st) != PSX_ERR_FORMAT) {
        free(blob);
        return PSX_ERR_FORMAT;
    }
    blob[sz - 1u] ^= 0x5Au;

    /* 2. Mutate CPU & RAM, then restore clean state and verify exact match */
    sys->cpu.gpr[29] ^= 0xDEADBEEFu;
    if (sys->bus.ram) {
        sys->bus.ram[0x100] ^= 0xFFu;
    }

    StateIO load_st = {
        .writing = false,
        .buf = blob,
        .pos = 0u,
        .cap = sz,
        .error = false
    };
    rc = psx_state_load(sys, &load_st);
    free(blob);
    if (rc != PSX_OK) {
        return rc;
    }
    if (sys->cpu.pc != saved_pc || sys->cpu.gpr[29] != saved_sp) {
        return PSX_ERR_FORMAT;
    }
    return PSX_OK;
}

static bool write_text_output(const char *path, const char *text, size_t len) {
    if (strcmp(path, "-") == 0) {
        return fwrite(text, 1u, len, stdout) == len;
    }
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return false;
    }
    bool ok = (fwrite(text, 1u, len, fp) == len);
    if (fclose(fp) != 0) {
        ok = false;
    }
    return ok;
}

int main(int argc, char **argv) {
    PsxConfig cfg;
    psx_config_defaults(&cfg, true);
    cfg.gpu_backend = GPU_BACKEND_SOFT;
    cfg.spu_async = false;

    const char *bios_arg = NULL;
    bool        hle_flag = false;
    const char *exe_path = NULL;
    const char *cd_path = NULL;
    u32         max_frames = 600u;
    bool        frames_explicit = false;
    u64         max_cycles = 0ULL;
    const char *tty_out_path = NULL;
    const char *stop_tty = NULL;
    char        ppm_path[512] = { 0 };
    u32         ppm_every = 0u;
    const char *wav_path = NULL;
    bool        savestate_test = false;
    u32         trace_steps = 0u;
    const char *trace_hash_path = NULL;
    u64         hash_every = 10000ULL;
    const char *json_out_path = NULL;
#if defined(PSX3DS_BENCH_DEFAULT)
    bool        bench_mode = true;
#else
    bool        bench_mode = false;
#endif

    plat_host_reset_capture();
    plat_host_input_clear();

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(a, "--version") == 0 || strcmp(a, "-V") == 0) {
            printf("psx3ds v%s\n", PSX3DS_VERSION_STR);
            return 0;
        } else if (strcmp(a, "--bios") == 0 && i + 1 < argc) {
            bios_arg = argv[++i];
        } else if (strcmp(a, "--hle") == 0) {
            hle_flag = true;
        } else if (strcmp(a, "--exe") == 0 && i + 1 < argc) {
            exe_path = argv[++i];
        } else if (strcmp(a, "--cd") == 0 && i + 1 < argc) {
            cd_path = argv[++i];
        } else if ((strcmp(a, "--cpu") == 0 || strcmp(a, "--engine") == 0) && i + 1 < argc) {
            if (!parse_cpu_engine(argv[++i], &cfg.cpu_engine)) {
                fprintf(stderr, "Invalid CPU engine: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(a, "--gpu") == 0 && i + 1 < argc) {
            const char *g = argv[++i];
            if (strcmp(g, "soft") == 0) {
                cfg.gpu_backend = GPU_BACKEND_SOFT;
            } else if (strcmp(g, "hw") == 0) {
                cfg.gpu_backend = GPU_BACKEND_HW;
            } else {
                fprintf(stderr, "Invalid GPU backend: %s\n", g);
                return 1;
            }
        } else if (strcmp(a, "--frames") == 0 && i + 1 < argc) {
            max_frames = (u32)strtoul(argv[++i], NULL, 0);
            frames_explicit = true;
        } else if (strcmp(a, "--seconds") == 0 && i + 1 < argc) {
            double secs = strtod(argv[++i], NULL);
            max_frames = secs > 0.0 ? (u32)(secs * 60.0 + 0.5) : 1u;
            frames_explicit = true;
        } else if (strcmp(a, "--max-cycles") == 0 && i + 1 < argc) {
            max_cycles = (u64)strtoull(argv[++i], NULL, 0);
        } else if (strcmp(a, "--tty-out") == 0 && i + 1 < argc) {
            tty_out_path = argv[++i];
        } else if ((strcmp(a, "--stop-on-tty") == 0 || strcmp(a, "--until-printf") == 0) && i + 1 < argc) {
            stop_tty = argv[++i];
            if (!frames_explicit) {
                max_frames = 36000u; /* 10 min safety cap for long CPU/GTE test suites */
            }
        } else if ((strcmp(a, "--dump-ppm") == 0 || strcmp(a, "--dump-frame") == 0) && i + 1 < argc) {
            const char *arg = argv[++i];
            const char *colon = strrchr(arg, ':');
            if (colon && colon[1] >= '0' && colon[1] <= '9') {
                size_t plen = (size_t)(colon - arg);
                if (plen >= sizeof(ppm_path)) {
                    plen = sizeof(ppm_path) - 1u;
                }
                memcpy(ppm_path, arg, plen);
                ppm_path[plen] = '\0';
                ppm_every = (u32)strtoul(colon + 1, NULL, 0);
            } else {
                snprintf(ppm_path, sizeof(ppm_path), "%s", arg);
            }
        } else if ((strcmp(a, "--dump-wav") == 0 || strcmp(a, "--dump-audio") == 0) && i + 1 < argc) {
            wav_path = argv[++i];
        } else if (strcmp(a, "--savestate-test") == 0) {
            savestate_test = true;
        } else if (strcmp(a, "--trace") == 0 && i + 1 < argc) {
            trace_steps = (u32)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(a, "--trace-hash") == 0 && i + 1 < argc) {
            trace_hash_path = argv[++i];
        } else if (strcmp(a, "--hash-every") == 0 && i + 1 < argc) {
            hash_every = (u64)strtoull(argv[++i], NULL, 0);
            if (hash_every == 0ULL) {
                hash_every = 10000ULL;
            }
        } else if (strcmp(a, "--bench") == 0) {
            bench_mode = true;
        } else if ((strcmp(a, "--bench-json") == 0 || strcmp(a, "--stats") == 0) && i + 1 < argc) {
            json_out_path = argv[++i];
        } else if (strcmp(a, "--pad-press") == 0 && i + 1 < argc) {
            if (!parse_pad_spec(argv[++i])) {
                fprintf(stderr, "Invalid --pad-press spec: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(a, "--input") == 0 && i + 1 < argc) {
            if (!load_input_script(argv[++i])) {
                fprintf(stderr, "Failed to load --input script: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(a, "--cycle-mult") == 0 && i + 1 < argc) {
            cfg.cycle_mult = (u16)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(a, "--small-mem") == 0) {
            cfg.small_mem = true;
        } else if (strcmp(a, "--no-ibtc") == 0) {
            cfg.jit_ibtc = false;
        } else if (strcmp(a, "--no-link") == 0) {
            cfg.jit_link = false;
        } else if (strcmp(a, "--no-idle-skip") == 0) {
            cfg.jit_idle_skip = false;
        } else if (strcmp(a, "--no-align-check") == 0) {
            cfg.jit_check_align = false;
        } else if (strcmp(a, "--no-hle-exec") == 0) {
            cfg.jit_allow_hle_exec = false;
        } else if (strcmp(a, "--png") == 0) {
            /* Accepted for CLI flag compatibility; PPM writer used */
        } else {
            fprintf(stderr, "Unknown or incomplete argument: %s\n", a);
            print_usage(argv[0]);
            return 1;
        }
    }

    /* Multi-engine benchmark mode (psx3ds-bench or --bench) */
    if (bench_mode) {
        PsxBenchConfig bcfg;
        memset(&bcfg, 0, sizeof(bcfg));
        bcfg.bios_path = bios_arg;
        bcfg.hle_bios = hle_flag;
        bcfg.exe_path = exe_path;
        bcfg.cd_path = cd_path;
        bcfg.frames = frames_explicit ? max_frames : 120u;
        bcfg.all_engines = true;
        bcfg.all_gpus = false;

        u32 jcap = 256u * 1024u;
        char *jbuf = (char *)malloc(jcap);
        if (!jbuf) {
            return 1;
        }
        PsxResult brc = psx_bench_run_json(&bcfg, jbuf, jcap);
        if (brc != PSX_OK) {
            fprintf(stderr, "Benchmark failed (rc=%d)\n", (int)brc);
            free(jbuf);
            return 1;
        }
        const char *dest = json_out_path ? json_out_path : "-";
        bool ok = write_text_output(dest, jbuf, strlen(jbuf));
        free(jbuf);
        return ok ? 0 : 1;
    }

    if (!bios_arg && !hle_flag && !exe_path && !cd_path) {
        print_usage(argv[0]);
        return 1;
    }

    cfg.hle_bios = hle_flag;
    if (!hle_flag) {
        const char *bpath = bios_arg ? bios_arg : "auto";
        snprintf(cfg.bios_path, sizeof(cfg.bios_path), "%s", bpath);
    }

    if (ppm_path[0] != '\0') {
        plat_host_set_video_capture(true, ppm_path, ppm_every);
    }
    if (wav_path != NULL) {
        plat_host_set_audio_capture(true);
    }

    PsxSystem *sys = (PsxSystem *)calloc(1u, sizeof(PsxSystem));
    if (!sys) {
        fprintf(stderr, "Failed to allocate PsxSystem\n");
        return 1;
    }

    PsxResult rc = psx_system_init(sys, &cfg);
    if (rc != PSX_OK) {
        fprintf(stderr, "psx_system_init failed: %d\n", (int)rc);
        free(sys);
        return 1;
    }

    CliTtySink tty_sink = { .fp = NULL, .close_on_exit = false };
    if (tty_out_path) {
        if (strcmp(tty_out_path, "-") == 0) {
            tty_sink.fp = stdout;
        } else {
            tty_sink.fp = fopen(tty_out_path, "wb");
            if (!tty_sink.fp) {
                fprintf(stderr, "Failed to open --tty-out: %s\n", tty_out_path);
                psx_system_shutdown(sys);
                free(sys);
                return 1;
            }
            tty_sink.close_on_exit = true;
        }
        bios_set_tty_callback(sys, cli_tty_callback, &tty_sink);
    }

    if (stop_tty) {
        bios_set_stop_pattern(sys, stop_tty);
    }

    if (cd_path) {
        rc = psx_load_cd(sys, cd_path);
        if (rc != PSX_OK) {
            fprintf(stderr, "psx_load_cd(%s) failed: %d\n", cd_path, (int)rc);
            if (tty_sink.close_on_exit && tty_sink.fp) fclose(tty_sink.fp);
            psx_system_shutdown(sys);
            free(sys);
            return 1;
        }
    }

    if (exe_path) {
        rc = psx_load_exe(sys, exe_path);
        if (rc != PSX_OK) {
            fprintf(stderr, "psx_load_exe(%s) failed: %d\n", exe_path, (int)rc);
            if (tty_sink.close_on_exit && tty_sink.fp) fclose(tty_sink.fp);
            psx_system_shutdown(sys);
            free(sys);
            return 1;
        }
    }

    FILE *hash_fp = NULL;
    bool  hash_close = false;
    if (trace_hash_path) {
        if (strcmp(trace_hash_path, "-") == 0) {
            hash_fp = stdout;
        } else {
            hash_fp = fopen(trace_hash_path, "wb");
            if (!hash_fp) {
                fprintf(stderr, "Failed to open --trace-hash: %s\n", trace_hash_path);
                if (tty_sink.close_on_exit && tty_sink.fp) fclose(tty_sink.fp);
                psx_system_shutdown(sys);
                free(sys);
                return 1;
            }
            hash_close = true;
        }
    }

    /* Optional single-step instruction trace (--trace <N>) */
    if (trace_steps > 0u) {
        for (u32 step = 0u; step < trace_steps; step++) {
            u64 cyc = sys->cpu.total_cycles > sys->sched.total ? sys->cpu.total_cycles : sys->sched.total;
            fprintf(stderr,
                    "[trace %06u cyc=%" PRIu64 "] pc=%08X sp=%08X ra=%08X a0=%08X v0=%08X sr=%08X\n",
                    (unsigned)step, (uint64_t)cyc,
                    sys->cpu.pc, sys->cpu.gpr[29], sys->cpu.gpr[31],
                    sys->cpu.gpr[4], sys->cpu.gpr[2], sys->cpu.cop0[COP0_SR]);
            u32 prev_pc = sys->cpu.pc;
            u64 prev_cyc = cyc;
            sched_run(sys);
            if (!bios_pc_hook(sys, sys->cpu.pc)) {
                cpu_step_one(sys);
            }
            u64 now_cyc = sys->cpu.total_cycles > sys->sched.total ? sys->cpu.total_cycles : sys->sched.total;
            if (sys->quit || sys->bios.until_matched ||
                (max_cycles > 0ULL && now_cyc >= max_cycles) ||
                (now_cyc == prev_cyc && sys->cpu.pc == prev_pc)) {
                break;
            }
        }
    }

    PsxStats accum;
    psx_stats_reset(&accum);
    u32 frames_ran = 0u;
    u32 ss_frame = max_frames > 2u ? (max_frames / 2u) : 1u;
    bool ss_done = false;
    u64 next_hash_cyc = hash_every;
    u64 wall_t0 = plat_time_us();

    while (frames_ran < max_frames && !sys->quit && !sys->bios.until_matched) {
        plat_host_input_set_frame(frames_ran);
        PsxHostKeys hkeys;
        plat_input_poll(sys->pads, &hkeys);

        if (hash_fp) {
            /* Fine-grained slice stepping so trace hashes align across interp/cached/jit */
            sys->frame_done = false;
            u32 zero_prog = 0u;
            while (!sys->frame_done && !sys->quit && !sys->bios.until_matched) {
                u64 prev_c = sys->cpu.total_cycles > sys->sched.total ? sys->cpu.total_cycles : sys->sched.total;
                sched_run(sys);
                if (sys->frame_done || sys->quit || sys->bios.until_matched) {
                    break;
                }
                u64 cur_c = sys->cpu.total_cycles > sys->sched.total ? sys->cpu.total_cycles : sys->sched.total;
                if (next_hash_cyc > cur_c) {
                    u64 rem = next_hash_cyc - cur_c;
                    if (rem < (u64)sys->cpu.cycles_left && rem > 0ULL) {
                        sys->cpu.slice_len = (s32)rem;
                        sys->cpu.cycles_left = (s32)rem;
                    }
                }
                switch ((PsxCpuEngine)sys->cpu_mode) {
                    case CPU_JIT:    jit_run(sys); break;
                    case CPU_CACHED: cpu_cached_run(sys); break;
                    default:         cpu_run_interp(sys); break;
                }
                u64 after_c = sys->cpu.total_cycles > sys->sched.total ? sys->cpu.total_cycles : sys->sched.total;
                while (after_c >= next_hash_cyc) {
                    u64 h = hash_cpu_arch_state(&sys->cpu);
                    fprintf(hash_fp, "%012" PRIu64 " %016" PRIx64 " %08X\n",
                            (uint64_t)after_c, (uint64_t)h, sys->cpu.pc);
                    next_hash_cyc += hash_every;
                }
                if (max_cycles > 0ULL && after_c >= max_cycles) {
                    break;
                }
                if (after_c == prev_c) {
                    if (++zero_prog >= 16u) {
                        sys->frame_done = true;
                        break;
                    }
                } else {
                    zero_prog = 0u;
                }
            }
            frames_ran++;
            accum.frames = frames_ran;
        } else {
            psx_run_frame(sys);
            psx_stats_accumulate(&accum, &sys->stats);
            frames_ran++;
        }

        if (savestate_test && !ss_done && frames_ran >= ss_frame) {
            ss_done = true;
            PsxResult ssrc = run_savestate_self_test(sys);
            if (ssrc != PSX_OK) {
                fprintf(stderr, "Savestate self-test FAILED at frame %u (rc=%d)\n",
                        (unsigned)frames_ran, (int)ssrc);
                if (hash_close && hash_fp) fclose(hash_fp);
                if (tty_sink.close_on_exit && tty_sink.fp) fclose(tty_sink.fp);
                psx_system_shutdown(sys);
                plat_host_reset_capture();
                free(sys);
                return 1;
            }
        }

        u64 cur_cycles = sys->cpu.total_cycles > sys->sched.total
                             ? sys->cpu.total_cycles
                             : sys->sched.total;
        if (max_cycles > 0ULL && cur_cycles >= max_cycles) {
            break;
        }
    }

    if (savestate_test && !ss_done) {
        PsxResult ssrc = run_savestate_self_test(sys);
        if (ssrc != PSX_OK) {
            fprintf(stderr, "Savestate self-test FAILED (rc=%d)\n", (int)ssrc);
            if (hash_close && hash_fp) fclose(hash_fp);
            if (tty_sink.close_on_exit && tty_sink.fp) fclose(tty_sink.fp);
            psx_system_shutdown(sys);
            plat_host_reset_capture();
            free(sys);
            return 1;
        }
    }

    u64 wall_us = plat_time_us() - wall_t0;

    if (ppm_path[0] != '\0') {
        PsxResult prc = PSX_ERR_IO;
        if (plat_host_has_presented_frame()) {
            prc = plat_host_write_last_ppm(ppm_path);
        } else if (sys->gpu.vram) {
            if (sys->gpu.be && sys->gpu.be->sync_to_shadow) {
                sys->gpu.be->sync_to_shadow(sys->gpu.be_self, 0u, 0u, PSX_VRAM_W, PSX_VRAM_H);
            }
            u16 w = sys->gpu.disp.hres ? sys->gpu.disp.hres : 320u;
            u16 h = sys->gpu.disp.vres ? sys->gpu.disp.vres : 240u;
            u16 vx = sys->gpu.disp.vram_x < PSX_VRAM_W ? sys->gpu.disp.vram_x : 0u;
            u16 vy = sys->gpu.disp.vram_y < PSX_VRAM_H ? sys->gpu.disp.vram_y : 0u;
            if ((u32)vx + (u32)w > PSX_VRAM_W) w = (u16)(PSX_VRAM_W - vx);
            if ((u32)vy + (u32)h > PSX_VRAM_H) h = (u16)(PSX_VRAM_H - vy);
            const u16 *src = sys->gpu.vram + (size_t)vy * PSX_VRAM_W + (size_t)vx;
            prc = video_ppm_write_rgb555(ppm_path, src, w, h, PSX_VRAM_W, sys->gpu.disp.color24 != 0u);
        }
        if (prc != PSX_OK) {
            fprintf(stderr, "Failed to write PPM to %s (rc=%d)\n", ppm_path, (int)prc);
            if (hash_close && hash_fp) fclose(hash_fp);
            if (tty_sink.close_on_exit && tty_sink.fp) fclose(tty_sink.fp);
            psx_system_shutdown(sys);
            plat_host_reset_capture();
            free(sys);
            return 1;
        }
    }

    if (wav_path != NULL) {
        PsxResult wrc = plat_host_write_captured_wav(wav_path);
        if (wrc != PSX_OK) {
            fprintf(stderr, "Failed to write WAV to %s (rc=%d)\n", wav_path, (int)wrc);
            if (hash_close && hash_fp) fclose(hash_fp);
            if (tty_sink.close_on_exit && tty_sink.fp) fclose(tty_sink.fp);
            psx_system_shutdown(sys);
            plat_host_reset_capture();
            free(sys);
            return 1;
        }
    }

    if (json_out_path != NULL) {
        char jbuf[4096];
        const char *rom_label = exe_path ? exe_path : (cd_path ? cd_path : (hle_flag ? "hle" : "bios"));
        u32 jlen = psx_stats_format_json(sys, &accum, rom_label, wall_us, frames_ran, jbuf, sizeof(jbuf));
        if (!write_text_output(json_out_path, jbuf, jlen)) {
            fprintf(stderr, "Failed to write JSON stats to %s\n", json_out_path);
        }
    }

    int exit_code = 0;
    if (stop_tty != NULL) {
        if (!sys->bios.until_matched) {
            exit_code = 3; /* Timeout before pattern matched */
        } else if (tty_contains_failure(sys->bios.tty_buf)) {
            exit_code = 2; /* Guest test reported failure */
        }
    }

    if (hash_close && hash_fp) {
        fclose(hash_fp);
    }
    if (tty_sink.close_on_exit && tty_sink.fp) {
        fclose(tty_sink.fp);
    }
    psx_system_shutdown(sys);
    plat_host_reset_capture();
    free(sys);
    return exit_code;
}
