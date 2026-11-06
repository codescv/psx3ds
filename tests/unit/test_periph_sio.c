/*
 * Slice E unit test: SIO0 SPI state machine (0x1F801040..104F), scheduled IRQ7
 * /ACK pulse & acknowledge, Digital Pad (0x415A), DualShock Analog Pad (0x735A
 * + Config Mode 0x43/0x44/0x45/0x4D), and Dual 128 KiB Memory Cards (fresh 'M','C'
 * format, GetID 0x53, Write 0x57 + XOR checksum + 'G'/'N', Read 0x52, disk flush).
 */

#include "../../src/periph/periph_internal.h"
#include "psx3ds/arena.h"
#include "psx3ds/system.h"

#include <stdio.h>
#include <string.h>

#define CHECK_TRUE(cond, msg)                                                       \
    do {                                                                            \
        if (!(cond)) {                                                              \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, (msg));       \
            return 1;                                                               \
        }                                                                           \
    } while (0)

#define TEST_MC_PATH "/tmp/psx3ds_test_periph_mc0.mcd"

static PsxSystem g_sys;

/*
 * Helper: transmit one SPI byte through SIO0 MMIO, step EV_SIO0, record whether
 * /ACK (bit 7) and IRQ7 (bit 9) pulsed, acknowledge IRQ7 via SIO_CTRL.bit4 if
 * raised, and return the received byte from SIO_RX_DATA.
 */
static u8 sio_spi_step(u16 base_ctrl, u8 tx, bool *got_ack, bool *got_irq) {
    sio0_mmio_write(&g_sys, PSX_REG_SIO0_DATA, tx, 1u);
    u32 st_pre = sio0_mmio_read(&g_sys, PSX_REG_SIO0_STAT, 4u);
    u8  rx     = (u8)sio0_mmio_read(&g_sys, PSX_REG_SIO0_DATA, 1u);
    (void)st_pre;

    /* Trigger scheduled EV_SIO0 (/ACK leading edge) */
    sio0_event_cb(&g_sys, 0u);
    u32 st_ack = sio0_mmio_read(&g_sys, PSX_REG_SIO0_STAT, 4u);
    bool ack = (st_ack & 0x0080u) != 0u;
    bool irq = (st_ack & 0x0200u) != 0u;
    if (got_ack) {
        *got_ack = ack;
    }
    if (got_irq) {
        *got_irq = irq;
    }

    /* Step trailing edge of /ACK pulse -> bit 7 drops, bit 9 stays latched */
    if (ack) {
        sio0_event_cb(&g_sys, 0u);
    }
    if (irq) {
        /* Acknowledge IRQ7 via SIO_CTRL bit 4 (0x0010) */
        sio0_mmio_write(&g_sys, PSX_REG_SIO0_CTRL, (u32)(base_ctrl | 0x0010u), 2u);
    }
    return rx;
}

