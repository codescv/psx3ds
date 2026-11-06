/*
 * CHD v1–v5 Compressed Disc Streaming & Multi-Hunk LRU Cache Unit Test:
 *   1. Pure-C spec-exact CHD v5 encoder exercising ALL 4 CD codecs:
 *        - cdzl (RFC 1951 Deflate LZ77 + Fixed Huffman + Reed-Solomon ECC strip)
 *        - cdlz (7z LZMA1 lc=3,lp=0,pb=2 range coder + LZ77/rep0 matches + ECC strip)
 *        - cdzs (RFC 8878 Zstandard frame: Raw_Block + RLE_Block + Compressed_Block)
 *        - cdfl (RFC 9639 lossless FLAC 44.1kHz 16-bit stereo Fixed-1 Rice + Deflate subcode)
 *      plus COMPRESSION_NONE (raw hunk) and COMPRESSION_SELF (hunk deduplication).
 *   2. Multi-track CHD v5 vs .cue/.bin 100% 2352-byte parity across:
 *        - Track 1: MODE2_RAW, 17 sectors (non-multiple-of-4 -> 3 poisoned CHD padding
 *          frames verifying exact CD_TRACK_PADDING=4 LBA remap!), SCEA license at LBA 4,
 *          stripped sync+ECC Mode 2 Form 1 sectors, and XA-ADPCM Form 2 sector at LBA 10.
 *        - Track 2: AUDIO, 6-sector virtual pregap + 25 stereo sine/ramp sectors (non-
 *          multiple-of-4 -> 3 poisoned CHD padding frames!) verifying big-endian ->
 *          little-endian 16-bit PCM sample byte-swap across cdfl, cdzl, cdlz, cdzs, raw!
 *        - Track 3: Cooked MODE1 (2048B) header/MSF synthesis + Mode 1 (+16) payload.
 *        - Track 4: Cooked MODE2_FORM1 (2048B) header/subheader + (+24) payload + SELF hunk.
 *   3. In-CHD 'V'-prefixed pregaps (PGTYPE:VMODE2_RAW / PGTYPE:VAUDIO), empty PGTYPE:
 *      tokens, POSTGAP, legacy CHCD binary TOC, MComprHD magic detection (.img), O3DS
 *      small_mem 4-slot cache mode, and corrupt/non-CD CHD clean rejection (0 leaks).
 *   4. 8-slot LRU decoded hunk cache hit-rate verification (>= 87.5% sequential cold,
 *      100% warm 8-hunk working set, true LRU victim eviction).
 *   5. Full PsxCdrom integration: GetID ("SCEA"), GetTN, GetTD (padding-free MSF),
 *      Core 2 async cdrom_ra_pump, Setloc+SeekL+GetlocL+GetlocP, ReadN 2048B DMA3 on
 *      both Mode 2 Form 1 and Mode 1 tracks, ReadS 2340B DMA3, XA-ADPCM routing, and
 *      Play CD-DA stereo waveform verification in SPU xa_ring.
 *   6. Multi-MB CHD v5 per-codec decompression + cached streaming MB/s benchmark.
 */

#include "../../src/periph/periph_internal.h"
#include "psx3ds/arena.h"
#include "psx3ds/platform.h"
#include "psx3ds/system.h"

#include <libchdr/cdrom.h>
#include <libchdr/chd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK_TRUE(cond, msg)                                                       \
    do {                                                                            \
        if (!(cond)) {                                                              \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, (msg));       \
            return 1;                                                               \
        }                                                                           \
    } while (0)

#define TEST_CHD_MAIN_PATH   "/tmp/psx3ds_test_chd_main.chd"
#define TEST_CHD_VPG_PATH    "/tmp/psx3ds_test_chd_vpg.img"
#define TEST_CHD_LEGACY_PATH "/tmp/psx3ds_test_chd_legacy.chd"
#define TEST_CHD_BAD_PATH    "/tmp/psx3ds_test_chd_bad.chd"
#define TEST_CHD_BENCH_PATH  "/tmp/psx3ds_test_chd_bench.chd"
#define TEST_REF_CUE_PATH    "/tmp/psx3ds_test_chd_ref.cue"
#define TEST_REF_T1_PATH     "/tmp/psx3ds_test_chd_ref_t1.bin"
#define TEST_REF_T2_PATH     "/tmp/psx3ds_test_chd_ref_t2.bin"

static PsxSystem g_sys;

static void cleanup_temp_files(void) {
    (void)remove(TEST_CHD_MAIN_PATH);
    (void)remove(TEST_CHD_VPG_PATH);
    (void)remove(TEST_CHD_LEGACY_PATH);
    (void)remove(TEST_CHD_BAD_PATH);
    (void)remove(TEST_CHD_BENCH_PATH);
    (void)remove(TEST_REF_CUE_PATH);
    (void)remove(TEST_REF_T1_PATH);
    (void)remove(TEST_REF_T2_PATH);
}

/* ==========================================================================
 * Big-endian / Little-endian & CRC helpers (CHD v5 CRC-16/CCITT + FLAC CRC)
 * ========================================================================== */
static void put_be16(u8 *p, u16 v) {
    p[0] = (u8)(v >> 8);
    p[1] = (u8)(v & 0xFFu);
}

static void put_be24(u8 *p, u32 v) {
    p[0] = (u8)((v >> 16) & 0xFFu);
    p[1] = (u8)((v >> 8) & 0xFFu);
    p[2] = (u8)(v & 0xFFu);
}

static void put_be32(u8 *p, u32 v) {
    p[0] = (u8)(v >> 24);
    p[1] = (u8)((v >> 16) & 0xFFu);
    p[2] = (u8)((v >> 8) & 0xFFu);
    p[3] = (u8)(v & 0xFFu);
}

static void put_be48(u8 *p, u64 v) {
    p[0] = (u8)((v >> 40) & 0xFFu);
    p[1] = (u8)((v >> 32) & 0xFFu);
    p[2] = (u8)((v >> 24) & 0xFFu);
    p[3] = (u8)((v >> 16) & 0xFFu);
    p[4] = (u8)((v >> 8) & 0xFFu);
    p[5] = (u8)(v & 0xFFu);
}

static void put_be64(u8 *p, u64 v) {
    put_be32(p + 0, (u32)(v >> 32));
    put_be32(p + 4, (u32)(v & 0xFFFFFFFFu));
}

/* CHD CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, MSB-first) */
static u16 calc_chd_crc16(const u8 *data, size_t len) {
    u16 crc = 0xFFFFu;
    for (size_t i = 0u; i < len; i++) {
        crc ^= (u16)((u16)data[i] << 8);
        for (u32 b = 0u; b < 8u; b++) {
            crc = (crc & 0x8000u) ? (u16)((crc << 1) ^ 0x1021u) : (u16)(crc << 1);
        }
    }
    return crc;
}

/* FLAC frame header CRC-8 (poly 0x07, init 0x00) */
static u8 calc_flac_crc8(const u8 *data, size_t len) {
    u8 crc = 0x00u;
    for (size_t i = 0u; i < len; i++) {
        crc ^= data[i];
        for (u32 b = 0u; b < 8u; b++) {
            crc = (crc & 0x80u) ? (u8)((crc << 1) ^ 0x07u) : (u8)(crc << 1);
        }
    }
    return crc;
}

/* FLAC frame footer CRC-16 (poly 0x8005, init 0x0000) */
static u16 calc_flac_crc16(const u8 *data, size_t len) {
    u16 crc = 0x0000u;
    for (size_t i = 0u; i < len; i++) {
        crc ^= (u16)((u16)data[i] << 8);
        for (u32 b = 0u; b < 8u; b++) {
            crc = (crc & 0x8000u) ? (u16)((crc << 1) ^ 0x8005u) : (u16)(crc << 1);
        }
    }
    return crc;
}

/* ==========================================================================
 * Codec 1: Pure-C RFC 1951 Raw Deflate Encoder (LZ77 + Fixed Huffman, cdzl)
 * ========================================================================== */
typedef struct LsbBitWriter {
    u8    *buf;
    size_t cap;
    size_t byte_pos;
    u32    bit_buf;
    u32    bit_cnt;
} LsbBitWriter;

static void lsb_bw_init(LsbBitWriter *bw, u8 *buf, size_t cap) {
    bw->buf      = buf;
    bw->cap      = cap;
    bw->byte_pos = 0u;
    bw->bit_buf  = 0u;
    bw->bit_cnt  = 0u;
}

static void lsb_bw_put(LsbBitWriter *bw, u32 val, u32 nbits) {
    bw->bit_buf |= (val & ((1u << nbits) - 1u)) << bw->bit_cnt;
    bw->bit_cnt += nbits;
    while (bw->bit_cnt >= 8u) {
        if (bw->byte_pos < bw->cap) {
            bw->buf[bw->byte_pos] = (u8)(bw->bit_buf & 0xFFu);
        }
        bw->byte_pos++;
        bw->bit_buf >>= 8;
        bw->bit_cnt -= 8u;
    }
}

static void lsb_bw_put_huff_msb(LsbBitWriter *bw, u32 code, u32 nbits) {
    for (u32 i = nbits; i > 0u; i--) {
        lsb_bw_put(bw, (code >> (i - 1u)) & 1u, 1u);
    }
}

static size_t lsb_bw_finish(LsbBitWriter *bw) {
    if (bw->bit_cnt > 0u) {
        if (bw->byte_pos < bw->cap) {
            bw->buf[bw->byte_pos] = (u8)(bw->bit_buf & 0xFFu);
        }
        bw->byte_pos++;
        bw->bit_buf = 0u;
        bw->bit_cnt = 0u;
    }
    return bw->byte_pos;
}

static void deflate_emit_litlen(LsbBitWriter *bw, u32 sym) {
    if (sym <= 143u) {
        lsb_bw_put_huff_msb(bw, 0x30u + sym, 8u);
    } else if (sym <= 255u) {
        lsb_bw_put_huff_msb(bw, 0x190u + (sym - 144u), 9u);
    } else if (sym <= 279u) {
        lsb_bw_put_huff_msb(bw, sym - 256u, 7u);
    } else {
        lsb_bw_put_huff_msb(bw, 0xC0u + (sym - 280u), 8u);
    }
}

static void deflate_emit_match(LsbBitWriter *bw, u32 len, u32 dist) {
    static const u16 k_len_base[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
    };
    static const u8 k_len_extra[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
    };
    static const u16 k_dist_base[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
        8193, 12289, 16385, 24577
    };
    static const u8 k_dist_extra[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
    };

    u32 lc = 28u;
    for (u32 i = 0u; i < 28u; i++) {
        if (len >= k_len_base[i] && len < k_len_base[i + 1u]) {
            lc = i;
            break;
        }
    }
    if (len == 258u) {
        lc = 28u;
    }
    deflate_emit_litlen(bw, 257u + lc);
    if (k_len_extra[lc] > 0u) {
        lsb_bw_put(bw, len - k_len_base[lc], k_len_extra[lc]);
    }

    u32 dc = 29u;
    for (u32 i = 0u; i < 29u; i++) {
        if (dist >= k_dist_base[i] && dist < k_dist_base[i + 1u]) {
            dc = i;
            break;
        }
    }
    lsb_bw_put_huff_msb(bw, dc, 5u);
    if (k_dist_extra[dc] > 0u) {
        lsb_bw_put(bw, dist - k_dist_base[dc], k_dist_extra[dc]);
    }
}

static u32 encode_raw_deflate(const u8 *src, u32 src_len, u8 *dst, u32 dst_cap) {
    s32 head[1024];
    for (u32 i = 0u; i < 1024u; i++) {
        head[i] = -1;
    }

    LsbBitWriter bw;
    lsb_bw_init(&bw, dst, dst_cap);
    /* Final block (BFINAL=1), Fixed Huffman (BTYPE=01 in LSB order -> 1, 0) */
    lsb_bw_put(&bw, 1u, 1u);
    lsb_bw_put(&bw, 1u, 2u);

    u32 pos = 0u;
    while (pos < src_len) {
        u32 best_len  = 0u;
        u32 best_dist = 0u;

        /* Check immediate RLE / short repeat + hash lookup */
        if (pos + 3u <= src_len) {
            u32 h = (((u32)src[pos] * 2654435761u) ^
                     ((u32)src[pos + 1u] << 5) ^
                     (u32)src[pos + 2u]) & 1023u;
            s32 cand = head[h];
            head[h]  = (s32)pos;

            u32 max_l = src_len - pos;
            if (max_l > 258u) {
                max_l = 258u;
            }
            if (pos > 0u && src[pos] == src[pos - 1u]) {
                u32 l = 0u;
                while (l < max_l && src[pos + l] == src[pos - 1u + l]) {
                    l++;
                }
                if (l >= 3u) {
                    best_len  = l;
                    best_dist = 1u;
                }
            }
            if (cand >= 0 && (pos - (u32)cand) <= 32768u && best_len < max_l) {
                u32 d = pos - (u32)cand;
                u32 l = 0u;
                while (l < max_l && src[pos + l] == src[(u32)cand + l]) {
                    l++;
                }
                if (l > best_len && l >= 3u) {
                    best_len  = l;
                    best_dist = d;
                }
            }
        }

        if (best_len >= 3u) {
            deflate_emit_match(&bw, best_len, best_dist);
            for (u32 k = 1u; k < best_len && (pos + k + 3u) <= src_len; k++) {
                u32 hk = (((u32)src[pos + k] * 2654435761u) ^
                          ((u32)src[pos + k + 1u] << 5) ^
                          (u32)src[pos + k + 2u]) & 1023u;
                head[hk] = (s32)(pos + k);
            }
            pos += best_len;
        } else {
            deflate_emit_litlen(&bw, src[pos]);
            pos++;
        }
    }

    deflate_emit_litlen(&bw, 256u); /* EOB */
    size_t out_sz = lsb_bw_finish(&bw);
    return (out_sz <= dst_cap) ? (u32)out_sz : 0u;
}

