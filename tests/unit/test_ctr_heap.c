/*
 * Regression test for the v0.2.0 New 3DS startup crash (svcBreak(USERBREAK_PANIC) in
 * libctru __system_allocateHeaps): verifies the startup heap split used by the
 * __system_allocateHeaps override in src/platform/3ds/main_3ds.c across every
 * Old/New 3DS RESLIMIT_COMMIT configuration. Pure arithmetic: runs on T1/T2/T3.
 */
#include "psx3ds/ctr_heap.h"

#include <stdio.h>

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            fprintf(stderr, "FAIL [%s:%d] %s: %s\n", __FILE__, __LINE__, name, (msg)); \
            return 1;                                                               \
        }                                                                           \
    } while (0)

/* libctru (as linked by v0.2.0) __system_allocateHeaps sizing, reproduced from disassembly. */
static void libctru_stock_split(u32 remaining, u32 heap_in, u32 linear_in, u32 *heap_out,
                                u32 *linear_out) {
    u32 heap = heap_in;
    u32 linear = linear_in;
    if (heap == 0u) {
        if (linear != 0u) {
            heap = remaining - linear; /* uncapped branch hit by v0.2.0 */
        } else {
            linear = (remaining / 2u) & ~0xFFFu;
            heap = remaining - linear;
            if (heap > PSX_CTR_MIB(24)) {
                heap = PSX_CTR_MIB(24);
                linear = remaining - heap;
                if (linear > PSX_CTR_MIB(32)) {
                    linear = PSX_CTR_MIB(32);
                    heap = remaining - linear;
                }
            }
        }
    } else if (linear == 0u) {
        linear = remaining - heap;
    }
    *heap_out = heap;
    *linear_out = linear;
}

typedef struct HeapCase {
    const char *name;
    s64 limit;
    s64 current;
    u32 expect_heap;   /* 0 = don't check exact value */
    u32 expect_linear; /* 0 = don't check exact value */
} HeapCase;

static int check_case(const HeapCase *c) {
    const char *name = c->name;
    PsxCtrHeapSplit s;
    CHECK(psx_ctr_compute_heap_split(c->limit, c->current, &s), "split must succeed");

    u32 remaining = (u32)((c->limit - c->current) & ~(s64)0xFFF);
    CHECK(s.remaining == remaining, "remaining page-aligned commit");
    CHECK((s.heap_size & 0xFFFu) == 0u, "heap 4 KiB aligned");
    CHECK((s.linear_size & 0xFFFu) == 0u, "linear 4 KiB aligned");
    CHECK((u64)s.heap_size + (u64)s.linear_size <= (u64)s.remaining,
          "heap + linear never exceeds RESLIMIT_COMMIT remaining");

    /* Core regression guarantees */
    CHECK(s.heap_size >= PSX_CTR_HEAP_MIN_BYTES, "normal heap >= 36 MiB");
    CHECK(s.heap_size <= PSX_CTR_HEAP_MAX_BYTES, "normal heap <= 80 MiB");
    CHECK(s.heap_size < PSX_CTR_HEAP_AREA_BYTES, "normal heap < 96 MiB VA window");
    CHECK(psx_ctr_heap_fits_va(s.heap_size), "heap fits VA");
    CHECK((u64)PSX_CTR_HEAP_AREA_BEGIN + s.heap_size <= (u64)PSX_CTR_HEAP_AREA_END,
          "0x08000000 + heap <= 0x0E000000");
    CHECK(s.linear_size >= PSX_CTR_LINEAR_MIN_BYTES, "linear heap >= 20 MiB");
    CHECK(s.linear_size <= PSX_CTR_LINEAR_AREA_BYTES, "linear heap <= 128 MiB VA window");

    if (c->expect_heap) {
        CHECK(s.heap_size == c->expect_heap, "exact heap size");
    }
    if (c->expect_linear) {
        CHECK(s.linear_size == c->expect_linear, "exact linear size");
    }
    printf("  %-34s limit=%3u MiB heap=%9.4f MiB linear=%9.4f MiB end=0x%08X\n", name,
           (unsigned)(c->limit >> 20), (double)s.heap_size / 1048576.0,
           (double)s.linear_size / 1048576.0,
           (unsigned)(PSX_CTR_HEAP_AREA_BEGIN + s.heap_size));
    return 0;
}

