/*
 * Comprehensive Unit & Differential Verification Suite for Slice B Inline GTE:
 * - ARM11v6K `jit_gte_div_unr_asm` exact Newton-Raphson reciprocal division
 * - Inline `MFC2/MTC2/CFC2/CTC2/LWC2/SWC2` (`SXYP` FIFO, `IRGB/ORGB`, `LZCS/LZCR`, `FLAG`)
 *   + GPR register-allocator spill/reload integrity across inline GTE kernels
 * - Bit-for-bit differential fuzzing of ALL 22 GTE opcodes (`RTPS`, `RTPT`, `NCLIP`,
 *   `AVSZ3`, `AVSZ4`, `MVMVA` [all mx/vx/cv/sf/lm], `OP`, `SQR`, `GPF`, `GPL`,
 *   `DPCS`, `INTPL`, lighting/color ops) across 1,500+ adversarial & random vectors
 *   comparing Reference C GTE vs Cached Interpreter vs Live ARM11v6K Inline GTE
 * - Inline vs Helper GTE Geometry Pipeline Microbenchmark
 *
 * Portable C11: compiles cleanly on T1 (3DS), T2 (ARM11 Linux QEMU), and T3 (x86_64 ASan/UBSan).
 */

#include "../../src/jit/jit_internal.h"
#if PSX3DS_HAVE_JIT
#include "../../src/jit/arm_emit.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK_TRUE(cond, msg)                                                       \
    do {                                                                            \
        if (!(cond)) {                                                              \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, (msg));       \
            return 1;                                                               \
        }                                                                           \
    } while (0)

/* ---- MIPS & COP2 Encoding Helpers ---- */
#define M_NOP                   0x00000000u
#define M_LUI(rt, imm)          ((0x0Fu << 26) | (((u32)(rt) & 31u) << 16) | ((u32)(imm) & 0xFFFFu))
#define M_ORI(rt, rs, imm)      ((0x0Du << 26) | (((u32)(rs) & 31u) << 21) | (((u32)(rt) & 31u) << 16) | ((u32)(imm) & 0xFFFFu))
#define M_ADDIU(rt, rs, imm)    ((0x09u << 26) | (((u32)(rs) & 31u) << 21) | (((u32)(rt) & 31u) << 16) | ((u32)(imm) & 0xFFFFu))
#define M_ADDU(rd, rs, rt)      ((((u32)(rs) & 31u) << 21) | (((u32)(rt) & 31u) << 16) | (((u32)(rd) & 31u) << 11) | 0x21u)
#define M_XOR(rd, rs, rt)       ((((u32)(rs) & 31u) << 21) | (((u32)(rt) & 31u) << 16) | (((u32)(rd) & 31u) << 11) | 0x26u)
#define M_BEQ(rs, rt, off)      ((0x04u << 26) | (((u32)(rs) & 31u) << 21) | (((u32)(rt) & 31u) << 16) | ((u32)(off) & 0xFFFFu))
#define M_BNE(rs, rt, off)      ((0x05u << 26) | (((u32)(rs) & 31u) << 21) | (((u32)(rt) & 31u) << 16) | ((u32)(off) & 0xFFFFu))
#define M_SW(rt, off, rs)       ((0x2Bu << 26) | (((u32)(rs) & 31u) << 21) | (((u32)(rt) & 31u) << 16) | ((u32)(off) & 0xFFFFu))

#define M_MFC2(rt, rd)          ((0x12u << 26) | (0x00u << 21) | (((u32)(rt) & 31u) << 16) | (((u32)(rd) & 31u) << 11))
#define M_CFC2(rt, rd)          ((0x12u << 26) | (0x02u << 21) | (((u32)(rt) & 31u) << 16) | (((u32)(rd) & 31u) << 11))
#define M_MTC2(rt, rd)          ((0x12u << 26) | (0x04u << 21) | (((u32)(rt) & 31u) << 16) | (((u32)(rd) & 31u) << 11))
#define M_CTC2(rt, rd)          ((0x12u << 26) | (0x06u << 21) | (((u32)(rt) & 31u) << 16) | (((u32)(rd) & 31u) << 11))
#define M_LWC2(rt, off, rs)     ((0x32u << 26) | (((u32)(rs) & 31u) << 21) | (((u32)(rt) & 31u) << 16) | ((u32)(off) & 0xFFFFu))
#define M_SWC2(rt, off, rs)     ((0x3Au << 26) | (((u32)(rs) & 31u) << 21) | (((u32)(rt) & 31u) << 16) | ((u32)(off) & 0xFFFFu))

#define M_GTE(sf, mx, vx, cv, lm, cmd) \
    ((0x12u << 26) | (1u << 25) | (((u32)(sf) & 1u) << 19) | \
     (((u32)(mx) & 3u) << 17) | (((u32)(vx) & 3u) << 15) | \
     (((u32)(cv) & 3u) << 13) | (((u32)(lm) & 1u) << 10) | ((u32)(cmd) & 0x3Fu))

typedef struct TestEnv {
    bool full_sys_init;
} TestEnv;

static PSX_ALIGNED(32) PsxSystem g_sys[3]; /* 0: oracle, 1: cached, 2: arm_jit */
static TestEnv g_env[3];

