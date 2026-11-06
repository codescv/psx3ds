/*
 * Unit tests for Slice A: MIPS R3000A Reference Interpreter & COP0.
 * Covers ALU/DIV/Overflow, Load Delay Hazards, LWL/LWR Bypass Forwarding,
 * Branch Delay Slots (BLTZAL/BGEZAL/JALR/Branch-in-DS), COP0/RFE/CpU.
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

/* Minimal MIPS-I instruction encoders */
#define M_R(rs, rt, rd, sa, fn) \
    (((u32)(rs) << 21) | ((u32)(rt) << 16) | ((u32)(rd) << 11) | ((u32)(sa) << 6) | (u32)(fn))
#define M_I(op, rs, rt, imm) \
    (((u32)(op) << 26) | ((u32)(rs) << 21) | ((u32)(rt) << 16) | ((u32)(imm) & 0xFFFFu))
#define M_J(op, target) \
    (((u32)(op) << 26) | (((u32)(target) >> 2) & 0x03FFFFFFu))
#define M_NOP 0x00000000u

static void setup_cpu_test(void) {
    arena_destroy(&g_sys.arena);
    memset(&g_sys, 0, sizeof(g_sys));
    psx_config_defaults(&g_sys.cfg, true);
    g_sys.cfg.cycle_mult = 256u; /* exact 1 cycle/insn */

    (void)arena_init(&g_sys.arena, 20u * 1024u * 1024u);
    sched_init(&g_sys.sched);
    bus_init(&g_sys);
    irq_init(&g_sys);
    cpu_init(&g_sys);
    gte_init(&g_sys);

    /* Start in kernel mode, BEV=0 (RAM vector 0x80000080), PC=0x80010000 */
    cpu_set_sr(&g_sys, 0u);
    g_sys.cpu.pc = 0x80010000u;
    g_sys.cpu.next_pc = 0x80010004u;
    g_sys.cpu.cycles_left = 10000;
    g_sys.cpu.slice_len = 10000;
}

static void emit_prog(u32 base_va, const u32 *insns, u32 count) {
    u32 off = base_va & PSX_RAM_MASK;
    memcpy(g_sys.bus.ram + off, insns, (size_t)count * 4u);
}

static void step_n(u32 n) {
    for (u32 i = 0u; i < n; i++) {
        cpu_step_one(&g_sys);
    }
}

