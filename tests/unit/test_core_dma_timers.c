/*
 * Unit tests for Slice A: 7-Channel DMA Controller & Root Counters (Timers 0/1/2).
 * Covers OTC D6_CHCR hardwired masks & BCR=0, chunked DMA2 linked-list & self-loop
 * abort (chain-looping), SyncMode 0 chopping, SyncMode 1 blocks, DPCR priority/enable,
 * DICR write-1-to-clear & IRQ3 edge, plus Timers clock sources, target+1 reset,
 * MODE flag ack rules, sync/gate modes, and scheduled timer IRQs.
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

static void setup_dma_timer_test(void) {
    arena_destroy(&g_sys.arena);
    memset(&g_sys, 0, sizeof(g_sys));
    psx_config_defaults(&g_sys.cfg, true);
    g_sys.cfg.cycle_mult = 256u;

    (void)arena_init(&g_sys.arena, 20u * 1024u * 1024u);
    sched_init(&g_sys.sched);
    bus_init(&g_sys);
    irq_init(&g_sys);
    cpu_init(&g_sys);
    dma_init(&g_sys);
    timers_init(&g_sys);
    gte_init(&g_sys);

    cpu_set_sr(&g_sys, 0u);
    g_sys.cpu.cycles_left = 10000;
    g_sys.cpu.slice_len = 10000;
}

static void advance_cycles(u32 cyc) {
    PsxCpu *c = &g_sys.cpu;
    u32 rem = cyc;
    while (rem > 0u) {
        u32 step = (c->cycles_left > 0 && (u32)c->cycles_left < rem)
                       ? (u32)c->cycles_left
                       : rem;
        c->cycles_left -= (s32)step;
        rem -= step;
        if (c->cycles_left <= 0) {
            sched_run(&g_sys);
        }
    }
}

int main(void) {
    /* ----------------------------------------------------------------------
     * 1. DMA6 OTC: D6_CHCR hardwired bits, trigger guards, descending chain
     * ---------------------------------------------------------------------- */
    setup_dma_timer_test();
    PsxCpu *c = &g_sys.cpu;

    /* Reset value of D6_CHCR has bit 1 hardwired to 1 */
    CHECK(psx_read32(c, PSX_REG_DMA_CHCR(6)) == 0x00000002u);

    /* Hardwired zero/one test (otc-test.exe): write all bits except Start bit 24 */
    psx_write32(c, PSX_REG_DMA_CHCR(6), 0xFEFFFFFFu);
    CHECK(psx_read32(c, PSX_REG_DMA_CHCR(6)) == 0x50000002u);

    const u32 dpcr_all_en = PSX_DPCR_DEFAULT | 0x08888888u;
    psx_write32(c, PSX_REG_DPCR, dpcr_all_en);

    /* Won't start without trigger bit 28 */
    psx_write32(c, 0x8000400Cu, 0xAAAAAAAAu);
    psx_write32(c, PSX_REG_DMA_MADR(6), 0x0000400Cu);
    psx_write32(c, PSX_REG_DMA_BCR(6), 4u);
    psx_write32(c, PSX_REG_DMA_CHCR(6), 0x01000002u);
    CHECK(psx_read32(c, 0x8000400Cu) == 0xAAAAAAAAu);

    /* Won't start when DPCR ch6 master enable (bit 27) is 0 */
    psx_write32(c, PSX_REG_DPCR, dpcr_all_en & ~(1u << 27));
    psx_write32(c, PSX_REG_DMA_CHCR(6), 0x11000002u);
    CHECK(psx_read32(c, 0x8000400Cu) == 0xAAAAAAAAu);

    /* Re-enabling DPCR bit 27 immediately runs the armed OTC transfer! */
    psx_write32(c, PSX_REG_DICR, (1u << 23) | (1u << (16 + 6))); /* master + ch6 IRQ */
    psx_write32(c, PSX_REG_DPCR, dpcr_all_en);
    CHECK(psx_read32(c, PSX_REG_DMA_CHCR(6)) == 0x00000002u);
    CHECK(psx_read32(c, 0x8000400Cu) == 0x00004008u);
    CHECK(psx_read32(c, 0x80004008u) == 0x00004004u);
    CHECK(psx_read32(c, 0x80004004u) == 0x00004000u);
    CHECK(psx_read32(c, 0x80004000u) == 0x00FFFFFFu);

    /* Verify DICR ch6 flag (bit 30) + master flag (bit 31) + I_STAT IRQ_DMA */
    u32 dicr = psx_read32(c, PSX_REG_DICR);
    CHECK((dicr & 0xC0000000u) == 0xC0000000u);
    CHECK((psx_read32(c, PSX_REG_I_STAT) & (1u << IRQ_DMA)) != 0u);

    /* Acknowledge IRQ_DMA + byte-3 write-1-to-clear on DICR (0x1F8010F7) */
    psx_write32(c, PSX_REG_I_STAT, ~(1u << IRQ_DMA));
    psx_write8(c, PSX_REG_DICR + 3u, 0x40u); /* clears bit 30 without touching bits 0..23 */
    dicr = psx_read32(c, PSX_REG_DICR);
    CHECK((dicr & 0xC0000000u) == 0u);
    CHECK((dicr & ((1u << 23) | (1u << 22))) == ((1u << 23) | (1u << 22)));

    /* OTC with chopping bits written still clears to 0x00000002 after transfer */
    psx_write32(c, PSX_REG_DMA_MADR(6), 0x00004020u);
    psx_write32(c, PSX_REG_DMA_BCR(6), 2u);
    psx_write32(c, PSX_REG_DMA_CHCR(6), 0x11110102u);
    CHECK(psx_read32(c, PSX_REG_DMA_CHCR(6)) == 0x00000002u);
    CHECK(psx_read32(c, 0x80004020u) == 0x0000401Cu);
    CHECK(psx_read32(c, 0x8000401Cu) == 0x00FFFFFFu);

    /* ----------------------------------------------------------------------
     * 2. DMA2 Linked-List: finite chain completion & self-loop concurrent abort
     * ---------------------------------------------------------------------- */
    psx_write8(c, PSX_REG_DICR + 3u, 0x7Fu);
    psx_write32(c, PSX_REG_I_STAT, 0u);
    psx_write32(c, PSX_REG_DICR, (1u << 23) | (1u << (16 + 2)));

    /* Finite 2-node chain at 0x5000 -> 0x5010 -> 0xFFFFFF */
    psx_write32(c, 0x80005000u, (2u << 24) | 0x005010u);
    psx_write32(c, 0x80005004u, 0x00000000u);
    psx_write32(c, 0x80005008u, 0x00000000u);
    psx_write32(c, 0x80005010u, (0u << 24) | 0xFFFFFFu);

    psx_write32(c, PSX_REG_DMA_MADR(2), 0x00005000u);
    psx_write32(c, PSX_REG_DMA_CHCR(2), 0x01000401u); /* sync=2, from RAM, start */
    CHECK((psx_read32(c, PSX_REG_DMA_CHCR(2)) & (1u << 24)) == 0u);
    CHECK((psx_read32(c, PSX_REG_DICR) & (1u << 26)) != 0u);

    /* Self-referencing chain (chain-looping.exe): must NOT hang CPU or finish! */
    psx_write8(c, PSX_REG_DICR + 3u, 0x7Fu);
    psx_write32(c, PSX_REG_I_STAT, 0u);
    psx_write32(c, 0x80006000u, (0u << 24) | 0x006000u); /* points to itself! */

    psx_write32(c, PSX_REG_DMA_MADR(2), 0x00006000u);
    psx_write32(c, PSX_REG_DMA_CHCR(2), 0x01000401u);
    /* Busy stays 1, IRQ stays 0 while CPU advances 500 cycles across EV_DMA ticks */
    CHECK((psx_read32(c, PSX_REG_DMA_CHCR(2)) & (1u << 24)) != 0u);
    CHECK((psx_read32(c, PSX_REG_DICR) & (1u << 26)) == 0u);
    advance_cycles(500u);
    CHECK((psx_read32(c, PSX_REG_DMA_CHCR(2)) & (1u << 24)) != 0u);
    CHECK((psx_read32(c, PSX_REG_DICR) & (1u << 26)) == 0u);

    /* CPU aborts loop by writing 0 to D2_CHCR */
    psx_write32(c, PSX_REG_DMA_CHCR(2), 0u);
    CHECK(g_sys.dma.running_mask == 0u);
    CHECK((g_sys.sched.active_mask & (1u << EV_DMA)) == 0u);

    /* Chopped SyncMode 0 DMA (PIO -> RAM fills 0xFFFFFFFF across windows) */
    psx_write32(c, 0x80007000u, 0u);
    psx_write32(c, 0x8000701Cu, 0u);
    psx_write32(c, PSX_REG_DMA_MADR(5), 0x00007000u);
    psx_write32(c, PSX_REG_DMA_BCR(5), 8u); /* 8 words, dma_win = 2^1 = 2 words, cpu_win = 2^3 = 8 cyc */
    psx_write32(c, PSX_REG_DMA_CHCR(5), 0x11310100u);
    /* First 2 words done immediately, remaining 6 words queued on EV_DMA */
    CHECK((psx_read32(c, PSX_REG_DMA_CHCR(5)) & (1u << 24)) != 0u);
    CHECK(psx_read32(c, 0x80007000u) == 0xFFFFFFFFu);
    CHECK(psx_read32(c, 0x8000701Cu) == 0u);
    advance_cycles(200u);
    CHECK((psx_read32(c, PSX_REG_DMA_CHCR(5)) & (1u << 24)) == 0u);
    CHECK(psx_read32(c, 0x8000701Cu) == 0xFFFFFFFFu);

    /* ----------------------------------------------------------------------
     * 3. Timers 0/1/2: Target+1 reset, sysclk/8 sub-tick accuracy, MODE flags,
     *    Sync modes, and Scheduled Timer IRQs
     * ---------------------------------------------------------------------- */
    setup_dma_timer_test();
    c = &g_sys.cpu;

    /* Timer 2 reset-on-target = 10: reaches 10 (observable!), resets to 0 on tick 11 */
    psx_write32(c, PSX_REG_TMR_TARGET(2), 10u);
    psx_write32(c, PSX_REG_TMR_MODE(2), 0x0008u); /* reset_on_target=1, sysclk */
    (void)psx_read32(c, PSX_REG_TMR_MODE(2));     /* clear flags */

    advance_cycles(10u);
    CHECK(psx_read16(c, PSX_REG_TMR_VAL(2)) == 10u);
    u32 m2 = psx_read16(c, PSX_REG_TMR_MODE(2));
    CHECK((m2 & (1u << 11)) != 0u); /* reached_target == 1 */
    CHECK((m2 & (1u << 12)) == 0u); /* reached_ffff == 0 */
    /* Reading MODE cleared reached_target! */
    CHECK((psx_read16(c, PSX_REG_TMR_MODE(2)) & (1u << 11)) == 0u);

    /* Next cycle (tick 11): resets to 0 (11 is NEVER reached!) */
    advance_cycles(1u);
    CHECK(psx_read16(c, PSX_REG_TMR_VAL(2)) == 0u);

    /* Writing MODE resets counter to 0, sets bit 10=1, and PRESERVES reached_target! */
    advance_cycles(10u); /* reaches target=10 again */
    psx_write16(c, PSX_REG_TMR_MODE(2), 0x0008u);
    CHECK(psx_read16(c, PSX_REG_TMR_VAL(2)) == 0u);
    u32 m2_after_write = psx_read16(c, PSX_REG_TMR_MODE(2));
    CHECK((m2_after_write & ((1u << 11) | (1u << 10))) == ((1u << 11) | (1u << 10)));
    CHECK((psx_read16(c, PSX_REG_TMR_MODE(2)) & (1u << 11)) == 0u);

    /* Timer 2 sysclk/8: tight 3-cycle polling must NOT lose fractional cycles! */
    psx_write16(c, PSX_REG_TMR_MODE(2), 0x0200u); /* clk = sysclk/8, wrap at 0xFFFF */
    for (u32 step = 0u; step < 8u; step++) {
        advance_cycles(3u); /* 8 * 3 = 24 CPU cycles = 3 timer ticks */
        (void)psx_read16(c, PSX_REG_TMR_VAL(2));
    }
    CHECK(psx_read16(c, PSX_REG_TMR_VAL(2)) == 3u);

    /* Timer 2 sync mode 0 stops counter */
    psx_write16(c, PSX_REG_TMR_MODE(2), 0x0001u); /* sync_en=1, sync_mode=0 -> stop */
    advance_cycles(100u);
    CHECK(psx_read16(c, PSX_REG_TMR_VAL(2)) == 0u);

    /* Timer 0 Toggle + Repeat IRQ on Target: fires IRQ4 ONLY on 1 -> 0 edge of bit 10! */
    psx_write32(c, PSX_REG_I_STAT, 0u);
    psx_write16(c, PSX_REG_TMR_TARGET(0), 49u); /* period = 50 cycles */
    /* bits: reset_tgt(3) | irq_tgt(4) | repeat(6) | toggle(7) = 0x00D8 */
    psx_write16(c, PSX_REG_TMR_MODE(0), 0x00D8u);

    /* Hit #1 at 49 cycles: bit 10 toggles 1 -> 0 -> raises IRQ_TMR0! */
    advance_cycles(50u);
    CHECK((psx_read32(c, PSX_REG_I_STAT) & (1u << IRQ_TMR0)) != 0u);
    CHECK((psx_read16(c, PSX_REG_TMR_MODE(0)) & (1u << 10)) == 0u);
    psx_write32(c, PSX_REG_I_STAT, ~(1u << IRQ_TMR0));

    /* Hit #2 at +50 cycles: bit 10 toggles 0 -> 1 -> does NOT raise IRQ_TMR0! */
    advance_cycles(50u);
    CHECK((psx_read32(c, PSX_REG_I_STAT) & (1u << IRQ_TMR0)) == 0u);
    CHECK((psx_read16(c, PSX_REG_TMR_MODE(0)) & (1u << 10)) != 0u);

    /* Hit #3 at +50 cycles: bit 10 toggles 1 -> 0 -> raises IRQ_TMR0 again! */
    advance_cycles(50u);
    CHECK((psx_read32(c, PSX_REG_I_STAT) & (1u << IRQ_TMR0)) != 0u);

    /* Timer 1 HBlank clock source + VBlank frame boundary (~566,124 cycles NTSC) */
    psx_write32(c, PSX_REG_I_STAT, 0u);
    psx_write16(c, PSX_REG_TMR_MODE(1), 0x0100u); /* HBlank clock source */
    advance_cycles(PSX_CYCLES_PER_FRAME_NTSC + 5000u);
    CHECK(psx_read16(c, PSX_REG_TMR_VAL(1)) >= 260u);
    CHECK((psx_read32(c, PSX_REG_I_STAT) & (1u << IRQ_VBLANK)) != 0u);
    CHECK(g_sys.frame_done == true);

    arena_destroy(&g_sys.arena);
    printf("test_core_dma_timers PASS\n");
    return 0;
}