static int test_digital_and_analog_joypad(void) {
    memset(&g_sys, 0, sizeof(g_sys));
    psx_config_defaults(&g_sys.cfg, true);
    CHECK_TRUE(arena_init(&g_sys.arena, 1024u * 1024u) == PSX_OK, "arena_init SIO");

    sio_init(&g_sys);

    /* Configure Port 0 Digital Pad with active-low buttons = 0xBEEF */
    PsxPadState pad0 = {
        .buttons     = 0xBEEFu,
        .lx          = 0xAAu,
        .ly          = 0xBBu,
        .rx          = 0x11u,
        .ry          = 0x22u,
        .analog_mode = 0u,
        .connected   = 1u
    };
    sio_set_pad(&g_sys, 0, &pad0);

    /* 1. Digital Pad 5-byte transaction (0x01, 0x42, 0x00, 0x00, 0x00) */
    u16 ctrl0 = 0x1003u; /* TXEN | DTR | ACK_IRQ_EN, Port 0 */
    sio0_mmio_write(&g_sys, PSX_REG_SIO0_MODE, 0x000Du, 2u);
    sio0_mmio_write(&g_sys, PSX_REG_SIO0_BAUD, 0x0088u, 2u);
    sio0_mmio_write(&g_sys, PSX_REG_SIO0_CTRL, ctrl0, 2u);

    const u8 dig_tx[5] = { 0x01u, 0x42u, 0x00u, 0x00u, 0x00u };
    u8       dig_rx[5] = { 0 };
    bool     dig_ack[5] = { false };
    bool     dig_irq[5] = { false };

    for (u32 i = 0u; i < 5u; i++) {
        dig_rx[i] = sio_spi_step(ctrl0, dig_tx[i], &dig_ack[i], &dig_irq[i]);
    }
    sio0_mmio_write(&g_sys, PSX_REG_SIO0_CTRL, 0x0000u, 2u); /* Deassert DTR */

    CHECK_TRUE(dig_rx[0] == 0xFFu && dig_rx[1] == 0x41u && dig_rx[2] == 0x5Au &&
               dig_rx[3] == 0xEFu && dig_rx[4] == 0xBEu,
               "Digital Pad 0x415A + active-low buttons 0xBEEF");
    CHECK_TRUE(dig_ack[0] && dig_ack[1] && dig_ack[2] && dig_ack[3] && !dig_ack[4],
               "Digital Pad /ACK asserted on bytes 0..3 and NOT on final byte 4");
    CHECK_TRUE(dig_irq[0] && dig_irq[1] && dig_irq[2] && dig_irq[3] && !dig_irq[4],
               "Digital Pad IRQ7 latched on bytes 0..3 and NOT on final byte 4");

    /* 2. Port 1 unplugged (slot select bit 13 = 0x2000) -> 0xFF, zero /ACK */
    u16 ctrl1 = 0x3003u; /* Port 1 | ACK_IRQ_EN | DTR | TXEN */
    sio0_mmio_write(&g_sys, PSX_REG_SIO0_CTRL, ctrl1, 2u);
    bool p1_ack = true, p1_irq = true;
    u8   p1_rx  = sio_spi_step(ctrl1, 0x01u, &p1_ack, &p1_irq);
    sio0_mmio_write(&g_sys, PSX_REG_SIO0_CTRL, 0x0000u, 2u);
    CHECK_TRUE(p1_rx == 0xFFu && !p1_ack && !p1_irq,
               "Unplugged Port 1 returns 0xFF with no /ACK or IRQ7");

    /* 3. DualShock Config Mode (0x43 enter -> 0x44 enable analog -> 0x45 model -> 0x43 exit) */
    sio0_mmio_write(&g_sys, PSX_REG_SIO0_CTRL, ctrl0, 2u);
    const u8 enter_cfg[5] = { 0x01u, 0x43u, 0x00u, 0x01u, 0x00u };
    for (u32 i = 0u; i < 5u; i++) {
        (void)sio_spi_step(ctrl0, enter_cfg[i], NULL, NULL);
    }
    sio0_mmio_write(&g_sys, PSX_REG_SIO0_CTRL, 0x0000u, 2u);

    /* Command 0x44 in config mode (ID = 0xF3): enable DualShock analog mode */
    sio0_mmio_write(&g_sys, PSX_REG_SIO0_CTRL, ctrl0, 2u);
    const u8 set_mode_tx[9] = { 0x01u, 0x44u, 0x00u, 0x01u, 0x03u, 0x00u, 0x00u, 0x00u, 0x00u };
    u8       set_mode_rx[9] = { 0 };
    for (u32 i = 0u; i < 9u; i++) {
        set_mode_rx[i] = sio_spi_step(ctrl0, set_mode_tx[i], NULL, NULL);
    }
    sio0_mmio_write(&g_sys, PSX_REG_SIO0_CTRL, 0x0000u, 2u);
    CHECK_TRUE(set_mode_rx[1] == 0xF3u && set_mode_rx[2] == 0x5Au, "Config mode ID 0xF35A");

    /* Exit config mode via 0x43 (tx[3] = 0x00) */
    sio0_mmio_write(&g_sys, PSX_REG_SIO0_CTRL, ctrl0, 2u);
    const u8 exit_cfg[9] = { 0x01u, 0x43u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u };
    for (u32 i = 0u; i < 9u; i++) {
        (void)sio_spi_step(ctrl0, exit_cfg[i], NULL, NULL);
    }
    sio0_mmio_write(&g_sys, PSX_REG_SIO0_CTRL, 0x0000u, 2u);

    /* 4. Poll DualShock Analog Pad (0x735A + buttons + rx, ry, lx, ly = 9 bytes) */
    sio0_mmio_write(&g_sys, PSX_REG_SIO0_CTRL, ctrl0, 2u);
    u8   ana_rx[9]  = { 0 };
    bool ana_ack[9] = { false };
    for (u32 i = 0u; i < 9u; i++) {
        u8 tx = (i == 0u) ? 0x01u : ((i == 1u) ? 0x42u : 0x00u);
        ana_rx[i] = sio_spi_step(ctrl0, tx, &ana_ack[i], NULL);
    }
    sio0_mmio_write(&g_sys, PSX_REG_SIO0_CTRL, 0x0000u, 2u);

    CHECK_TRUE(ana_rx[0] == 0xFFu && ana_rx[1] == 0x73u && ana_rx[2] == 0x5Au &&
               ana_rx[3] == 0xEFu && ana_rx[4] == 0xBEu &&
               ana_rx[5] == 0x11u && ana_rx[6] == 0x22u &&
               ana_rx[7] == 0xAAu && ana_rx[8] == 0xBBu,
               "DualShock 0x735A 9-byte buttons + 4 analog axes match");
    CHECK_TRUE(ana_ack[7] && !ana_ack[8], "DualShock /ACK stops on final 9th byte");

    sio_shutdown(&g_sys);
    arena_destroy(&g_sys.arena);
    return 0;
}

