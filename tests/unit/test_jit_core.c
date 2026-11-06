/*
 * Comprehensive Unit & Differential Verification Suite for Slice B:
 * - Strict ARM11v6K Emitter (zero ARMv7 opcodes, Rd != Rm multiply rules)
 * - Block Cache, Pinned Vectors, & SMC Page Invalidation Lifecycle
 * - 3-Engine Differential R3000A Execution (Oracle vs Cached Interpreter vs ARM11 JIT)
 * - Microbenchmark (Step-One Oracle vs Cached Interpreter vs ARM11 Dynarec)
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

/* ---- MIPS-I Mini-Assembler Macros ---- */
#define R_ZERO 0u
#define R_AT   1u
#define R_V0   2u
#define R_V1   3u
#define R_A0   4u
#define R_A1   5u
#define R_A2   6u
#define R_A3   7u
#define R_T0   8u
#define R_T1   9u
#define R_T2   10u
#define R_T3   11u
#define R_T4   12u
#define R_T5   13u
#define R_T6   14u
#define R_T7   15u
#define R_S0   16u
#define R_S1   17u
#define R_S2   18u
#define R_S3   19u
#define R_S4   20u
#define R_S5   21u
#define R_S6   22u
#define R_S7   23u
#define R_T8   24u
#define R_T9   25u
#define R_K0   26u
#define R_K1   27u
#define R_GP   28u
#define R_SP   29u
#define R_FP   30u
#define R_RA   31u

#define M_SPEC(rs, rt, rd, sh, fn) \
    ((((u32)(rs) & 31u) << 21) | (((u32)(rt) & 31u) << 16) | \
     (((u32)(rd) & 31u) << 11) | (((u32)(sh) & 31u) << 6) | ((u32)(fn) & 63u))
#define M_IMM(op, rs, rt, imm) \
    ((((u32)(op) & 63u) << 26) | (((u32)(rs) & 31u) << 21) | \
     (((u32)(rt) & 31u) << 16) | ((u32)(imm) & 0xFFFFu))
#define M_JTYPE(op, target_va) \
    ((((u32)(op) & 63u) << 26) | ((((u32)(target_va)) >> 2) & 0x03FFFFFFu))

#define M_NOP                   0x00000000u
#define M_SLL(rd, rt, sh)       M_SPEC(0u, rt, rd, sh, 0x00u)
#define M_SRL(rd, rt, sh)       M_SPEC(0u, rt, rd, sh, 0x02u)
#define M_SRA(rd, rt, sh)       M_SPEC(0u, rt, rd, sh, 0x03u)
#define M_SLLV(rd, rt, rs)      M_SPEC(rs, rt, rd, 0u, 0x04u)
#define M_SRLV(rd, rt, rs)      M_SPEC(rs, rt, rd, 0u, 0x06u)
#define M_SRAV(rd, rt, rs)      M_SPEC(rs, rt, rd, 0u, 0x07u)
#define M_JR(rs)                M_SPEC(rs, 0u, 0u, 0u, 0x08u)
#define M_JALR(rd, rs)          M_SPEC(rs, 0u, rd, 0u, 0x09u)
#define M_SYSCALL               M_SPEC(0u, 0u, 0u, 0u, 0x0Cu)
#define M_BREAK                 M_SPEC(0u, 0u, 0u, 0u, 0x0Du)
#define M_MFHI(rd)              M_SPEC(0u, 0u, rd, 0u, 0x10u)
#define M_MTHI(rs)              M_SPEC(rs, 0u, 0u, 0u, 0x11u)
#define M_MFLO(rd)              M_SPEC(0u, 0u, rd, 0u, 0x12u)
#define M_MTLO(rs)              M_SPEC(rs, 0u, 0u, 0u, 0x13u)
#define M_MULT(rs, rt)          M_SPEC(rs, rt, 0u, 0u, 0x18u)
#define M_MULTU(rs, rt)         M_SPEC(rs, rt, 0u, 0u, 0x19u)
#define M_DIV(rs, rt)           M_SPEC(rs, rt, 0u, 0u, 0x1Au)
#define M_DIVU(rs, rt)          M_SPEC(rs, rt, 0u, 0u, 0x1Bu)
#define M_ADD(rd, rs, rt)       M_SPEC(rs, rt, rd, 0u, 0x20u)
#define M_ADDU(rd, rs, rt)      M_SPEC(rs, rt, rd, 0u, 0x21u)
#define M_SUB(rd, rs, rt)       M_SPEC(rs, rt, rd, 0u, 0x22u)
#define M_SUBU(rd, rs, rt)      M_SPEC(rs, rt, rd, 0u, 0x23u)
#define M_AND(rd, rs, rt)       M_SPEC(rs, rt, rd, 0u, 0x24u)
#define M_OR(rd, rs, rt)        M_SPEC(rs, rt, rd, 0u, 0x25u)
#define M_XOR(rd, rs, rt)       M_SPEC(rs, rt, rd, 0u, 0x26u)
#define M_NOR(rd, rs, rt)       M_SPEC(rs, rt, rd, 0u, 0x27u)
#define M_SLT(rd, rs, rt)       M_SPEC(rs, rt, rd, 0u, 0x2Au)
#define M_SLTU(rd, rs, rt)      M_SPEC(rs, rt, rd, 0u, 0x2Bu)

#define M_BLTZ(rs, off)         M_IMM(0x01u, rs, 0x00u, off)
#define M_BGEZ(rs, off)         M_IMM(0x01u, rs, 0x01u, off)
#define M_BLTZAL(rs, off)       M_IMM(0x01u, rs, 0x10u, off)
#define M_BGEZAL(rs, off)       M_IMM(0x01u, rs, 0x11u, off)
#define M_J(va)                 M_JTYPE(0x02u, va)
#define M_JAL(va)               M_JTYPE(0x03u, va)
#define M_BEQ(rs, rt, off)      M_IMM(0x04u, rs, rt, off)
#define M_BNE(rs, rt, off)      M_IMM(0x05u, rs, rt, off)
#define M_BLEZ(rs, off)         M_IMM(0x06u, rs, 0u, off)
#define M_BGTZ(rs, off)         M_IMM(0x07u, rs, 0u, off)

#define M_ADDI(rt, rs, imm)     M_IMM(0x08u, rs, rt, imm)
#define M_ADDIU(rt, rs, imm)    M_IMM(0x09u, rs, rt, imm)
#define M_SLTI(rt, rs, imm)     M_IMM(0x0Au, rs, rt, imm)
#define M_SLTIU(rt, rs, imm)    M_IMM(0x0Bu, rs, rt, imm)
#define M_ANDI(rt, rs, imm)     M_IMM(0x0Cu, rs, rt, imm)
#define M_ORI(rt, rs, imm)      M_IMM(0x0Du, rs, rt, imm)
#define M_XORI(rt, rs, imm)     M_IMM(0x0Eu, rs, rt, imm)
#define M_LUI(rt, imm)          M_IMM(0x0Fu, 0u, rt, imm)

#define M_MFC0(rt, rd)          ((((u32)0x10u) << 26) | (0x00u << 21) | (((u32)(rt) & 31u) << 16) | (((u32)(rd) & 31u) << 11))
#define M_MTC0(rt, rd)          ((((u32)0x10u) << 26) | (0x04u << 21) | (((u32)(rt) & 31u) << 16) | (((u32)(rd) & 31u) << 11))
#define M_RFE                   0x42000010u
#define M_COP1_NOP              0x46000000u
#define M_RSVD_OP               0xFC000000u /* major op 0x3F -> RI */

#define M_LB(rt, off, rs)       M_IMM(0x20u, rs, rt, off)
#define M_LH(rt, off, rs)       M_IMM(0x21u, rs, rt, off)
#define M_LWL(rt, off, rs)      M_IMM(0x22u, rs, rt, off)
#define M_LW(rt, off, rs)       M_IMM(0x23u, rs, rt, off)
#define M_LBU(rt, off, rs)      M_IMM(0x24u, rs, rt, off)
#define M_LHU(rt, off, rs)      M_IMM(0x25u, rs, rt, off)
#define M_LWR(rt, off, rs)      M_IMM(0x26u, rs, rt, off)
#define M_SB(rt, off, rs)       M_IMM(0x28u, rs, rt, off)
#define M_SH(rt, off, rs)       M_IMM(0x29u, rs, rt, off)
#define M_SWL(rt, off, rs)      M_IMM(0x2Au, rs, rt, off)
#define M_SW(rt, off, rs)       M_IMM(0x2Bu, rs, rt, off)
#define M_SWR(rt, off, rs)      M_IMM(0x2Eu, rs, rt, off)

/* ---- Test System Harness (works identically pre-merge & post-merge) ---- */
typedef struct TestEnv {
    bool full_sys_init;
} TestEnv;

static PSX_ALIGNED(32) PsxSystem g_sys[3];
static TestEnv g_env[3];