/* ==========================================================================
 * Codec 2: Pure-C 7z LZMA1 Range Encoder (lc=3, lp=0, pb=2, headerless, cdlz)
 * ========================================================================== */
#define LZ_STATES     12u
#define LZ_PB_STATES  4u
#define LZ_PROB_INIT  1024u

typedef struct LzmaRcEnc {
    u8    *out;
    size_t cap;
    size_t pos;
    u64    low;
    u64    cache_size;
    u32    range;
    u8     cache;
} LzmaRcEnc;

typedef struct LzmaLenModel {
    u16 choice;
    u16 choice2;
    u16 low[LZ_PB_STATES][8];
    u16 mid[LZ_PB_STATES][8];
    u16 high[256];
} LzmaLenModel;

typedef struct LzmaEncState {
    u16          is_match[LZ_STATES][LZ_PB_STATES];
    u16          is_rep[LZ_STATES];
    u16          is_rep_g0[LZ_STATES];
    u16          is_rep0_long[LZ_STATES][LZ_PB_STATES];
    u16          pos_slot[4][64];
    u16          spec_pos[128];
    u16          align[16];
    u16          lit[8][0x300]; /* lc=3, lp=0 -> 8 literal tables */
    LzmaLenModel len_enc;
    LzmaLenModel rep_len_enc;
} LzmaEncState;

static void lz_rc_shift_low(LzmaRcEnc *rc) {
    if ((u32)rc->low < 0xFF000000u || (u32)(rc->low >> 32) != 0u) {
        u8 temp = rc->cache;
        do {
            if (rc->pos < rc->cap) {
                rc->out[rc->pos] = (u8)(temp + (u8)(rc->low >> 32));
            }
            rc->pos++;
            temp = 0xFFu;
        } while (--rc->cache_size != 0u);
        rc->cache = (u8)((u32)rc->low >> 24);
    }
    rc->cache_size++;
    rc->low = (u64)((u32)rc->low << 8);
}

static void lz_rc_bit(LzmaRcEnc *rc, u16 *prob, u32 bit) {
    u32 ttt   = *prob;
    u32 bound = (rc->range >> 11) * ttt;
    if (bit == 0u) {
        rc->range = bound;
        *prob     = (u16)(ttt + ((2048u - ttt) >> 5));
    } else {
        rc->low += bound;
        rc->range -= bound;
        *prob = (u16)(ttt - (ttt >> 5));
    }
    if (rc->range < 0x01000000u) {
        rc->range <<= 8;
        lz_rc_shift_low(rc);
    }
}

static void lz_rc_tree(LzmaRcEnc *rc, u16 *probs, u32 nbits, u32 sym) {
    u32 m = 1u;
    for (u32 i = nbits; i > 0u; i--) {
        u32 bit = (sym >> (i - 1u)) & 1u;
        lz_rc_bit(rc, &probs[m], bit);
        m = (m << 1) | bit;
    }
}

static void lz_rc_rev_tree(LzmaRcEnc *rc, u16 *prob_base, u32 nbits, u32 sym) {
    u32 idx = 1u;
    u32 m   = 1u;
    for (u32 k = 0u; k < nbits; k++) {
        u32 bit = (sym >> k) & 1u;
        lz_rc_bit(rc, &prob_base[idx], bit);
        if (bit == 0u) {
            idx += m;
            m += m;
        } else {
            m += m;
            idx += m;
        }
    }
}

static void lz_rc_direct(LzmaRcEnc *rc, u32 val, u32 nbits) {
    for (u32 i = nbits; i > 0u; i--) {
        rc->range >>= 1;
        if (((val >> (i - 1u)) & 1u) != 0u) {
            rc->low += rc->range;
        }
        if (rc->range < 0x01000000u) {
            rc->range <<= 8;
            lz_rc_shift_low(rc);
        }
    }
}

static void lz_enc_len(LzmaRcEnc *rc, LzmaLenModel *lm, u32 pos_state, u32 len_minus_2) {
    if (len_minus_2 < 8u) {
        lz_rc_bit(rc, &lm->choice, 0u);
        lz_rc_tree(rc, lm->low[pos_state], 3u, len_minus_2);
    } else if (len_minus_2 < 16u) {
        lz_rc_bit(rc, &lm->choice, 1u);
        lz_rc_bit(rc, &lm->choice2, 0u);
        lz_rc_tree(rc, lm->mid[pos_state], 3u, len_minus_2 - 8u);
    } else {
        lz_rc_bit(rc, &lm->choice, 1u);
        lz_rc_bit(rc, &lm->choice2, 1u);
        lz_rc_tree(rc, lm->high, 8u, len_minus_2 - 16u);
    }
}

static u32 lz_get_pos_slot(u32 dist0) {
    if (dist0 < 4u) {
        return dist0;
    }
    u32 hi = 31u - (u32)__builtin_clz(dist0);
    return (hi << 1) + ((dist0 >> (hi - 1u)) & 1u);
}

static u32 encode_raw_lzma(const u8 *src, u32 src_len, u8 *dst, u32 dst_cap) {
    static LzmaEncState st;
    u16 *all_probs = (u16 *)&st;
    size_t nprobs  = sizeof(st) / sizeof(u16);
    for (size_t i = 0u; i < nprobs; i++) {
        all_probs[i] = LZ_PROB_INIT;
    }

    s32 head[1024];
    for (u32 i = 0u; i < 1024u; i++) {
        head[i] = -1;
    }

    LzmaRcEnc rc;
    rc.out        = dst;
    rc.cap        = dst_cap;
    rc.pos        = 0u;
    rc.low        = 0u;
    rc.cache_size = 1u;
    rc.range      = 0xFFFFFFFFu;
    rc.cache      = 0u;

    u32 state = 0u;
    u32 rep0 = 1u, rep1 = 1u, rep2 = 1u, rep3 = 1u;
    (void)rep3;

    u32 pos = 0u;
    while (pos < src_len) {
        u32 pb       = pos & 3u;
        u32 max_l    = (src_len - pos > 273u) ? 273u : (src_len - pos);
        u32 rep0_len = 0u;
        if (pos >= rep0) {
            while (rep0_len < max_l && src[pos + rep0_len] == src[pos - rep0 + rep0_len]) {
                rep0_len++;
            }
        }

        u32 best_len  = 0u;
        u32 best_dist = 0u;
        if (pos + 3u <= src_len) {
            u32 h = (((u32)src[pos] * 2654435761u) ^
                     ((u32)src[pos + 1u] << 5) ^
                     (u32)src[pos + 2u]) & 1023u;
            s32 cand = head[h];
            head[h]  = (s32)pos;
            if (cand >= 0 && (pos - (u32)cand) <= 16384u) {
                u32 d = pos - (u32)cand;
                u32 l = 0u;
                while (l < max_l && src[pos + l] == src[(u32)cand + l]) {
                    l++;
                }
                if (l >= 3u) {
                    best_len  = l;
                    best_dist = d;
                }
            }
        }

        if (rep0_len >= 2u && rep0_len + 1u >= best_len) {
            lz_rc_bit(&rc, &st.is_match[state][pb], 1u);
            lz_rc_bit(&rc, &st.is_rep[state], 1u);
            lz_rc_bit(&rc, &st.is_rep_g0[state], 0u);
            lz_rc_bit(&rc, &st.is_rep0_long[state][pb], 1u);
            lz_enc_len(&rc, &st.rep_len_enc, pb, rep0_len - 2u);
            state = (state < 7u) ? 8u : 11u;
            for (u32 k = 1u; k < rep0_len && (pos + k + 3u) <= src_len; k++) {
                u32 hk = (((u32)src[pos + k] * 2654435761u) ^
                          ((u32)src[pos + k + 1u] << 5) ^
                          (u32)src[pos + k + 2u]) & 1023u;
                head[hk] = (s32)(pos + k);
            }
            pos += rep0_len;
        } else if (best_len >= 3u) {
            lz_rc_bit(&rc, &st.is_match[state][pb], 1u);
            lz_rc_bit(&rc, &st.is_rep[state], 0u);
            u32 len_m2 = best_len - 2u;
            lz_enc_len(&rc, &st.len_enc, pb, len_m2);

            u32 dist0 = best_dist - 1u;
            u32 l2p   = (len_m2 < 4u) ? len_m2 : 3u;
            u32 pslot = lz_get_pos_slot(dist0);
            lz_rc_tree(&rc, st.pos_slot[l2p], 6u, pslot);
            if (pslot >= 4u) {
                u32 footer_bits = (pslot >> 1) - 1u;
                u32 base        = (2u | (pslot & 1u)) << footer_bits;
                u32 rem         = dist0 - base;
                if (pslot < 14u) {
                    lz_rc_rev_tree(&rc, &st.spec_pos[base], footer_bits, rem);
                } else {
                    lz_rc_direct(&rc, rem >> 4, footer_bits - 4u);
                    lz_rc_rev_tree(&rc, st.align, 4u, rem & 0x0Fu);
                }
            }
            rep3  = rep2;
            rep2  = rep1;
            rep1  = rep0;
            rep0  = best_dist;
            state = (state < 7u) ? 7u : 10u;
            for (u32 k = 1u; k < best_len && (pos + k + 3u) <= src_len; k++) {
                u32 hk = (((u32)src[pos + k] * 2654435761u) ^
                          ((u32)src[pos + k + 1u] << 5) ^
                          (u32)src[pos + k + 2u]) & 1023u;
                head[hk] = (s32)(pos + k);
            }
            pos += best_len;
        } else {
            lz_rc_bit(&rc, &st.is_match[state][pb], 0u);
            u8  prev = (pos == 0u) ? 0u : src[pos - 1u];
            u16 *lit = st.lit[prev >> 5];
            u8  sym  = src[pos];
            if (state < 7u) {
                lz_rc_tree(&rc, lit, 8u, sym);
                state = (state < 4u) ? 0u : (state - 3u);
            } else {
                u8   mb_byte = src[pos - rep0];
                u32  m       = 1u;
                bool matched = true;
                for (u32 i = 8u; i > 0u; i--) {
                    u32 bit = (sym >> (i - 1u)) & 1u;
                    u32 mb  = (mb_byte >> (i - 1u)) & 1u;
                    u32 ctx = matched ? (0x100u + (mb << 8) + m) : m;
                    lz_rc_bit(&rc, &lit[ctx], bit);
                    m = (m << 1) | bit;
                    if (mb != bit) {
                        matched = false;
                    }
                }
                state = (state < 10u) ? (state - 3u) : (state - 6u);
            }
            pos++;
        }
    }

    for (u32 i = 0u; i < 5u; i++) {
        lz_rc_shift_low(&rc);
    }
    return (rc.pos <= dst_cap) ? (u32)rc.pos : 0u;
}

/* ==========================================================================
 * Codec 3: Pure-C RFC 8878 Zstandard Frame Encoder (cdzs)
 *   Emits RLE_Block + Compressed_Block (set_rle literals + nbSeq=0) + Raw_Block
 * ========================================================================== */
