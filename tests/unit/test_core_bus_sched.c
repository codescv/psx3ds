/*
 * Unit tests for Slice A: Arena, Bus, 4 KiB Modular-uintptr_t Fastmem,
 * SR.IsC Cache Isolation, SMC Write-Trapping, Scheduler, and IRQ Controller.
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

static void init_core_subsystems(bool isc_swap, bool strict_ram) {
    arena_destroy(&g_sys.arena);
    memset(&g_sys, 0, sizeof(g_sys));
    psx_config_defaults(&g_sys.cfg, true);
    g_sys.cfg.isc_swap_table = isc_swap;
    g_sys.cfg.strict_ram_size = strict_ram;
    g_sys.cfg.cycle_mult = 256u;

    /* 28 MiB covers 2-3x 8 MiB 64-bit fastmem LUTs + 2 MiB RAM + 512 KiB BIOS */
    (void)arena_init(&g_sys.arena, 28u * 1024u * 1024u);
    sched_init(&g_sys.sched);
    bus_init(&g_sys);
    irq_init(&g_sys);
    cpu_init(&g_sys);
    dma_init(&g_sys);
    timers_init(&g_sys);
    gte_init(&g_sys);
}

static u32 g_ev_order[8];
static u32 g_ev_late[8];
static u32 g_ev_n;

static void test_ev_cb_a(PsxSystem *sys, u32 late) {
    (void)sys;
    if (g_ev_n < 8u) {
        g_ev_order[g_ev_n] = 1u;
        g_ev_late[g_ev_n] = late;
        g_ev_n++;
    }
}

static void test_ev_cb_b(PsxSystem *sys, u32 late) {
    (void)sys;
    if (g_ev_n < 8u) {
        g_ev_order[g_ev_n] = 2u;
        g_ev_late[g_ev_n] = late;
        g_ev_n++;
    }
}

static void test_ev_cb_c(PsxSystem *sys, u32 late) {
    (void)sys;
    if (g_ev_n < 8u) {
        g_ev_order[g_ev_n] = 3u;
        g_ev_late[g_ev_n] = late;
        g_ev_n++;
    }
}

