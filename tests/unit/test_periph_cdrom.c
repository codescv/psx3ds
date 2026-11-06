/*
 * Slice E unit test: CD-ROM 4-bank MMIO, BCD/MSF, multi-track CUE+BIN & 2048B ISO
 * parser, GetID (no-disc vs licensed SCEA), Test(0x20), Setloc/SeekL/GetlocL/GetlocP,
 * ReadN 2-stage queued IRQs + BFRD + DMA3 (2048B & 2340B), XA-ADPCM streaming,
 * and 128-sector lock-free SPSC Readahead Cache.
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

#define TEST_CUE_PATH  "/tmp/psx3ds_test_periph_cd.cue"
#define TEST_BIN1_PATH "/tmp/psx3ds_test_periph_cd_t1.bin"
#define TEST_BIN2_PATH "/tmp/psx3ds_test_periph_cd_t2.bin"
#define TEST_ISO_PATH  "/tmp/psx3ds_test_periph_cd.iso"

static PsxSystem g_sys;

static void cleanup_temp_files(void) {
    (void)remove(TEST_CUE_PATH);
    (void)remove(TEST_BIN1_PATH);
    (void)remove(TEST_BIN2_PATH);
    (void)remove(TEST_ISO_PATH);
}

static void cd_write_idx(u8 idx) {
    cdrom_mmio_write(&g_sys, PSX_REG_CDROM_STATUS, idx, 1u);
}

static void cd_push_param(u8 p) {
    cd_write_idx(0u);
    cdrom_mmio_write(&g_sys, PSX_REG_CDROM_REG2, p, 1u);
}

static void cd_send_cmd(u8 cmd) {
    cd_write_idx(0u);
    cdrom_mmio_write(&g_sys, PSX_REG_CDROM_REG1, cmd, 1u);
}

static u8 cd_irq_flag(void) {
    cd_write_idx(1u);
    return (u8)(cdrom_mmio_read(&g_sys, PSX_REG_CDROM_REG3, 1u) & 0x1Fu);
}

static void cd_ack_irq(u8 mask) {
    cd_write_idx(1u);
    cdrom_mmio_write(&g_sys, PSX_REG_CDROM_REG3, mask, 1u);
}

static u32 cd_drain_resp(u8 *out, u32 max_n) {
    u32 n = 0u;
    while (n < max_n && (cdrom_mmio_read(&g_sys, PSX_REG_CDROM_STATUS, 1u) & 0x20u) != 0u) {
        out[n++] = (u8)cdrom_mmio_read(&g_sys, PSX_REG_CDROM_REG1, 1u);
    }
    return n;
}

static void make_mode2_sector(u8 sec[2352], u32 lba, bool form2_xa, u8 xa_file, u8 xa_chan) {
    static const u8 k_sync[12] = {
        0x00u, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
        0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0x00u
    };
    memset(sec, 0, 2352u);
    memcpy(sec, k_sync, 12u);

    u8 mm = 0u, ss = 0u, ff = 0u;
    cd_lba_to_msf(lba, &mm, &ss, &ff);
    sec[12] = cd_bin_to_bcd(mm);
    sec[13] = cd_bin_to_bcd(ss);
    sec[14] = cd_bin_to_bcd(ff);
    sec[15] = 0x02u; /* Mode 2 */

    /* Subheader + copy */
    u8 submode = form2_xa ? 0x64u /* Form2 + Realtime + Audio */ : 0x08u /* Data */;
    u8 coding  = form2_xa ? 0x01u /* Stereo, 37.8 kHz, 4-bit ADPCM */ : 0x00u;
    sec[16] = sec[20] = xa_file;
    sec[17] = sec[21] = xa_chan;
    sec[18] = sec[22] = submode;
    sec[19] = sec[23] = coding;

    if (form2_xa) {
        /* Populate 18 valid XA sound groups with non-zero nibbles */
        for (u32 sg = 0u; sg < 18u; sg++) {
            u8 *grp = &sec[24u + sg * 128u];
            memset(&grp[4], 0x02, 12u);   /* filter 0, shift 2 */
            memset(&grp[16], 0x44, 112u); /* positive ADPCM nibbles */
        }
    } else {
        for (u32 i = 0u; i < 2048u; i++) {
            sec[24u + i] = (u8)((lba * 13u + i) & 0xFFu);
        }
    }
}