static u32 encode_zstd_frame(const u8 *src, u32 src_len, u8 *dst, u32 dst_cap) {
    if (src_len < 256u || src_len > 65791u || dst_cap < 16u) {
        return 0u;
    }
    u32 out = 0u;
    /* Magic 0xFD2FB528 little-endian */
    dst[out++] = 0x28u;
    dst[out++] = 0xB5u;
    dst[out++] = 0x2Fu;
    dst[out++] = 0xFDu;
    /* FHD: FCS_Field_Size=2 (01), Single_Segment_Flag=1 -> 0x60 */
    dst[out++] = 0x60u;
    u16 fcs = (u16)(src_len - 256u);
    dst[out++] = (u8)(fcs & 0xFFu);
    dst[out++] = (u8)(fcs >> 8);

    bool used_cblock = false;
    u32  pos         = 0u;
    while (pos < src_len) {
        /* Measure RLE run at pos */
        u32 run = 1u;
        while (pos + run < src_len && src[pos + run] == src[pos]) {
            run++;
        }
        if (run >= 6u) {
            u32 last = (pos + run == src_len) ? 1u : 0u;
            /* Exercise Block_Type=2 (Compressed_Block) once with set_rle + nbSeq=0 */
            if (!used_cblock && run >= 16u && src_len > 64u) {
                used_cblock = true;
                u32 csize   = 5u; /* 3B RLE_Literals_Header + 1B value + 1B nbSeq=0 */
                u32 bhdr    = last | (2u << 1) | (csize << 3);
                if (out + 3u + csize > dst_cap) {
                    return 0u;
                }
                dst[out++] = (u8)(bhdr & 0xFFu);
                dst[out++] = (u8)((bhdr >> 8) & 0xFFu);
                dst[out++] = (u8)((bhdr >> 16) & 0xFFu);
                u32 lhdr   = (run << 4) | 0x0Du; /* set_rle (01) | lhlCode=3 (11<<2) */
                dst[out++] = (u8)(lhdr & 0xFFu);
                dst[out++] = (u8)((lhdr >> 8) & 0xFFu);
                dst[out++] = (u8)((lhdr >> 16) & 0xFFu);
                dst[out++] = src[pos];
                dst[out++] = 0x00u; /* Number_of_Sequences = 0 */
            } else {
                u32 bhdr = last | (1u << 1) | (run << 3); /* RLE_Block */
                if (out + 4u > dst_cap) {
                    return 0u;
                }
                dst[out++] = (u8)(bhdr & 0xFFu);
                dst[out++] = (u8)((bhdr >> 8) & 0xFFu);
                dst[out++] = (u8)((bhdr >> 16) & 0xFFu);
                dst[out++] = src[pos];
            }
            pos += run;
            continue;
        }

        /* Advance literal span up to the next >=6-byte RLE run */
        u32 lit_end = pos + 1u;
        while (lit_end < src_len) {
            u32 r = 1u;
            while (lit_end + r < src_len && r < 6u && src[lit_end + r] == src[lit_end]) {
                r++;
            }
            if (r >= 6u) {
                break;
            }
            lit_end++;
        }
        u32 lit_len = lit_end - pos;
        u32 last    = (lit_end == src_len) ? 1u : 0u;
        u32 bhdr    = last | (0u << 1) | (lit_len << 3); /* Raw_Block */
        if (out + 3u + lit_len > dst_cap) {
            return 0u;
        }
        dst[out++] = (u8)(bhdr & 0xFFu);
        dst[out++] = (u8)((bhdr >> 8) & 0xFFu);
        dst[out++] = (u8)((bhdr >> 16) & 0xFFu);
        memcpy(&dst[out], &src[pos], lit_len);
        out += lit_len;
        pos = lit_end;
    }
    return out;
}

/* ==========================================================================
 * Codec 4: Pure-C RFC 9639 Headerless FLAC Encoder (44.1kHz 2ch 16-bit, cdfl)
 *   Fixed Predictor Order 1 + Optimal Rice Coding + CRC-8 / CRC-16!
 * ========================================================================== */
typedef struct MsbBitWriter {
    u8    *buf;
    size_t cap;
    size_t byte_pos;
    u32    bit_buf;
    u32    bit_cnt;
} MsbBitWriter;

static void msb_bw_init(MsbBitWriter *bw, u8 *buf, size_t cap) {
    bw->buf      = buf;
    bw->cap      = cap;
    bw->byte_pos = 0u;
    bw->bit_buf  = 0u;
    bw->bit_cnt  = 0u;
}

static void msb_bw_put(MsbBitWriter *bw, u32 val, u32 nbits) {
    for (u32 i = nbits; i > 0u; i--) {
        bw->bit_buf = (bw->bit_buf << 1) | ((val >> (i - 1u)) & 1u);
        bw->bit_cnt++;
        if (bw->bit_cnt == 8u) {
            if (bw->byte_pos < bw->cap) {
                bw->buf[bw->byte_pos] = (u8)(bw->bit_buf & 0xFFu);
            }
            bw->byte_pos++;
            bw->bit_buf = 0u;
            bw->bit_cnt = 0u;
        }
    }
}

static size_t msb_bw_align_zero(MsbBitWriter *bw) {
    if (bw->bit_cnt > 0u) {
        msb_bw_put(bw, 0u, 8u - bw->bit_cnt);
    }
    return bw->byte_pos;
}

static void flac_encode_fixed1_subframe(MsbBitWriter *bw, const s16 *ch_pcm, u32 block_samples) {
    /* Subframe header: 0 pad (1b) | FIXED order=1 (001001 = 9, 6b) | wasted=0 (1b) -> 0x12 */
    msb_bw_put(bw, 0x12u, 8u);
    /* Unencoded 16-bit two's-complement warm-up sample s[0] */
    msb_bw_put(bw, (u16)ch_pcm[0], 16u);

    /* Compute folded Rice unsigned residuals for Fixed order 1: r[i] = s[i] - s[i-1] */
    u64 sum_u = 0u;
    for (u32 i = 1u; i < block_samples; i++) {
        s32 r = (s32)ch_pcm[i] - (s32)ch_pcm[i - 1u];
        u32 u = (r >= 0) ? ((u32)r << 1) : ((u32)(-r - 1) << 1) | 1u;
        sum_u += u;
    }
    u32 avg_u = (u32)(sum_u / (block_samples - 1u));
    u32 k     = 0u;
    while (k < 14u && (1u << k) < avg_u) {
        k++;
    }
    if (k < 4u) {
        k = 4u; /* Prevent overly long unary runs on sudden transients */
    }

    /* Residual coding method 00 (2b), partition order 0000 (4b), 4-bit Rice parameter k */
    msb_bw_put(bw, 0u, 2u);
    msb_bw_put(bw, 0u, 4u);
    msb_bw_put(bw, k, 4u);

    u32 mask = (k == 0u) ? 0u : ((1u << k) - 1u);
    for (u32 i = 1u; i < block_samples; i++) {
        s32 r = (s32)ch_pcm[i] - (s32)ch_pcm[i - 1u];
        u32 u = (r >= 0) ? ((u32)r << 1) : (((u32)(-r - 1) << 1) | 1u);
        u32 q = u >> k;
        while (q > 0u) {
            u32 chunk = (q > 24u) ? 24u : q;
            msb_bw_put(bw, 0u, chunk);
            q -= chunk;
        }
        msb_bw_put(bw, 1u, 1u); /* Unary stop bit */
        if (k > 0u) {
            msb_bw_put(bw, u & mask, k);
        }
    }
}

/*
 * Encodes an 8-sector CD-DA hunk for CHD `cdfl`:
 *   Input `be_base_18816` holds big-endian 16-bit stereo PCM (4704 stereo frames).
 *   Splits into two 2352-sample FLAC frames (matching cdfl_codec_blocksize(18816)=2352)
 *   followed immediately by raw-Deflate compressed 768-byte subcode.
 */
static u32 encode_cdfl_hunk(const u8 *be_base_18816, const u8 *sub_768,
                            u8 *dst, u32 dst_cap) {
    s16 ch_l[2352];
    s16 ch_r[2352];
    u32 out = 0u;

    for (u32 fn = 0u; fn < 2u; fn++) {
        const u8 *fsrc = be_base_18816 + fn * 2352u * 4u;
        for (u32 s = 0u; s < 2352u; s++) {
            u16 ul = ((u16)fsrc[s * 4u + 0u] << 8) | (u16)fsrc[s * 4u + 1u];
            u16 ur = ((u16)fsrc[s * 4u + 2u] << 8) | (u16)fsrc[s * 4u + 3u];
            ch_l[s] = (s16)ul;
            ch_r[s] = (s16)ur;
        }

        if (out + 16u > dst_cap) {
            return 0u;
        }
        u8 *fhdr = dst + out;
        fhdr[0] = 0xFFu;
        fhdr[1] = 0xF8u; /* Fixed-blocksize sync */
        fhdr[2] = 0x79u; /* 16-bit (blocksize-1) at end of header (7) | 44.1 kHz (9) */
        fhdr[3] = 0x18u; /* 2ch L/R independent (1) | 16-bit samples (4<<1) | 0 */
        fhdr[4] = (u8)fn; /* UTF-8 frame number 0 or 1 */
        put_be16(&fhdr[5], 2352u - 1u);
        fhdr[7] = calc_flac_crc8(fhdr, 7u);

        MsbBitWriter bw;
        msb_bw_init(&bw, fhdr + 8u, dst_cap - out - 10u);
        flac_encode_fixed1_subframe(&bw, ch_l, 2352u);
        flac_encode_fixed1_subframe(&bw, ch_r, 2352u);
        size_t body_bytes = msb_bw_align_zero(&bw);
        size_t frame_wo_crc = 8u + body_bytes;
        if (out + frame_wo_crc + 2u > dst_cap) {
            return 0u;
        }
        u16 fcrc = calc_flac_crc16(fhdr, frame_wo_crc);
        put_be16(fhdr + frame_wo_crc, fcrc);
        out += (u32)(frame_wo_crc + 2u);
    }

    u32 sub_len = encode_raw_deflate(sub_768, 768u, dst + out, dst_cap - out);
    if (sub_len == 0u) {
        return 0u;
    }
    return out + sub_len;
}

/* Wrap base + subcode into CHD CD codec container (cdzl / cdlz / cdzs) */
static u32 encode_cd_wrapped_hunk(u32 codec_tag, const u8 *hunk_19584, u8 ecc_mask,
                                  u8 *dst, u32 dst_cap) {
    static u8 base_buf[8u * 2352u];
    static u8 sub_buf[8u * 96u];
    static u8 c_base[32768];
    static u8 c_sub[4096];

    for (u32 f = 0u; f < 8u; f++) {
        u8 *sec = &base_buf[f * 2352u];
        memcpy(sec, &hunk_19584[f * 2448u], 2352u);
        memcpy(&sub_buf[f * 96u], &hunk_19584[f * 2448u + 2352u], 96u);
        if ((ecc_mask & (1u << f)) != 0u) {
            /* Strip 12B CD sync + 276B P/Q ECC so libchdr reconstructs them! */
            memset(sec, 0, 12u);
            ecc_clear(sec);
        }
    }

    u32 cb = 0u, cs = 0u;
    if (codec_tag == CHD_CODEC_CD_ZLIB) {
        cb = encode_raw_deflate(base_buf, sizeof(base_buf), c_base, sizeof(c_base));
        cs = encode_raw_deflate(sub_buf, sizeof(sub_buf), c_sub, sizeof(c_sub));
    } else if (codec_tag == CHD_CODEC_CD_LZMA) {
        cb = encode_raw_lzma(base_buf, sizeof(base_buf), c_base, sizeof(c_base));
        cs = encode_raw_deflate(sub_buf, sizeof(sub_buf), c_sub, sizeof(c_sub));
    } else if (codec_tag == CHD_CODEC_CD_ZSTD) {
        cb = encode_zstd_frame(base_buf, sizeof(base_buf), c_base, sizeof(c_base));
        cs = encode_zstd_frame(sub_buf, sizeof(sub_buf), c_sub, sizeof(c_sub));
    }
    if (cb == 0u || cb > 0xFFFFu || cs == 0u || 3u + cb + cs > dst_cap) {
        return 0u;
    }

    dst[0] = ecc_mask;
    put_be16(&dst[1], (u16)cb);
    memcpy(&dst[3], c_base, cb);
    memcpy(&dst[3u + cb], c_sub, cs);
    return 3u + cb + cs;
}

/* ==========================================================================
 * Spec-Exact CHD v5 File Writer (Header + Compressed Map + Metadata List)
 * ========================================================================== */
typedef enum TestHunkComp {
    THUNK_CDZL = 0, /* Compressor slot 0: 'cdzl' */
    THUNK_CDLZ = 1, /* Compressor slot 1: 'cdlz' */
    THUNK_CDZS = 2, /* Compressor slot 2: 'cdzs' */
    THUNK_CDFL = 3, /* Compressor slot 3: 'cdfl' */
    THUNK_NONE = 4, /* COMPRESSION_NONE (raw 19,584B hunk) */
    THUNK_SELF = 5  /* COMPRESSION_SELF (deduplicated hunk reference) */
} TestHunkComp;

typedef struct TestMetaBlob {
    u32        tag;
    const u8  *data;
    u32        len;
} TestMetaBlob;

