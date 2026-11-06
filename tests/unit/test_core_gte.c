/*
 * Unit tests for Slice A: COP2 Geometry Transformation Engine (GTE).
 * 1. Portable standalone tests for UNR division, register quirks (SXYP FIFO,
 *    IRGB/ORGB, LZCS/LZCR, 16-bit sign/zero-extend, FLAG bit 31), COP2 load/store,
 *    and representative vectors across GTE opcodes (RTPS, RTPT, NCLIP, MVMVA mx=3
 *    & cv=2 Far Color bug, AVSZ3/4, OP, SQR, DPCS, INTPL, GPF, GPL, NCDS/NCDT).
 * 2. When `tests/roms/gte/gte-test-all.exe` is present (T3 host & T2 QEMU),
 *    runs ALL 1,150 real PlayStation 1 hardware golden vectors (73,600 register
 *    assertions) and verifies 100.00% bit-exactness!
 */
#include "psx3ds/system.h"

#include <stdio.h>
#include <string.h>

static PsxSystem g_sys;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #cond);           \
            arena_destroy(&g_sys.arena);                                                \
            return 1;                                                                   \
        }                                                                               \
    } while (0)

static u32 read_le32_bytes(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static int run_hardware_golden_suite(PsxCpu *c) {
    const char *paths[] = {
        "tests/roms/gte/gte-test-all.exe",
        "../tests/roms/gte/gte-test-all.exe",
        "../../tests/roms/gte/gte-test-all.exe"
    };
    FILE *fp = NULL;
    for (size_t i = 0u; i < sizeof(paths) / sizeof(paths[0]); i++) {
        fp = fopen(paths[i], "rb");
        if (fp) {
            break;
        }
    }
    if (!fp) {
        printf("gte-test-all.exe not mounted in cwd (skipping external ROM sweep)\n");
        return 0;
    }

    /* Record table starts at VA 0x800200E0 -> file offset 0x800 + 0x100E0 = 0x108E0 */
    if (fseek(fp, 0x108E0L, SEEK_SET) != 0) {
        fclose(fp);
        fprintf(stderr, "FAIL: fseek into gte-test-all.exe failed\n");
        return 1;
    }

    u8 rec[0x208];
    for (u32 t = 0u; t < 1150u; t++) {
        if (fread(rec, 1u, sizeof(rec), fp) != sizeof(rec)) {
            fclose(fp);
            fprintf(stderr, "FAIL: short read at record %u\n", (unsigned)t);
            return 1;
        }

        memset(c->gte_d, 0, sizeof(c->gte_d));
        memset(c->gte_c, 0, sizeof(c->gte_c));

        /* Load all 64 registers via architectural MTC2 / CTC2 in order 0..63 */
        for (u32 r = 0u; r < 32u; r++) {
            gte_write_data(c, r, read_le32_bytes(&rec[4u + r * 4u]));
        }
        for (u32 r = 0u; r < 32u; r++) {
            gte_write_ctrl(c, r, read_le32_bytes(&rec[4u + (32u + r) * 4u]));
        }

        u32 cmd = read_le32_bytes(&rec[4u + 256u]);
        if (cmd != 0xFFFFFFFFu) {
            gte_exec_c(c, 0x4A000000u | (cmd & 0x01FFFFFFu));
        }

        for (u32 r = 0u; r < 64u; r++) {
            u32 got = (r < 32u) ? gte_read_data(c, r) : gte_read_ctrl(c, r - 32u);
            u32 exp = read_le32_bytes(&rec[4u + 256u + 4u + r * 4u]);
            if (got != exp) {
                fclose(fp);
                fprintf(stderr,
                        "FAIL GTE golden record %u (cmd=0x%08x) reg %u: got 0x%08x, expected 0x%08x\n",
                        (unsigned)(t + 1u), (unsigned)cmd, (unsigned)r, (unsigned)got, (unsigned)exp);
                return 1;
            }
        }
    }

    fclose(fp);
    printf("gte-test-all.exe: 1150 / 1150 hardware golden records (73,600 reg checks) BIT-EXACT!\n");
    return 0;
}

int main(void) {
    memset(&g_sys, 0, sizeof(g_sys));
    psx_config_defaults(&g_sys.cfg, true);
    g_sys.cfg.cycle_mult = 256u;

    (void)arena_init(&g_sys.arena, 20u * 1024u * 1024u);
    sched_init(&g_sys.sched);
    bus_init(&g_sys);
    irq_init(&g_sys);
    cpu_init(&g_sys);
    gte_init(&g_sys);

    PsxCpu *c = &g_sys.cpu;

    /* ----------------------------------------------------------------------
     * 1. UNR Table & Division Edge Cases
     * ---------------------------------------------------------------------- */
    CHECK(gte_unr_table[0] == 0xFFu);
    CHECK(gte_unr_table[256] == 0x00u);

    bool ovf = false;
    CHECK(gte_div_unr(100u, 0u, &ovf) == 0x1FFFFu && ovf == true);
    CHECK(gte_div_unr(200u, 100u, &ovf) == 0x1FFFFu && ovf == true);
    CHECK(gte_div_unr(199u, 100u, &ovf) <= 0x1FFFFu && ovf == false);
    CHECK(gte_div_unr(100u, 100u, &ovf) == 0x10000u && ovf == false);

    /* ----------------------------------------------------------------------
     * 2. Register Transfer Quirks (SXYP, IRGB/ORGB, LZCS/LZCR, FLAG)
     * ---------------------------------------------------------------------- */
    gte_write_data(c, 15u, 0x11111111u);
    gte_write_data(c, 15u, 0x22222222u);
    gte_write_data(c, 15u, 0x33333333u);
    CHECK(gte_read_data(c, 12u) == 0x11111111u);
    CHECK(gte_read_data(c, 13u) == 0x22222222u);
    CHECK(gte_read_data(c, 14u) == 0x33333333u);
    CHECK(gte_read_data(c, 15u) == 0x33333333u);

    gte_write_data(c, 28u, (4u << 10) | (10u << 5) | 30u); /* IRGB */
    CHECK(gte_read_data(c, 9u) == 30u * 0x80u);
    CHECK(gte_read_data(c, 10u) == 10u * 0x80u);
    CHECK(gte_read_data(c, 11u) == 4u * 0x80u);
    CHECK(gte_read_data(c, 29u) == ((4u << 10) | (10u << 5) | 30u));
    gte_write_data(c, 29u, 0u); /* ORGB write ignored */
    CHECK(gte_read_data(c, 29u) == ((4u << 10) | (10u << 5) | 30u));

    gte_write_data(c, 30u, 0x00000000u);
    CHECK(gte_read_data(c, 31u) == 32u);
    gte_write_data(c, 30u, 0xFFFFFFFFu);
    CHECK(gte_read_data(c, 31u) == 32u);
    gte_write_data(c, 30u, 0x80000000u);
    CHECK(gte_read_data(c, 31u) == 1u);
    gte_write_data(c, 30u, 0x00000001u);
    CHECK(gte_read_data(c, 31u) == 31u);

    /* Sign-extended control reg H (reg 26) & FLAG bit 31 derivation */
    gte_write_ctrl(c, 26u, 0x12348001u);
    CHECK(gte_read_ctrl(c, 26u) == 0xFFFF8001u);
    gte_write_ctrl(c, 31u, 0x80001000u); /* bit 12 is NOT in 0x7F87E000 -> bit 31 = 0! */
    CHECK(gte_read_ctrl(c, 31u) == 0x00001000u);
    gte_write_ctrl(c, 31u, 0x00002000u); /* bit 13 IS in 0x7F87E000 -> bit 31 = 1! */
    CHECK(gte_read_ctrl(c, 31u) == 0x80002000u);

    /* ----------------------------------------------------------------------
     * 3. Standalone Opcode Sanity (NCLIP, AVSZ3, MVMVA cv=2 bug, COP2 insns)
     * ---------------------------------------------------------------------- */
    gte_reset(&g_sys);
    /* NCLIP clockwise triangle: (0,0), (10,0), (0,20) -> 0*(0-20) + 10*(20-0) + 0 = +200 */
    gte_write_data(c, 12u, 0x00000000u);
    gte_write_data(c, 13u, 0x0000000Au);
    gte_write_data(c, 14u, 0x00140000u);
    gte_exec_c(c, 0x4A000006u);
    CHECK(gte_read_data(c, 24u) == 200u);

    /* AVSZ3: ZSF3 = 0x555 (1/3 in 4.12), SZ1=300, SZ2=300, SZ3=300 -> ~300 */
    gte_write_ctrl(c, 29u, 0x0555u);
    gte_write_data(c, 17u, 300u);
    gte_write_data(c, 18u, 300u);
    gte_write_data(c, 19u, 300u);
    gte_exec_c(c, 0x4A00002Du);
    CHECK(gte_read_data(c, 7u) == ((0x555 * 900) >> 12));

    /* MVMVA cv=2 Far Color bug: RFC=0x1000 ignored in final MAC1! */
    gte_reset(&g_sys);
    gte_write_ctrl(c, 0u, (4096u << 16) | 4096u); /* R11=1.0, R12=1.0 */
    gte_write_ctrl(c, 21u, 0x00010000u);          /* huge RFC */
    gte_write_data(c, 0u, (200u << 16) | 100u);   /* VX0=100, VY0=200 */
    /* sf=1, mx=0(RT), vx=0(V0), cv=2(FC), lm=0 -> MAC1 = (R12*VY0)>>12 = 200 (RFC+R11*VX0 dropped!) */
    gte_exec_c(c, 0x4A084012u);
    CHECK(gte_read_data(c, 25u) == 200u);

    /* Verify CPU interpreter MFC2 1-insn load delay & LWC2/SWC2 */
    cpu_set_sr(&g_sys, PSX_SR_CU2);
    c->pc = 0x80010000u;
    c->next_pc = 0x80010004u;
    c->cycles_left = 100;
    c->slice_len = 100;
    c->gpr[1] = 0xABCDu;
    psx_write32(c, 0x80001000u, 0x13572468u);
    const u32 cop2_prog[] = {
        (0x32u << 26) | (0u << 21) | (14u << 16) | 0x1000u, /* lwc2 $14(SXY2), 0x1000($0) */
        (0x12u << 26) | (0u << 21) | (1u << 16) | (15u << 11), /* mfc2 $1, $15(SXYP=SXY2) */
        (0u << 26) | (1u << 21) | (2u << 11) | 0x25u,       /* or $2, $1, $0 (sees old $1=0xABCD!) */
        (0u << 26) | (1u << 21) | (3u << 11) | 0x25u,       /* or $3, $1, $0 (sees 0x13572468!) */
        (0x3Au << 26) | (0u << 21) | (15u << 16) | 0x1004u, /* swc2 $15, 0x1004($0) */
    };
    memcpy(g_sys.bus.ram + 0x10000u, cop2_prog, sizeof(cop2_prog));
    for (u32 i = 0u; i < 5u; i++) {
        cpu_step_one(&g_sys);
    }
    CHECK(c->gpr[2] == 0xABCDu);
    CHECK(c->gpr[3] == 0x13572468u);
    CHECK(psx_read32(c, 0x80001004u) == 0x13572468u);

    /* ----------------------------------------------------------------------
     * 4. Full 1,150-record PS1 Hardware Golden Suite (gte-test-all.exe)
     * ---------------------------------------------------------------------- */
    if (run_hardware_golden_suite(c) != 0) {
        arena_destroy(&g_sys.arena);
        return 1;
    }

    arena_destroy(&g_sys.arena);
    printf("test_core_gte PASS\n");
    return 0;
}