int main(void) {
    /* ----------------------------------------------------------------------
     * 1. ALU, Shifts, DIV/DIVU Hardware Edge Cases, and 2's Complement Ov
     * ---------------------------------------------------------------------- */
    setup_cpu_test();
    PsxCpu *c = &g_sys.cpu;

    const u32 prog_alu[] = {
        M_I(0x09, 0, 1, -1),          /* addiu $1, $0, -1 (0xFFFFFFFF) */
        M_I(0x0F, 0, 2, 0x8000),      /* lui   $2, 0x8000 (INT32_MIN) */
        M_R(2, 1, 0, 0, 0x1A),        /* div   $2, $1 -> LO=0x80000000, HI=0 */
        M_R(0, 0, 3, 0, 0x12),        /* mflo  $3 */
        M_R(0, 0, 4, 0, 0x10),        /* mfhi  $4 */
        M_I(0x09, 0, 5, 42),          /* addiu $5, $0, 42 */
        M_R(5, 0, 0, 0, 0x1A),        /* div   $5, $0 (+42 / 0 -> LO=-1, HI=42) */
        M_R(0, 0, 6, 0, 0x12),        /* mflo  $6 */
        M_R(0, 0, 7, 0, 0x10),        /* mfhi  $7 */
        M_R(2, 0, 0, 0, 0x1A),        /* div   $2, $0 (-2^31 / 0 -> LO=+1, HI=0x80000000) */
        M_R(0, 0, 8, 0, 0x12),        /* mflo  $8 */
        M_R(0, 0, 9, 0, 0x10),        /* mfhi  $9 */
        M_R(5, 0, 0, 0, 0x1B),        /* divu  $5, $0 (42u / 0 -> LO=0xFFFFFFFF, HI=42) */
        M_R(0, 0, 10, 0, 0x12),       /* mflo  $10 */
        M_R(0, 0, 11, 0, 0x10),       /* mfhi  $11 */
        M_I(0x09, 0, 12, 37),         /* addiu $12, $0, 37 (shift count masked to 5) */
        M_R(12, 2, 13, 0, 0x07),      /* srav  $13, $2, $12 (0x80000000 >> 5 = 0xFC000000) */
        M_R(1, 1, 0, 0, 0x21),        /* addu  $0, $1, $1 ($zero stays 0) */
    };
    emit_prog(0x80010000u, prog_alu, sizeof(prog_alu) / 4u);
    step_n(sizeof(prog_alu) / 4u);

    CHECK(c->gpr[0] == 0u);
    CHECK(c->gpr[3] == 0x80000000u && c->gpr[4] == 0u);
    CHECK(c->gpr[6] == 0xFFFFFFFFu && c->gpr[7] == 42u);
    CHECK(c->gpr[8] == 1u && c->gpr[9] == 0x80000000u);
    CHECK(c->gpr[10] == 0xFFFFFFFFu && c->gpr[11] == 42u);
    CHECK(c->gpr[13] == 0xFC000000u);

    /* Signed ADD / SUB / ADDI overflow trap (Ov = 12), leaving destination GPR unmodified */
    setup_cpu_test();
    c = &g_sys.cpu;
    c->gpr[1] = 0x7FFFFFFFu;
    c->gpr[2] = 1u;
    c->gpr[3] = 0xDEADBEEFu;
    const u32 prog_ov[] = {
        M_R(1, 2, 3, 0, 0x20), /* add $3, $1, $2 -> Ov! */
    };
    emit_prog(0x80010000u, prog_ov, 1u);
    step_n(1u);
    CHECK(c->pc == PSX_EXC_VEC_RAM);
    CHECK(c->cop0[COP0_EPC] == 0x80010000u);
    CHECK(((c->cop0[COP0_CAUSE] >> 2) & 0x1Fu) == PSX_EXC_OV);
    CHECK(c->gpr[3] == 0xDEADBEEFu);

    /* ----------------------------------------------------------------------
     * 2. Load Delay Slots, Cancellation, & LWL/LWR In-Flight Bypass Merge
     * ---------------------------------------------------------------------- */
    setup_cpu_test();
    c = &g_sys.cpu;

    const u32 data0 = 0x11223344u;
    const u32 data1 = 0x55667788u;
    const u32 data2 = 0xAABBCCDDu;
    memcpy(g_sys.bus.ram + 0x2000u, &data0, 4u);
    memcpy(g_sys.bus.ram + 0x2004u, &data1, 4u);
    memcpy(g_sys.bus.ram + 0x2008u, &data2, 4u);

    c->gpr[4] = 0x80002000u;
    c->gpr[1] = 0x99999999u;
    c->gpr[2] = 0x77777777u;
    c->gpr[5] = 0xEEEEEEEEu;

    const u32 prog_ld[] = {
        /* (a) Delay slot sees OLD $1 */
        M_I(0x23, 4, 1, 0),       /* lw    $1, 0($4)   -> delayed 0x11223344 */
        M_R(1, 0, 6, 0, 0x25),    /* or    $6, $1, $0  -> reads old $1 (0x99999999) */
        M_R(1, 0, 7, 0, 0x25),    /* or    $7, $1, $0  -> reads new $1 (0x11223344) */

        /* (b) ALU write to $2 in delay slot CANCELS delayed load! */
        M_I(0x23, 4, 2, 4),       /* lw    $2, 4($4)   -> delayed 0x55667788 */
        M_I(0x09, 0, 2, 0x1234),  /* addiu $2, $0, 0x1234 -> overrides $2! */
        M_NOP,

        /* (c) Back-to-back loads to $5: second load wins after its delay slot */
        M_I(0x23, 4, 5, 0),       /* lw    $5, 0($4) */
        M_I(0x23, 4, 5, 4),       /* lw    $5, 4($4)   -> cancels first, queues 0x55667788 */
        M_R(5, 0, 8, 0, 0x25),    /* or    $8, $5, $0  -> still sees 0xEEEEEEEE! */
        M_R(5, 0, 9, 0, 0x25),    /* or    $9, $5, $0  -> sees 0x55667788! */

        /* (d) Unaligned LWL + LWR at offset +1..+4 with NO NOP in between!
         * Bytes at 0x2000..0x2007: 44 33 22 11 | 88 77 66 55
         * Unaligned 32-bit LE word at 0x2001..0x2004 = 0x88112233! */
        M_I(0x22, 4, 10, 4),      /* lwl   $10, 4($4)  (addr 0x80002004) */
        M_I(0x26, 4, 10, 1),      /* lwr   $10, 1($4)  (addr 0x80002001) -> merges pending $10! */
        M_NOP,

        /* (e) Unaligned SWL + SWR to 0x80002011..0x80002014 */
        M_I(0x2A, 4, 10, 0x14),   /* swl   $10, 0x14($4) */
        M_I(0x2E, 4, 10, 0x11),   /* swr   $10, 0x11($4) */
    };
    emit_prog(0x80010000u, prog_ld, sizeof(prog_ld) / 4u);
    step_n(sizeof(prog_ld) / 4u);

    CHECK(c->gpr[6] == 0x99999999u);
    CHECK(c->gpr[7] == 0x11223344u);
    CHECK(c->gpr[2] == 0x00001234u);
    CHECK(c->gpr[8] == 0xEEEEEEEEu);
    CHECK(c->gpr[9] == 0x55667788u);
    CHECK(c->gpr[10] == 0x88112233u);

    u32 unaligned_stored = 0u;
    memcpy(&unaligned_stored, g_sys.bus.ram + 0x2011u, 4u);
    CHECK(unaligned_stored == 0x88112233u);

    /* ----------------------------------------------------------------------
     * 3. Branch Hazards: Untaken BGEZAL, BLTZAL $ra ($rs==31), JALR $rd==$rs,
     *    Delay-slot exception (BD + JUMPDEST), and jr $k0; rfe!
     * ---------------------------------------------------------------------- */
    setup_cpu_test();
    c = &g_sys.cpu;
    c->gpr[1] = 0xFFFFFFFFu; /* -1: BGEZAL untaken, must STILL write $ra! */
    c->gpr[31] = 0xFFFFFFFFu; /* -1: BLTZAL $ra, +4 must read old $ra=-1 -> TAKEN! */
    c->gpr[5] = 0x80010030u;  /* JALR $5, $5 target */

    const u32 prog_br[] = {
        /* 0x80010000 */ M_I(0x01, 1, 0x11, 8),    /* bgezal $1, +8 (untaken, $ra = 0x80010008) */
        /* 0x80010004 */ M_R(31, 0, 2, 0, 0x25),   /* or $2, $31, $0 (delay slot sees new $ra!) */
        /* 0x80010008 */ M_I(0x09, 0, 31, -5),     /* addiu $31, $0, -5 */
        /* 0x8001000C */ M_I(0x01, 31, 0x10, 3),   /* bltzal $31, +3 (-> 0x8001001C; $ra = 0x80010014) */
        /* 0x80010010 */ M_R(31, 0, 3, 0, 0x25),   /* or $3, $31, $0 (delay slot sees 0x80010014) */
        /* 0x80010014 */ M_I(0x09, 0, 4, 0xBAD),   /* skipped! */
        /* 0x80010018 */ M_NOP,
        /* 0x8001001C */ M_R(5, 0, 5, 0, 0x09),    /* jalr $5, $5 (jumps to old $5=0x80010030, $5=0x80010024) */
        /* 0x80010020 */ M_NOP,
        /* 0x80010024 */ M_NOP,
        /* 0x80010028 */ M_NOP,
        /* 0x8001002C */ M_NOP,
        /* 0x80010030 */ M_J(0x02, 0x80010100u),   /* j 0x80010100 */
        /* 0x80010034 */ M_R(0, 0, 0, 0, 0x0C),    /* syscall in taken delay slot! */
    };
    emit_prog(0x80010000u, prog_br, sizeof(prog_br) / 4u);
    step_n(9u); /* runs up through syscall in delay slot */

    CHECK(c->gpr[2] == 0x80010008u);
    CHECK(c->gpr[3] == 0x80010014u);
    CHECK(c->gpr[4] == 0u);
    CHECK(c->gpr[5] == 0x80010024u);
    CHECK(c->pc == PSX_EXC_VEC_RAM);
    CHECK(c->cop0[COP0_EPC] == 0x80010030u);
    CHECK((c->cop0[COP0_CAUSE] & PSX_CAUSE_BD) != 0u);
    CHECK(c->cop0[COP0_JUMPDEST] == 0x80010100u);

    /* Exception handler at 0x80000080: `jr $k0; rfe` returning to 0x80010200 */
    cpu_set_sr(&g_sys, 0x0000000Cu); /* KUp=1, IEp=1, KUc=0, IEc=0 */
    c->gpr[26] = 0x80010200u;        /* $k0 */
    const u32 handler_rfe[] = {
        M_R(26, 0, 0, 0, 0x08),                  /* jr  $k0 */
        (0x10u << 26) | (0x10u << 21) | 0x10u,   /* rfe */
    };
    emit_prog(PSX_EXC_VEC_RAM, handler_rfe, 2u);
    c->pc = PSX_EXC_VEC_RAM;
    c->next_pc = PSX_EXC_VEC_RAM + 4u;
    cpu_step_until_clean(&g_sys);

    CHECK(c->pc == 0x80010200u);
    CHECK((c->cop0[COP0_SR] & 0x0Fu) == 0x03u); /* KUc=1, IEc=1 restored! */

    /* User mode (KUc=1) executing MFC0 -> CpU with CE=0! */
    const u32 user_cop0[] = {
        (0x10u << 26) | (0u << 21) | (1u << 16) | (12u << 11), /* mfc0 $1, SR */
    };
    emit_prog(0x00010200u, user_cop0, 1u);
    c->pc = 0x00010200u;
    c->next_pc = 0x00010204u;
    step_n(1u);
    CHECK(c->pc == PSX_EXC_VEC_RAM);
    CHECK(((c->cop0[COP0_CAUSE] >> 2) & 0x1Fu) == PSX_EXC_CPU);
    CHECK(((c->cop0[COP0_CAUSE] >> 28) & 3u) == 0u);

    /* Kernel mode, CU2=0 executing LWC2 -> CpU with CE=2! */
    const u32 kern_lwc2[] = {
        M_I(0x32, 0, 0, 0x1000), /* lwc2 $0, 0x1000($0) */
    };
    emit_prog(0x80010300u, kern_lwc2, 1u);
    c->pc = 0x80010300u;
    c->next_pc = 0x80010304u;
    step_n(1u);
    CHECK(((c->cop0[COP0_CAUSE] >> 2) & 0x1Fu) == PSX_EXC_CPU);
    CHECK(((c->cop0[COP0_CAUSE] >> 28) & 3u) == 2u);

    arena_destroy(&g_sys.arena);
    printf("test_core_cpu PASS\n");
    return 0;
}