int main(void) {
    /* User crash dump: N3DS limit 0x07C00000, current 0x0016C000 -> R3 = 0x06694000. */
    const s64 user_current = 0x0016C000;
    const u32 user_remaining = 0x07C00000u - 0x0016C000u;

    const HeapCase cases[] = {
        /* Old 3DS 64 MiB (.cia / .cci SystemMode 64MB) */
        {"o3ds_64_cia", PSX_CTR_MIB(64), user_current, PSX_CTR_MIB(64) - 0x16C000u - PSX_CTR_MIB(20),
         PSX_CTR_MIB(20)},
        /* Old 3DS 64 MiB .3dsx under hbmenu (larger loader footprint) */
        {"o3ds_64_3dsx", PSX_CTR_MIB(64), PSX_CTR_MIB(4), PSX_CTR_MIB(40), PSX_CTR_MIB(20)},
        {"o3ds_64_3dsx_8mib_footprint", PSX_CTR_MIB(64), PSX_CTR_MIB(8), PSX_CTR_MIB(36),
         PSX_CTR_MIB(20)},
        /* Old 3DS 80 MiB extended-memory .3dsx */
        {"o3ds_80_3dsx", PSX_CTR_MIB(80), user_current, PSX_CTR_MIB(80) - 0x16C000u - PSX_CTR_MIB(20),
         PSX_CTR_MIB(20)},
        {"o3ds_96_dev", PSX_CTR_MIB(96), user_current, 0u, PSX_CTR_MIB(20)},
        /* New 3DS 124 MiB (.cia / .cci / .3dsx) — the user's crash configuration */
        {"n3ds_124_cia_user_dump", 0x07C00000, user_current, PSX_CTR_MIB(80),
         user_remaining - PSX_CTR_MIB(80)},
        {"n3ds_124_3dsx", PSX_CTR_MIB(124), PSX_CTR_MIB(4), PSX_CTR_MIB(80), PSX_CTR_MIB(40)},
        /* New 3DS 178 MiB extended-memory .3dsx */
        {"n3ds_178_3dsx", PSX_CTR_MIB(178), user_current, PSX_CTR_MIB(80),
         PSX_CTR_MIB(178) - 0x16C000u - PSX_CTR_MIB(80)},
        {"n3ds_178_3dsx_4mib_footprint", PSX_CTR_MIB(178), PSX_CTR_MIB(4), PSX_CTR_MIB(80),
         PSX_CTR_MIB(94)},
    };

    printf("test_ctr_heap: 3DS startup heap split\n");
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (check_case(&cases[i]) != 0) {
            return 1;
        }
    }

    /* Prove the v0.2.0 bug: stock libctru split with linear=20 MiB, heap=0 on N3DS. */
    {
        const char *name = "libctru_v020_regression";
        u32 heap = 0u, linear = 0u;
        libctru_stock_split(user_remaining, 0u, PSX_CTR_MIB(20), &heap, &linear);
        CHECK(heap == 0x06694000u, "stock libctru reproduces crash R3 = 0x06694000");
        CHECK(!psx_ctr_heap_fits_va(heap), "stock split overflows 0x0E000000");

        PsxCtrHeapSplit s;
        CHECK(psx_ctr_compute_heap_split(0x07C00000, user_current, &s), "fixed split");
        CHECK(s.heap_size != 0x06694000u && psx_ctr_heap_fits_va(s.heap_size),
              "fixed split stays inside VA window");

        /* libctru default (both zero) on N3DS 124 fits, but on 178 MiB it would overflow too. */
        libctru_stock_split(user_remaining, 0u, 0u, &heap, &linear);
        CHECK(psx_ctr_heap_fits_va(heap), "libctru default fits on 124 MiB");
        libctru_stock_split(PSX_CTR_MIB(178) - 0x16C000u, 0u, 0u, &heap, &linear);
        CHECK(!psx_ctr_heap_fits_va(heap), "libctru default overflows on 178 MiB");
    }

    /* Small-mem profile threshold: O3DS -> small, N3DS -> full. */
    {
        const char *name = "profile_threshold";
        PsxCtrHeapSplit o3ds, o3ds80, n3ds;
        CHECK(psx_ctr_compute_heap_split(PSX_CTR_MIB(64), user_current, &o3ds), "o3ds");
        CHECK(psx_ctr_compute_heap_split(PSX_CTR_MIB(80), user_current, &o3ds80), "o3ds80");
        CHECK(psx_ctr_compute_heap_split(PSX_CTR_MIB(124), user_current, &n3ds), "n3ds");
        CHECK(o3ds.heap_size < PSX_CTR_HEAP_FULL_PROFILE_BYTES, "O3DS 64 -> small_mem");
        CHECK(o3ds80.heap_size < PSX_CTR_HEAP_FULL_PROFILE_BYTES, "O3DS 80 -> small_mem");
        CHECK(n3ds.heap_size >= PSX_CTR_HEAP_FULL_PROFILE_BYTES, "N3DS 124 -> full profile");
        /* Full profile: arena 41 MiB + JIT 16 MiB must fit with >= 16 MiB headroom. */
        CHECK(n3ds.heap_size >= PSX_CTR_MIB(41 + 16 + 16), "N3DS full profile headroom");
        /* Small profile: arena 26 MiB + JIT 6 MiB with >= 4 MiB headroom at 36 MiB floor. */
        CHECK(PSX_CTR_HEAP_MIN_BYTES >= PSX_CTR_MIB(26 + 6 + 4), "O3DS small profile headroom");
    }

    /* Degenerate / tiny envelopes never panic arithmetic-wise and never exceed caps. */
    {
        const char *name = "edge_cases";
        PsxCtrHeapSplit s;
        CHECK(!psx_ctr_compute_heap_split(0, 0, &s), "zero limit fails");
        CHECK(!psx_ctr_compute_heap_split(PSX_CTR_MIB(64), PSX_CTR_MIB(64), &s), "exhausted fails");
        CHECK(!psx_ctr_compute_heap_split(PSX_CTR_MIB(64), PSX_CTR_MIB(65), &s), "over fails");
        CHECK(!psx_ctr_compute_heap_split(PSX_CTR_MIB(64), -1, &s), "negative current fails");
        CHECK(!psx_ctr_compute_heap_split(PSX_CTR_MIB(64), 0, NULL), "null out fails");
        CHECK(psx_ctr_compute_heap_split(PSX_CTR_MIB(32), 0, &s), "32 MiB applet envelope");
        CHECK(s.heap_size + s.linear_size <= s.remaining && s.linear_size > 0u, "applet split sane");
        CHECK(s.heap_size > s.linear_size, "applet split favors normal heap");
        CHECK(psx_ctr_compute_heap_split((s64)4 << 30, 0, &s), "4 GiB absurd limit");
        CHECK(s.heap_size == PSX_CTR_HEAP_MAX_BYTES, "absurd limit heap capped");
        CHECK(s.linear_size == PSX_CTR_LINEAR_AREA_BYTES, "absurd limit linear capped");
        for (u32 mib = 1u; mib <= 256u; mib++) {
            if (!psx_ctr_compute_heap_split(PSX_CTR_MIB(mib), 0x16C000, &s)) {
                continue;
            }
            CHECK(psx_ctr_heap_fits_va(s.heap_size), "sweep: heap fits VA");
            CHECK(s.heap_size <= PSX_CTR_HEAP_MAX_BYTES, "sweep: heap <= 80 MiB");
            CHECK((u64)s.heap_size + s.linear_size <= s.remaining, "sweep: within commit");
            if (mib >= 64u) {
                CHECK(s.heap_size >= PSX_CTR_HEAP_MIN_BYTES, "sweep: heap >= 36 MiB");
                CHECK(s.linear_size >= PSX_CTR_LINEAR_MIN_BYTES, "sweep: linear >= 20 MiB");
            }
        }
    }

    printf("test_ctr_heap: OK\n");
    return 0;
}