static bool write_chd_v5_file(const char *path,
                              const u8 *raw_frames, u32 total_frames,
                              const u8 *frame_is_ecc,
                              const u8 *hunk_comp, const u32 *hunk_self_ref,
                              const TestMetaBlob *metas, u32 meta_count,
                              u64 *out_file_bytes) {
    u32 hunk_count = (total_frames + 7u) / 8u;
    if (hunk_count == 0u) {
        return false;
    }

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return false;
    }

    /* Reserve 124-byte CHD v5 header at offset 0 */
    u8 hdr[124];
    memset(hdr, 0, sizeof(hdr));
    if (fwrite(hdr, 1u, sizeof(hdr), fp) != sizeof(hdr)) {
        fclose(fp);
        return false;
    }

    u8  *rawmap   = (u8 *)calloc(hunk_count, 12u);
    u32 *comp_len = (u32 *)calloc(hunk_count, sizeof(u32));
    u16 *hunk_crc = (u16 *)calloc(hunk_count, sizeof(u16));
    u8  *hunk_buf = (u8 *)malloc(19584u);
    u8  *comp_buf = (u8 *)malloc(32768u);
    if (!rawmap || !comp_len || !hunk_crc || !hunk_buf || !comp_buf) {
        free(rawmap); free(comp_len); free(hunk_crc); free(hunk_buf); free(comp_buf);
        fclose(fp);
        return false;
    }

    static const u32 k_slot_tags[4] = {
        CHD_CODEC_CD_ZLIB, CHD_CODEC_CD_LZMA, CHD_CODEC_CD_ZSTD, CHD_CODEC_CD_FLAC
    };

    u64 first_offs = 124u;
    u64 cur_offs   = first_offs;
    bool ok        = true;

    for (u32 h = 0u; h < hunk_count && ok; h++) {
        memset(hunk_buf, 0, 19584u);
        u8 ecc_mask = 0u;
        for (u32 f = 0u; f < 8u; f++) {
            u32 gf = h * 8u + f;
            if (gf < total_frames) {
                memcpy(&hunk_buf[f * 2448u], &raw_frames[(size_t)gf * 2448u], 2448u);
                if (frame_is_ecc && frame_is_ecc[gf]) {
                    ecc_mask |= (u8)(1u << f);
                }
            }
        }
        hunk_crc[h] = calc_chd_crc16(hunk_buf, 19584u);

        u8   ctype = hunk_comp[h];
        u8  *rm    = &rawmap[h * 12u];
        rm[0]      = ctype;

        if (ctype <= 3u) {
            u32 clen = 0u;
            if (ctype == THUNK_CDFL) {
                static u8 flac_base[8u * 2352u];
                static u8 flac_sub[8u * 96u];
                for (u32 f = 0u; f < 8u; f++) {
                    memcpy(&flac_base[f * 2352u], &hunk_buf[f * 2448u], 2352u);
                    memcpy(&flac_sub[f * 96u], &hunk_buf[f * 2448u + 2352u], 96u);
                }
                clen = encode_cdfl_hunk(flac_base, flac_sub, comp_buf, 32768u);
            } else {
                clen = encode_cd_wrapped_hunk(k_slot_tags[ctype], hunk_buf, ecc_mask,
                                              comp_buf, 32768u);
            }
            if (clen == 0u || fwrite(comp_buf, 1u, clen, fp) != clen) {
                ok = false;
                break;
            }
            comp_len[h] = clen;
            put_be24(&rm[1], clen);
            put_be48(&rm[4], cur_offs);
            put_be16(&rm[10], hunk_crc[h]);
            cur_offs += clen;
        } else if (ctype == THUNK_NONE) {
            if (fwrite(hunk_buf, 1u, 19584u, fp) != 19584u) {
                ok = false;
                break;
            }
            comp_len[h] = 19584u;
            put_be24(&rm[1], 19584u);
            put_be48(&rm[4], cur_offs);
            put_be16(&rm[10], hunk_crc[h]);
            cur_offs += 19584u;
        } else if (ctype == THUNK_SELF) {
            u32 ref = hunk_self_ref ? hunk_self_ref[h] : 0u;
            put_be24(&rm[1], 0u);
            put_be48(&rm[4], ref);
            put_be16(&rm[10], 0u);
        } else {
            ok = false;
        }
    }

    /* Build CHD v5 compressed map at map_offset = cur_offs */
    u64 map_offset = cur_offs;
    size_t map_cap = 64u + (size_t)hunk_count * 8u;
    u8 *map_bits   = (u8 *)calloc(1u, map_cap);
    if (!map_bits) {
        ok = false;
    }

    u32 self_bits = 8u;
    while ((1u << self_bits) <= hunk_count && self_bits < 24u) {
        self_bits++;
    }

    size_t map_bytes = 0u;
    if (ok) {
        MsbBitWriter mbw;
        msb_bw_init(&mbw, map_bits, map_cap);
        /*
         * Canonical RLE Huffman tree for 16 map symbols (maxbits=8 -> 4-bit nibbles):
         *   Symbols 0..7 get length 3 (codes 000..111, exact Kraft sum 8 * 2^-3 = 1.0)
         *   Symbols 8..15 get length 0 (unused)
         * Encoded RLE nibbles: 1, 3, (8-3)=5, 1, 0, (8-3)=5.
         */
        msb_bw_put(&mbw, 1u, 4u);
        msb_bw_put(&mbw, 3u, 4u);
        msb_bw_put(&mbw, 5u, 4u);
        msb_bw_put(&mbw, 1u, 4u);
        msb_bw_put(&mbw, 0u, 4u);
        msb_bw_put(&mbw, 5u, 4u);

        /* Pass 1: 3-bit canonical code == hunk_comp[h] for every hunk */
        for (u32 h = 0u; h < hunk_count; h++) {
            msb_bw_put(&mbw, hunk_comp[h], 3u);
        }
        /* Pass 2: per-hunk length + CRC16 (or self-ref index) */
        for (u32 h = 0u; h < hunk_count; h++) {
            u8 ctype = hunk_comp[h];
            if (ctype <= 3u) {
                msb_bw_put(&mbw, comp_len[h], 16u);
                msb_bw_put(&mbw, hunk_crc[h], 16u);
            } else if (ctype == THUNK_NONE) {
                msb_bw_put(&mbw, hunk_crc[h], 16u);
            } else if (ctype == THUNK_SELF) {
                msb_bw_put(&mbw, hunk_self_ref[h], self_bits);
            }
        }
        map_bytes = msb_bw_align_zero(&mbw);

        u8 map_hdr[16];
        memset(map_hdr, 0, sizeof(map_hdr));
        put_be32(&map_hdr[0], (u32)map_bytes);
        put_be48(&map_hdr[4], first_offs);
        put_be16(&map_hdr[10], calc_chd_crc16(rawmap, (size_t)hunk_count * 12u));
        map_hdr[12] = 16u; /* lengthbits */
        map_hdr[13] = (u8)self_bits;
        map_hdr[14] = 8u;  /* parentbits */
        map_hdr[15] = 0u;

        if (fwrite(map_hdr, 1u, 16u, fp) != 16u ||
            fwrite(map_bits, 1u, map_bytes, fp) != map_bytes) {
            ok = false;
        }
        cur_offs += 16u + (u64)map_bytes;
    }

    /* Append CHD metadata linked list AFTER the map (so mapoffset+mapbytes < file_size) */
    u64 meta_offset = (meta_count > 0u) ? cur_offs : 0u;
    for (u32 m = 0u; m < meta_count && ok; m++) {
        u64 next_ofs = (m + 1u < meta_count) ? (cur_offs + 16u + (u64)metas[m].len) : 0u;
        u8  mhdr[16];
        put_be32(&mhdr[0], metas[m].tag);
        mhdr[4] = CHD_MDFLAGS_CHECKSUM;
        put_be24(&mhdr[5], metas[m].len);
        put_be64(&mhdr[8], next_ofs);
        if (fwrite(mhdr, 1u, 16u, fp) != 16u ||
            fwrite(metas[m].data, 1u, metas[m].len, fp) != metas[m].len) {
            ok = false;
            break;
        }
        cur_offs += 16u + (u64)metas[m].len;
    }

    /* Trailing 16-byte pad so mapoffset + mapbytes < file_size even if meta_count == 0 */
    if (ok && meta_count == 0u) {
        u8 pad[16] = { 0 };
        if (fwrite(pad, 1u, sizeof(pad), fp) != sizeof(pad)) {
            ok = false;
        }
        cur_offs += sizeof(pad);
    }

    /* Finalize 124-byte CHD v5 header at file offset 0 */
    if (ok) {
        memcpy(&hdr[0], "MComprHD", 8u);
        put_be32(&hdr[8], 124u);
        put_be32(&hdr[12], 5u);
        put_be32(&hdr[16], CHD_CODEC_CD_ZLIB);
        put_be32(&hdr[20], CHD_CODEC_CD_LZMA);
        put_be32(&hdr[24], CHD_CODEC_CD_ZSTD);
        put_be32(&hdr[28], CHD_CODEC_CD_FLAC);
        put_be64(&hdr[32], (u64)hunk_count * 19584u);
        put_be64(&hdr[40], map_offset);
        put_be64(&hdr[48], meta_offset);
        put_be32(&hdr[56], 19584u);
        put_be32(&hdr[60], 2448u);
        /* Deliberately non-zero rawsha1/sha1 while parentsha1[104..123] stays 0 */
        hdr[64] = 0x5Au;
        hdr[84] = 0xA5u;
        if (fseek(fp, 0L, SEEK_SET) != 0 || fwrite(hdr, 1u, sizeof(hdr), fp) != sizeof(hdr)) {
            ok = false;
        }
    }

    fclose(fp);
    if (out_file_bytes) {
        *out_file_bytes = cur_offs;
    }
    free(map_bits);
    free(rawmap);
    free(comp_len);
    free(hunk_crc);
    free(hunk_buf);
    free(comp_buf);
    return ok;
}

/* ==========================================================================
 * Deterministic PS1 Data / XA / CD-DA Sector Generators
 * ========================================================================== */
static void gen_mode2_raw_sector(u8 sec[2352], u32 disc_lba, bool form2_xa, bool *out_has_ecc) {
    memset(sec, 0, 2352u);
    cd_synth_sync_msf(disc_lba, 0x02u, sec);

    if (form2_xa) {
        sec[16] = sec[20] = 1u;    /* file 1 */
        sec[17] = sec[21] = 2u;    /* chan 2 */
        sec[18] = sec[22] = 0x64u; /* Form 2 + Realtime + Audio */
        sec[19] = sec[23] = 0x01u; /* Stereo 37.8 kHz 4-bit ADPCM */
        for (u32 sg = 0u; sg < 18u; sg++) {
            u8 *grp = &sec[24u + sg * 128u];
            memset(&grp[4], 0x02, 12u);
            memset(&grp[16], 0x44, 112u);
        }
        *out_has_ecc = false;
    } else {
        sec[18] = sec[22] = 0x08u; /* Form 1 Data */
        for (u32 i = 0u; i < 2048u; i++) {
            /* Mix structured runs + position-dependent data for real compression */
            sec[24u + i] = ((i & 0x3Fu) < 24u)
                               ? (u8)((disc_lba * 7u) & 0xFFu)
                               : (u8)((disc_lba * 13u + i) & 0xFFu);
        }
        if (disc_lba == 4u) {
            const char *lic = "          Licensed  by          Sony Computer Entertainment Amer";
            memcpy(&sec[24], lic, strlen(lic));
        }
        /* Generate real Red Book Mode 2 Form 1 P/Q Reed-Solomon ECC! */
        ecc_generate(sec);
        *out_has_ecc = true;
    }
}

static void gen_cdda_sample_pair(u32 audio_sec_idx, u32 sample_idx, s16 *out_l, s16 *out_r) {
    /*
     * Smooth triangle/ramp waveform (compresses cleanly in FLAC Fixed-1 Rice!)
     * with asymmetric L vs R and guaranteed hi_byte != lo_byte on samples so any
     * missing or double big-endian <-> little-endian swap is caught immediately!
     */
    s32 phase = (s32)((audio_sec_idx * 588u + sample_idx) % 256u);
    s32 tri   = (phase < 128) ? (phase * 180 - 11520) : ((256 - phase) * 180 - 11520);
    s16 l     = (s16)(tri + (s32)(audio_sec_idx * 17u) + 0x0123);
    s16 r     = (s16)(-tri + (s32)(audio_sec_idx * 9u) - 0x0234);
    if (((u16)l >> 8) == ((u16)l & 0xFFu)) {
        l = (s16)(l + 1);
    }
    if (((u16)r >> 8) == ((u16)r & 0xFFu)) {
        r = (s16)(r + 1);
    }
    *out_l = l;
    *out_r = r;
}

static void gen_cdda_sector(u8 sec[2352], u32 audio_sec_idx, bool big_endian) {
    for (u32 i = 0u; i < 588u; i++) {
        s16 l = 0, r = 0;
        gen_cdda_sample_pair(audio_sec_idx, i, &l, &r);
        u16 ul = (u16)l;
        u16 ur = (u16)r;
        if (big_endian) {
            sec[i * 4u + 0u] = (u8)(ul >> 8);
            sec[i * 4u + 1u] = (u8)(ul & 0xFFu);
            sec[i * 4u + 2u] = (u8)(ur >> 8);
            sec[i * 4u + 3u] = (u8)(ur & 0xFFu);
        } else {
            sec[i * 4u + 0u] = (u8)(ul & 0xFFu);
            sec[i * 4u + 1u] = (u8)(ul >> 8);
            sec[i * 4u + 2u] = (u8)(ur & 0xFFu);
            sec[i * 4u + 3u] = (u8)(ur >> 8);
        }
    }
}