static int build_test_disc_images(void) {
    static u8 sec[2352];

    /* Track 1: 160 sectors (LBA 0..159) MODE2/2352 */
    FILE *f1 = fopen(TEST_BIN1_PATH, "wb");
    CHECK_TRUE(f1 != NULL, "create track 1 bin");
    for (u32 lba = 0u; lba < 160u; lba++) {
        bool is_xa = (lba == 10u);
        make_mode2_sector(sec, lba, is_xa, 1u, 2u);
        if (lba == 4u) {
            const char *lic = "          Licensed  by          Sony Computer Entertainment Amer";
            memcpy(&sec[24], lic, strlen(lic));
        }
        CHECK_TRUE(fwrite(sec, 1u, 2352u, f1) == 2352u, "write track 1 sector");
    }
    fclose(f1);

    /* Track 2: 20 sectors AUDIO/2352 (with 2-second 150-sector pregap in CUE) */
    FILE *f2 = fopen(TEST_BIN2_PATH, "wb");
    CHECK_TRUE(f2 != NULL, "create track 2 bin");
    for (u32 s = 0u; s < 170u; s++) {
        for (u32 i = 0u; i < 2352u; i += 2u) {
            s16 pcm = (s16)(4000 + (s32)s * 10);
            sec[i]      = (u8)(pcm & 0xFF);
            sec[i + 1u] = (u8)((u16)pcm >> 8);
        }
        CHECK_TRUE(fwrite(sec, 1u, 2352u, f2) == 2352u, "write track 2 audio sector");
    }
    fclose(f2);

    FILE *fc = fopen(TEST_CUE_PATH, "wb");
    CHECK_TRUE(fc != NULL, "create cue file");
    fprintf(fc,
            "FILE \"%s\" BINARY\n"
            "  TRACK 01 MODE2/2352\n"
            "    INDEX 01 00:00:00\n"
            "FILE \"%s\" BINARY\n"
            "  TRACK 02 AUDIO\n"
            "    INDEX 00 00:00:00\n"
            "    INDEX 01 00:02:00\n",
            TEST_BIN1_PATH, TEST_BIN2_PATH);
    fclose(fc);

    /* Standalone 2048-byte .iso image (32 sectors) */
    FILE *fi = fopen(TEST_ISO_PATH, "wb");
    CHECK_TRUE(fi != NULL, "create iso file");
    for (u32 lba = 0u; lba < 32u; lba++) {
        for (u32 i = 0u; i < 2048u; i++) {
            sec[i] = (u8)((lba + i * 3u) & 0xFFu);
        }
        if (lba == 16u) {
            memcpy(&sec[1], "CD001", 5u); /* ISO9660 PVD magic */
        }
        CHECK_TRUE(fwrite(sec, 1u, 2048u, fi) == 2048u, "write iso sector");
    }
    fclose(fi);

    return 0;
}