static void build_standalone_fastmem(PsxBus *b) {
    memset(b->fm_r, 0, PSX_FM_BYTES);
    memset(b->fm_w, 0, PSX_FM_BYTES);
    const u32 ram_segs[3] = { 0x00000u, 0x80000u, 0xA0000u };
    for (u32 s = 0u; s < 3u; ++s) {
        for (u32 p = 0u; p < 2048u; ++p) {
            u32 vp = ram_segs[s] + p;
            u32 phys_off = (p & 0x1FFu) << 12;
            uintptr_t bias = (uintptr_t)b->ram + (uintptr_t)phys_off - ((uintptr_t)vp << 12);
            b->fm_r[vp] = bias;
            b->fm_w[vp] = bias;
        }
    }
    b->fm_r[0x1F800u] = (uintptr_t)b->scratch_page - (uintptr_t)0x1F800000u;
    b->fm_w[0x1F800u] = (uintptr_t)b->scratch_page - (uintptr_t)0x1F800000u;
    b->fm_r[0x9F800u] = (uintptr_t)b->scratch_page - (uintptr_t)0x9F800000u;
    b->fm_w[0x9F800u] = (uintptr_t)b->scratch_page - (uintptr_t)0x9F800000u;
}

static bool test_sys_init(PsxSystem *sys, TestEnv *env) {
    memset(sys, 0, sizeof(*sys));
    memset(env, 0, sizeof(*env));

    PsxConfig cfg;
    psx_config_defaults(&cfg, true);
    cfg.hle_bios = false;
    cfg.jit_inline_gte = true;
    cfg.cycle_mult = 256u;
    cfg.cycles_per_mem = 0u;
    cfg.jit_code_bytes = 2u * 1024u * 1024u;
    cfg.jit_max_blocks = 4096u;

    if (psx_system_init(sys, &cfg) == PSX_OK) {
        psx_system_reset(sys);
        env->full_sys_init = true;
        return true;
    }

    sys->cfg = cfg;
    if (arena_init(&sys->arena, 28u * 1024u * 1024u) != PSX_OK) {
        return false;
    }
    sys->bus.ram = (u8 *)arena_alloc(&sys->arena, PSX_RAM_SIZE, 4096u);
    sys->bus.scratch_page = (u8 *)arena_alloc(&sys->arena, 4096u, 4096u);
    sys->bus.bios = (u8 *)arena_alloc(&sys->arena, PSX_BIOS_SIZE, 4096u);
    sys->bus.fm_r = (uintptr_t *)arena_alloc(&sys->arena, (u32)PSX_FM_BYTES, 4096u);
    sys->bus.fm_w = (uintptr_t *)arena_alloc(&sys->arena, (u32)PSX_FM_BYTES, 4096u);
    sys->bus.ram_size_reg = PSX_RAM_SIZE_DEFAULT;
    build_standalone_fastmem(&sys->bus);

    sys->cpu.sys.p = sys;
    sys->cpu.ram_base.p = sys->bus.ram;
    sys->cpu.scratch_base.p = sys->bus.scratch_page;
    sys->cpu.fastmem_r.p = sys->bus.fm_r;
    sys->cpu.fastmem_w.p = sys->bus.fm_w;
    sys->cpu.cop0[COP0_SR] = PSX_SR_CU2;
    sys->cpu.cop0[COP0_PRID] = PSX_COP0_PRID_VALUE;

    jit_init(sys);
    return true;
}

static void test_sys_shutdown(PsxSystem *sys, TestEnv *env) {
    if (env->full_sys_init) {
        psx_system_shutdown(sys);
    } else {
        jit_shutdown(sys);
        arena_destroy(&sys->arena);
    }
}