int main(void) {
    /* ----------------------------------------------------------------------
     * 1. Fastmem RAM 4x mirrors across KUSEG / KSEG0 / KSEG1
     * ---------------------------------------------------------------------- */
    init_core_subsystems(false, false);
    PsxCpu *c = &g_sys.cpu;

    psx_write32(c, 0x00001234u, 0xCAFEBABEu);
    CHECK(psx_read32(c, 0x00201234u) == 0xCAFEBABEu);
    CHECK(psx_read32(c, 0x00401234u) == 0xCAFEBABEu);
    CHECK(psx_read32(c, 0x00601234u) == 0xCAFEBABEu);
    CHECK(psx_read32(c, 0x80001234u) == 0xCAFEBABEu);
    CHECK(psx_read32(c, 0x80601234u) == 0xCAFEBABEu);
    CHECK(psx_read32(c, 0xA0201234u) == 0xCAFEBABEu);
    CHECK(psx_read16(c, 0x80001234u) == 0xBABEu);
    CHECK(psx_read16(c, 0x80001236u) == 0xCAFEu);
    CHECK(psx_read8(c, 0xA0401235u) == 0xBAu);

    /* ----------------------------------------------------------------------
     * 2. Scratchpad at 0x1F800000 (KUSEG) & 0x9F800000 (KSEG0), NOT KSEG1!
     * ---------------------------------------------------------------------- */
    psx_write32(c, 0x1F800100u, 0x11223344u);
    CHECK(psx_read32(c, 0x9F800100u) == 0x11223344u);
    psx_write16(c, 0x9F800102u, 0x99AAu);
    CHECK(psx_read32(c, 0x1F800100u) == 0x99AA3344u);

    /* 0xBF800000 (KSEG1 scratchpad) is unmapped on real PSX -> DBE! */
    c->exc_pending = 0u;
    (void)psx_read32(c, 0xBF800000u);
    CHECK(c->exc_pending == 1u);
    CHECK(((c->cop0[COP0_CAUSE] >> 2) & 0x1Fu) == PSX_EXC_DBE);

    /* Past 1 KiB scratchpad on slow path (0x1F800400) & unmapped page (0x1F803000) -> DBE! */
    c->exc_pending = 0u;
    (void)psx_bus_read32_slow(c, 0x1F800400u);
    CHECK(c->exc_pending == 1u);
    CHECK(((c->cop0[COP0_CAUSE] >> 2) & 0x1Fu) == PSX_EXC_DBE);

    c->exc_pending = 0u;
    (void)psx_read32(c, 0x1F803000u);
    CHECK(c->exc_pending == 1u);
    CHECK(((c->cop0[COP0_CAUSE] >> 2) & 0x1Fu) == PSX_EXC_DBE);

    /* ----------------------------------------------------------------------
     * 3. BIOS ROM read-only & Cache Control 0xFFFE0130 & EXP2
     * ---------------------------------------------------------------------- */
    const u32 bios_word = 0x3C080013u;
    memcpy(g_sys.bus.bios + 0x100u, &bios_word, 4u);
    CHECK(psx_read32(c, 0x1FC00100u) == bios_word);
    CHECK(psx_read32(c, 0x9FC00100u) == bios_word);
    CHECK(psx_read32(c, 0xBFC00100u) == bios_word);
    psx_write32(c, 0xBFC00100u, 0xDEADBEEFu);
    CHECK(psx_read32(c, 0xBFC00100u) == bios_word);

    psx_write32(c, PSX_CACHE_CTRL_ADDR, 0x00000804u);
    CHECK(psx_read32(c, PSX_CACHE_CTRL_ADDR) == 0x00000804u);

    CHECK(psx_read8(c, 0x9F802021u) == 0x04u); /* DUART_SRB TXRDY */
    psx_write8(c, 0x9F802041u, 0x09u);
    CHECK(g_sys.bus.exp2_post == 0x09u);

    /* ----------------------------------------------------------------------
     * 4. SR.IsC cache isolation: drops RAM writes without wiping RAM,
     *    but still checks alignment/KUc BEFORE dropping!
     * ---------------------------------------------------------------------- */
    psx_write32(c, 0x80002000u, 0xA5A55A5Au);
    cpu_set_sr(&g_sys, PSX_SR_BEV | PSX_SR_ISC);
    CHECK(g_sys.bus.isolated == true);
    psx_write32(c, 0x80002000u, 0x00000000u);
    psx_write32(c, 0x00002000u, 0x00000000u);
    psx_write32(c, 0x1F800100u, 0x00000000u);
    /* Misaligned write while IsC=1 MUST still raise AdES! */
    c->exc_pending = 0u;
    psx_write32(c, 0x80002001u, 0x12345678u);
    CHECK(c->exc_pending == 1u);
    CHECK(((c->cop0[COP0_CAUSE] >> 2) & 0x1Fu) == PSX_EXC_ADES);
    CHECK(c->cop0[COP0_BADVADDR] == 0x80002001u);

    cpu_set_sr(&g_sys, PSX_SR_BEV);
    CHECK(g_sys.bus.isolated == false);
    CHECK(psx_read32(c, 0x80002000u) == 0xA5A55A5Au);
    CHECK(psx_read32(c, 0x1F800100u) == 0x99AA3344u);

    /* Also verify cfg.isc_swap_table mode */
    init_core_subsystems(true, true);
    c = &g_sys.cpu;
    psx_write32(c, 0x80000100u, 0x55667788u);
    cpu_set_sr(&g_sys, PSX_SR_BEV | PSX_SR_ISC);
    CHECK(c->fastmem_w.p == g_sys.bus.fm_w_isc_null);
    psx_write32(c, 0x80000100u, 0u);
    cpu_set_sr(&g_sys, PSX_SR_BEV);
    CHECK(c->fastmem_w.p == g_sys.bus.fm_w);
    CHECK(psx_read32(c, 0x80000100u) == 0x55667788u);

    /* strict_ram_size with default RAM_SIZE=0x0B88 (mode 5 -> 2 MiB window): 0x80200000 faults! */
    c->exc_pending = 0u;
    (void)psx_read32(c, 0x80200000u);
    CHECK(c->exc_pending == 1u);
    CHECK(((c->cop0[COP0_CAUSE] >> 2) & 0x1Fu) == PSX_EXC_DBE);

    /* ----------------------------------------------------------------------
     * 5. SMC write-trapping, identical-rewrite skip, and in_jit fault abort
     * ---------------------------------------------------------------------- */
    init_core_subsystems(false, false);
    c = &g_sys.cpu;
    g_sys.cfg.smc_trap_budget = 2u;

    u32 page = 3u; /* phys 0x00003000 */
    psx_write32(c, 0x80003010u, 0x12345678u);
    bus_code_page_ref(&g_sys.bus, page);
    CHECK(g_sys.bus.fm_w[0x00003u] == 0u);
    CHECK(g_sys.bus.fm_w[0x80003u] == 0u);
    CHECK(g_sys.bus.fm_w[0xA0003u] == 0u);

    /* Identical rewrites increment code_page_traps without invalidating */
    psx_write32(c, 0x80003010u, 0x12345678u);
    CHECK(g_sys.bus.code_page_traps[page] == 1u);
    CHECK(g_sys.bus.code_page_mode[page] == (u8)PSX_PAGE_TRAP);
    psx_write32(c, 0x80003010u, 0x12345678u);
    psx_write32(c, 0x80003010u, 0x12345678u); /* exceeds budget=2 -> flips to VERIFY */
    CHECK(g_sys.bus.code_page_mode[page] == (u8)PSX_PAGE_VERIFY);
    CHECK(g_sys.bus.fm_w[0x80003u] != 0u);

    bus_code_page_unref(&g_sys.bus, page);
    CHECK(g_sys.bus.code_page_refs[page] == 0u);
    CHECK(g_sys.bus.code_page_mode[page] == (u8)PSX_PAGE_TRAP);

    /* in_jit fault recording: sets jit_abort + abort_badvaddr without modifying PC/EPC */
    c->pc = 0x80010000u;
    c->cop0[COP0_EPC] = 0x11111110u;
    c->in_jit = 1u;
    c->jit_abort = 0u;
    (void)psx_read32(c, 0x80000003u);
    CHECK(c->jit_abort == PSX_EXC_ADEL + 1u);
    CHECK(c->abort_badvaddr == 0x80000003u);
    CHECK(c->pc == 0x80010000u);
    CHECK(c->cop0[COP0_EPC] == 0x11111110u);
    c->in_jit = 0u;
    c->jit_abort = 0u;

    /* ----------------------------------------------------------------------
     * 6. Instruction fetch rules (Scratchpad -> IBE, MMIO -> succeeds!)
     * ---------------------------------------------------------------------- */
    c->exc_pending = 0u;
    (void)psx_bus_fetch32(c, 0x1F800000u);
    CHECK(c->exc_pending == 1u && ((c->cop0[COP0_CAUSE] >> 2) & 0x1Fu) == PSX_EXC_IBE);

    c->exc_pending = 0u;
    (void)psx_bus_fetch32(c, 0x9F800000u);
    CHECK(c->exc_pending == 1u && ((c->cop0[COP0_CAUSE] >> 2) & 0x1Fu) == PSX_EXC_IBE);

    /* Store `jr $ra` (0x03E00008) in D0_BCR (0x1F801084) & DPCR (0x1F8010F0) and fetch! */
    psx_write32(c, PSX_REG_DMA_BCR(0), 0x03E00008u);
    c->exc_pending = 0u;
    u32 fetched_mmio = psx_bus_fetch32(c, 0x9F801084u);
    CHECK(c->exc_pending == 0u && fetched_mmio == 0x03E00008u);

    psx_write32(c, PSX_REG_DPCR, 0x03E00008u);
    c->exc_pending = 0u;
    CHECK(psx_bus_fetch32(c, 0x1F8010F0u) == 0x03E00008u && c->exc_pending == 0u);

    /* I_STAT/I_MASK (0x1F801070), MDEC (0x1F801820), & unmapped MMIO holes -> IBE! */
    c->exc_pending = 0u;
    (void)psx_bus_fetch32(c, 0x1F801070u);
    CHECK(c->exc_pending == 1u && ((c->cop0[COP0_CAUSE] >> 2) & 0x1Fu) == PSX_EXC_IBE);
    c->exc_pending = 0u;
    (void)psx_bus_fetch32(c, 0x1F801820u);
    CHECK(c->exc_pending == 1u && ((c->cop0[COP0_CAUSE] >> 2) & 0x1Fu) == PSX_EXC_IBE);
    c->exc_pending = 0u;
    (void)psx_bus_fetch32(c, 0x1F801900u);
    CHECK(c->exc_pending == 1u && ((c->cop0[COP0_CAUSE] >> 2) & 0x1Fu) == PSX_EXC_IBE);

    /* ----------------------------------------------------------------------
     * 7. Scheduler 32-bit wrap-around, slice shortening, idle_skip & IRQ
     * ---------------------------------------------------------------------- */
    sched_reset(&g_sys);
    g_sys.sched.now = 0xFFFFFFF0u;
    g_sys.sched.slice_start = 0xFFFFFFF0u;
    c->slice_len = 100;
    c->cycles_left = 100;

    g_ev_n = 0u;
    sched_set_handler(&g_sys.sched, EV_CDROM, test_ev_cb_a);
    sched_set_handler(&g_sys.sched, EV_SPU, test_ev_cb_b);
    sched_set_handler(&g_sys.sched, EV_SIO0, test_ev_cb_c);

    sched_schedule(&g_sys, EV_SPU, 0x20u);   /* fires at 0x00000010 */
    sched_schedule(&g_sys, EV_CDROM, 0x08u); /* fires at 0xFFFFFFF8 (shortens slice to 8!) */
    sched_schedule(&g_sys, EV_SIO0, 0x30u);  /* fires at 0x00000020 */
    CHECK(c->cycles_left == 8 && c->slice_len == 8);

    /* Consume 0x34 cycles across 2^32 wrap (overshooting EV_CDROM, EV_SPU, EV_SIO0) */
    c->cycles_left = 8 - 0x34;
    sched_run(&g_sys);
    CHECK(g_sys.sched.now == 0x00000024u);
    CHECK(g_ev_n == 3u);
    CHECK(g_ev_order[0] == 1u && g_ev_late[0] == 0x2Cu);
    CHECK(g_ev_order[1] == 2u && g_ev_late[1] == 0x14u);
    CHECK(g_ev_order[2] == 3u && g_ev_late[2] == 0x04u);

    /* Test idle_skip jumping directly to next event */
    g_ev_n = 0u;
    sched_schedule(&g_sys, EV_CDROM, 500u);
    c->idle_skip = 1u;
    c->cycles_left = 0;
    sched_run(&g_sys);
    CHECK(g_ev_n == 1u && g_ev_late[0] == 0u);
    CHECK(g_sys.sched.now == 0x00000024u + 500u);

    /* Test I_STAT byte-lane write-0-to-clear & I_MASK -> Cause.IP2 -> IRQ vector */
    psx_write32(c, 0xBF801074u, (1u << IRQ_VBLANK) | (1u << IRQ_GPU) | (1u << IRQ_SPU));
    irq_raise(&g_sys, IRQ_VBLANK);
    irq_raise(&g_sys, IRQ_SPU); /* bit 9 (in byte 1) */
    CHECK(psx_read32(c, 0xBF801070u) == ((1u << IRQ_VBLANK) | (1u << IRQ_SPU)));
    CHECK((c->cop0[COP0_CAUSE] & PSX_CAUSE_IP2) != 0u);

    /* 8-bit write to byte 0 clearing VBLANK must NOT clear SPU in byte 1! */
    psx_write8(c, 0xBF801070u, 0xFEu);
    CHECK(psx_read32(c, 0xBF801070u) == (1u << IRQ_SPU));
    CHECK((c->cop0[COP0_CAUSE] & PSX_CAUSE_IP2) != 0u);

    /* Enable IEc + IM2 (bit 10) in SR and verify sched_run dispatches IRQ vector */
    c->pc = 0x80010200u;
    c->next_pc = 0x80010204u;
    cpu_set_sr(&g_sys, PSX_SR_IEC | PSX_CAUSE_IP2); /* SR.BEV=0 -> vector 0x80000080 */
    CHECK(c->irq_pending == 1u);
    sched_run(&g_sys);
    CHECK(c->pc == PSX_EXC_VEC_RAM);
    CHECK(c->cop0[COP0_EPC] == 0x80010200u);
    CHECK(((c->cop0[COP0_CAUSE] >> 2) & 0x1Fu) == PSX_EXC_INT);

    arena_destroy(&g_sys.arena);
    printf("test_core_bus_sched PASS\n");
    return 0;
}