static int test_bcd_and_iso_parser(void) {
    for (u32 v = 0u; v <= 99u; v++) {
        bool ok  = false;
        u8   bcd = cd_bin_to_bcd((u8)v);
        u8   dec = cd_bcd_to_bin(bcd, &ok);
        CHECK_TRUE(ok && dec == (u8)v, "BCD roundtrip 0..99");
    }
    bool bad_ok = true;
    (void)cd_bcd_to_bin(0x2Au, &bad_ok);
    CHECK_TRUE(!bad_ok, "Invalid BCD nibble rejected");

    u8 mm = 0u, ss = 0u, ff = 0u;
    cd_lba_to_msf(0u, &mm, &ss, &ff);
    CHECK_TRUE(mm == 0u && ss == 2u && ff == 0u, "LBA 0 -> 00:02:00");
    CHECK_TRUE(cd_msf_to_lba(1u, 2u, 34u) == (1u * 60u + 2u) * 75u + 34u - 150u, "MSF -> LBA");

    /* Verify 2048B .iso synthesizes 2352B Mode2 Form1 sync + BCD MSF header */
    static CdImage iso_img;
    cd_image_init(&iso_img);
    CHECK_TRUE(cd_image_open(&iso_img, TEST_ISO_PATH) == PSX_OK, "open 2048B .iso");
    CHECK_TRUE(iso_img.track_count == 1u && iso_img.total_lba == 32u, "ISO geometry");

    u8 raw[2352];
    CHECK_TRUE(cd_image_read_sector(&iso_img, 16u, raw), "read ISO sector 16");
    CHECK_TRUE(raw[0] == 0x00u && raw[1] == 0xFFu && raw[11] == 0x00u, "ISO sync pattern");
    CHECK_TRUE(raw[12] == 0x00u && raw[13] == 0x02u && raw[14] == 0x16u && raw[15] == 0x02u,
               "ISO synthesized BCD MSF 00:02:16 Mode 2");
    CHECK_TRUE(memcmp(&raw[25], "CD001", 5u) == 0, "ISO payload preserved at +24");
    cd_image_close(&iso_img);
    return 0;
}