static void gen_cooked_payload_2048(u8 out_2048[2048], u32 tag_seed, u32 rel_sec) {
    for (u32 i = 0u; i < 2048u; i++) {
        out_2048[i] = ((i & 0x1Fu) < 12u)
                          ? (u8)(tag_seed + rel_sec)
                          : (u8)((tag_seed * 29u + rel_sec * 11u + i * 3u) & 0xFFu);
    }
    if (tag_seed == 0x40u) {
        memcpy(&out_2048[1], "CD001", 5u);
    }
}

/* ==========================================================================
 * Build Multi-Track Reference .cue/.bin + Multi-Codec CHD v5 Disc
 *
 * Disc Layout (65 logical sectors, 68 CHD frames = 9 hunks across ALL codecs):
 *   Track 1 (MODE2_RAW):  frames=17 (LBA 0..16)  -> CHD frames 0..16  + 3 POISON pad [17..19]
 *   Track 2 (AUDIO):      PREGAP:6 virtual (LBA 17..22), frames=25 (LBA 23..47)
 *                                                -> CHD frames 20..44 + 3 POISON pad [45..47]
 *   Track 3 (MODE1 2048): empty PGTYPE:, frames=9 (LBA 48..56)
 *                                                -> CHD frames 48..56 + 3 POISON pad [57..59]
 *   Track 4 (MODE2_FORM1): frames=16 (LBA 57..72), where hunk 8 (CHD frames 64..71)
 *                          is identical to hunk 7 (CHD frames 56..63? -> make Track 4 = 20
 *                          frames so CHD frames 60..79: hunk 8 [64..71] & hunk 9 [72..79]
 *                          are identical -> hunk 9 uses COMPRESSION_SELF -> hunk 8!)
 * ========================================================================== */
#define T1_LEN          17u
#define T2_PREGAP       6u
#define T2_LEN          25u
#define T2_START_LBA    (T1_LEN + T2_PREGAP)         /* 23 */
#define T3_START_LBA    (T2_START_LBA + T2_LEN)      /* 48 */
#define T3_LEN          9u
#define T4_START_LBA    (T3_START_LBA + T3_LEN)      /* 57 */
#define T4_LEN          20u
#define MAIN_TOTAL_LBA  (T4_START_LBA + T4_LEN)      /* 77 */
#define MAIN_CHD_FRAMES (20u + 28u + 12u + 20u)      /* 80 = exact 10 hunks! */

static int build_main_chd_and_cue(void) {
    u8 *frames = (u8 *)calloc(MAIN_CHD_FRAMES, 2448u);
    u8  is_ecc[MAIN_CHD_FRAMES];
    memset(is_ecc, 0, sizeof(is_ecc));
    CHECK_TRUE(frames != NULL, "alloc main CHD frame buffer");

    FILE *ft1 = fopen(TEST_REF_T1_PATH, "wb");
    FILE *ft2 = fopen(TEST_REF_T2_PATH, "wb");
    CHECK_TRUE(ft1 != NULL && ft2 != NULL, "open ref .bin files");

    /* Track 1: 17 MODE2_RAW sectors at LBA 0..16 -> CHD frames 0..16 */
    u8 sec[2352];
    for (u32 i = 0u; i < T1_LEN; i++) {
        bool has_ecc = false;
        gen_mode2_raw_sector(sec, i, (i == 10u), &has_ecc);
        if (has_ecc) {
            CHECK_TRUE(ecc_verify(sec) != 0, "ecc_generate valid Mode 2 Form 1 ECC");
        }
        memcpy(&frames[i * 2448u], sec, 2352u);
        frames[i * 2448u + 2352u] = 0x11u; /* deterministic subcode pattern */
        is_ecc[i] = has_ecc ? 1u : 0u;
        CHECK_TRUE(fwrite(sec, 1u, 2352u, ft1) == 2352u, "write ref t1 sector");
    }
    fclose(ft1);

    /* Poison Track 1's 3 CHD alignment padding frames [17..19] */
    for (u32 pf = 17u; pf < 20u; pf++) {
        memset(&frames[pf * 2448u], 0xDE, 2352u);
    }

    /* Reference Track 2 .bin: 6 zero pregap sectors + 25 little-endian CD-DA sectors */
    memset(sec, 0, 2352u);
    for (u32 p = 0u; p < T2_PREGAP; p++) {
        CHECK_TRUE(fwrite(sec, 1u, 2352u, ft2) == 2352u, "write ref t2 pregap");
    }
    /* CHD Track 2: 25 BIG-ENDIAN CD-DA sectors at CHD frames 20..44 */
    for (u32 i = 0u; i < T2_LEN; i++) {
        u8 le_sec[2352];
        gen_cdda_sector(le_sec, i, false);
        CHECK_TRUE(fwrite(le_sec, 1u, 2352u, ft2) == 2352u, "write ref t2 le cdda");

        gen_cdda_sector(&frames[(20u + i) * 2448u], i, true);
        memset(&frames[(20u + i) * 2448u + 2352u], 0x22, 96u);
    }
    fclose(ft2);

    /* Poison Track 2's 3 CHD alignment padding frames [45..47] */
    for (u32 pf = 45u; pf < 48u; pf++) {
        memset(&frames[pf * 2448u], 0xAD, 2352u);
    }

    /* Write reference .cue for Tracks 1 & 2 (INDEX 00 00:00:00, INDEX 01 00:00:06) */
    FILE *fcue = fopen(TEST_REF_CUE_PATH, "wb");
    CHECK_TRUE(fcue != NULL, "open ref .cue");
    fprintf(fcue,
            "FILE \"%s\" BINARY\n"
            "  TRACK 01 MODE2/2352\n"
            "    INDEX 01 00:00:00\n"
            "FILE \"%s\" BINARY\n"
            "  TRACK 02 AUDIO\n"
            "    INDEX 00 00:00:00\n"
            "    INDEX 01 00:00:06\n",
            TEST_REF_T1_PATH, TEST_REF_T2_PATH);
    fclose(fcue);

    /* Track 3: 9 cooked MODE1 (2048B) sectors -> CHD frames 48..56 + 3 poison [57..59] */
    for (u32 i = 0u; i < T3_LEN; i++) {
        gen_cooked_payload_2048(&frames[(48u + i) * 2448u], 0x30u, i);
    }
    for (u32 pf = 57u; pf < 60u; pf++) {
        memset(&frames[pf * 2448u], 0xBE, 2352u);
    }

    /* Track 4: 20 cooked MODE2_FORM1 (2048B) sectors -> CHD frames 60..79 */
    for (u32 i = 0u; i < 12u; i++) {
        gen_cooked_payload_2048(&frames[(60u + i) * 2448u], 0x40u, i);
    }
    /* Make hunk 9 (CHD frames 72..79) an exact duplicate of hunk 8 (CHD frames 64..71) */
    memcpy(&frames[72u * 2448u], &frames[64u * 2448u], 8u * 2448u);

    /*
     * Assign codecs across all 10 hunks (every single CHD v5 codec + NONE + SELF!):
     *   Hunk 0 (frames  0.. 7, T1 MODE2_RAW + SCEA + ECC): THUNK_CDZL (Deflate)
     *   Hunk 1 (frames  8..15, T1 MODE2_RAW + XA  + ECC):  THUNK_CDLZ (LZMA)
     *   Hunk 2 (frames 16..23, T1 tail + pad + T2 AUDIO):  THUNK_CDZS (Zstandard)
     *   Hunk 3 (frames 24..31, pure T2 CD-DA AUDIO!):      THUNK_CDFL (FLAC CD-DA!)
     *   Hunk 4 (frames 32..39, pure T2 CD-DA AUDIO!):      THUNK_CDFL (FLAC CD-DA!)
     *   Hunk 5 (frames 40..47, T2 AUDIO tail + pad):       THUNK_NONE (Uncompressed!)
     *   Hunk 6 (frames 48..55, T3 cooked MODE1):           THUNK_CDLZ (LZMA)
     *   Hunk 7 (frames 56..63, T3 tail + pad + T4 head):   THUNK_CDZL (Deflate)
     *   Hunk 8 (frames 64..71, T4 cooked MODE2_FORM1):     THUNK_CDZS (Zstandard)
     *   Hunk 9 (frames 72..79, duplicate of hunk 8):       THUNK_SELF -> hunk 8!
     */
    static const u8 k_hunk_comp[10] = {
        THUNK_CDZL, THUNK_CDLZ, THUNK_CDZS, THUNK_CDFL, THUNK_CDFL,
        THUNK_NONE, THUNK_CDLZ, THUNK_CDZL, THUNK_CDZS, THUNK_SELF
    };
    static const u32 k_hunk_self[10] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 8u };

    /* Note Track 1 & 3 intentionally test chdman's empty "PGTYPE: " token! */
    static const char k_m1[] =
        "TRACK:1 TYPE:MODE2_RAW SUBTYPE:NONE FRAMES:17 PREGAP:0 PGTYPE: PGSUB:RW POSTGAP:0";
    static const char k_m2[] =
        "TRACK:2 TYPE:AUDIO SUBTYPE:NONE FRAMES:25 PREGAP:6 PGTYPE:AUDIO PGSUB:RW POSTGAP:0";
    static const char k_m3[] =
        "TRACK:3 TYPE:MODE1 SUBTYPE:NONE FRAMES:9 PREGAP:0 PGTYPE: PGSUB:NONE POSTGAP:0";
    static const char k_m4[] =
        "TRACK:4 TYPE:MODE2_FORM1 SUBTYPE:NONE FRAMES:20";

    TestMetaBlob metas[4] = {
        { CDROM_TRACK_METADATA2_TAG, (const u8 *)k_m1, (u32)sizeof(k_m1) },
        { CDROM_TRACK_METADATA2_TAG, (const u8 *)k_m2, (u32)sizeof(k_m2) },
        { CDROM_TRACK_METADATA2_TAG, (const u8 *)k_m3, (u32)sizeof(k_m3) },
        { CDROM_TRACK_METADATA_TAG,  (const u8 *)k_m4, (u32)sizeof(k_m4) },
    };

    u64 chd_bytes = 0u;
    bool ok = write_chd_v5_file(TEST_CHD_MAIN_PATH, frames, MAIN_CHD_FRAMES,
                                is_ecc, k_hunk_comp, k_hunk_self, metas, 4u, &chd_bytes);
    free(frames);
    CHECK_TRUE(ok && chd_bytes > 124u && chd_bytes < (u64)MAIN_CHD_FRAMES * 2448u,
               "write compressed multi-track multi-codec CHD v5 disc");
    return 0;
}

/* ==========================================================================
 * Test 1: Multi-Track CHD v5 vs .cue/.bin 100% 2352-Byte Parity + Cooked Modes
 * ========================================================================== */