static void build_standalone_fastmem(PsxBus *b) {
    memset(b->fm_r, 0, PSX_FM_BYTES);
    memset(b->fm_w, 0, PSX_FM_BYTES);

    const u32 ram_segs[3] = { 0x00000u, 0x80000u, 0xA0000u };
    u32 max_pages = b->strict_ram_size ? 512u : 2048u;
    for (u32 s = 0u; s < 3u; ++s) {
        for (u32 p = 0u; p < max_pages; ++p) {
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

    const u32 bios_segs[3] = { 0x1FC00u, 0x9FC00u, 0xBFC00u };
    for (u32 s = 0u; s < 3u; ++s) {
        for (u32 p = 0u; p < 128u; ++p) {
            u32 vp = bios_segs[s] + p;
            b->fm_r[vp] = (uintptr_t)b->bios + ((uintptr_t)p << 12) - ((uintptr_t)vp << 12);
        }
    }
}

static bool test_sys_init(PsxSystem *sys, TestEnv *env) {
    memset(sys, 0, sizeof(*sys));
    memset(env, 0, sizeof(*env));

    PsxConfig cfg;
    psx_config_defaults(&cfg, true);
    cfg.hle_bios = false;
    cfg.cycle_mult = 256u; /* 1.0x exact integer cycles per instruction */
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
    sys->bus.strict_ram_size = cfg.strict_ram_size;
    build_standalone_fastmem(&sys->bus);

    sys->cpu.sys.p = sys;
    sys->cpu.ram_base.p = sys->bus.ram;
    sys->cpu.scratch_base.p = sys->bus.scratch_page;
    sys->cpu.fastmem_r.p = sys->bus.fm_r;
    sys->cpu.fastmem_w.p = sys->bus.fm_w;
    sys->cpu.cop0[COP0_SR] = PSX_SR_CU2;
    sys->cpu.cop0[COP0_PRID] = PSX_COP0_PRID_VALUE;
    sys->gpu.gpustat = 0x1C002000u;

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

static void test_sys_prepare(PsxSystem *sys, u32 start_pc) {
    jit_flush_all(sys);
    memset(sys->bus.ram, 0, 0x40000u);
    memset(sys->bus.scratch_page, 0, 4096u);
    memset(&sys->stats, 0, sizeof(sys->stats));

    memset(sys->cpu.gpr, 0, sizeof(sys->cpu.gpr));
    sys->cpu.pc = start_pc;
    sys->cpu.next_pc = start_pc + 4u;
    sys->cpu.hi = 0u;
    sys->cpu.lo = 0u;
    sys->cpu.in_delay_slot = 0u;
    sys->cpu.branch_taken = 0u;
    sys->cpu.branch_target = 0u;
    sys->cpu.load_delay_reg = 0u;
    sys->cpu.load_delay_val = 0u;
    sys->cpu.irq_pending = 0u;
    sys->cpu.jit_abort = 0u;
    sys->cpu.exc_pending = 0u;
    memset(sys->cpu.cop0, 0, sizeof(sys->cpu.cop0));
    sys->cpu.cop0[COP0_SR] = PSX_SR_CU2; /* Kernel mode, BEV=0 (RAM vector 0x80000080), COP2 enabled */
    sys->cpu.cop0[COP0_PRID] = PSX_COP0_PRID_VALUE;
    if (sys->bus.isolated) {
        jit_set_sr_wrap(sys, PSX_SR_CU2);
    }
    sys->irq.stat = 0u;
    sys->irq.mask = 0u;
}

static void write_code(PsxSystem *sys, u32 phys_off, const u32 *words, u32 n) {
    memcpy(sys->bus.ram + phys_off, words, (size_t)n * sizeof(u32));
}

static u32 read_ram32(const PsxSystem *sys, u32 phys_off) {
    u32 v = 0u;
    memcpy(&v, sys->bus.ram + phys_off, 4u);
    return v;
}

static void write_ram32(PsxSystem *sys, u32 phys_off, u32 v) {
    memcpy(sys->bus.ram + phys_off, &v, 4u);
}

static void run_oracle_until_halt(PsxSystem *sys, u32 halt_pc, u32 max_steps) {
    sys->cpu.cycles_left = (s32)max_steps;
    sys->cpu.slice_len = (s32)max_steps;
    for (u32 step = 0u; step < max_steps; ++step) {
        if (sys->cpu.pc == halt_pc &&
            sys->cpu.in_delay_slot == 0u &&
            sys->cpu.load_delay_reg == 0u) {
            break;
        }
        sys->cpu.cycles_left = 1000;
        sys->cpu.slice_len = 1000;
        jit_step_one_wrap(sys);
    }
}

static void run_engine_until_halt(PsxSystem *sys, bool use_jit, u32 halt_pc, s32 slice_cycles, u32 max_slices) {
    for (u32 s = 0u; s < max_slices; ++s) {
        if (sys->cpu.pc == halt_pc &&
            sys->cpu.in_delay_slot == 0u &&
            sys->cpu.load_delay_reg == 0u) {
            break;
        }
        sys->cpu.cycles_left = slice_cycles;
        sys->cpu.slice_len = slice_cycles;
        if (use_jit) {
            jit_run(sys);
        } else {
            cpu_cached_run(sys);
        }
    }
}

static int compare_three_engines(const char *label, u32 halt_pc, u32 ram_check_off, u32 ram_check_words) {
    const PsxCpu *ref = &g_sys[0].cpu;
    for (int eng = 1; eng <= 2; ++eng) {
        const char *ename = (eng == 1) ? "cpu_cached" : "arm_jit";
        const PsxCpu *got = &g_sys[eng].cpu;
        if (ref->pc != halt_pc || got->pc != ref->pc) {
            fprintf(stderr, "[%s/%s] PC mismatch: ref=0x%08lX got=0x%08lX (halt=0x%08lX)\n",
                    label, ename, (unsigned long)ref->pc, (unsigned long)got->pc,
                    (unsigned long)halt_pc);
            return 1;
        }
        if (got->gpr[0] != 0u) {
            fprintf(stderr, "[%s/%s] $zero corrupted: 0x%08lX\n",
                    label, ename, (unsigned long)got->gpr[0]);
            return 1;
        }
        for (u32 r = 0u; r < 32u; ++r) {
            if (got->gpr[r] != ref->gpr[r]) {
                fprintf(stderr, "[%s/%s] GPR[%lu] mismatch: ref=0x%08lX got=0x%08lX\n",
                        label, ename, (unsigned long)r,
                        (unsigned long)ref->gpr[r], (unsigned long)got->gpr[r]);
                return 1;
            }
        }
        if (got->hi != ref->hi || got->lo != ref->lo) {
            fprintf(stderr, "[%s/%s] HI/LO mismatch: ref=(%08lX,%08lX) got=(%08lX,%08lX)\n",
                    label, ename, (unsigned long)ref->hi, (unsigned long)ref->lo,
                    (unsigned long)got->hi, (unsigned long)got->lo);
            return 1;
        }
        const u32 cop0_regs[5] = { COP0_JUMPDEST, COP0_BADVADDR, COP0_SR, COP0_CAUSE, COP0_EPC };
        for (u32 k = 0u; k < 5u; ++k) {
            u32 cr = cop0_regs[k];
            if (got->cop0[cr] != ref->cop0[cr]) {
                fprintf(stderr, "[%s/%s] COP0[%lu] mismatch: ref=0x%08lX got=0x%08lX\n",
                        label, ename, (unsigned long)cr,
                        (unsigned long)ref->cop0[cr], (unsigned long)got->cop0[cr]);
                return 1;
            }
        }
        for (u32 w = 0u; w < ram_check_words; ++w) {
            u32 off = ram_check_off + w * 4u;
            u32 rv = read_ram32(&g_sys[0], off);
            u32 gv = read_ram32(&g_sys[eng], off);
            if (gv != rv) {
                fprintf(stderr, "[%s/%s] RAM[0x%05lX] mismatch: ref=0x%08lX got=0x%08lX\n",
                        label, ename, (unsigned long)off,
                        (unsigned long)rv, (unsigned long)gv);
                return 1;
            }
        }
    }
    return 0;
}

/* ============================================================================
 * Test 1: Strict ARM11v6K Code Emitter & Zero-ARMv7 Instruction Audit
 * ============================================================================ */
#if PSX3DS_HAVE_JIT
static bool is_forbidden_armv7_word(u32 w) {
    u32 cond = w >> 28;
    if (cond == 0xFu) {
        /* Unconditional space: forbid ARMv7 DMB/DSB/ISB (0xF57FF0xx) */
        if ((w & 0xFFFFFF00u) == 0xF57FF000u) {
            return true;
        }
        return false;
    }
    /* MOVW / MOVT (ARMv6T2 / ARMv7) */
    if ((w & 0x0FF00000u) == 0x03000000u || (w & 0x0FF00000u) == 0x03400000u) {
        return true;
    }
    /* SBFX / UBFX (ARMv6T2 / ARMv7) */
    if ((w & 0x0FE00070u) == 0x07A00050u || (w & 0x0FE00070u) == 0x07E00050u) {
        return true;
    }
    /* BFI / BFC (ARMv6T2 / ARMv7) */
    if ((w & 0x0FE00070u) == 0x07C00010u) {
        return true;
    }
    /* SDIV / UDIV (ARMv7-R / ARMv7VE) */
    if ((w & 0x0FF000F0u) == 0x07100010u || (w & 0x0FF000F0u) == 0x07300010u) {
        return true;
    }
    return false;
}

static int audit_emitted_armv6k(const u32 *words, u32 n_words, const char *ctx) {
    for (u32 i = 0u; i < n_words; ++i) {
        u32 w = words[i];
        if (is_forbidden_armv7_word(w)) {
            fprintf(stderr, "FAIL [%s]: forbidden ARMv7 instruction 0x%08lX at word %lu\n",
                    ctx, (unsigned long)w, (unsigned long)i);
            return 1;
        }
        /* Check ARMv6 MUL (0x00000090) & SMULL/UMULL/SMLAL (0x00800090..0x00E00090) Rd != Rm */
        if ((w >> 28) != 0xFu && (w & 0x0FC000F0u) == 0x00000090u) {
            u32 rd = (w >> 16) & 15u;
            u32 rm = w & 15u;
            if (rd == rm) {
                fprintf(stderr, "FAIL [%s]: MUL Rd == Rm (r%lu) in 0x%08lX\n",
                        ctx, (unsigned long)rd, (unsigned long)w);
                return 1;
            }
        }
        if ((w >> 28) != 0xFu && (w & 0x0F8000F0u) == 0x00800090u) {
            u32 rdhi = (w >> 16) & 15u;
            u32 rdlo = (w >> 12) & 15u;
            u32 rm = w & 15u;
            if (rdhi == rm || rdlo == rm) {
                fprintf(stderr, "FAIL [%s]: 64-bit MULL RdHi/RdLo == Rm in 0x%08lX\n",
                        ctx, (unsigned long)w);
                return 1;
            }
        }
    }
    return 0;
}

static int test_emitter_armv6k_purity(PsxSystem *sys) {
    u32 *rw = (u32 *)sys->jit.mem.rw;
    u8  *rx = sys->jit.mem.rx ? sys->jit.mem.rx : sys->jit.mem.rw;
    ptrdiff_t rw_to_rx = rx - (u8 *)rw;

    const u32 k_consts[] = {
        0x00000000u, 0x000000FFu, 0xFF000000u, 0x80000002u,
        0xFFFFFFFFu, 0xFFFFFF00u, 0x12340000u, 0x00FF00FFu,
        0xFFFF1234u, 0x12345678u, 0x80010000u, 0xDEADBEEFu
    };

    for (u32 i = 0u; i < (u32)(sizeof(k_consts) / sizeof(k_consts[0])); ++i) {
        Emitter e;
        arm_emit_reset_dual(&e, rw, rw_to_rx, 1024u);
        emit_const(&e, ARM_R0, k_consts[i]);
        emit_bx(&e, ARM_CC_AL, ARM_LR);
        u32 code_words = arm_emit_offset_words(&e);
        CHECK_TRUE(code_words <= 3u, "emit_const <= 2 insns (+ bx lr)");
        CHECK_TRUE(audit_emitted_armv6k(rw, code_words, "emit_const") == 0, "emit_const ARMv6K");

        u32 total_bytes = 0u;
        CHECK_TRUE(arm_emit_finalize(&e, &total_bytes), "arm_emit_finalize const");
        jit_mem_commit(&sys->jit.mem, rw, total_bytes);

        typedef u32 (*ConstFn)(void);
        ConstFn fn = (ConstFn)(uintptr_t)rx;
        u32 got = fn();
        CHECK_TRUE(got == k_consts[i], "live ARM11 execution of emit_const");
    }

    /* Verify MUL / SMULL / UMULL / SMLAL Rd != Rm operand auto-swap */
    Emitter e;
    arm_emit_reset_dual(&e, rw, rw_to_rx, 1024u);
    emit_mul(&e, ARM_CC_AL, ARM_R0, ARM_R0, ARM_R1);
    emit_smull(&e, ARM_CC_AL, ARM_R0, ARM_R1, ARM_R0, ARM_R2);
    emit_umull(&e, ARM_CC_AL, ARM_R0, ARM_R1, ARM_R1, ARM_R2);
    emit_smlal(&e, ARM_CC_AL, ARM_R2, ARM_R3, ARM_R3, ARM_R0);
    CHECK_TRUE(audit_emitted_armv6k(rw, arm_emit_offset_words(&e), "mul_swap") == 0,
               "MUL/MULL Rd != Rm swap");

    /* Verify shared literal-pool load on final instruction before .ltorg (PC - 4 offset) */
    arm_emit_reset_dual(&e, rw, rw_to_rx, 1024u);
    emit_ldr_lit_cc(&e, ARM_CC_EQ, ARM_R0, 0x13579BDFu);
    emit_ldr_lit_cc(&e, ARM_CC_AL, ARM_R0, 0x13579BDFu);
    u32 lit_bytes = 0u;
    CHECK_TRUE(arm_emit_finalize(&e, &lit_bytes) && lit_bytes == 12u,
               "arm_emit_finalize handles negative -4 PC-relative literal offset");
    return 0;
}
#endif

/* ============================================================================
 * Test 2: Block Cache, Pinned Vectors, & SMC Page Invalidation Lifecycle
 * ============================================================================ */
static int test_block_cache_lifecycle(PsxSystem *sys) {
    jit_flush_all(sys);
    CHECK_TRUE(sys->jit.mem.rw != NULL, "jit_mem.rw allocated on all targets");
    CHECK_TRUE(sys->jit.tab_ram[BLOCKTAB_RAM_IDX(0xA0u)] == (void *)jit_bios_vec_stub, "0xA0 pinned");
    CHECK_TRUE(sys->jit.tab_ram[BLOCKTAB_RAM_IDX(0xB0u)] == (void *)jit_bios_vec_stub, "0xB0 pinned");
    CHECK_TRUE(sys->jit.tab_ram[BLOCKTAB_RAM_IDX(0xC0u)] == (void *)jit_bios_vec_stub, "0xC0 pinned");
    CHECK_TRUE(sys->jit.tab_ram[BLOCKTAB_RAM_IDX(PSX_SHELL_PC)] == (void *)jit_compile_stub,
               "0x80030000 unpinned when psexe_pending==0");

    sys->cpu.psexe_pending = 1u;
    jit_refresh_pins(sys);
    CHECK_TRUE(sys->jit.tab_ram[BLOCKTAB_RAM_IDX(PSX_SHELL_PC)] == (void *)jit_psexe_stub,
               "0x80030000 pinned when psexe_pending==1");
    sys->cpu.psexe_pending = 0u;
    jit_refresh_pins(sys);
    CHECK_TRUE(sys->jit.tab_ram[BLOCKTAB_RAM_IDX(PSX_SHELL_PC)] == (void *)jit_compile_stub,
               "0x80030000 restored when psexe_pending cleared");

    /* Register a synthetic 2-page block spanning physical pages 0x10 and 0x11 */
    u16 bidx = (u16)sys->jit.n_blocks++;
    JitBlock *b = &sys->jit.blocks[bidx];
    memset(b, 0, sizeof(*b));
    b->pc = 0x80010FF8u;
    b->phys_page[0] = JIT_PAGE_NONE;
    b->phys_page[1] = JIT_PAGE_NONE;
    b->next_in_page[0] = JIT_PAGE_NONE;
    b->next_in_page[1] = JIT_PAGE_NONE;
    sys->jit.tab_ram[BLOCKTAB_RAM_IDX(b->pc)] = (void *)jit_exit;
    sys->stats.jit_blocks_live = 1u;

    CHECK_TRUE(jit_block_add_ram_page(sys, bidx, 0x10u), "add page 0x10");
    CHECK_TRUE(jit_block_add_ram_page(sys, bidx, 0x11u), "add page 0x11");
    CHECK_TRUE(sys->bus.code_page_refs[0x10u] == 1u && sys->bus.code_page_refs[0x11u] == 1u,
               "code_page_refs incremented on both pages");
    CHECK_TRUE(sys->bus.fm_w[0x80010u] == 0u && sys->bus.fm_w[0x80011u] == 0u,
               "fm_w write-protected on both pages");

    /* Invalidate across page boundary via jit_invalidate_range */
    jit_invalidate_range(sys, 0x00010FFCu, 8u);
    CHECK_TRUE((b->flags & JB_DEAD) != 0u, "block marked JB_DEAD");
    CHECK_TRUE(sys->stats.jit_blocks_live == 0u && sys->stats.smc_invalidations == 1u,
               "live block count & smc_invalidations updated");
    CHECK_TRUE(sys->bus.code_page_refs[0x10u] == 0u && sys->bus.code_page_refs[0x11u] == 0u,
               "code_page_refs decremented back to 0");
    CHECK_TRUE(sys->bus.fm_w[0x80010u] != 0u && sys->bus.fm_w[0x80011u] != 0u,
               "fm_w write bias restored after invalidation");
    CHECK_TRUE(sys->jit.tab_ram[BLOCKTAB_RAM_IDX(0x80010FF8u)] == (void *)jit_compile_stub,
               "tab_ram reset to jit_compile_stub");

    jit_flush_all(sys);
    return 0;
}

/* ============================================================================
 * Test 3: Differential R3000A Programs (Oracle vs Cached vs ARM11 JIT)
 * ============================================================================ */
static int run_diff_program(const char *label, u32 start_pc, u32 halt_pc,
                            const u32 *code, u32 n_words, u32 code_phys_off,
                            void (*setup_cb)(PsxSystem *sys),
                            u32 ram_check_off, u32 ram_check_words) {
    for (int i = 0; i < 3; ++i) {
        test_sys_prepare(&g_sys[i], start_pc);
        write_code(&g_sys[i], code_phys_off, code, n_words);
        if (setup_cb) {
            setup_cb(&g_sys[i]);
        }
    }
    run_oracle_until_halt(&g_sys[0], halt_pc, 200000u);
    run_engine_until_halt(&g_sys[1], false, halt_pc, 4096, 256u);
    run_engine_until_halt(&g_sys[2], true,  halt_pc, 4096, 256u);
    return compare_three_engines(label, halt_pc, ram_check_off, ram_check_words);
}

/* Case A: Recursive Fibonacci fib(14) == 377 (JAL, JR $ra, stack frames, IBTC) */
static int test_diff_fibonacci(void) {
    const u32 base = 0x80010000u;
    const u32 fib_va = base + 8u * 4u;
    const u32 halt_va = base + 6u * 4u;
    const u32 prog[] = {
        /* 0: main */
        M_LUI(R_SP, 0x8002u),
        M_ORI(R_A0, R_ZERO, 14u),
        M_JAL(fib_va),
        M_NOP,
        M_LUI(R_T0, 0x8001u),
        M_SW(R_V0, 0x2000, R_T0),
        /* 6: halt */
        M_BEQ(R_ZERO, R_ZERO, -1),
        M_NOP,
        /* 8: fib(a0) */
        M_SLTI(R_T0, R_A0, 2),
        M_BEQ(R_T0, R_ZERO, 3),   /* -> 13 (recursive case) */
        M_NOP,
        M_JR(R_RA),
        M_ADDU(R_V0, R_A0, R_ZERO),
        /* 13: recursive */
        M_ADDIU(R_SP, R_SP, -16),
        M_SW(R_RA, 0, R_SP),
        M_SW(R_S0, 4, R_SP),
        M_SW(R_S1, 8, R_SP),
        M_ADDU(R_S0, R_A0, R_ZERO),
        M_ADDIU(R_A0, R_S0, -1),
        M_JAL(fib_va),
        M_NOP,
        M_ADDU(R_S1, R_V0, R_ZERO),
        M_ADDIU(R_A0, R_S0, -2),
        M_JAL(fib_va),
        M_NOP,
        M_ADDU(R_V0, R_V0, R_S1),
        M_LW(R_RA, 0, R_SP),
        M_LW(R_S0, 4, R_SP),
        M_LW(R_S1, 8, R_SP),
        M_JR(R_RA),
        M_ADDIU(R_SP, R_SP, 16)
    };
    CHECK_TRUE(run_diff_program("fib14", base, halt_va, prog,
                                (u32)(sizeof(prog) / sizeof(prog[0])),
                                0x10000u, NULL, 0x12000u, 1u) == 0,
               "fib(14) 3-engine agreement");
    CHECK_TRUE(g_sys[2].cpu.gpr[R_V0] == 377u && read_ram32(&g_sys[2], 0x12000u) == 377u,
               "fib(14) == 377");
#if PSX3DS_HAVE_JIT
    /* 753 recursive JR $ra returns across 3 call sites: <= 4 cold misses => ~99.6% inline IBTC hits! */
    u32 ret_main_va = base + 4u * 4u;
    CHECK_TRUE(g_sys[2].stats.step_one_fallbacks == 0u &&
               g_sys[2].stats.ibtc_misses > 0u && g_sys[2].stats.ibtc_misses <= 5u &&
               g_sys[2].jit.ibtc[(ret_main_va >> 2) & 0x3FFu].tag == ret_main_va,
               "IBTC populated & >=99% inline hit rate on 753 JR $ra returns");
#endif
    return 0;
}

/* Case B: Bubble Sort of 32 signed integers in RAM */
static void setup_bubblesort_data(PsxSystem *sys) {
    static const s32 k_init[32] = {
        42, -17, 0, 2147483647, -2147483647 - 1, 99, -1, 1,
        1024, -1024, 7, 7, -999999, 123456, -42, 8,
        -32768, 32767, 65535, -65536, 5, 3, -5, -3,
        19, -19, 256, -256, 11, -11, 2, -2
    };
    for (u32 i = 0u; i < 32u; ++i) {
        write_ram32(sys, 0x12000u + i * 4u, (u32)k_init[i]);
    }
}

static int test_diff_bubblesort(void) {
    const u32 base = 0x80010000u;
    const u32 halt_va = base + 20u * 4u;
    const u32 prog[] = {
        /* 0 */ M_LUI(R_S0, 0x8001u),
        /* 1 */ M_ORI(R_S0, R_S0, 0x2000u), /* s0 = &arr[0] */
        /* 2 */ M_ORI(R_S1, R_ZERO, 31u),   /* outer count = 31 */
        /* 3: outer */
        M_ADDU(R_T0, R_S0, R_ZERO),         /* ptr = s0 */
        M_ADDU(R_T1, R_S1, R_ZERO),         /* j = s1 */
        /* 5: inner */
        M_LW(R_T2, 0, R_T0),
        M_LW(R_T3, 4, R_T0),
        M_NOP,
        M_SLT(R_T4, R_T3, R_T2),            /* t4 = (arr[j+1] < arr[j]) */
        M_BEQ(R_T4, R_ZERO, 3),             /* -> 13 (no swap) */
        M_NOP,
        M_SW(R_T3, 0, R_T0),
        M_SW(R_T2, 4, R_T0),
        /* 13: next_inner */
        M_ADDIU(R_T0, R_T0, 4),
        M_ADDIU(R_T1, R_T1, -1),
        M_BNE(R_T1, R_ZERO, -11),           /* -> 5 (inner) */
        M_NOP,
        M_ADDIU(R_S1, R_S1, -1),
        M_BNE(R_S1, R_ZERO, -16),           /* -> 3 (outer) */
        M_NOP,
        /* 20: halt */
        M_BEQ(R_ZERO, R_ZERO, -1),
        M_NOP
    };
    CHECK_TRUE(run_diff_program("bubblesort32", base, halt_va, prog,
                                (u32)(sizeof(prog) / sizeof(prog[0])),
                                0x10000u, setup_bubblesort_data, 0x12000u, 32u) == 0,
               "bubblesort32 3-engine agreement");
    for (u32 i = 0u; i + 1u < 32u; ++i) {
        s32 a = (s32)read_ram32(&g_sys[2], 0x12000u + i * 4u);
        s32 b = (s32)read_ram32(&g_sys[2], 0x12000u + (i + 1u) * 4u);
        CHECK_TRUE(a <= b, "bubblesort32 monotonic ascending");
    }
    return 0;
}

/* Case C: ALU, Shifts, Constant Propagation & MULT/MULTU/DIV/DIVU Edge Cases */
static int test_diff_alu_mul_div(void) {
    const u32 base = 0x80010000u;
    const u32 prog[] = {
        M_LUI(R_S7, 0x8001u),
        M_ORI(R_S7, R_S7, 0x2000u),

        /* Constant-prop chain + writes to $zero ignored */
        M_LUI(R_ZERO, 0xDEADu),
        M_ORI(R_ZERO, R_ZERO, 0xBEEFu),
        M_LUI(R_S0, 0x8000u),               /* s0 = 0x80000000 */
        M_ADDIU(R_S1, R_ZERO, -1),          /* s1 = 0xFFFFFFFF (-1) */
        M_ORI(R_S2, R_ZERO, 0x25u),         /* shift = 37 -> &31 == 5 */

        M_SLLV(R_T0, R_S1, R_S2),           /* 0xFFFFFFE0 */
        M_SRLV(R_T1, R_S0, R_S2),           /* 0x04000000 */
        M_SRAV(R_T2, R_S0, R_S2),           /* 0xFC000000 */
        M_NOR(R_T3, R_T0, R_T1),
        M_XOR(R_T4, R_T2, R_T3),
        M_SLT(R_T5, R_S0, R_S1),            /* -2^31 < -1 -> 1 */
        M_SLTU(R_T6, R_S0, R_S1),           /* 0x80000000 < 0xFFFFFFFF -> 1 */
        M_SLTI(R_T7, R_S0, -1),             /* 1 */
        M_SLTIU(R_A0, R_S1, -1),            /* 0xFFFFFFFF < 0xFFFFFFFF -> 0 */

        M_SW(R_T0, 0, R_S7),
        M_SW(R_T1, 4, R_S7),
        M_SW(R_T2, 8, R_S7),
        M_SW(R_T4, 12, R_S7),

        /* MULT & MULTU */
        M_MULT(R_S0, R_S1),                 /* -2^31 * -1 = +2^31 -> HI=0, LO=0x80000000 */
        M_MFHI(R_A1),
        M_MFLO(R_A2),
        M_SW(R_A1, 16, R_S7),
        M_SW(R_A2, 20, R_S7),

        M_MULTU(R_S1, R_S1),                /* 0xFFFFFFFF^2 = 0xFFFFFFFE_00000001 */
        M_MFHI(R_A1),
        M_MFLO(R_A2),
        M_SW(R_A1, 24, R_S7),
        M_SW(R_A2, 28, R_S7),

        /* DIV edge 1: +12345 / 0 -> LO = -1, HI = 12345 */
        M_ORI(R_A3, R_ZERO, 12345u),
        M_DIV(R_A3, R_ZERO),
        M_MFHI(R_A1),
        M_MFLO(R_A2),
        M_SW(R_A1, 32, R_S7),
        M_SW(R_A2, 36, R_S7),

        /* DIV edge 2: -12345 / 0 -> LO = +1, HI = -12345 */
        M_SUBU(R_A3, R_ZERO, R_A3),
        M_DIV(R_A3, R_ZERO),
        M_MFHI(R_A1),
        M_MFLO(R_A2),
        M_SW(R_A1, 40, R_S7),
        M_SW(R_A2, 44, R_S7),

        /* DIV edge 3: INT32_MIN / -1 -> LO = 0x80000000, HI = 0 */
        M_DIV(R_S0, R_S1),
        M_MFHI(R_A1),
        M_MFLO(R_A2),
        M_SW(R_A1, 48, R_S7),
        M_SW(R_A2, 52, R_S7),

        /* DIVU edge 4: 0x80000000 / 0 -> LO = 0xFFFFFFFF, HI = 0x80000000 */
        M_DIVU(R_S0, R_ZERO),
        M_MFHI(R_A1),
        M_MFLO(R_A2),
        M_SW(R_A1, 56, R_S7),
        M_SW(R_A2, 60, R_S7),

        /* Normal signed DIV: -100 / 7 -> LO = -14, HI = -2 */
        M_ADDIU(R_S3, R_ZERO, -100),
        M_ORI(R_S4, R_ZERO, 7u),
        M_DIV(R_S3, R_S4),
        M_MFHI(R_A1),
        M_MFLO(R_A2),
        M_SW(R_A1, 64, R_S7),
        M_SW(R_A2, 68, R_S7),

        /* halt */
        M_BEQ(R_ZERO, R_ZERO, -1),
        M_NOP
    };
    u32 n_words = (u32)(sizeof(prog) / sizeof(prog[0]));
    u32 halt_va = base + (n_words - 2u) * 4u;
    CHECK_TRUE(run_diff_program("alu_mul_div", base, halt_va, prog, n_words,
                                0x10000u, NULL, 0x12000u, 18u) == 0,
               "alu_mul_div 3-engine agreement");
    return 0;
}

/* Case D & E: Load-Delay Hazards, LWL/LWR/SWL/SWR, Scratchpad, & Branch-Link Corner Cases */
static void setup_load_hazard_data(PsxSystem *sys) {
    write_ram32(sys, 0x12100u, 0x11223344u);
    write_ram32(sys, 0x12104u, 0x55667788u);
    write_ram32(sys, 0x12108u, 0x800180FEu);
}

static int test_diff_load_delay_and_branches(void) {
    const u32 base = 0x80010000u;
    const u32 dyn_tgt = base + 39u * 4u;
    const u32 prog[] = {
        /* 0 */  M_LUI(R_S0, 0x8001u),
        /* 1 */  M_ORI(R_A0, R_S0, 0x2100u), /* input data */
        /* 2 */  M_ORI(R_S7, R_S0, 0x2000u), /* output results */

        /* 1. Classic load delay: t1 sees OLD t0 (0xAAAA), t2 sees NEW t0 (0x11223344) */
        /* 3 */  M_ORI(R_T0, R_ZERO, 0xAAAAu),
        /* 4 */  M_LW(R_T0, 0, R_A0),
        /* 5 */  M_ADDU(R_T1, R_T0, R_ZERO),
        /* 6 */  M_ADDU(R_T2, R_T0, R_ZERO),
        /* 7 */  M_SW(R_T1, 0, R_S7),
        /* 8 */  M_SW(R_T2, 4, R_S7),

        /* 2. ALU overwrite in delay slot cancels load */
        /* 9 */  M_LW(R_T0, 0, R_A0),
        /* 10 */ M_ORI(R_T0, R_ZERO, 0xBEEFu),
        /* 11 */ M_NOP,
        /* 12 */ M_SW(R_T0, 8, R_S7),

        /* 3. Back-to-back LW to SAME reg: 2nd LW's delay slot sees 1st LW, then 2nd LW wins */
        /* 13 */ M_ORI(R_T0, R_ZERO, 0x1111u),
        /* 14 */ M_LW(R_T0, 0, R_A0),
        /* 15 */ M_LW(R_T0, 4, R_A0),
        /* 16 */ M_ADDU(R_T3, R_T0, R_ZERO), /* must be 0x11223344! */
        /* 17 */ M_SW(R_T3, 12, R_S7),
        /* 18 */ M_SW(R_T0, 16, R_S7),       /* must be 0x55667788! */

        /* 4. Unaligned LWL + LWR MEM->MEM bypass & SWL + SWR unaligned store */
        /* 19 */ M_LWL(R_T4, 4, R_A0),       /* bytes at +1..+4 of 0x12100 */
        /* 20 */ M_LWR(R_T4, 1, R_A0),       /* merges into pending t4! */
        /* 21 */ M_NOP,
        /* 22 */ M_SWL(R_T4, 23, R_S7),      /* unaligned store at s7 + 20..23 */
        /* 23 */ M_SWR(R_T4, 20, R_S7),

        /* 5. Subword LB/LBU/LH/LHU & Scratchpad (0x1F800000 & 0x9F800000 alias) */
        /* 24 */ M_LB(R_T5, 8, R_A0),        /* 0xFE -> -2 */
        /* 25 */ M_LH(R_T6, 10, R_A0),       /* 0x8001 -> -32767 */
        /* 26 */ M_LUI(R_A1, 0x1F80u),
        /* 27 */ M_SW(R_T5, 0x10, R_A1),
        /* 28 */ M_LUI(R_A2, 0x9F80u),
        /* 29 */ M_LW(R_T7, 0x10, R_A2),
        /* 30 */ M_SW(R_T6, 24, R_S7),
        /* 31 */ M_SW(R_T7, 28, R_S7),

        /* 6. Untaken BGEZAL with rs == $ra (must NOT take branch, MUST link $ra!) */
        /* 32 */ M_LUI(R_RA, 0x8000u),       /* $ra < 0 */
        /* 33 */ M_BGEZAL(R_RA, 10),         /* untaken! $ra becomes base + 35*4 */
        /* 34 */ M_SW(R_RA, 32, R_S7),       /* delay slot sees new linked $ra! */

        /* 7. Load in branch delay slot across dynamic JR where target reads $t0 immediately */
        /* 35 */ M_ORI(R_T0, R_ZERO, 0x7777u),
        /* 36 */ M_LUI(R_A3, (dyn_tgt >> 16) & 0xFFFFu),
        /* 37 */ M_JR(R_A3),                 /* wait: set low bits of a3 before JR! */
        /* 38 */ M_NOP,
        /* 39: dyn_tgt */
        /* 39 */ M_ADDU(R_S1, R_T0, R_ZERO), /* sees OLD t0 when jumped with LW in DS */
        /* 40 */ M_ADDU(R_S2, R_T0, R_ZERO), /* sees NEW t0 */
        /* 41 */ M_SW(R_S1, 36, R_S7),
        /* 42 */ M_SW(R_S2, 40, R_S7),
        /* 43: halt */
        /* 43 */ M_BEQ(R_ZERO, R_ZERO, -1),
        /* 44 */ M_NOP
    };

    /* Patch indices 36..38 properly so JR has LW in its delay slot */
    u32 patched[sizeof(prog) / sizeof(prog[0])];
    memcpy(patched, prog, sizeof(prog));
    patched[36] = M_ORI(R_A3, R_S0, dyn_tgt & 0xFFFFu);
    patched[37] = M_JR(R_A3);
    patched[38] = M_LW(R_T0, 0, R_A0); /* load in JR delay slot! */

    u32 n_words = (u32)(sizeof(patched) / sizeof(patched[0]));
    u32 halt_va = base + 43u * 4u;
    CHECK_TRUE(run_diff_program("load_delay_branches", base, halt_va, patched, n_words,
                                0x10000u, setup_load_hazard_data, 0x12000u, 11u) == 0,
               "load_delay_branches 3-engine agreement");
    CHECK_TRUE(read_ram32(&g_sys[2], 0x12000u + 36u) == 0x7777u &&
               read_ram32(&g_sys[2], 0x12000u + 40u) == 0x11223344u,
               "JR load-delay slot carried across block boundary into target insn 0");
    return 0;
}

/* Case F: Exceptions (Ov normal & in taken/untaken DS, AdEL/AdES, Misaligned JR, SYSCALL/BREAK/RI/CpU, KUc + RFE) */
static void install_test_exc_vector(PsxSystem *sys) {
    /*
     * Exception handler at 0x80000080 ($s6 points to log buffer in RAM):
     * Logs 4 words per trap: [CAUSE, EPC, BADVADDR, JUMPDEST], increments $s5.
     * If EPC is misaligned ((EPC & 3) != 0), returns to $s4 (recovery PC).
     * Else if CAUSE.BD != 0, returns to EPC + 8 (skipping branch + faulting DS).
     * Else returns to EPC + 4, restoring kernel mode if $s3 != 0 (for KUc test exit).
     */
    const u32 vec[] = {
        M_MFC0(R_K0, COP0_CAUSE),
        M_MFC0(R_K1, COP0_EPC),
        M_NOP,
        M_SW(R_K0, 0, R_S6),
        M_SW(R_K1, 4, R_S6),
        M_MFC0(R_AT, COP0_BADVADDR),
        M_NOP,
        M_SW(R_AT, 8, R_S6),
        M_MFC0(R_AT, COP0_JUMPDEST),
        M_NOP,
        M_SW(R_AT, 12, R_S6),
        M_ADDIU(R_S6, R_S6, 16),
        M_ADDIU(R_S5, R_S5, 1),

        /* Check misaligned EPC (from misaligned JR) */
        M_ANDI(R_AT, R_K1, 3u),
        M_BEQ(R_AT, R_ZERO, 3),    /* -> aligned */
        M_NOP,
        M_JR(R_S4),
        M_RFE,

        /* Aligned EPC: check CAUSE.BD (bit 31 -> k0 < 0) */
        M_BLTZ(R_K0, 3),           /* -> bd_set */
        M_ADDIU(R_K1, R_K1, 4),    /* DS: k1 = EPC + 4 */
        M_JR(R_K1),
        M_RFE,

        /* bd_set: k1 already EPC + 4, add another +4 -> EPC + 8 */
        M_ADDIU(R_K1, R_K1, 4),
        M_JR(R_K1),
        M_RFE
    };
    write_code(sys, 0x00080u, vec, (u32)(sizeof(vec) / sizeof(vec[0])));
}

static int test_diff_exceptions_and_kuc(void) {
    const u32 base = 0x80010000u;
    const u32 after_misaligned_jr = base + 24u * 4u;
    const u32 user_entry_kuseg = 0x00010000u + 29u * 4u;
    const u32 after_user_trap  = base + 33u * 4u;

    const u32 prog[] = {
        /* 0 */  M_LUI(R_S6, 0x8001u),
        /* 1 */  M_ORI(R_S6, R_S6, 0x2000u), /* log ptr */
        /* 2 */  M_ORI(R_S5, R_ZERO, 0u),    /* trap counter */

        /* Trap 1: ADDI signed overflow (t0 must stay 0x1234!) */
        /* 3 */  M_ORI(R_T0, R_ZERO, 0x1234u),
        /* 4 */  M_LUI(R_T1, 0x7FFFu),
        /* 5 */  M_ORI(R_T1, R_T1, 0xFFF0u),
        /* 6 */  M_ADDI(R_T0, R_T1, 0x7FFFu), /* Ov! */

        /* Trap 2: ADD overflow in TAKEN branch delay slot (with pending LW before branch!) */
        /* 7 */  M_ORI(R_T2, R_ZERO, 0x5555u),
        /* 8 */  M_BEQ(R_ZERO, R_ZERO, 4),    /* taken -> JUMPDEST = base + 13*4 */
        /* 9 */  M_ADD(R_T2, R_T1, R_T1),     /* DS Ov! Handler returns to EPC+8 = index 10 */

        /* Trap 3: SUB overflow in UNTAKEN branch delay slot */
        /* 10 */ M_LUI(R_T3, 0x8000u),
        /* 11 */ M_BNE(R_ZERO, R_ZERO, 6),    /* untaken -> JUMPDEST = base + 13*4 */
        /* 12 */ M_SUB(R_T3, R_T3, R_S6),     /* wait: 0x80000000 - (-0x7FFE...) doesn't ov; patch below! */

        /* Trap 4 & 5: Unaligned LW (AdEL) & SW (AdES) */
        /* 13 */ M_LW(R_T4, 2, R_S6),
        /* 14 */ M_SW(R_T0, 1, R_S6),

        /* Trap 6, 7, 8, 9: SYSCALL, BREAK, RI, COP1 CpU (CE=1) */
        /* 15 */ M_SYSCALL,
        /* 16 */ M_BREAK,
        /* 17 */ M_RSVD_OP,
        /* 18 */ M_COP1_NOP,

        /* Trap 10: Misaligned JR (0x80010502 -> AdEL at fetch with EPC == BadVAddr == 0x80010502!) */
        /* 19 */ M_LUI(R_S4, (after_misaligned_jr >> 16) & 0xFFFFu),
        /* 20 */ M_ORI(R_S4, R_S4, after_misaligned_jr & 0xFFFFu),
        /* 21 */ M_LUI(R_T5, 0x8001u),
        /* 22 */ M_ORI(R_T5, R_T5, 0x0502u),
        /* 23 */ M_JR(R_T5),
        /* 24: after_misaligned_jr */
        /* 24 */ M_NOP,

        /* Trap 11: Enter User Mode (SR.KUp=1, then jr user_kuseg; rfe -> KUc=1) & fault on KSEG0 load */
        /* 25 */ M_LUI(R_T6, 0x4000u),        /* CU2 | KUp (bit 2 = 0x04) */
        /* 26 */ M_ORI(R_T6, R_T6, 0x0008u),  /* wait: RFE shifts bits[5:2]->[3:0], so bit 3 (0x08) -> KUc (0x02)! */
        /* 27 */ M_MTC0(R_T6, COP0_SR),
        /* 28 */ M_NOP,

        /* 29 (executed in KUSEG 0x00010074 with KUc == 1!): */
        /* 29 */ M_LUI(R_T7, 0x8001u),
        /* 30 */ M_LW(R_AT, 0, R_T7),         /* User-mode KSEG0 read -> AdEL! */
        /* Handler returns via RFE to KUSEG index 31 still in User mode; SYSCALL + kernel recovery */
        /* 31 */ M_J(after_user_trap & 0x0FFFFFFFu), /* KUSEG J -> 0x00010084 */
        /* 32 */ M_NOP,

        /* 33: halt (accessible in KUSEG 0x00010084 AND KSEG0 0x80010084!) */
        /* 33 */ M_BEQ(R_ZERO, R_ZERO, -1),
        /* 34 */ M_NOP
    };

    /* Let's make index 12 cleanly SUB(0x80000000, +1) -> Ov, and index 28 jump cleanly to user_entry_kuseg */
    const u32 prog2[] = {
        /* 0 */  M_LUI(R_S6, 0x8001u),
        /* 1 */  M_ORI(R_S6, R_S6, 0x2000u),
        /* 2 */  M_ORI(R_S5, R_ZERO, 0u),
        /* 3 */  M_ORI(R_T0, R_ZERO, 0x1234u),
        /* 4 */  M_LUI(R_T1, 0x7FFFu),
        /* 5 */  M_ORI(R_T1, R_T1, 0xFFF0u),
        /* 6 */  M_ADDI(R_T0, R_T1, 0x7FFFu),          /* Trap 1: Ov */
        /* 7 */  M_ORI(R_T2, R_ZERO, 0x5555u),
        /* 8 */  M_BEQ(R_ZERO, R_ZERO, 4),
        /* 9 */  M_ADD(R_T2, R_T1, R_T1),              /* Trap 2: Ov in taken DS */
        /* 10 */ M_LUI(R_T3, 0x8000u),
        /* 11 */ M_ORI(R_T4, R_ZERO, 1u),
        /* 12 */ M_BNE(R_ZERO, R_ZERO, 5),
        /* 13 */ M_SUB(R_T3, R_T3, R_T4),              /* Trap 3: Ov in untaken DS */
        /* 14 */ M_LW(R_T4, 2, R_S6),                  /* Trap 4: AdEL */
        /* 15 */ M_SW(R_T0, 1, R_S6),                  /* Trap 5: AdES */
        /* 16 */ M_SYSCALL,                            /* Trap 6: Syscall */
        /* 17 */ M_BREAK,                              /* Trap 7: Bp */
        /* 18 */ M_RSVD_OP,                            /* Trap 8: RI */
        /* 19 */ M_COP1_NOP,                           /* Trap 9: CpU (CE=1) */
        /* 20 */ M_LUI(R_S4, ((base + 25u * 4u) >> 16) & 0xFFFFu),
        /* 21 */ M_ORI(R_S4, R_S4, (base + 25u * 4u) & 0xFFFFu),
        /* 22 */ M_LUI(R_T5, 0x8001u),
        /* 23 */ M_ORI(R_T5, R_T5, 0x0502u),
        /* 24 */ M_JR(R_T5),                           /* Trap 10: Misaligned JR -> AdEL */
        /* 25 */ M_NOP,
        /* Enter User mode at KUSEG 0x0001007C (index 31) */
        /* 26 */ M_LUI(R_T6, 0x4000u),
        /* 27 */ M_ORI(R_T6, R_T6, 0x0008u),           /* KUp=1 -> RFE sets KUc=1 */
        /* 28 */ M_MTC0(R_T6, COP0_SR),
        /* 29 */ M_ORI(R_T7, R_ZERO, 0x0001u),
        /* 30 */ M_SLL(R_T7, R_T7, 16),                /* 0x00010000 */
        /* 31 */ M_ORI(R_T7, R_T7, 34u * 4u),
        /* 32 */ M_JR(R_T7),
        /* 33 */ M_RFE,
        /* 34 (KUSEG User mode!): */
        /* 34 */ M_LUI(R_A0, 0x8001u),
        /* 35 */ M_LW(R_A1, 0, R_A0),                  /* Trap 11: User-mode KSEG0 AdEL! */
        /* 36 (still KUSEG User mode): halt cleanly! */
        /* 36 */ M_BEQ(R_ZERO, R_ZERO, -1),
        /* 37 */ M_NOP
    };
    (void)prog;
    (void)after_misaligned_jr;
    (void)user_entry_kuseg;
    (void)after_user_trap;

    u32 n_words = (u32)(sizeof(prog2) / sizeof(prog2[0]));
    u32 halt_kuseg = 0x00010000u + 36u * 4u;
    CHECK_TRUE(run_diff_program("exceptions_kuc", base, halt_kuseg, prog2, n_words,
                                0x10000u, install_test_exc_vector, 0x12000u, 44u) == 0,
               "exceptions_kuc 3-engine agreement");
    CHECK_TRUE(g_sys[2].cpu.gpr[R_S5] == 11u, "all 11 exceptions trapped & recovered");
    CHECK_TRUE(g_sys[2].cpu.gpr[R_T0] == 0x1234u && g_sys[2].cpu.gpr[R_T2] == 0x5555u,
               "Ov destination GPRs preserved unmodified");
    CHECK_TRUE(read_ram32(&g_sys[2], 0x12000u + 9u * 16u + 4u) == 0x80010502u &&
               read_ram32(&g_sys[2], 0x12000u + 9u * 16u + 8u) == 0x80010502u,
               "Misaligned JR recorded exact EPC == BadVAddr == 0x80010502");
    return 0;
}

/* Case G: Virtual Segment Aliasing (0x8001xxxx vs 0x0001xxxx sharing tab_ram) */
static int test_diff_segment_alias(void) {
    /*
     * Subroutine at physical 0x10100 executes BLTZAL $zero, 0; jr $t9; nop,
     * returning its own virtual PC + 8 in $ra!
     * Call it first via KSEG0 (0x80010100), then via KUSEG (0x00010100):
     * $ra MUST reflect 0x80010108 on call 1 and 0x00010108 on call 2!
     */
    const u32 base = 0x80010000u;
    const u32 main_code[] = {
        M_LUI(R_S7, 0x8001u),
        M_ORI(R_S7, R_S7, 0x2000u),
        /* Call 1 at 0x80010100 */
        M_LUI(R_T8, 0x8001u),
        M_ORI(R_T8, R_T8, 0x0100u),
        M_JALR(R_T9, R_T8),
        M_NOP,
        M_SW(R_RA, 0, R_S7),
        /* Call 2 at 0x00010100 */
        M_LUI(R_T8, 0x0001u),
        M_ORI(R_T8, R_T8, 0x0100u),
        M_JALR(R_T9, R_T8),
        M_NOP,
        M_SW(R_RA, 4, R_S7),
        /* halt */
        M_BEQ(R_ZERO, R_ZERO, -1),
        M_NOP
    };
    const u32 sub_code[] = {
        M_BLTZAL(R_ZERO, 1),
        M_NOP,
        M_JR(R_T9),
        M_NOP
    };

    for (int i = 0; i < 3; ++i) {
        test_sys_prepare(&g_sys[i], base);
        write_code(&g_sys[i], 0x10000u, main_code, (u32)(sizeof(main_code) / 4u));
        write_code(&g_sys[i], 0x10100u, sub_code, (u32)(sizeof(sub_code) / 4u));
    }
    u32 halt_va = base + 12u * 4u;
    run_oracle_until_halt(&g_sys[0], halt_va, 10000u);
    run_engine_until_halt(&g_sys[1], false, halt_va, 4096, 16u);
    run_engine_until_halt(&g_sys[2], true,  halt_va, 4096, 16u);
    CHECK_TRUE(compare_three_engines("segment_alias", halt_va, 0x12000u, 2u) == 0,
               "segment_alias 3-engine agreement");
    CHECK_TRUE(read_ram32(&g_sys[2], 0x12000u) == 0x80010108u &&
               read_ram32(&g_sys[2], 0x12004u) == 0x00010108u,
               "entry_tab guard caught KSEG0 vs KUSEG alias");
    return 0;
}

/* Case H: Idle-Loop Skipping & Counted-Loop Folding */
static int test_idle_and_counted_loops(void) {
    /* 1. MMIO I_STAT (0x1F801070 == 0) tight polling loop */
    const u32 base = 0x80010000u;
    const u32 poll_prog[] = {
        M_LUI(R_GP, 0x1F80u),
        /* poll_loop: */
        M_LW(R_T0, 0x1070, R_GP),
        M_NOP,
        M_BEQ(R_T0, R_ZERO, -3),
        M_NOP
    };
    for (int eng = 1; eng <= 2; ++eng) {
        test_sys_prepare(&g_sys[eng], base);
        write_code(&g_sys[eng], 0x10000u, poll_prog, 5u);
        g_sys[eng].cpu.cycles_left = 50000;
        g_sys[eng].cpu.slice_len = 50000;
        if (eng == 1) {
            cpu_cached_run(&g_sys[eng]);
        } else {
            jit_run(&g_sys[eng]);
        }
        CHECK_TRUE(g_sys[eng].stats.idle_skips > 0u && g_sys[eng].cpu.cycles_left <= 0,
                   "MMIO poll loop triggered idle_skip");
    }

    /* 2. Counted delay loop: loop: addiu $t0, $t0, -1; bne $t0, $0, loop; nop */
    const u32 cnt_prog[] = {
        M_ORI(R_T0, R_ZERO, 600u),
        /* loop: */
        M_ADDIU(R_T0, R_T0, -1),
        M_BNE(R_T0, R_ZERO, -2),
        M_NOP,
        /* halt: */
        M_BEQ(R_ZERO, R_ZERO, -1),
        M_NOP
    };
    u32 halt_va = base + 4u * 4u;
    CHECK_TRUE(run_diff_program("counted_loop", base, halt_va, cnt_prog, 6u,
                                0x10000u, NULL, 0x12000u, 0u) == 0,
               "counted_loop 3-engine agreement");
    CHECK_TRUE(g_sys[2].cpu.gpr[R_T0] == 0u && g_sys[2].stats.idle_skips > 0u,
               "counted_loop folded to 0 with idle_skips telemetry");
    return 0;
}

/* Case I: Live Self-Modifying Code (SMC) */
static int test_diff_live_smc(void) {
    const u32 base = 0x80010000u;
    const u32 fn_va = 0x80011000u;
    const u32 new_insn = M_ORI(R_V0, R_ZERO, 99u);

    const u32 main_code[] = {
        M_LUI(R_S7, 0x8001u),
        M_ORI(R_S7, R_S7, 0x2000u),
        /* 1st call -> returns 11 (compiles & write-protects page 0x11) */
        M_JAL(fn_va),
        M_NOP,
        M_SW(R_V0, 0, R_S7),
        /* Overwrite 0x80011000 with `ori $v0, $zero, 99` */
        M_LUI(R_T0, 0x8001u),
        M_ORI(R_T0, R_T0, 0x1000u),
        M_LUI(R_T1, (new_insn >> 16) & 0xFFFFu),
        M_ORI(R_T1, R_T1, new_insn & 0xFFFFu),
        M_SW(R_T1, 0, R_T0),
        /* 2nd call -> MUST recompile & return 99! */
        M_JAL(fn_va),
        M_NOP,
        M_SW(R_V0, 4, R_S7),
        /* halt */
        M_BEQ(R_ZERO, R_ZERO, -1),
        M_NOP
    };
    const u32 fn_code[] = {
        M_ORI(R_V0, R_ZERO, 11u),
        M_JR(R_RA),
        M_NOP
    };

    for (int i = 0; i < 3; ++i) {
        test_sys_prepare(&g_sys[i], base);
        write_code(&g_sys[i], 0x10000u, main_code, (u32)(sizeof(main_code) / 4u));
        write_code(&g_sys[i], 0x11000u, fn_code, (u32)(sizeof(fn_code) / 4u));
    }
    u32 halt_va = base + 13u * 4u;
    run_oracle_until_halt(&g_sys[0], halt_va, 10000u);
    run_engine_until_halt(&g_sys[1], false, halt_va, 4096, 16u);
    run_engine_until_halt(&g_sys[2], true,  halt_va, 4096, 16u);

    CHECK_TRUE(compare_three_engines("live_smc", halt_va, 0x12000u, 2u) == 0,
               "live_smc 3-engine agreement");
    CHECK_TRUE(read_ram32(&g_sys[2], 0x12000u) == 11u &&
               read_ram32(&g_sys[2], 0x12004u) == 99u,
               "live_smc returned 11 before patch and 99 after patch");
    CHECK_TRUE(g_sys[1].stats.smc_invalidations > 0u &&
               g_sys[2].stats.smc_invalidations > 0u,
               "SMC page invalidation triggered on store");
#if PSX3DS_HAVE_JIT
    CHECK_TRUE(g_sys[2].stats.step_one_fallbacks == 0u,
               "live_smc executed 100% natively in JIT with zero step_one fallbacks");
#endif
    return 0;
}

/* ============================================================================
 * Test 4: Microbenchmark (Oracle vs Cached Interpreter vs ARM11 JIT)
 * ============================================================================ */
static int test_microbenchmark(void) {
    /*
     * Compute + RAM + call benchmark (~750,000 guest instructions):
     * 6,000 outer iterations x (Xorshift32 + 16-word RAM mix + subroutine call).
     */
    const u32 base = 0x80010000u;
    const u32 mix_sub = base + 22u * 4u;
    const u32 halt_va = base + 20u * 4u;
    const u32 bench_prog[] = {
        /* 0 */  M_LUI(R_S0, 0x8001u),
        /* 1 */  M_ORI(R_S0, R_S0, 0x2000u), /* buf */
        /* 2 */  M_LUI(R_S1, 0x1234u),
        /* 3 */  M_ORI(R_S1, R_S1, 0x5678u), /* state */
        /* 4 */  M_ORI(R_S2, R_ZERO, 6000u), /* outer iters */
        /* 5: outer */
        M_ORI(R_S3, R_ZERO, 0u),
        /* 6: inner (16 words = 64 bytes) */
        M_SLL(R_T0, R_S1, 13),
        M_XOR(R_S1, R_S1, R_T0),
        M_SRL(R_T0, R_S1, 17),
        M_XOR(R_S1, R_S1, R_T0),
        M_SLL(R_T0, R_S1, 5),
        M_XOR(R_S1, R_S1, R_T0),
        M_ADDU(R_T1, R_S0, R_S3),
        M_LW(R_T2, 0, R_T1),
        M_ADDIU(R_S3, R_S3, 4),
        M_XOR(R_T2, R_T2, R_S1),
        M_SW(R_T2, 0, R_T1),
        M_SLTIU(R_T3, R_S3, 64),
        M_BNE(R_T3, R_ZERO, -13),            /* -> 6 (inner) */
        M_NOP,
        /* 16 */
        M_JAL(mix_sub),
        M_ADDIU(R_S2, R_S2, -1),
        M_BNE(R_S2, R_ZERO, -16),            /* -> 5 (outer); wait: 5 - (18+1) = -14! Fix below */
        M_NOP,
        /* 20: halt */
        M_BEQ(R_ZERO, R_ZERO, -1),
        M_NOP,
        /* 22: mix_sub */
        M_ADDU(R_S1, R_S1, R_T2),
        M_JR(R_RA),
        M_XORI(R_S1, R_S1, 0x5A5Au)
    };

    /* Compute exact relative branch offsets */
    u32 patched[sizeof(bench_prog) / sizeof(bench_prog[0])];
    memcpy(patched, bench_prog, sizeof(bench_prog));
    /* Let's verify indices explicitly:
     * 0..4: init (5 insns)
     * 5: ori s3, zero, 0
     * 6..17: inner body (12 insns: 6..17), 18: bne t3, zero, (6 - 19 = -13), 19: nop -> 20 insns so far!
     * Let's construct cleanly with exact index assertions! */
    u32 idx = 0u;
    u32 code[32];
    code[idx++] = M_LUI(R_S0, 0x8001u);
    code[idx++] = M_ORI(R_S0, R_S0, 0x2000u);
    code[idx++] = M_LUI(R_S1, 0x1234u);
    code[idx++] = M_ORI(R_S1, R_S1, 0x5678u);
    code[idx++] = M_ORI(R_S2, R_ZERO, 6000u);
    u32 outer_idx = idx;
    code[idx++] = M_ORI(R_S3, R_ZERO, 0u);
    u32 inner_idx = idx;
    code[idx++] = M_SLL(R_T0, R_S1, 13);
    code[idx++] = M_XOR(R_S1, R_S1, R_T0);
    code[idx++] = M_SRL(R_T0, R_S1, 17);
    code[idx++] = M_XOR(R_S1, R_S1, R_T0);
    code[idx++] = M_SLL(R_T0, R_S1, 5);
    code[idx++] = M_XOR(R_S1, R_S1, R_T0);
    code[idx++] = M_ADDU(R_T1, R_S0, R_S3);
    code[idx++] = M_LW(R_T2, 0, R_T1);
    code[idx++] = M_ADDIU(R_S3, R_S3, 4);
    code[idx++] = M_XOR(R_T2, R_T2, R_S1);
    code[idx++] = M_SW(R_T2, 0, R_T1);
    code[idx++] = M_SLTIU(R_T3, R_S3, 64);
    code[idx]   = M_BNE(R_T3, R_ZERO, (s32)inner_idx - (s32)(idx + 1u));
    idx++;
    code[idx++] = M_NOP;
    u32 jal_slot = idx++;
    code[idx++] = M_ADDIU(R_S2, R_S2, -1);
    code[idx]   = M_BNE(R_S2, R_ZERO, (s32)outer_idx - (s32)(idx + 1u));
    idx++;
    code[idx++] = M_NOP;
    u32 real_halt_va = base + idx * 4u;
    code[idx++] = M_BEQ(R_ZERO, R_ZERO, -1);
    code[idx++] = M_NOP;
    u32 real_sub_va = base + idx * 4u;
    code[jal_slot] = M_JAL(real_sub_va);
    code[idx++] = M_ADDU(R_S1, R_S1, R_T2);
    code[idx++] = M_JR(R_RA);
    code[idx++] = M_XORI(R_S1, R_S1, 0x5A5Au);
    (void)patched;
    (void)mix_sub;
    (void)halt_va;

    double ms[3] = { 0.0, 0.0, 0.0 };
    for (int i = 0; i < 3; ++i) {
        test_sys_prepare(&g_sys[i], base);
        write_code(&g_sys[i], 0x10000u, code, idx);
        clock_t t0 = clock();
        if (i == 0) {
            run_oracle_until_halt(&g_sys[0], real_halt_va, 2000000u);
        } else {
            run_engine_until_halt(&g_sys[i], i == 2, real_halt_va, 2000000, 4u);
        }
        clock_t t1 = clock();
        ms[i] = 1000.0 * (double)(t1 - t0) / (double)CLOCKS_PER_SEC;
    }

    CHECK_TRUE(compare_three_engines("microbench", real_halt_va, 0x12000u, 16u) == 0,
               "microbench 3-engine checksum agreement");
    printf("BENCH [~1.37M guest insns]: oracle=%.2f ms | cpu_cached=%.2f ms (%.2fx) | arm_jit=%.2f ms (%.2fx vs oracle, %.2fx vs cached)\n",
           ms[0], ms[1],
           (ms[1] > 0.001) ? (ms[0] / ms[1]) : 0.0,
           ms[2],
           (ms[2] > 0.001) ? (ms[0] / ms[2]) : 0.0,
           (ms[2] > 0.001) ? (ms[1] / ms[2]) : 0.0);
    return 0;
}

int main(void) {
    for (int i = 0; i < 3; ++i) {
        CHECK_TRUE(test_sys_init(&g_sys[i], &g_env[i]), "test_sys_init");
    }

#if PSX3DS_HAVE_JIT
    if (test_emitter_armv6k_purity(&g_sys[2]) != 0) return 1;
#endif
    if (test_block_cache_lifecycle(&g_sys[2]) != 0) return 1;
    if (test_diff_fibonacci() != 0) return 1;
    if (test_diff_bubblesort() != 0) return 1;
    if (test_diff_alu_mul_div() != 0) return 1;
    if (test_diff_load_delay_and_branches() != 0) return 1;
    if (test_diff_exceptions_and_kuc() != 0) return 1;
    if (test_diff_segment_alias() != 0) return 1;
    if (test_idle_and_counted_loops() != 0) return 1;
    if (test_diff_live_smc() != 0) return 1;
    if (test_microbenchmark() != 0) return 1;

    for (int i = 0; i < 3; ++i) {
        test_sys_shutdown(&g_sys[i], &g_env[i]);
    }
    printf("test_jit_core PASS (have_jit=%d)\n", PSX3DS_HAVE_JIT);
    return 0;
}