static int test_cdrom_commands_and_readahead(void) {
    memset(&g_sys, 0, sizeof(g_sys));
    psx_config_defaults(&g_sys.cfg, true);
    g_sys.cfg.spu_async = false;
    CHECK_TRUE(arena_init(&g_sys.arena, 2u * 1024u * 1024u) == PSX_OK, "arena_init CD");

    spu_init(&g_sys);
    cdrom_init(&g_sys);

    /* Enable all CD IRQs in HINTMSK */
    cd_write_idx(1u);
    cdrom_mmio_write(&g_sys, PSX_REG_CDROM_REG2, 0x1Fu, 1u);

    /* 1. Test(0x20) -> INT3 response 97 01 10 C2 */
    u8 resp[16];
    cd_push_param(0x20u);
    cd_send_cmd(0x19u);
    cdrom_event_cb(&g_sys, 0u);
    CHECK_TRUE(cd_irq_flag() == 3u, "Test(0x20) -> INT3");
    CHECK_TRUE(cd_drain_resp(resp, 16u) == 4u &&
               resp[0] == 0x97u && resp[1] == 0x01u && resp[2] == 0x10u && resp[3] == 0xC2u,
               "Test(0x20) BIOS version 97 01 10 C2");
    cd_ack_irq(0x07u);

    /* 2. GetID (0x1A) with empty tray -> INT3 then INT5 error (no disc) */
    cd_send_cmd(0x1Au);
    cdrom_event_cb(&g_sys, 0u);
    CHECK_TRUE(cd_irq_flag() == 3u, "GetID empty tray -> INT3");
    (void)cd_drain_resp(resp, 16u);
    cd_ack_irq(0x07u);
    cdrom_event_cb(&g_sys, 0u);
    CHECK_TRUE(cd_irq_flag() == 5u, "GetID empty tray stage 2 -> INT5");
    CHECK_TRUE(cd_drain_resp(resp, 16u) == 8u && (resp[1] & 0x40u) != 0u,
               "GetID no-disc error payload");
    cd_ack_irq(0x07u);

    /* 3. Load multi-track CUE+BIN and verify licensed SCEA GetID (INT3 -> INT2) */
    CHECK_TRUE(cdrom_load(&g_sys, TEST_CUE_PATH) == PSX_OK, "cdrom_load multi-track CUE");
    CHECK_TRUE(g_sys.cdrom.region == 'A', "Auto-detected SCEA region from sector 4");

    cd_send_cmd(0x1Au);
    cdrom_event_cb(&g_sys, 0u);
    CHECK_TRUE(cd_irq_flag() == 3u, "GetID licensed -> INT3");
    (void)cd_drain_resp(resp, 16u);
    /* Verify 2nd stage does NOT fire before INT3 is acknowledged! */
    CHECK_TRUE(cd_irq_flag() == 3u, "Stage 2 held until INT3 ack");
    cd_ack_irq(0x07u);
    cdrom_event_cb(&g_sys, 0u);
    CHECK_TRUE(cd_irq_flag() == 2u, "GetID licensed stage 2 -> INT2");
    CHECK_TRUE(cd_drain_resp(resp, 16u) == 8u &&
               resp[0] == 0x02u && resp[1] == 0x00u &&
               resp[4] == 'S' && resp[5] == 'C' && resp[6] == 'E' && resp[7] == 'A',
               "GetID licensed 'SCEA' response");
    cd_ack_irq(0x07u);

    /* 4. GetTN (0x13) & GetTD (0x14) */
    cd_send_cmd(0x13u);
    cdrom_event_cb(&g_sys, 0u);
    CHECK_TRUE(cd_irq_flag() == 3u && cd_drain_resp(resp, 16u) == 3u &&
               resp[1] == 0x01u && resp[2] == 0x02u,
               "GetTN tracks 01..02");
    cd_ack_irq(0x07u);

    cd_push_param(0x01u);
    cd_send_cmd(0x14u);
    cdrom_event_cb(&g_sys, 0u);
    CHECK_TRUE(cd_irq_flag() == 3u && cd_drain_resp(resp, 16u) == 3u &&
               resp[1] == 0x00u && resp[2] == 0x02u,
               "GetTD track 1 starts at 00:02");
    cd_ack_irq(0x07u);

    /* 5. GetlocL before any read -> INT5; then Setloc(00:02:06 = LBA 6) + SeekL -> GetlocL/GetlocP */
    cd_send_cmd(0x10u);
    cdrom_event_cb(&g_sys, 0u);
    CHECK_TRUE(cd_irq_flag() == 5u, "GetlocL before sector header latched -> INT5");
    (void)cd_drain_resp(resp, 16u);
    cd_ack_irq(0x07u);

    cd_push_param(0x00u);
    cd_push_param(0x02u);
    cd_push_param(0x06u); /* 00:02:06 = LBA 6 */
    cd_send_cmd(0x02u);   /* Setloc */
    cdrom_event_cb(&g_sys, 0u);
    CHECK_TRUE(cd_irq_flag() == 3u, "Setloc -> INT3");
    (void)cd_drain_resp(resp, 16u);
    cd_ack_irq(0x07u);

    cd_send_cmd(0x15u); /* SeekL */
    cdrom_event_cb(&g_sys, 0u);
    CHECK_TRUE(cd_irq_flag() == 3u, "SeekL -> INT3");
    (void)cd_drain_resp(resp, 16u);
    cd_ack_irq(0x07u);
    cdrom_event_cb(&g_sys, 0u);
    CHECK_TRUE(cd_irq_flag() == 2u, "SeekL complete -> INT2");
    (void)cd_drain_resp(resp, 16u);
    cd_ack_irq(0x07u);

    cd_send_cmd(0x10u); /* GetlocL */
    cdrom_event_cb(&g_sys, 0u);
    CHECK_TRUE(cd_irq_flag() == 3u && cd_drain_resp(resp, 16u) == 8u &&
               resp[0] == 0x00u && resp[1] == 0x02u && resp[2] == 0x06u && resp[3] == 0x02u,
               "GetlocL returns 8-byte sector header for 00:02:06");
    cd_ack_irq(0x07u);

    cd_send_cmd(0x11u); /* GetlocP */
    cdrom_event_cb(&g_sys, 0u);
    CHECK_TRUE(cd_irq_flag() == 3u && cd_drain_resp(resp, 16u) == 8u &&
               resp[0] == 0x01u && resp[1] == 0x01u && resp[5] == 0x00u && resp[6] == 0x02u && resp[7] == 0x06u,
               "GetlocP returns 8-byte BCD SubQ position");
    cd_ack_irq(0x07u);

    /* 6. 128-Sector Lock-Free SPSC Readahead Cache verification */
    CHECK_TRUE(g_sys.cdrom.ra.cap == 128u, "Readahead ring capacity == 128 sectors");
    cdrom_ra_seek(&g_sys, 6u);
    u32 pumped = cdrom_ra_pump(&g_sys, 128u);
    CHECK_TRUE(pumped == 128u, "Pumped 128 sectors into lock-free SPSC readahead ring");

    /* 7. Setmode(2x speed, 2048B) + ReadN (LBA 6) -> INT3 -> INT1 + BFRD + DMA3 */
    cd_push_param(0x80u); /* 2x speed, 2048-byte data */
    cd_send_cmd(0x0Eu);   /* Setmode */
    cdrom_event_cb(&g_sys, 0u);
    (void)cd_drain_resp(resp, 16u);
    cd_ack_irq(0x07u);

    u64 hits_before = g_sys.stats.cd_ra_hits;
    cd_send_cmd(0x06u); /* ReadN */
    cdrom_event_cb(&g_sys, 0u);
    CHECK_TRUE(cd_irq_flag() == 3u, "ReadN -> INT3");
    (void)cd_drain_resp(resp, 16u);
    cd_ack_irq(0x07u);

    cdrom_event_cb(&g_sys, 0u); /* Sector timer fires -> INT1 DataReady */
    CHECK_TRUE(cd_irq_flag() == 1u, "ReadN sector 6 -> INT1 DataReady");
    CHECK_TRUE(g_sys.stats.cd_ra_hits == hits_before + 1u,
               "ReadN satisfied from 128-sector SPSC readahead cache hit");
    (void)cd_drain_resp(resp, 16u);

    /* Enable BFRD (bit 7 on bank 0 reg 3) and DMA3 read 512 words (2048 bytes) */
    cd_write_idx(0u);
    cdrom_mmio_write(&g_sys, PSX_REG_CDROM_REG3, 0x80u, 1u);
    CHECK_TRUE((cdrom_mmio_read(&g_sys, PSX_REG_CDROM_STATUS, 1u) & 0x40u) != 0u,
               "DRQSTS set when BFRD enabled");

    u32 dma_buf[600];
    CHECK_TRUE(cdrom_dma_read_words(&g_sys, dma_buf, 512u) == 512u, "DMA3 512 words");
    u8 *dma_bytes = (u8 *)dma_buf;
    for (u32 i = 0u; i < 2048u; i++) {
        u8 expect = (u8)((6u * 13u + i) & 0xFFu);
        if (dma_bytes[i] != expect) {
            fprintf(stderr, "DMA3 mismatch at byte %u: got 0x%02X expected 0x%02X\n",
                    (unsigned)i, (unsigned)dma_bytes[i], (unsigned)expect);
            return 1;
        }
    }
    cd_ack_irq(0x07u);

    /* Pause (0x09) -> INT3 -> INT2 */
    cd_send_cmd(0x09u);
    cdrom_event_cb(&g_sys, 0u);
    CHECK_TRUE(cd_irq_flag() == 3u, "Pause -> INT3");
    (void)cd_drain_resp(resp, 16u);
    cd_ack_irq(0x07u);
    cdrom_event_cb(&g_sys, 0u);
    CHECK_TRUE(cd_irq_flag() == 2u, "Pause -> INT2");
    (void)cd_drain_resp(resp, 16u);
    cd_ack_irq(0x07u);

    /* 8. Whole-sector 2340B mode (Setmode 0xA0) at LBA 7 */
    cd_push_param(0xA0u);
    cd_send_cmd(0x0Eu);
    cdrom_event_cb(&g_sys, 0u);
    (void)cd_drain_resp(resp, 16u);
    cd_ack_irq(0x07u);

    cd_push_param(0x00u);
    cd_push_param(0x02u);
    cd_push_param(0x07u); /* LBA 7 */
    cd_send_cmd(0x02u);
    cdrom_event_cb(&g_sys, 0u);
    (void)cd_drain_resp(resp, 16u);
    cd_ack_irq(0x07u);

    cd_send_cmd(0x1Bu); /* ReadS */
    cdrom_event_cb(&g_sys, 0u);
    (void)cd_drain_resp(resp, 16u);
    cd_ack_irq(0x07u);
    cdrom_event_cb(&g_sys, 0u);
    CHECK_TRUE(cd_irq_flag() == 1u, "ReadS whole-sector -> INT1");
    (void)cd_drain_resp(resp, 16u);

    cd_write_idx(0u);
    cdrom_mmio_write(&g_sys, PSX_REG_CDROM_REG3, 0x80u, 1u);
    CHECK_TRUE(cdrom_dma_read_words(&g_sys, dma_buf, 585u) == 585u, "DMA3 585 words (2340B)");
    CHECK_TRUE(dma_bytes[0] == 0x00u && dma_bytes[1] == 0x02u &&
               dma_bytes[2] == 0x07u && dma_bytes[3] == 0x02u,
               "2340B whole-sector starts at BCD MSF header 00:02:07");
    cd_ack_irq(0x07u);

    /* 9. XA-ADPCM filtered streaming at LBA 10 (file=1, channel=2) -> SPU xa_ring */
    cd_send_cmd(0x09u); /* Pause */
    cdrom_event_cb(&g_sys, 0u);
    (void)cd_drain_resp(resp, 16u);
    cd_ack_irq(0x07u);
    cdrom_event_cb(&g_sys, 0u);
    (void)cd_drain_resp(resp, 16u);
    cd_ack_irq(0x07u);

    cd_push_param(1u); /* file 1 */
    cd_push_param(2u); /* chan 2 */
    cd_send_cmd(0x0Du); /* Setfilter */
    cdrom_event_cb(&g_sys, 0u);
    (void)cd_drain_resp(resp, 16u);
    cd_ack_irq(0x07u);

    cd_push_param(0x48u); /* XA-ADPCM enable + XA filter enable */
    cd_send_cmd(0x0Eu);
    cdrom_event_cb(&g_sys, 0u);
    (void)cd_drain_resp(resp, 16u);
    cd_ack_irq(0x07u);

    cd_push_param(0x00u);
    cd_push_param(0x02u);
    cd_push_param(0x10u); /* BCD 00:02:10 = LBA 10 (XA sector) */
    cd_send_cmd(0x02u);
    cdrom_event_cb(&g_sys, 0u);
    (void)cd_drain_resp(resp, 16u);
    cd_ack_irq(0x07u);

    u32 xa_before = g_sys.spu.xa_w;
    cd_send_cmd(0x1Bu); /* ReadS */
    cdrom_event_cb(&g_sys, 0u);
    (void)cd_drain_resp(resp, 16u);
    cd_ack_irq(0x07u);
    cdrom_event_cb(&g_sys, 0u); /* Reads LBA 10 -> routed to SPU XA ring without INT1! */
    CHECK_TRUE(g_sys.spu.xa_w > xa_before, "XA-ADPCM sector decoded & resampled into SPU xa_ring");

    cdrom_shutdown(&g_sys);
    spu_shutdown(&g_sys);
    arena_destroy(&g_sys.arena);
    return 0;
}

int main(void) {
    cleanup_temp_files();
    if (build_test_disc_images() != 0) {
        cleanup_temp_files();
        return 1;
    }
    if (test_bcd_and_iso_parser() != 0) {
        cleanup_temp_files();
        return 1;
    }
    if (test_cdrom_commands_and_readahead() != 0) {
        cleanup_temp_files();
        return 1;
    }
    cleanup_temp_files();
    printf("test_periph_cdrom PASS\n");
    return 0;
}