static int test_chd_parity_and_padding(void) {
    static CdImage cue_img;
    static CdImage chd_img;
    cd_image_init(&cue_img);
    cd_image_init(&chd_img);

    CHECK_TRUE(cd_image_open(&cue_img, TEST_REF_CUE_PATH) == PSX_OK, "open ref .cue");
    CHECK_TRUE(cd_image_open(&chd_img, TEST_CHD_MAIN_PATH) == PSX_OK, "open main .chd");

    CHECK_TRUE(chd_img.chd != NULL, "chd state populated");
    CHECK_TRUE(chd_img.track_count == 4u && chd_img.first_track == 1u && chd_img.last_track == 4u,
               "CHD 4-track TOC count");
    CHECK_TRUE(chd_img.total_lba == MAIN_TOTAL_LBA, "CHD total_lba excludes 4-frame CHD padding");
    CHECK_TRUE(chd_img.region_char == 'A', "CHD auto-detected SCEA region from cdzl Track 1 LBA 4");

    /* Verify Track 1..4 exact LBA mapping matches CUE (no 3-frame drift from padding!) */
    CHECK_TRUE(chd_img.tracks[0].start_lba == 0u && chd_img.tracks[0].length_lba == T1_LEN,
               "Track 1 LBA 0..16");
    CHECK_TRUE(chd_img.tracks[1].index0_lba == cue_img.tracks[1].index0_lba &&
               chd_img.tracks[1].start_lba == cue_img.tracks[1].start_lba &&
               chd_img.tracks[1].length_lba == cue_img.tracks[1].length_lba,
               "Track 2 index0/start/length matches CUE exactly (LBA 17/23/25)");
    CHECK_TRUE(chd_img.tracks[2].start_lba == T3_START_LBA && chd_img.tracks[2].length_lba == T3_LEN,
               "Track 3 LBA 48..56 (ignores Track 2's 3 padding frames)");
    CHECK_TRUE(chd_img.tracks[3].start_lba == T4_START_LBA && chd_img.tracks[3].length_lba == T4_LEN,
               "Track 4 LBA 57..76 (ignores Track 3's 3 padding frames)");

    /* 100% byte-for-byte parity across EVERY sector of Track 1 + pregap + Track 2 (LBA 0..47)! */
    u8 cue_sec[2352];
    u8 chd_sec[2352];
    for (u32 lba = 0u; lba < T3_START_LBA; lba++) {
        CHECK_TRUE(cd_image_read_sector(&cue_img, lba, cue_sec), "read cue sector");
        CHECK_TRUE(cd_image_read_sector(&chd_img, lba, chd_sec), "read chd sector");
        if (memcmp(cue_sec, chd_sec, 2352u) != 0) {
            for (u32 b = 0u; b < 2352u; b++) {
                if (cue_sec[b] != chd_sec[b]) {
                    fprintf(stderr,
                            "CUE vs CHD mismatch at LBA %u byte %u: cue=0x%02X chd=0x%02X\n",
                            (unsigned)lba, (unsigned)b,
                            (unsigned)cue_sec[b], (unsigned)chd_sec[b]);
                    break;
                }
            }
            cd_image_close(&cue_img);
            cd_image_close(&chd_img);
            return 1;
        }
    }

    /* Explicitly verify reconstructed Reed-Solomon ECC on Track 1 Form 1 sectors */
    CHECK_TRUE(cd_image_read_sector(&chd_img, 4u, chd_sec) && ecc_verify(chd_sec) != 0,
               "libchdr reconstructed Mode 2 Form 1 sync + P/Q ECC passes ecc_verify");

    /* Explicitly spot-check CD-DA audio endianness across cdzs (LBA 23), cdfl (LBA 27..42), NONE (LBA 43..47) */
    for (u32 a = 0u; a < T2_LEN; a++) {
        CHECK_TRUE(cd_image_read_sector(&chd_img, T2_START_LBA + a, chd_sec), "read cdda sector");
        s16 exp_l = 0, exp_r = 0;
        gen_cdda_sample_pair(a, 0u, &exp_l, &exp_r);
        s16 got_l = (s16)((u16)chd_sec[0] | ((u16)chd_sec[1] << 8));
        s16 got_r = (s16)((u16)chd_sec[2] | ((u16)chd_sec[3] << 8));
        CHECK_TRUE(got_l == exp_l && got_r == exp_r,
                   "CD-DA 16-bit PCM swapped BE -> exact LE across cdzs/cdfl/NONE");
    }

    /* Verify Track 3 cooked MODE1 (2048B -> synthesized Mode 1 header + payload at +16) */
    u8 exp_2048[2048];
    for (u32 i = 0u; i < T3_LEN; i++) {
        u32 lba = T3_START_LBA + i;
        CHECK_TRUE(cd_image_read_sector(&chd_img, lba, chd_sec), "read cooked MODE1 sector");
        u8 mm = 0u, ss = 0u, ff = 0u;
        cd_lba_to_msf(lba, &mm, &ss, &ff);
        CHECK_TRUE(chd_sec[0] == 0x00u && chd_sec[1] == 0xFFu && chd_sec[11] == 0x00u &&
                   chd_sec[12] == cd_bin_to_bcd(mm) && chd_sec[13] == cd_bin_to_bcd(ss) &&
                   chd_sec[14] == cd_bin_to_bcd(ff) && chd_sec[15] == 0x01u,
                   "Cooked MODE1 synthesized sync + BCD MSF + mode 0x01");
        gen_cooked_payload_2048(exp_2048, 0x30u, i);
        CHECK_TRUE(memcmp(&chd_sec[16], exp_2048, 2048u) == 0, "Cooked MODE1 payload at +16");
    }

    /* Verify Track 4 cooked MODE2_FORM1 (+24 payload) including COMPRESSION_SELF hunk 9! */
    for (u32 i = 0u; i < T4_LEN; i++) {
        u32 lba = T4_START_LBA + i;
        CHECK_TRUE(cd_image_read_sector(&chd_img, lba, chd_sec), "read cooked MODE2_FORM1 sector");
        CHECK_TRUE(chd_sec[15] == 0x02u && chd_sec[18] == 0x08u && chd_sec[22] == 0x08u,
                   "Cooked MODE2_FORM1 synthesized Mode 2 Form 1 subheader");
        u32 src_i = (i >= 12u) ? (i - 8u) : i; /* CHD frames 72..79 duplicate 64..71 (i=4..11) */
        gen_cooked_payload_2048(exp_2048, 0x40u, src_i);
        CHECK_TRUE(memcmp(&chd_sec[24], exp_2048, 2048u) == 0,
                   "Cooked MODE2_FORM1 + COMPRESSION_SELF payload at +24");
    }

    /* Out-of-range read fails cleanly */
    CHECK_TRUE(!cd_image_read_sector(&chd_img, MAIN_TOTAL_LBA, chd_sec),
               "Out-of-bounds CHD LBA read rejected");

    cd_image_close(&cue_img);
    cd_image_close(&chd_img);
    return 0;
}

/* ==========================================================================
 * Test 2: 8-Slot (and 4-Slot O3DS) LRU Decoded Hunk Cache Verification
 * ========================================================================== */
static int test_chd_lru_hunk_cache(void) {
    static CdImage img;
    cd_image_init(&img);
    CHECK_TRUE(cd_image_open(&img, TEST_CHD_MAIN_PATH) == PSX_OK, "open CHD for cache test");

    u64 hits = 0u, misses = 0u;
    u32 slots = 0u;
    cd_chd_get_cache_stats(&img, &hits, &misses, &slots);
    CHECK_TRUE(slots == 8u && hits == 0u && misses == 0u,
               "8-slot hunk cache initialized & stats reset after region detect");

    /*
     * Sequential scan over 16 Track-1 + 25 Track-2 + 9 Track-3 + 20 Track-4 = 70
     * physical CHD-backed sectors (spanning 10 8-sector hunks -> >= 85% overall),
     * AND test an exact contiguous 64-sector (8-hunk) run first:
     * Track 2 LBA 23..47 (CHD frames 20..44) — even cleaner: Track 4 + Track 3 or
     * open our vpg/bench disc, OR check Track 4's 16 contiguous sectors at CHD frames
     * 64..79 (LBA 61..76 = exactly hunks 8 & 9 = 2 misses + 14 hits = 87.50%!), plus
     * full-disc sequential pass!
     */
    u8 sec[2352];
    for (u32 lba = 61u; lba <= 76u; lba++) { /* CHD frames 64..79 = exact hunks 8 & 9 */
        CHECK_TRUE(cd_image_read_sector(&img, lba, sec), "read hunk 8..9 sector");
    }
    cd_chd_get_cache_stats(&img, &hits, &misses, &slots);
    CHECK_TRUE(misses == 2u && hits == 14u, "Exact 7/8 = 87.5% cold sequential hunk cache hit rate");

    /* Now load 6 more distinct hunks (hunks 0, 1, 2, 3, 4, 5) to fill all 8 LRU slots! */
    static const u32 k_hunk_lba[8] = {
        61u, /* hunk 8 */
        69u, /* hunk 9 */
        0u,  /* hunk 0 */
        8u,  /* hunk 1 */
        16u, /* hunk 2 */
        27u, /* hunk 3 (CHD frame 24) */
        35u, /* hunk 4 (CHD frame 32) */
        43u  /* hunk 5 (CHD frame 40) */
    };
    for (u32 i = 2u; i < 8u; i++) {
        CHECK_TRUE(cd_image_read_sector(&img, k_hunk_lba[i], sec), "fill 8-slot cache");
    }

    /* Warm pass across ALL sectors in those 8 cached hunks -> 100.0% cache hits (0 misses)! */
    cd_chd_reset_cache_stats(&img);
    for (u32 pass = 0u; pass < 2u; pass++) {
        for (u32 i = 0u; i < 8u; i++) {
            CHECK_TRUE(cd_image_read_sector(&img, k_hunk_lba[7u - i], sec), "warm 8-slot read");
        }
    }
    cd_chd_get_cache_stats(&img, &hits, &misses, &slots);
    CHECK_TRUE(misses == 0u && hits == 16u, "100% warm cache hit rate across 8 live hunks");

    /*
     * True LRU eviction check:
     * Right now last access order ended with k_hunk_lba[0] (hunk 8 = MRU), while
     * k_hunk_lba[7] (hunk 5, LBA 43) was touched earliest in the second pass (= LRU)!
     * Touch hunk 5 (LBA 43) so hunk 4 (LBA 35) becomes the true LRU victim, then load
     * a 9th distinct hunk (hunk 6, LBA 48): hunk 5 MUST stay cached, hunk 4 MUST be evicted!
     */
    CHECK_TRUE(cd_image_read_sector(&img, 43u, sec), "touch hunk 5 to promote out of LRU");
    cd_chd_reset_cache_stats(&img);
    CHECK_TRUE(cd_image_read_sector(&img, 48u, sec), "read 9th hunk (hunk 6) -> evicts hunk 4");
    CHECK_TRUE(cd_image_read_sector(&img, 43u, sec), "hunk 5 remained resident");
    cd_chd_get_cache_stats(&img, &hits, &misses, &slots);
    CHECK_TRUE(misses == 1u && hits == 1u, "Hunk 5 survived LRU eviction");
    CHECK_TRUE(cd_image_read_sector(&img, 35u, sec), "re-read evicted hunk 4 -> miss");
    cd_chd_get_cache_stats(&img, &hits, &misses, &slots);
    CHECK_TRUE(misses == 2u && hits == 1u, "True LRU victim (hunk 4) was evicted");

    cd_image_close(&img);

    /* Verify O3DS small_mem = true selects 4-slot (~78 KB) LRU cache */
    cd_image_init(&img);
    img.small_mem = true;
    CHECK_TRUE(cd_image_open(&img, TEST_CHD_MAIN_PATH) == PSX_OK, "open CHD in small_mem mode");
    cd_chd_get_cache_stats(&img, &hits, &misses, &slots);
    CHECK_TRUE(slots == 4u, "O3DS small_mem uses 4-slot LRU hunk cache");
    cd_image_close(&img);
    return 0;
}

/* ==========================================================================
 * Test 3: In-CHD 'V' Pregaps, POSTGAP, Magic .img Detect, Legacy CHCD & Bad CHD
 * ========================================================================== */