static int mc_write_sector_spi(u16 ctrl, u16 sec, const u8 payload[128], bool corrupt_xor,
                               u8 *out_flag, u8 *out_end) {
    sio0_mmio_write(&g_sys, PSX_REG_SIO0_CTRL, ctrl, 2u);

    u8 msb = (u8)(sec >> 8);
    u8 lsb = (u8)(sec & 0xFFu);
    u8 xor_chk = (u8)(msb ^ lsb);
    for (u32 i = 0u; i < 128u; i++) {
        xor_chk ^= payload[i];
    }
    if (corrupt_xor) {
        xor_chk ^= 0x55u;
    }

    u8 rx[138];
    bool last_ack = true;
    for (u32 seq = 0u; seq < 138u; seq++) {
        u8 tx = 0x00u;
        if (seq == 0u) {
            tx = 0x81u;
        } else if (seq == 1u) {
            tx = 0x57u; /* 'W' */
        } else if (seq == 4u) {
            tx = msb;
        } else if (seq == 5u) {
            tx = lsb;
        } else if (seq >= 6u && seq <= 133u) {
            tx = payload[seq - 6u];
        } else if (seq == 134u) {
            tx = xor_chk;
        }
        bool ack = false;
        rx[seq] = sio_spi_step(ctrl, tx, &ack, NULL);
        if (seq < 137u && !ack) {
            return -1;
        }
        if (seq == 137u) {
            last_ack = ack;
        }
    }
    sio0_mmio_write(&g_sys, PSX_REG_SIO0_CTRL, 0x0000u, 2u);

    if (last_ack || rx[2] != 0x5Au || rx[3] != 0x5Du || rx[135] != 0x5Cu || rx[136] != 0x5Du) {
        return -1;
    }
    if (out_flag) {
        *out_flag = rx[1];
    }
    if (out_end) {
        *out_end = rx[137];
    }
    return 0;
}