static u32 xorshift32(u32 *state) {
    u32 x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* ============================================================================
 * Test 1: ARM11v6K `jit_gte_div_unr_asm` Exact Reciprocal Division Leaf
 * ============================================================================ */
#if PSX3DS_HAVE_JIT
static int test_div_unr_asm(void) {
    typedef u64 (*UnrAsmFn)(u32 h, u32 sz3, const u8 *unr_table);
    UnrAsmFn asm_fn = (UnrAsmFn)(uintptr_t)jit_gte_div_unr_asm;
    const u8 *tbl = jit_gte_unr_table_ptr();

    static const u16 k_edges[] = {
        0u, 1u, 2u, 3u, 0x7Fu, 0x80u, 0x81u, 0xFFu,
        0x100u, 0x7FFFu, 0x8000u, 0x8001u, 0xFFFEu, 0xFFFFu
    };
    u32 n_edges = (u32)(sizeof(k_edges) / sizeof(k_edges[0]));
    for (u32 i = 0u; i < n_edges; ++i) {
        for (u32 j = 0u; j < n_edges; ++j) {
            u32 h = k_edges[i];
            u32 sz = k_edges[j];
            bool ref_ov = false;
            u32 ref_u = jit_gte_div_unr_wrap(h, sz, &ref_ov);
            u64 got64 = asm_fn(h, sz, tbl);
            u32 got_u = (u32)(got64 & 0xFFFFFFFFu);
            u32 got_ov = (u32)(got64 >> 32);
            if (got_u != ref_u || got_ov != (ref_ov ? 1u : 0u)) {
                fprintf(stderr, "UNR mismatch (h=0x%04lX, sz3=0x%04lX): ref=(0x%lX,%d) got=(0x%lX,%lu)\n",
                        (unsigned long)h, (unsigned long)sz,
                        (unsigned long)ref_u, (int)ref_ov,
                        (unsigned long)got_u, (unsigned long)got_ov);
                return 1;
            }
        }
    }

    u32 rng = 0x9E3779B9u;
    for (u32 iter = 0u; iter < 5000u; ++iter) {
        u32 r = xorshift32(&rng);
        u32 h = r & 0xFFFFu;
        u32 sz = (r >> 16) & 0xFFFFu;
        bool ref_ov = false;
        u32 ref_u = jit_gte_div_unr_wrap(h, sz, &ref_ov);
        u64 got64 = asm_fn(h, sz, tbl);
        u32 got_u = (u32)(got64 & 0xFFFFFFFFu);
        u32 got_ov = (u32)(got64 >> 32);
        if (got_u != ref_u || got_ov != (ref_ov ? 1u : 0u)) {
            fprintf(stderr, "UNR random mismatch (h=0x%04lX, sz3=0x%04lX): ref=(0x%lX,%d) got=(0x%lX,%lu)\n",
                    (unsigned long)h, (unsigned long)sz,
                    (unsigned long)ref_u, (int)ref_ov,
                    (unsigned long)got_u, (unsigned long)got_ov);
            return 1;
        }
    }
    return 0;
}
#endif

/* ============================================================================
 * Test 2: Inline COP2 Transfers (`SXYP`, `IRGB/ORGB`, `LZCS/LZCR`, `FLAG`)
 *         & Cached MIPS GPR Spill/Reload Integrity Around Inline GTE
 * ============================================================================ */
static int test_cop2_transfers_and_gpr_integrity(void) {
    const u32 base = 0x80010000u;
    const u32 prog[] = {
        /* Populate 8 live MIPS GPRs ($s0..$s7) to stress all 7 register-allocator slots */
        M_LUI(16u, 0x1111u), M_ORI(16u, 16u, 0x2222u), /* s0 */
        M_LUI(17u, 0x3333u), M_ORI(17u, 17u, 0x4444u), /* s1 */
        M_LUI(18u, 0x5555u), M_ORI(18u, 18u, 0x6666u), /* s2 */
        M_LUI(19u, 0x7777u), M_ORI(19u, 19u, 0x8888u), /* s3 */
        M_LUI(20u, 0x9999u), M_ORI(20u, 20u, 0xAAAAu), /* s4 */
        M_LUI(21u, 0xBBBBu), M_ORI(21u, 21u, 0xCCCCu), /* s5 */
        M_LUI(22u, 0xDDDDu), M_ORI(22u, 22u, 0xEEEEu), /* s6 */
        M_LUI(23u, 0x8001u), M_ORI(23u, 23u, 0x2000u), /* s7 = RAM ptr */

        /* 1. SXYP (d14) FIFO push 3 coordinates -> SXY0, SXY1, SXY2 */
        M_LUI(8u, 0x0010u), M_ORI(8u, 8u, 0x0010u),
        M_MTC2(8u, 14u),
        M_LUI(8u, 0x0020u), M_ORI(8u, 8u, 0x0050u),
        M_MTC2(8u, 14u),
        M_LUI(8u, 0x0040u), M_ORI(8u, 8u, 0x0010u),
        M_MTC2(8u, 14u),

        /* Inline NCLIP (must clobber ARM regs without corrupting $s0..$s7!) */
        M_GTE(0u, 0u, 0u, 0u, 0u, 0x06u),
        M_MFC2(9u, 24u), /* MAC0 */
        M_NOP,
        M_SW(9u, 0, 23u),

        /* Lighting / colour / projection kernels with 8 live GPRs: r12 / lr slots stay
         * live (not spilled) across inline GTE, so any r12 / lr clobber corrupts $s* */
        M_GTE(1u, 0u, 0u, 0u, 0u, 0x13u), /* NCDS */
        M_GTE(1u, 0u, 0u, 0u, 1u, 0x3Fu), /* NCCT */
        M_GTE(0u, 0u, 0u, 0u, 0u, 0x0Cu), /* OP   */
        M_GTE(1u, 0u, 0u, 0u, 0u, 0x2Au), /* DPCT */
        M_GTE(1u, 0u, 0u, 0u, 0u, 0x30u), /* RTPT */
        M_GTE(1u, 0u, 0u, 0u, 1u, 0x1Cu), /* CC   */
        M_GTE(1u, 0u, 0u, 0u, 0u, 0x20u), /* NCT  */

        /* 2. IRGB (d28) write -> ORGB (d29) read & IR1..3 check */
        M_ORI(8u, 0u, (29u << 10) | (15u << 5) | 3u),
        M_MTC2(8u, 28u),
        M_MFC2(10u, 29u),
        M_MFC2(11u, 9u),
        M_NOP,
        M_SW(10u, 4, 23u),
        M_SW(11u, 8, 23u),

        /* 3. LZCS (d30) -> LZCR (d31) on 0, -1, +1, 0x80000000, 0xC0000000 */
        M_MTC2(0u, 30u),
        M_MFC2(12u, 31u),
        M_NOP,
        M_SW(12u, 12, 23u),

        M_ADDIU(8u, 0u, -1),
        M_MTC2(8u, 30u),
        M_MFC2(12u, 31u),
        M_NOP,
        M_SW(12u, 16, 23u),

        M_LUI(8u, 0xC000u),
        M_MTC2(8u, 30u),
        M_MFC2(12u, 31u),
        M_NOP,
        M_SW(12u, 20, 23u),

        /* 4. H (c26) 0xFF80 sign-extended on CFC2, FLAG (c31) bit-31 summary */
        M_ORI(8u, 0u, 0xFF80u),
        M_CTC2(8u, 26u),
        M_CFC2(13u, 26u),
        M_LUI(8u, 0x0004u), /* bit 18 in 0x7F87E000 -> sets bit 31! */
        M_CTC2(8u, 31u),
        M_CFC2(14u, 31u),
        M_NOP,
        M_SW(13u, 24, 23u),
        M_SW(14u, 28, 23u),

        /* 5. LWC2 & SWC2 round-trip + combine $s0..$s6 to prove zero corruption */
        M_SWC2(14u, 32, 23u),
        M_LWC2(1u, 32, 23u),
        M_XOR(15u, 16u, 17u),
        M_XOR(15u, 15u, 18u),
        M_XOR(15u, 15u, 19u),
        M_XOR(15u, 15u, 20u),
        M_XOR(15u, 15u, 21u),
        M_XOR(15u, 15u, 22u),
        M_SW(15u, 36, 23u),

        /* halt */
        M_BEQ(0u, 0u, -1),
        M_NOP
    };

    u32 n_words = (u32)(sizeof(prog) / sizeof(prog[0]));
    u32 halt_va = base + (n_words - 2u) * 4u;

    for (int i = 0; i < 3; ++i) {
        jit_flush_all(&g_sys[i]);
        memset(g_sys[i].bus.ram, 0, 0x20000u);
        memset(&g_sys[i].stats, 0, sizeof(g_sys[i].stats));
        memset(g_sys[i].cpu.gpr, 0, sizeof(g_sys[i].cpu.gpr));
        memset(g_sys[i].cpu.gte_d, 0, sizeof(g_sys[i].cpu.gte_d));
        memset(g_sys[i].cpu.gte_c, 0, sizeof(g_sys[i].cpu.gte_c));
        g_sys[i].cpu.cop0[COP0_SR] = PSX_SR_CU2;
        g_sys[i].cpu.pc = base;
        g_sys[i].cpu.next_pc = base + 4u;
        memcpy(g_sys[i].bus.ram + 0x10000u, prog, sizeof(prog));
    }

    for (u32 step = 0u; step < 500u; ++step) {
        if (g_sys[0].cpu.pc == halt_va && g_sys[0].cpu.load_delay_reg == 0u) break;
        g_sys[0].cpu.cycles_left = 100;
        g_sys[0].cpu.slice_len = 100;
        jit_step_one_wrap(&g_sys[0]);
    }
    for (int eng = 1; eng <= 2; ++eng) {
        for (u32 s = 0u; s < 16u; ++s) {
            if (g_sys[eng].cpu.pc == halt_va && g_sys[eng].cpu.load_delay_reg == 0u) break;
            g_sys[eng].cpu.cycles_left = 4096;
            g_sys[eng].cpu.slice_len = 4096;
            if (eng == 1) cpu_cached_run(&g_sys[eng]);
            else jit_run(&g_sys[eng]);
        }
    }

    for (int eng = 1; eng <= 2; ++eng) {
        CHECK_TRUE(memcmp(g_sys[0].cpu.gpr, g_sys[eng].cpu.gpr, sizeof(g_sys[0].cpu.gpr)) == 0,
                   "GPR[0..31] bit-for-bit identical after inline GTE + transfers");
        CHECK_TRUE(memcmp(g_sys[0].cpu.gte_d, g_sys[eng].cpu.gte_d, sizeof(g_sys[0].cpu.gte_d)) == 0,
                   "gte_d[0..31] identical after COP2 transfers");
        CHECK_TRUE(memcmp(g_sys[0].cpu.gte_c, g_sys[eng].cpu.gte_c, sizeof(g_sys[0].cpu.gte_c)) == 0,
                   "gte_c[0..31] identical after COP2 transfers");
        CHECK_TRUE(memcmp(g_sys[0].bus.ram + 0x12000u, g_sys[eng].bus.ram + 0x12000u, 40u) == 0,
                   "RAM outputs identical after COP2 transfers");
    }
    return 0;
}

/* ============================================================================
 * Test 3: Differential Fuzzing & Adversarial Suite Across ALL 22 GTE Opcodes
 * ============================================================================ */
static void populate_random_gte_state(PsxCpu *c, u32 *rng, u32 mode) {
    static const s16 k_interesting_s16[16] = {
        0, 1, -1, 2, -2, 64, -64, 4096,
        -4096, 16384, -16384, 32766, 32767, -32767, -32768, 12345
    };

    for (u32 r = 0u; r < 32u; ++r) {
        u32 x = xorshift32(rng);
        if (mode == 0u) {
            /* Adversarial extreme corners (saturations, -0x8000 * -0x8000, etc.) */
            u16 lo = (u16)k_interesting_s16[x & 15u];
            u16 hi = (u16)k_interesting_s16[(x >> 4) & 15u];
            jit_gte_write_data_wrap(c, r, ((u32)hi << 16) | lo);
        } else {
            jit_gte_write_data_wrap(c, r, x);
        }
    }
    for (u32 r = 0u; r < 32u; ++r) {
        u32 x = xorshift32(rng);
        u32 val = x;
        if (mode == 0u) {
            u16 lo = (u16)k_interesting_s16[x & 15u];
            u16 hi = (u16)k_interesting_s16[(x >> 4) & 15u];
            val = ((u32)hi << 16) | lo;
        }
        /* Exercise both fast inline |TR/BK/FC| < 2^30 AND >= 2^30 44-bit overflow bounce! */
        if ((r >= 5u && r <= 7u) || (r >= 13u && r <= 15u) || (r >= 21u && r <= 23u)) {
            u32 sel = (x >> 28) & 3u;
            if (sel == 0u) {
                val = (u32)((s32)(s16)(x & 0xFFFFu)); /* small signed translation */
            } else if (sel == 1u) {
                val = ((s32)x) >> 3;                  /* within +-2^28 (inline path) */
            } else if (sel == 2u) {
                val = 0x70000000u + (x & 0x0FFFFFFFu);/* >= +2^30 (44-bit overflow bounce!) */
            } else {
                val = 0x90000000u - (x & 0x0FFFFFFFu);/* <= -2^30 (44-bit overflow bounce!) */
            }
        }
        jit_gte_write_ctrl_wrap(c, r, val);
    }
    /* Ensure SXYP mirror invariant d[14] == d[15] */
    c->gte_d[14] = c->gte_d[15];
}

static int run_single_gte_diff(u32 opcode, const PsxCpu *init_state, const char *desc) {
    const u32 base = 0x80010000u;
    const u32 halt_va = base + 4u;
    const u32 code[3] = {
        opcode,
        M_BEQ(0u, 0u, -1),
        M_NOP
    };

    /* Engine 0: Direct reference C GTE */
    memcpy(g_sys[0].cpu.gte_d, init_state->gte_d, sizeof(init_state->gte_d));
    memcpy(g_sys[0].cpu.gte_c, init_state->gte_c, sizeof(init_state->gte_c));
    jit_gte_exec_wrap(&g_sys[0].cpu, opcode);

    /* Engine 1 (cpu_cached) & Engine 2 (arm_jit with jit_inline_gte = true) */
    for (int eng = 1; eng <= 2; ++eng) {
        PsxSystem *s = &g_sys[eng];
        memcpy(s->bus.ram + 0x10000u, code, sizeof(code));
        jit_invalidate_page(s, 0x10u);
        memcpy(s->cpu.gte_d, init_state->gte_d, sizeof(init_state->gte_d));
        memcpy(s->cpu.gte_c, init_state->gte_c, sizeof(init_state->gte_c));
        s->cpu.cop0[COP0_SR] = PSX_SR_CU2;
        s->cpu.pc = base;
        s->cpu.next_pc = base + 4u;
        s->cpu.in_delay_slot = 0u;
        s->cpu.load_delay_reg = 0u;
        s->cpu.cycles_left = 256;
        s->cpu.slice_len = 256;
        if (eng == 1) {
            cpu_cached_run(s);
        } else {
            jit_run(s);
        }

        const char *ename = (eng == 1) ? "cpu_cached" : "arm_jit_inline_gte";
        if (s->cpu.pc != halt_va) {
            fprintf(stderr, "[%s/%s] unexpected PC 0x%08lX\n",
                    desc, ename, (unsigned long)s->cpu.pc);
            return 1;
        }
        for (u32 r = 0u; r < 32u; ++r) {
            if (s->cpu.gte_d[r] != g_sys[0].cpu.gte_d[r]) {
                fprintf(stderr, "[%s/%s] op=0x%08lX gte_d[%lu] mismatch: ref=0x%08lX got=0x%08lX\n",
                        desc, ename, (unsigned long)opcode, (unsigned long)r,
                        (unsigned long)g_sys[0].cpu.gte_d[r], (unsigned long)s->cpu.gte_d[r]);
                return 1;
            }
        }
        for (u32 r = 0u; r < 32u; ++r) {
            if (s->cpu.gte_c[r] != g_sys[0].cpu.gte_c[r]) {
                fprintf(stderr, "[%s/%s] op=0x%08lX gte_c[%lu] mismatch: ref=0x%08lX got=0x%08lX\n",
                        desc, ename, (unsigned long)opcode, (unsigned long)r,
                        (unsigned long)g_sys[0].cpu.gte_c[r], (unsigned long)s->cpu.gte_c[r]);
                return 1;
            }
        }
    }
    return 0;
}

static int test_differential_all_gte_opcodes(void) {
    static const u8 k_cmds[] = {
        0x01u /*RTPS*/,  0x06u /*NCLIP*/, 0x0Cu /*OP*/,    0x10u /*DPCS*/,
        0x11u /*INTPL*/, 0x13u /*NCDS*/,  0x14u /*CDP*/,   0x16u /*NCDT*/,
        0x1Bu /*NCCS*/,  0x1Cu /*CC*/,    0x1Eu /*NCS*/,   0x20u /*NCT*/,
        0x28u /*SQR*/,   0x29u /*DCPL*/,  0x2Au /*DPCT*/,  0x2Du /*AVSZ3*/,
        0x2Eu /*AVSZ4*/, 0x30u /*RTPT*/,  0x3Du /*GPF*/,   0x3Eu /*GPL*/,
        0x3Fu /*NCCT*/
    };
    const u32 n_cmds = (u32)(sizeof(k_cmds) / sizeof(k_cmds[0]));

    jit_flush_all(&g_sys[1]);
    jit_flush_all(&g_sys[2]);
    g_sys[2].stats.gte_inline = 0u;

    PsxCpu init_cpu;
    memset(&init_cpu, 0, sizeof(init_cpu));
    u32 rng = 0xCAFE1234u;

    /* 1. Test all 21 non-MVMVA commands across (sf=0..1, lm=0..1) x 256 states
     *    (1,024 adversarial/random vectors per opcode, ~21.5k runs per engine) */
    for (u32 ci = 0u; ci < n_cmds; ++ci) {
        u8 cmd = k_cmds[ci];
        u32 inline_before = g_sys[2].stats.gte_inline;
        for (u32 sf = 0u; sf <= 1u; ++sf) {
            for (u32 lm = 0u; lm <= 1u; ++lm) {
                u32 op = M_GTE(sf, 0u, 0u, 0u, lm, cmd);
                for (u32 trial = 0u; trial < 256u; ++trial) {
                    populate_random_gte_state(&init_cpu, &rng, trial & 1u);
                    if (run_single_gte_diff(op, &init_cpu, "gte_cmd_fuzz") != 0) {
                        return 1;
                    }
                    /* Avoid exhausting 4096-block pool across thousands of invalidated single-insn blocks */
                    if (g_sys[2].jit.n_blocks > 3500u || g_sys[1].jit.n_blocks > 3500u) {
                        jit_flush_all(&g_sys[1]);
                        jit_flush_all(&g_sys[2]);
                    }
                }
            }
        }
#if PSX3DS_HAVE_JIT
        /* Every non-MVMVA command now has an inline ARM11v6K kernel (100% inline GTE) */
        CHECK_TRUE(jit_can_inline_gte(M_GTE(0u, 0u, 0u, 0u, 0u, cmd)), "jit_can_inline_gte(cmd)");
        if (g_sys[2].stats.gte_inline == inline_before) {
            fprintf(stderr, "cmd 0x%02X never took the inline path\n", (unsigned)cmd);
            return 1;
        }
#else
        (void)inline_before;
#endif
    }

    /* 2. Exhaustive MVMVA sweep: all 64 (sf, lm, mx=0..3, vx=0..3, cv=0..3) x 10 states (640 runs) */
    for (u32 sf = 0u; sf <= 1u; ++sf) {
        for (u32 lm = 0u; lm <= 1u; ++lm) {
            for (u32 mx = 0u; mx < 4u; ++mx) {
                for (u32 vx = 0u; vx < 4u; ++vx) {
                    for (u32 cv = 0u; cv < 4u; ++cv) {
                        u32 op = M_GTE(sf, mx, vx, cv, lm, 0x12u);
                        for (u32 trial = 0u; trial < 10u; ++trial) {
                            populate_random_gte_state(&init_cpu, &rng, trial & 1u);
                            if (run_single_gte_diff(op, &init_cpu, "gte_mvmva_fuzz") != 0) {
                                return 1;
                            }
                            if (g_sys[2].jit.n_blocks > 3500u || g_sys[1].jit.n_blocks > 3500u) {
                                jit_flush_all(&g_sys[1]);
                                jit_flush_all(&g_sys[2]);
                            }
                        }
                    }
                }
            }
        }
    }

#if PSX3DS_HAVE_JIT
    CHECK_TRUE(g_sys[2].stats.gte_inline > 0u, "stats.gte_inline incremented by inline GTE");
#endif
    return 0;
}

/* ============================================================================
 * Test 3b: Inline GTE code-size bound (scanner budget must never under-estimate)
 * ============================================================================ */
#if PSX3DS_HAVE_JIT
static Emitter g_size_emit;
static u32 g_size_buf[2048];

static int test_gte_inline_code_size(void) {
    u32 worst_all = 0u;
    for (u32 cmd = 0u; cmd < 64u; ++cmd) {
        u32 worst = 0u;
        bool any = false;
        for (u32 v = 0u; v < 256u; ++v) {
            u32 op = M_GTE(v & 1u, (v >> 1) & 3u, (v >> 3) & 3u, (v >> 5) & 3u, (v >> 7) & 1u, cmd);
            if (!jit_can_inline_gte(op)) {
                continue;
            }
            arm_emit_reset(&g_size_emit, g_size_buf, (u32)sizeof(g_size_buf));
            CHECK_TRUE(jit_emit_gte_inline(&g_size_emit, op), "jit_emit_gte_inline emitted");
            /* Instruction words only: the literal pool appended by finalize is data */
            u32 code_words = arm_emit_offset_words(&g_size_emit);
            u32 bytes = 0u;
            CHECK_TRUE(arm_emit_finalize(&g_size_emit, &bytes), "arm_emit_finalize");
            u32 words = bytes / 4u;
            if (words > jit_gte_inline_words(op)) {
                fprintf(stderr, "op=0x%08lX emits %lu words > estimate %lu\n", (unsigned long)op,
                        (unsigned long)words, (unsigned long)jit_gte_inline_words(op));
                return 1;
            }
            /* No r12 / lr writes inside inline kernels (cached GPR slots 5/6 stay live) */
            for (u32 w = 0u; w < code_words; ++w) {
                u32 ins = g_size_buf[w];
                bool is_push_pop = ((ins & 0x0FFF0000u) == 0x092D0000u) ||
                                   ((ins & 0x0FFF0000u) == 0x08BD0000u);
                bool is_bl = ((ins & 0x0F000000u) == 0x0B000000u) ||
                             ((ins & 0x0FFFFFF0u) == 0x012FFF30u);
                bool dp = ((ins & 0x0C000000u) == 0u) && ((ins & 0x0FC000F0u) != 0x00000090u) &&
                          ((ins & 0x0F8000F0u) != 0x00800090u) && ((ins & 0x0F900000u) != 0x01000000u);
                u32 rd = (ins >> 12) & 15u;
                if (dp && !is_push_pop && !is_bl && (rd == 12u || rd == 14u)) {
                    /* Allowed only as `ldr r12, =fn` / `blx r12` far-call sequence inside emit_call */
                    bool next_is_blx = (w + 1u < code_words) && ((g_size_buf[w + 1u] & 0x0FFFFFFFu) == 0x012FFF3Cu);
                    if (!next_is_blx) {
                        fprintf(stderr, "op=0x%08lX writes r%lu at word %lu (0x%08lX)\n",
                                (unsigned long)op, (unsigned long)rd, (unsigned long)w, (unsigned long)ins);
                        return 1;
                    }
                }
            }
            any = true;
            if (words > worst) worst = words;
        }
        if (any) {
            printf("  gte cmd 0x%02lX: max %lu words (budget %lu)\n", (unsigned long)cmd,
                   (unsigned long)worst, (unsigned long)jit_gte_inline_words(M_GTE(0u, 0u, 0u, 0u, 0u, cmd)));
            if (worst > worst_all) worst_all = worst;
        }
    }
    CHECK_TRUE(worst_all * 4u + 512u < JIT_MAX_BLOCK_BYTES, "largest inline GTE kernel fits a block");
    return 0;
}
#endif

/* ============================================================================
 * Test 4: Geometry Pipeline Microbenchmark (`RTPT + NCLIP + AVSZ3 + MVMVA`)
 * ============================================================================ */
static int test_gte_microbenchmark(void) {
    const u32 base = 0x80010000u;
    const u32 iters = 15000u;
    const u32 code[] = {
        /* 0 */  M_ORI(16u, 0u, iters),             /* s0 = loop count */
        /* 1: loop */
        /* 1 */  M_GTE(1u, 0u, 0u, 0u, 0u, 0x30u),  /* RTPT sf=1 */
        /* 2 */  M_GTE(0u, 0u, 0u, 0u, 0u, 0x06u),  /* NCLIP */
        /* 3 */  M_MFC2(8u, 24u),                   /* MAC0 */
        /* 4 */  M_GTE(1u, 0u, 0u, 0u, 1u, 0x2Du),  /* AVSZ3 */
        /* 5 */  M_GTE(1u, 0u, 0u, 0u, 1u, 0x12u),  /* MVMVA RT*V0+TR */
        /* 6 */  M_ADDU(17u, 17u, 8u),
        /* 7 */  M_ADDIU(16u, 16u, -1),
        /* 8 */  M_BNE(16u, 0u, -8),                /* -> 1 (loop) */
        /* 9 */  M_NOP,
        /* 10: halt */
        /* 10 */ M_BEQ(0u, 0u, -1),
        /* 11 */ M_NOP
    };
    const u32 halt_va = base + 10u * 4u;

    u32 rng = 0x778899AAu;
    PsxCpu seed_cpu;
    memset(&seed_cpu, 0, sizeof(seed_cpu));
    populate_random_gte_state(&seed_cpu, &rng, 1u);
    /* Keep TR within normal game range so inline fastpath executes */
    seed_cpu.gte_c[5] = 120u;
    seed_cpu.gte_c[6] = -80;
    seed_cpu.gte_c[7] = 1500u;
    seed_cpu.gte_c[26] = 512u; /* H */

    double ms_oracle = 0.0, ms_cached = 0.0, ms_jit_helper = 0.0, ms_jit_inline = 0.0;

    /* 1. Oracle */
    jit_flush_all(&g_sys[0]);
    memcpy(g_sys[0].bus.ram + 0x10000u, code, sizeof(code));
    memset(g_sys[0].cpu.gpr, 0, sizeof(g_sys[0].cpu.gpr));
    memcpy(g_sys[0].cpu.gte_d, seed_cpu.gte_d, sizeof(seed_cpu.gte_d));
    memcpy(g_sys[0].cpu.gte_c, seed_cpu.gte_c, sizeof(seed_cpu.gte_c));
    g_sys[0].cpu.cop0[COP0_SR] = PSX_SR_CU2;
    g_sys[0].cpu.pc = base;
    g_sys[0].cpu.next_pc = base + 4u;
    clock_t t0 = clock();
    for (u32 step = 0u; step < iters * 12u + 64u; ++step) {
        if (g_sys[0].cpu.pc == halt_va) break;
        g_sys[0].cpu.cycles_left = 1000;
        g_sys[0].cpu.slice_len = 1000;
        jit_step_one_wrap(&g_sys[0]);
    }
    ms_oracle = 1000.0 * (double)(clock() - t0) / (double)CLOCKS_PER_SEC;

    /* 2. Cached interpreter */
    jit_flush_all(&g_sys[1]);
    memcpy(g_sys[1].bus.ram + 0x10000u, code, sizeof(code));
    memset(g_sys[1].cpu.gpr, 0, sizeof(g_sys[1].cpu.gpr));
    memcpy(g_sys[1].cpu.gte_d, seed_cpu.gte_d, sizeof(seed_cpu.gte_d));
    memcpy(g_sys[1].cpu.gte_c, seed_cpu.gte_c, sizeof(seed_cpu.gte_c));
    g_sys[1].cpu.cop0[COP0_SR] = PSX_SR_CU2;
    g_sys[1].cpu.pc = base;
    g_sys[1].cpu.next_pc = base + 4u;
    t0 = clock();
    while (g_sys[1].cpu.pc != halt_va) {
        g_sys[1].cpu.cycles_left = 2000000;
        g_sys[1].cpu.slice_len = 2000000;
        cpu_cached_run(&g_sys[1]);
    }
    ms_cached = 1000.0 * (double)(clock() - t0) / (double)CLOCKS_PER_SEC;

    /* 3. ARM JIT with helper GTE (`jit_inline_gte = false`) */
    g_sys[2].cfg.jit_inline_gte = false;
    jit_flush_all(&g_sys[2]);
    memcpy(g_sys[2].bus.ram + 0x10000u, code, sizeof(code));
    memset(g_sys[2].cpu.gpr, 0, sizeof(g_sys[2].cpu.gpr));
    memcpy(g_sys[2].cpu.gte_d, seed_cpu.gte_d, sizeof(seed_cpu.gte_d));
    memcpy(g_sys[2].cpu.gte_c, seed_cpu.gte_c, sizeof(seed_cpu.gte_c));
    g_sys[2].cpu.cop0[COP0_SR] = PSX_SR_CU2;
    g_sys[2].cpu.pc = base;
    g_sys[2].cpu.next_pc = base + 4u;
    t0 = clock();
    while (g_sys[2].cpu.pc != halt_va) {
        g_sys[2].cpu.cycles_left = 2000000;
        g_sys[2].cpu.slice_len = 2000000;
        jit_run(&g_sys[2]);
    }
    ms_jit_helper = 1000.0 * (double)(clock() - t0) / (double)CLOCKS_PER_SEC;
    CHECK_TRUE(g_sys[2].cpu.gpr[17] == g_sys[0].cpu.gpr[17], "helper GTE loop checksum");

    /* 4. ARM JIT with inline ARM11v6K GTE (`jit_inline_gte = true`) */
    g_sys[2].cfg.jit_inline_gte = true;
    jit_flush_all(&g_sys[2]);
    memcpy(g_sys[2].bus.ram + 0x10000u, code, sizeof(code));
    memset(g_sys[2].cpu.gpr, 0, sizeof(g_sys[2].cpu.gpr));
    memcpy(g_sys[2].cpu.gte_d, seed_cpu.gte_d, sizeof(seed_cpu.gte_d));
    memcpy(g_sys[2].cpu.gte_c, seed_cpu.gte_c, sizeof(seed_cpu.gte_c));
    g_sys[2].cpu.cop0[COP0_SR] = PSX_SR_CU2;
    g_sys[2].cpu.pc = base;
    g_sys[2].cpu.next_pc = base + 4u;
    t0 = clock();
    while (g_sys[2].cpu.pc != halt_va) {
        g_sys[2].cpu.cycles_left = 2000000;
        g_sys[2].cpu.slice_len = 2000000;
        jit_run(&g_sys[2]);
    }
    ms_jit_inline = 1000.0 * (double)(clock() - t0) / (double)CLOCKS_PER_SEC;

    CHECK_TRUE(g_sys[1].cpu.gpr[17] == g_sys[0].cpu.gpr[17] &&
               g_sys[2].cpu.gpr[17] == g_sys[0].cpu.gpr[17],
               "inline GTE loop checksum matches oracle & cached");
    CHECK_TRUE(memcmp(g_sys[0].cpu.gte_d, g_sys[2].cpu.gte_d, sizeof(seed_cpu.gte_d)) == 0 &&
               memcmp(g_sys[0].cpu.gte_c, g_sys[2].cpu.gte_c, sizeof(seed_cpu.gte_c)) == 0,
               "final GTE state identical after 15,000 geometry pipeline iterations");

    printf("GTE BENCH [15k x (RTPT+NCLIP+AVSZ3+MVMVA)]: oracle=%.2f ms | cached=%.2f ms | jit_helper=%.2f ms | jit_inline=%.2f ms (%.2fx vs oracle, %.2fx vs helper)\n",
           ms_oracle, ms_cached, ms_jit_helper, ms_jit_inline,
           (ms_jit_inline > 0.001) ? (ms_oracle / ms_jit_inline) : 0.0,
           (ms_jit_inline > 0.001) ? (ms_jit_helper / ms_jit_inline) : 0.0);
    return 0;
}

int main(void) {
    for (int i = 0; i < 3; ++i) {
        CHECK_TRUE(test_sys_init(&g_sys[i], &g_env[i]), "test_sys_init");
    }

#if PSX3DS_HAVE_JIT
    if (test_div_unr_asm() != 0) return 1;
#endif
#if PSX3DS_HAVE_JIT
    if (test_gte_inline_code_size() != 0) return 1;
#endif
    if (test_cop2_transfers_and_gpr_integrity() != 0) return 1;
    if (test_differential_all_gte_opcodes() != 0) return 1;
    if (test_gte_microbenchmark() != 0) return 1;

    for (int i = 0; i < 3; ++i) {
        test_sys_shutdown(&g_sys[i], &g_env[i]);
    }
    printf("test_jit_gte PASS (have_jit=%d)\n", PSX3DS_HAVE_JIT);
    return 0;
}