static int test_chd_vpregap_legacy_and_errors(void) {
    /*
     * 1. Build 2-track CHD with '.img' extension (tests MComprHD magic auto-detect!),
     *    CHGD tag with PAD:2, and Track 2 in-CHD 'VAUDIO' pregap + POSTGAP:3:
     *      Track 1 (CHGD MODE2_RAW): FRAMES:5 PAD:2 POSTGAP:3 -> CHD frames 0..4 (+3 pad = 0..7)
     *        Logical LBAs: 0..4 active, 5..9 pad+postgap (zero Mode 2 sectors)
     *      Track 2 (CHT2 AUDIO):     FRAMES:8 PREGAP:3 PGTYPE:VAUDIO POSTGAP:2 -> CHD frames 8..15
     *        Logical LBAs: index0_lba=10..12 (in-CHD BE audio!), start_lba=13..17 (in-CHD BE audio!),
     *                      18..19 postgap silence -> total_lba = 20!
     */
    u8 *frames = (u8 *)calloc(16u, 2448u);
    u8  is_ecc[16];
    memset(is_ecc, 0, sizeof(is_ecc));
    CHECK_TRUE(frames != NULL, "alloc vpg frames");

    for (u32 i = 0u; i < 5u; i++) {
        bool he = false;
        gen_mode2_raw_sector(&frames[i * 2448u], i, false, &he);
        is_ecc[i] = he ? 1u : 0u;
    }
    for (u32 i = 0u; i < 8u; i++) {
        gen_cdda_sector(&frames[(8u + i) * 2448u], 100u + i, true);
    }

    static const u8 k_vcomp[2] = { THUNK_CDZL, THUNK_CDFL };
    static const char k_vm1[] =
        "TRACK:1 TYPE:MODE2_RAW SUBTYPE:NONE FRAMES:5 PAD:2 PREGAP:0 PGTYPE: PGSUB:NONE POSTGAP:3";
    static const char k_vm2[] =
        "TRACK:2 TYPE:AUDIO SUBTYPE:NONE FRAMES:8 PREGAP:3 PGTYPE:VAUDIO PGSUB:RW POSTGAP:2";
    TestMetaBlob vmetas[2] = {
        { GDROM_TRACK_METADATA_TAG,  (const u8 *)k_vm1, (u32)sizeof(k_vm1) },
        { CDROM_TRACK_METADATA2_TAG, (const u8 *)k_vm2, (u32)sizeof(k_vm2) },
    };
    CHECK_TRUE(write_chd_v5_file(TEST_CHD_VPG_PATH, frames, 16u, is_ecc, k_vcomp, NULL,
                                 vmetas, 2u, NULL),
               "write .img CHD with CHGD PAD + VAUDIO in-CHD pregap");

    static CdImage img;
    cd_image_init(&img);
    CHECK_TRUE(cd_chd_has_magic(TEST_CHD_VPG_PATH), "MComprHD magic detected on .img");
    CHECK_TRUE(cd_image_open(&img, TEST_CHD_VPG_PATH) == PSX_OK, "open .img CHD via magic");
    CHECK_TRUE(img.total_lba == 20u && img.tracks[1].index0_lba == 10u &&
               img.tracks[1].start_lba == 13u && img.tracks[1].length_lba == 5u,
               "CHGD PAD + VAUDIO pregap + POSTGAP logical geometry");

    u8 sec[2352], exp[2352];
    /* Pad/postgap of Track 1 (LBA 6) synthesizes clean Mode 2 zero sector */
    CHECK_TRUE(cd_image_read_sector(&img, 6u, sec) && sec[15] == 0x02u && sec[24] == 0u,
               "Track 1 PAD/POSTGAP synthesizes clean Mode 2 sector");
    /* In-CHD VAUDIO pregap (LBA 11 -> audio frame 101) swaps BE->LE! */
    gen_cdda_sector(exp, 101u, false);
    CHECK_TRUE(cd_image_read_sector(&img, 11u, sec) && memcmp(sec, exp, 2352u) == 0,
               "In-CHD VAUDIO pregap read & byte-swapped cleanly");
    /* Active Track 2 INDEX 01 (LBA 13 -> audio frame 103) */
    gen_cdda_sector(exp, 103u, false);
    CHECK_TRUE(cd_image_read_sector(&img, 13u, sec) && memcmp(sec, exp, 2352u) == 0,
               "Post-VAUDIO active track frame mapped & byte-swapped cleanly");
    /* Track 2 audio postgap (LBA 19 -> silence) */
    memset(exp, 0, 2352u);
    CHECK_TRUE(cd_image_read_sector(&img, 19u, sec) && memcmp(sec, exp, 2352u) == 0,
               "Track 2 POSTGAP returns clean silence");
    cd_image_close(&img);

    /* 2. Legacy CHCD binary TOC metadata test */
    u8 chcd_toc[4u + 2u * 24u];
    memset(chcd_toc, 0, sizeof(chcd_toc));
    put_be32(&chcd_toc[0], 2u); /* 2 tracks */
    /* Track 1: MODE2_RAW, 5 frames + 3 extra = 8 CHD frames */
    put_be32(&chcd_toc[4 + 0],  CD_TRACK_MODE2_RAW);
    put_be32(&chcd_toc[4 + 8],  2352u);
    put_be32(&chcd_toc[4 + 16], 5u);
    put_be32(&chcd_toc[4 + 20], 3u);
    /* Track 2: AUDIO, 8 frames + 0 extra */
    put_be32(&chcd_toc[28 + 0],  CD_TRACK_AUDIO);
    put_be32(&chcd_toc[28 + 8],  2352u);
    put_be32(&chcd_toc[28 + 16], 8u);
    put_be32(&chcd_toc[28 + 20], 0u);
    TestMetaBlob lmeta = { CDROM_OLD_METADATA_TAG, chcd_toc, (u32)sizeof(chcd_toc) };
    CHECK_TRUE(write_chd_v5_file(TEST_CHD_LEGACY_PATH, frames, 16u, is_ecc, k_vcomp, NULL,
                                 &lmeta, 1u, NULL),
               "write legacy CHCD TOC CHD");
    CHECK_TRUE(cd_image_open(&img, TEST_CHD_LEGACY_PATH) == PSX_OK &&
               img.track_count == 2u && img.total_lba == 13u && img.tracks[1].start_lba == 5u,
               "Legacy CHCD binary TOC parsed cleanly");
    gen_cdda_sector(exp, 100u, false);
    CHECK_TRUE(cd_image_read_sector(&img, 5u, sec) && memcmp(sec, exp, 2352u) == 0,
               "Legacy CHCD Track 2 CD-DA read & byte-swapped after 3 extraframes");
    cd_image_close(&img);

    /* 3. Non-CD Hard-Disk CHD (GDDD metadata) & corrupt/truncated .chd rejected with 0 leaks */
    static const char k_hdd[] = "CYLS:10,HEADS:2,SECS:17,BPS:512";
    TestMetaBlob hmeta = { HARD_DISK_METADATA_TAG, (const u8 *)k_hdd, (u32)sizeof(k_hdd) };
    CHECK_TRUE(write_chd_v5_file(TEST_CHD_BAD_PATH, frames, 16u, is_ecc, k_vcomp, NULL,
                                 &hmeta, 1u, NULL),
               "write non-CD GDDD CHD");
    CHECK_TRUE(cd_image_open(&img, TEST_CHD_BAD_PATH) == PSX_ERR_FORMAT && !img.loaded,
               "Hard-disk GDDD CHD cleanly rejected with PSX_ERR_FORMAT");

    free(frames);

    /* Truncated 96-byte CHD v5 header (requires 124 bytes -> CHDERR_READ_ERROR -> PSX_ERR_IO) */
    FILE *fbad = fopen(TEST_CHD_BAD_PATH, "wb");
    CHECK_TRUE(fbad != NULL, "open truncated CHD");
    u8 bad_bytes[256];
    memset(bad_bytes, 0, sizeof(bad_bytes));
    memcpy(bad_bytes, "MComprHD", 8u);
    put_be32(&bad_bytes[8], 124u);
    put_be32(&bad_bytes[12], 5u);
    (void)fwrite(bad_bytes, 1u, 96u, fbad);
    fclose(fbad);
    CHECK_TRUE(cd_image_open(&img, TEST_CHD_BAD_PATH) != PSX_OK && !img.loaded,
               "Truncated 96B CHD cleanly rejected with 0 leaks");

    /* Corrupt CHD version 99 (full header size -> PSX_ERR_FORMAT) */
    fbad = fopen(TEST_CHD_BAD_PATH, "wb");
    CHECK_TRUE(fbad != NULL, "open corrupt version CHD");
    put_be32(&bad_bytes[12], 99u);
    (void)fwrite(bad_bytes, 1u, sizeof(bad_bytes), fbad);
    fclose(fbad);
    CHECK_TRUE(cd_image_open(&img, TEST_CHD_BAD_PATH) == PSX_ERR_FORMAT && !img.loaded,
               "Corrupt version-99 CHD cleanly rejected with PSX_ERR_FORMAT and 0 leaks");

    return 0;
}

/* ==========================================================================
 * Test 4: Full PsxCdrom Hardware Commands + Core 2 Readahead + DMA3 + CD-DA
 * ========================================================================== */
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