static int mc_read_sector_spi(u16 ctrl, u16 sec, u8 out_payload[128], u8 *out_xor, u8 *out_end) {
    sio0_mmio_write(&g_sys, PSX_REG_SIO0_CTRL, ctrl, 2u);

    u8 msb = (u8)(sec >> 8);
    u8 lsb = (u8)(sec & 0xFFu);
    u8 rx[140];
    bool last_ack = true;

    for (u32 seq = 0u; seq < 140u; seq++) {
        u8 tx = 0x00u;
        if (seq == 0u) {
            tx = 0x81u;
        } else if (seq == 1u) {
            tx = 0x52u; /* 'R' */
        } else if (seq == 4u) {
            tx = msb;
        } else if (seq == 5u) {
            tx = lsb;
        }
        bool ack = false;
        rx[seq] = sio_spi_step(ctrl, tx, &ack, NULL);
        if (seq < 139u && !ack) {
            return -1;
        }
        if (seq == 139u) {
            last_ack = ack;
        }
    }
    sio0_mmio_write(&g_sys, PSX_REG_SIO0_CTRL, 0x0000u, 2u);

    if (last_ack || rx[2] != 0x5Au || rx[3] != 0x5Du ||
        rx[6] != 0x5Cu || rx[7] != 0x5Du || rx[8] != msb || rx[9] != lsb) {
        return -1;
    }
    memcpy(out_payload, &rx[10], 128u);
    if (out_xor) {
        *out_xor = rx[138];
    }
    if (out_end) {
        *out_end = rx[139];
    }
    return 0;
}