static int test_psx_cdrom_chd_integration(void) {
    memset(&g_sys, 0, sizeof(g_sys));
    psx_config_defaults(&g_sys.cfg, false); /* N3DS mode -> 8-slot CHD LRU cache */
    g_sys.cfg.spu_async = false;
    CHECK_TRUE(arena_init(&g_sys.arena, 2u * 1024u * 1024u) == PSX_OK, "arena_init CHD");

    spu_init(&g_sys);
    cdrom_init(&g_sys);

    cd_write_idx(1u);
    cdrom_mmio_write(&g_sys, PSX_REG_CDROM_REG2, 0x1Fu, 1u);

    CHECK_TRUE(!cdrom_is_chd(&g_sys), "cdrom_is_chd false before load");
    CHECK_TRUE(cdrom_load(&g_sys, TEST_CHD_MAIN_PATH) == PSX_OK, "cdrom_load main CHD v5");
    CHECK_TRUE(cdrom_is_chd(&g_sys), "cdrom_is_chd true after load");
    CHECK_TRUE(g_sys.cdrom.region == 'A', "cdrom_load detected SCEA region");

    u8 resp[16];

    /* 1. GetID -> licensed SCEA */
    cd_send_cmd(0x1Au);
    cdrom_event_cb(&g_sys, 0u);
    (void)cd_drain_resp(resp, 16u);
    cd_ack_irq(0x07u);
    cdrom_event_cb(&g_sys, 0u);
    CHECK_TRUE(cd_irq_flag() == 2u && cd_drain_resp(resp, 16u) == 8u &&
               memcmp(&resp[4], "SCEA", 4u) == 0,
               "GetID on CHD returns licensed SCEA");
    cd_ack_irq(0x07u);

    /* 2. GetTN (01..04) & GetTD (verifies MSF for Track 1, 2, 3, 4 & Lead-Out!) */
    cd_send_cmd(0x13u);
    cdrom_event_cb(&g_sys, 0u);
    CHECK_TRUE(cd_irq_flag() == 3u && cd_drain_resp(resp, 16u) == 3u &&
               resp[1] == 0x01u && resp[2] == 0x04u,
               "GetTN 01..04");
    cd_ack_irq(0x07u);

    /* Track 2 starts at LBA 23 -> 150+23 = 173 frames = 00:02:23 -> GetTD returns 00:02 */
    /* Track 3 starts at LBA 48 -> 150+48 = 198 frames = 00:02:48 -> GetTD returns 00:02 */
    /* Lead-out (00) = LBA 77 -> 150+77 = 227 frames = 00:03:02 -> GetTD returns 00:03! */
    cd_push_param(0x00u);
    cd_send_cmd(0x14u);
    cdrom_event_cb(&g_sys, 0u);
    CHECK_TRUE(cd_irq_flag() == 3u && cd_drain_resp(resp, 16u) == 3u &&
               resp[1] == 0x00u && resp[2] == 0x03u,
               "GetTD(00) lead-out == 00:03 (LBA 77 -> 00:03:02, zero padding drift!)");
    cd_ack_irq(0x07u);

    /* 3. Simulate Core 2 async 128-sector readahead pump over compressed CHD! */
    cdrom_ra_seek(&g_sys, 8u); /* Start at LBA 8 (LZMA hunk 1!) */
    u32 pumped = cdrom_ra_pump(&g_sys, 69u); /* LBA 8..76 */
    CHECK_TRUE(pumped == 69u, "Core 2 cdrom_ra_pump decoded 69 CHD sectors into SPSC ring");
    u64 chd_hits = 0u, chd_misses = 0u;
    u32 chd_slots = 0u;
    cdrom_chd_cache_stats(&g_sys, &chd_hits, &chd_misses, &chd_slots);
    CHECK_TRUE(chd_slots == 8u && chd_hits >= chd_misses * 5u,
               "Core 2 readahead leveraged 8-slot decoded hunk cache across LZMA/Zstd/FLAC/Zlib");

    /* 4. Setloc(00:02:09 = LBA 9 in LZMA hunk 1) + SeekL + Setmode(0x80) + ReadN + DMA3 2048B */
    cd_push_param(0x00u);
    cd_push_param(0x02u);
    cd_push_param(0x09u);
    cd_send_cmd(0x02u);
    cdrom_event_cb(&g_sys, 0u);
    (void)cd_drain_resp(resp, 16u);
    cd_ack_irq(0x07u);

    cd_send_cmd(0x15u); /* SeekL */
    cdrom_event_cb(&g_sys, 0u);
    (void)cd_drain_resp(resp, 16u);
    cd_ack_irq(0x07u);
    cdrom_event_cb(&g_sys, 0u);
    CHECK_TRUE(cd_irq_flag() == 2u, "SeekL(LBA 9) -> INT2");
    (void)cd_drain_resp(resp, 16u);
    cd_ack_irq(0x07u);

    cd_push_param(0x80u); /* 2x speed, 2048B */
    cd_send_cmd(0x0Eu);
    cdrom_event_cb(&g_sys, 0u);
    (void)cd_drain_resp(resp, 16u);
    cd_ack_irq(0x07u);

    u64 ra_hits_before = g_sys.stats.cd_ra_hits;
    cd_send_cmd(0x06u); /* ReadN */
    cdrom_event_cb(&g_sys, 0u);
    (void)cd_drain_resp(resp, 16u);
    cd_ack_irq(0x07u);
    cdrom_event_cb(&g_sys, 0u);
    CHECK_TRUE(cd_irq_flag() == 1u && g_sys.stats.cd_ra_hits == ra_hits_before + 1u,
               "ReadN(LBA 9 LZMA) -> INT1 with Core 2 SPSC readahead hit (0 Core 0 decode)");
    (void)cd_drain_resp(resp, 16u);

    cd_write_idx(0u);
    cdrom_mmio_write(&g_sys, PSX_REG_CDROM_REG3, 0x80u, 1u); /* BFRD */
    u32 dma_buf[600];
    CHECK_TRUE(cdrom_dma_read_words(&g_sys, dma_buf, 512u) == 512u, "DMA3 512 words LBA 9");
    u8 exp_sec[2352];
    bool dummy_ecc = false;
    gen_mode2_raw_sector(exp_sec, 9u, false, &dummy_ecc);
    CHECK_TRUE(memcmp(dma_buf, &exp_sec[24], 2048u) == 0,
               "DMA3 LBA 9 (cdlz LZMA) 2048B matches exact Mode 2 Form 1 payload");
    cd_ack_irq(0x07u);

    /* 5. ReadN + DMA3 2048B on Track 3 cooked MODE1 (LBA 50 -> 150+50=200 = 00:02:50) */
    cd_push_param(0x00u);
    cd_push_param(0x02u);
    cd_push_param(0x50u);
    cd_send_cmd(0x02u);
    cdrom_event_cb(&g_sys, 0u);
    (void)cd_drain_resp(resp, 16u);
    cd_ack_irq(0x07u);

    cd_send_cmd(0x06u); /* ReadN */
    cdrom_event_cb(&g_sys, 0u);
    (void)cd_drain_resp(resp, 16u);
    cd_ack_irq(0x07u);
    cdrom_event_cb(&g_sys, 0u);
    CHECK_TRUE(cd_irq_flag() == 1u, "ReadN(LBA 50 cooked MODE1) -> INT1");
    (void)cd_drain_resp(resp, 16u);

    cd_write_idx(0u);
    cdrom_mmio_write(&g_sys, PSX_REG_CDROM_REG3, 0x80u, 1u);
    CHECK_TRUE(cdrom_dma_read_words(&g_sys, dma_buf, 512u) == 512u, "DMA3 512 words LBA 50");
    u8 exp_m1[2048];
    gen_cooked_payload_2048(exp_m1, 0x30u, 50u - T3_START_LBA);
    CHECK_TRUE(memcmp(dma_buf, exp_m1, 2048u) == 0,
               "DMA3 cooked MODE1 extracts exact +16 payload via mode byte 0x01");
    cd_ack_irq(0x07u);

    /* 6. Whole-sector 2340B ReadS on Track 1 LBA 16 (cdzs Zstd hunk 2 -> 00:02:16) */
    cd_push_param(0xA0u);
    cd_send_cmd(0x0Eu);
    cdrom_event_cb(&g_sys, 0u);
    (void)cd_drain_resp(resp, 16u);
    cd_ack_irq(0x07u);

    cd_push_param(0x00u);
    cd_push_param(0x02u);
    cd_push_param(0x16u);
    cd_send_cmd(0x02u);
    cdrom_event_cb(&g_sys, 0u);
    (void)cd_drain_resp(resp, 16u);
    cd_ack_irq(0x07u);

    cd_send_cmd(0x1Bu); /* ReadS */
    cdrom_event_cb(&g_sys, 0u);
    (void)cd_drain_resp(resp, 16u);
    cd_ack_irq(0x07u);
    cdrom_event_cb(&g_sys, 0u);
    CHECK_TRUE(cd_irq_flag() == 1u, "ReadS(LBA 16 Zstd) -> INT1");
    (void)cd_drain_resp(resp, 16u);

    cd_write_idx(0u);
    cdrom_mmio_write(&g_sys, PSX_REG_CDROM_REG3, 0x80u, 1u);
    CHECK_TRUE(cdrom_dma_read_words(&g_sys, dma_buf, 585u) == 585u, "DMA3 585 words (2340B)");
    gen_mode2_raw_sector(exp_sec, 16u, false, &dummy_ecc);
    CHECK_TRUE(memcmp(dma_buf, &exp_sec[12], 2340u) == 0,
               "DMA3 2340B on cdzs Zstd sector matches BCD MSF + payload + reconstructed ECC");
    cd_ack_irq(0x07u);

    /* 7. CD-DA Play on FLAC-compressed Track 2 (Setloc 00:02:27 = LBA 27 = audio sec 4 in cdfl hunk 3!) */
    cd_push_param(0x00u);
    cd_send_cmd(0x0Eu); /* 1x CD-DA mode */
    cdrom_event_cb(&g_sys, 0u);
    (void)cd_drain_resp(resp, 16u);
    cd_ack_irq(0x07u);

    cd_push_param(0x00u);
    cd_push_param(0x02u);
    cd_push_param(0x27u); /* LBA 27 -> audio sector 4 inside cdfl FLAC hunk 3 */
    cd_send_cmd(0x02u);
    cdrom_event_cb(&g_sys, 0u);
    (void)cd_drain_resp(resp, 16u);
    cd_ack_irq(0x07u);

    /* Drain SPU xa_ring so we inspect freshly pushed CD-DA stereo frames */
    g_sys.spu.xa_r = g_sys.spu.xa_w;

    cd_send_cmd(0x03u); /* Play from setloc LBA 27 */
    cdrom_event_cb(&g_sys, 0u);
    CHECK_TRUE(cd_irq_flag() == 3u, "Play(cdfl LBA 27) -> INT3");
    (void)cd_drain_resp(resp, 16u);
    cd_ack_irq(0x07u);

    u32 xa_start = g_sys.spu.xa_w;
    cdrom_event_cb(&g_sys, 0u); /* Pushes 588 stereo frames from LBA 27 into SPU xa_ring */
    CHECK_TRUE((g_sys.spu.xa_w - xa_start) == 588u, "Play pushed 588 stereo CD-DA frames to SPU");

    /* Default cd->vol is L->L = 0x80 (1.0x), R->R = 0x80 (1.0x) -> exact waveform match! */
    for (u32 s = 0u; s < 588u; s++) {
        s16 exp_l = 0, exp_r = 0;
        gen_cdda_sample_pair(4u, s, &exp_l, &exp_r);
        u32 idx = (xa_start + s) & 4095u;
        s16 got_l = g_sys.spu.xa_ring[0][idx];
        s16 got_r = g_sys.spu.xa_ring[1][idx];
        if (got_l != exp_l || got_r != exp_r) {
            fprintf(stderr, "FLAC CD-DA SPU sample %u mismatch: got (%d,%d) exp (%d,%d)\n",
                    (unsigned)s, (int)got_l, (int)got_r, (int)exp_l, (int)exp_r);
            return 1;
        }
    }

    cdrom_shutdown(&g_sys);
    spu_shutdown(&g_sys);
    arena_destroy(&g_sys.arena);
    return 0;
}

/* ==========================================================================
 * Test 5: Multi-MB Per-Codec CHD v5 Decompression & LRU Cache MB/s Benchmark
 * ========================================================================== */
#define BENCH_HUNKS  96u
#define BENCH_FRAMES (BENCH_HUNKS * 8u) /* 768 sectors = 1.81 MB decoded sector stream/codec */

static int test_chd_codec_benchmarks(void) {
    static const struct {
        u8          comp;
        const char *name;
        bool        audio;
    } k_bench[4] = {
        { THUNK_CDZL, "cdzl(Deflate)", false },
        { THUNK_CDLZ, "cdlz(LZMA)",    false },
        { THUNK_CDZS, "cdzs(Zstd)",    false },
        { THUNK_CDFL, "cdfl(FLAC)",    true  },
    };

    u8 *frames = (u8 *)calloc(BENCH_FRAMES, 2448u);
    u8 *is_ecc = (u8 *)calloc(BENCH_FRAMES, 1u);
    u8 *hcomp  = (u8 *)malloc(BENCH_HUNKS);
    CHECK_TRUE(frames != NULL && is_ecc != NULL && hcomp != NULL, "alloc bench buffers");

    double mbs[4]    = { 0.0, 0.0, 0.0, 0.0 };
    double ratio[4]  = { 0.0, 0.0, 0.0, 0.0 };
    double cache_mbs = 0.0;
    u8     sec[2352];

    for (u32 c = 0u; c < 4u; c++) {
        memset(is_ecc, 0, BENCH_FRAMES);
        for (u32 s = 0u; s < BENCH_FRAMES; s++) {
            if (k_bench[c].audio) {
                gen_cdda_sector(&frames[(size_t)s * 2448u], s, true);
            } else {
                bool he = false;
                gen_mode2_raw_sector(&frames[(size_t)s * 2448u], s, false, &he);
                is_ecc[s] = he ? 1u : 0u;
            }
        }
        memset(hcomp, k_bench[c].comp, BENCH_HUNKS);

        char mstr[128];
        snprintf(mstr, sizeof(mstr),
                 "TRACK:1 TYPE:%s SUBTYPE:NONE FRAMES:%u PREGAP:0 PGTYPE: PGSUB:NONE POSTGAP:0",
                 k_bench[c].audio ? "AUDIO" : "MODE2_RAW", (unsigned)BENCH_FRAMES);
        TestMetaBlob mb = { CDROM_TRACK_METADATA2_TAG, (const u8 *)mstr, (u32)strlen(mstr) + 1u };

        u64 file_bytes = 0u;
        CHECK_TRUE(write_chd_v5_file(TEST_CHD_BENCH_PATH, frames, BENCH_FRAMES,
                                     is_ecc, hcomp, NULL, &mb, 1u, &file_bytes),
                   "write codec benchmark CHD");
        ratio[c] = (double)((u64)BENCH_FRAMES * 2448u) / (double)file_bytes;

        static CdImage img;
        cd_image_init(&img);
        CHECK_TRUE(cd_image_open(&img, TEST_CHD_BENCH_PATH) == PSX_OK, "open bench CHD");

        /*
         * Cold sequential scan across all 768 sectors (96 hunks > 8-slot cache, so pass 2
         * also evicts & re-decompresses ALL 96 hunks linearly -> 1536 sectors = 3.61 MB)!
         */
        u64 t0 = plat_time_us();
        u32 checksum = 0u;
        for (u32 pass = 0u; pass < 2u; pass++) {
            for (u32 lba = 0u; lba < BENCH_FRAMES; lba++) {
                if (!cd_image_read_sector(&img, lba, sec)) {
                    cd_image_close(&img);
                    free(frames); free(is_ecc); free(hcomp);
                    return 1;
                }
                checksum += sec[24];
            }
        }
        u64 dt = plat_time_us() - t0;
        if (dt == 0u) {
            dt = 1u;
        }
        double total_mb = (double)(2u * BENCH_FRAMES * 2352u) / (1024.0 * 1024.0);
        mbs[c] = total_mb / ((double)dt * 1e-6);

        u64 hits = 0u, misses = 0u;
        cd_chd_get_cache_stats(&img, &hits, &misses, NULL);
        /* 2 passes * 768 sectors = 1536 reads: 192 misses, 1344 hits = exact 87.50%! */
        CHECK_TRUE(hits * 8u >= (hits + misses) * 7u, "Sequential scan cache hit rate >= 87.5%");

        if (c == 1u) {
            /* Measure warm 8-slot LRU cache read bandwidth (64 sectors * 64 passes = 9.19 MB) */
            for (u32 lba = 0u; lba < 64u; lba++) {
                (void)cd_image_read_sector(&img, lba, sec);
            }
            u64 wt0 = plat_time_us();
            for (u32 p = 0u; p < 64u; p++) {
                for (u32 lba = 0u; lba < 64u; lba++) {
                    (void)cd_image_read_sector(&img, lba, sec);
                    checksum += sec[0];
                }
            }
            u64 wdt = plat_time_us() - wt0;
            if (wdt == 0u) {
                wdt = 1u;
            }
            double wmb = (double)(64u * 64u * 2352u) / (1024.0 * 1024.0);
            cache_mbs  = wmb / ((double)wdt * 1e-6);
        }
        if (checksum == 0xFFFFFFFFu) {
            printf("checksum=%u\n", (unsigned)checksum);
        }
        cd_image_close(&img);
    }

    printf("[CHD v5 bench] %s: %.1f MB/s (%.2fx) | %s: %.1f MB/s (%.2fx) | "
           "%s: %.1f MB/s (%.2fx) | %s: %.1f MB/s (%.2fx) | 8-slot cache warm: %.1f MB/s\n",
           k_bench[0].name, mbs[0], ratio[0],
           k_bench[1].name, mbs[1], ratio[1],
           k_bench[2].name, mbs[2], ratio[2],
           k_bench[3].name, mbs[3], ratio[3],
           cache_mbs);

    free(frames);
    free(is_ecc);
    free(hcomp);
    return 0;
}

int main(void) {
    cleanup_temp_files();

    int rc = build_main_chd_and_cue();
    if (rc == 0) {
        rc = test_chd_parity_and_padding();
    }
    if (rc == 0) {
        rc = test_chd_lru_hunk_cache();
    }
    if (rc == 0) {
        rc = test_chd_vpregap_legacy_and_errors();
    }
    if (rc == 0) {
        rc = test_psx_cdrom_chd_integration();
    }
    if (rc == 0) {
        rc = test_chd_codec_benchmarks();
    }

    cleanup_temp_files();
    if (rc != 0) {
        return rc;
    }
    printf("test_periph_chd PASS\n");
    return 0;
}