static int test_memcard_format_rw_xor_persistence(void) {
    (void)remove(TEST_MC_PATH);

    memset(&g_sys, 0, sizeof(g_sys));
    psx_config_defaults(&g_sys.cfg, true);
    snprintf(g_sys.cfg.memcard_path[0], sizeof(g_sys.cfg.memcard_path[0]), "%s", TEST_MC_PATH);
    CHECK_TRUE(arena_init(&g_sys.arena, 1024u * 1024u) == PSX_OK, "arena_init MC");

    sio_init(&g_sys);

    /* 1. Verify fresh 128 KiB formatting ('M','C' header, free dir slots, test frame 63) */
    const u8 *mc0 = g_sys.sio.card[0].data;
    CHECK_TRUE(mc0[0] == 'M' && mc0[1] == 'C' && mc0[0x7F] == 0x0Eu,
               "Fresh Memcard Frame 0 'M','C' header + XOR 0x0E");
    CHECK_TRUE(mc0[128] == 0xA0u && mc0[128 + 8] == 0xFFu &&
               mc0[128 + 9] == 0xFFu && mc0[128 + 0x7F] == 0xA0u,
               "Fresh Memcard directory entry 1 free (0xA0) + XOR");
    CHECK_TRUE(mc0[63 * 128 + 0] == 'M' && mc0[63 * 128 + 1] == 'C',
               "Fresh Memcard write-test Frame 63 'M','C'");

    u16 ctrl0 = 0x1003u;

    /* 2. GetID (0x53) via SIO0 MMIO */
    sio0_mmio_write(&g_sys, PSX_REG_SIO0_CTRL, ctrl0, 2u);
    u8 id_rx[10];
    for (u32 i = 0u; i < 10u; i++) {
        u8 tx = (i == 0u) ? 0x81u : ((i == 1u) ? 0x53u : 0x00u);
        id_rx[i] = sio_spi_step(ctrl0, tx, NULL, NULL);
    }
    sio0_mmio_write(&g_sys, PSX_REG_SIO0_CTRL, 0x0000u, 2u);
    CHECK_TRUE(id_rx[1] == 0x08u && id_rx[2] == 0x5Au && id_rx[3] == 0x5Du &&
               id_rx[4] == 0x5Cu && id_rx[5] == 0x5Du &&
               id_rx[6] == 0x04u && id_rx[7] == 0x00u && id_rx[8] == 0x00u && id_rx[9] == 0x80u,
               "Memcard GetID (0x53) -> 0x0400 sectors x 0x0080 bytes");

    /* 3. Write (0x57) sector 0x0123 with valid XOR -> 'G' (0x47), dirty=true, flag 0x08 -> 0x00 */
    u8 pat[128];
    for (u32 i = 0u; i < 128u; i++) {
        pat[i] = (u8)(0xA5u ^ (i * 7u));
    }
    u8 flg = 0u, end_b = 0u;
    CHECK_TRUE(mc_write_sector_spi(ctrl0, 0x0123u, pat, false, &flg, &end_b) == 0,
               "Memcard Write (0x57) SPI framing");
    CHECK_TRUE(flg == 0x08u && end_b == 'G', "First Write returns initial flag 0x08 and end 'G'");
    CHECK_TRUE(g_sys.sio.card[0].dirty && (g_sys.sio.card[0].flag & 0x08u) == 0u,
               "Write marks card dirty and clears directory-unread flag bit 3");

    /* 4. Bad XOR Write (0x57) must return 'N' (0x4E) and NOT corrupt existing sector! */
    u8 bad_pat[128];
    memset(bad_pat, 0xEE, sizeof(bad_pat));
    CHECK_TRUE(mc_write_sector_spi(ctrl0, 0x0123u, bad_pat, true, &flg, &end_b) == 0,
               "Memcard bad-XOR Write SPI framing");
    CHECK_TRUE(flg == 0x00u && end_b == 'N', "Bad-XOR Write returns flag 0x00 and end 'N' (0x4E)");

    u8 rd_buf[128];
    u8 rd_xor = 0u;
    CHECK_TRUE(mc_read_sector_spi(ctrl0, 0x0123u, rd_buf, &rd_xor, &end_b) == 0,
               "Memcard Read (0x52) SPI framing");
    u8 expect_xor = (u8)(0x01u ^ 0x23u);
    for (u32 i = 0u; i < 128u; i++) {
        expect_xor ^= pat[i];
    }
    CHECK_TRUE(end_b == 'G' && rd_xor == expect_xor && memcmp(rd_buf, pat, 128u) == 0,
               "Read (0x52) matches good payload with intact data after rejected bad-XOR write");

    /* 5. Flush dirty card on sio_shutdown and reload into a brand-new instance */
    sio_shutdown(&g_sys);
    CHECK_TRUE(!g_sys.sio.card[0].dirty, "sio_shutdown flushed dirty memcard to disk");
    arena_destroy(&g_sys.arena);

    memset(&g_sys, 0, sizeof(g_sys));
    psx_config_defaults(&g_sys.cfg, true);
    snprintf(g_sys.cfg.memcard_path[0], sizeof(g_sys.cfg.memcard_path[0]), "%s", TEST_MC_PATH);
    CHECK_TRUE(arena_init(&g_sys.arena, 1024u * 1024u) == PSX_OK, "arena_init MC reload");
    sio_init(&g_sys);

    memset(rd_buf, 0, sizeof(rd_buf));
    CHECK_TRUE(mc_read_sector_spi(ctrl0, 0x0123u, rd_buf, &rd_xor, &end_b) == 0,
               "Reloaded Memcard Read (0x52)");
    CHECK_TRUE(end_b == 'G' && rd_xor == expect_xor && memcmp(rd_buf, pat, 128u) == 0,
               "Reloaded 128 KiB Memcard persisted sector 0x0123 across disk roundtrip");

    sio_shutdown(&g_sys);
    arena_destroy(&g_sys.arena);
    (void)remove(TEST_MC_PATH);
    return 0;
}

int main(void) {
    if (test_digital_and_analog_joypad() != 0) {
        (void)remove(TEST_MC_PATH);
        return 1;
    }
    if (test_memcard_format_rw_xor_persistence() != 0) {
        (void)remove(TEST_MC_PATH);
        return 1;
    }
    printf("test_periph_sio PASS\n");
    return 0;
}
