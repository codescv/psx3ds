#include "psx3ds/bus.h"

#include <string.h>

/*
 * Computes the mapped physical RAM window (in bytes) inside [0, 0x00800000).
 * Default (!strict_ram_size): full 8 MiB window (4x 2 MiB mirrors).
 * Strict mode: honors RAM_SIZE (0x1F801060) bits 11:9 per PSX hardware decode.
 */
static u32 fastmem_ram_window_bytes(const PsxBus *b) {
    if (!b->strict_ram_size) {
        return PSX_RAM_MIRROR_END;
    }
    switch ((b->ram_size_reg >> 9) & 7u) {
        case 0u: return 0x00100000u; /* 1 MiB */
        case 1u: return 0x00400000u; /* 4 MiB */
        case 2u: return 0x00100000u; /* 1 MiB (+ HighZ) */
        case 3u: return 0x00400000u; /* 4 MiB (+ HighZ) */
        case 4u: return 0x00200000u; /* 2 MiB */
        case 5u: return 0x00200000u; /* 2 MiB (+ HighZ, 0x0B88 default) */
        case 6u: return 0x00400000u; /* 4 MiB (2x 2 MiB) */
        case 7u: return 0x00800000u; /* 8 MiB (4x 2 MiB) */
        default: return 0x00200000u;
    }
}

/*
 * Modular uintptr_t bias: host_addr_of_page_start - guest_addr_of_page_start.
 * Dereference at runtime is `*(T *)(bias + (uintptr_t)guest_u32_addr)`.
 * Never uses u8* pointer arithmetic (which would be UB in C).
 */
PSX_INLINE uintptr_t fm_bias_vp(const void *host_base, u32 host_off, u32 vp) {
    return (uintptr_t)host_base + (uintptr_t)host_off - ((uintptr_t)vp << PSX_FM_SHIFT);
}

PSX_INLINE bool fm_ram_page_writable(const PsxBus *b, u32 p, u32 window_pages) {
    if (p >= window_pages || b->isolated) {
        return false;
    }
    u32 phys_page = p & (PSX_RAM_PAGES - 1u);
    if (b->code_page_refs[phys_page] != 0u &&
        b->code_page_mode[phys_page] == (u8)PSX_PAGE_TRAP) {
        return false;
    }
    return true;
}

static void fastmem_refresh_phys_page(PsxBus *b, u32 phys_page) {
    static const u32 seg_vp[3] = { 0x00000u, 0x80000u, 0xA0000u };
    u32 window_pages = fastmem_ram_window_bytes(b) >> PSX_FM_SHIFT;
    u32 host_off = phys_page << PSX_FM_SHIFT;

    for (u32 s = 0u; s < 3u; s++) {
        for (u32 m = 0u; m < 4u; m++) {
            u32 p = (m * PSX_RAM_PAGES) + phys_page;
            u32 vp = seg_vp[s] + p;
            b->fm_w[vp] = fm_ram_page_writable(b, p, window_pages)
                              ? fm_bias_vp(b->ram, host_off, vp)
                              : 0u;
        }
    }
}

void fastmem_build(PsxBus *b) {
    if (!b || !b->fm_r || !b->fm_w) {
        return;
    }
    memset(b->fm_r, 0, PSX_FM_BYTES);
    memset(b->fm_w, 0, PSX_FM_BYTES);

    static const u32 seg_vp[3] = { 0x00000u, 0x80000u, 0xA0000u };
    u32 window_pages = fastmem_ram_window_bytes(b) >> PSX_FM_SHIFT;
    u32 bios_pages = PSX_BIOS_SIZE >> PSX_FM_SHIFT; /* 128 pages (512 KiB) */

    for (u32 s = 0u; s < 3u; s++) {
        /* RAM + mirrors (up to 2048 pages = 8 MiB per segment) */
        for (u32 p = 0u; p < window_pages; p++) {
            u32 vp = seg_vp[s] + p;
            u32 host_off = (p & (PSX_RAM_PAGES - 1u)) << PSX_FM_SHIFT;
            uintptr_t bias = fm_bias_vp(b->ram, host_off, vp);
            b->fm_r[vp] = bias;
            b->fm_w[vp] = fm_ram_page_writable(b, p, window_pages) ? bias : 0u;
        }

        /* BIOS ROM (512 KiB at 0x1FC00000, read-only across KUSEG/KSEG0/KSEG1) */
        for (u32 p = 0u; p < bios_pages; p++) {
            u32 vp = seg_vp[s] + (PSX_BIOS_PHYS >> PSX_FM_SHIFT) + p;
            b->fm_r[vp] = fm_bias_vp(b->bios, p << PSX_FM_SHIFT, vp);
            b->fm_w[vp] = 0u;
        }
    }

    /* Scratchpad (4 KiB backing page): ONLY KUSEG (0x1F800) and KSEG0 (0x9F800).
     * KSEG1 (0xBF800 / 0xBF800000) MUST remain 0 (unmapped -> DBE on real HW). */
    if (b->scratch_page) {
        uintptr_t bias_kuseg = fm_bias_vp(b->scratch_page, 0u, 0x1F800u);
        uintptr_t bias_kseg0 = fm_bias_vp(b->scratch_page, 0u, 0x9F800u);
        b->fm_r[0x1F800u] = bias_kuseg;
        b->fm_r[0x9F800u] = bias_kseg0;
        b->fm_w[0x1F800u] = b->isolated ? 0u : bias_kuseg;
        b->fm_w[0x9F800u] = b->isolated ? 0u : bias_kseg0;
    }
}

void fastmem_set_isolated(PsxBus *b, bool isc) {
    if (!b || !b->fm_w) {
        return;
    }
    b->isolated = isc;

    /* Rewrite ONLY the 6,146 writable entries (3 * 2048 RAM pages + 2 scratch pages),
     * preserving code_page_refs SMC write traps and strict_ram_size windowing. */
    static const u32 seg_vp[3] = { 0x00000u, 0x80000u, 0xA0000u };
    u32 window_pages = fastmem_ram_window_bytes(b) >> PSX_FM_SHIFT;

    for (u32 s = 0u; s < 3u; s++) {
        for (u32 p = 0u; p < 2048u; p++) {
            u32 vp = seg_vp[s] + p;
            if (fm_ram_page_writable(b, p, window_pages)) {
                u32 host_off = (p & (PSX_RAM_PAGES - 1u)) << PSX_FM_SHIFT;
                b->fm_w[vp] = fm_bias_vp(b->ram, host_off, vp);
            } else {
                b->fm_w[vp] = 0u;
            }
        }
    }

    if (b->scratch_page) {
        b->fm_w[0x1F800u] = isc ? 0u : fm_bias_vp(b->scratch_page, 0u, 0x1F800u);
        b->fm_w[0x9F800u] = isc ? 0u : fm_bias_vp(b->scratch_page, 0u, 0x9F800u);
    }
}

void bus_code_page_ref(PsxBus *b, u32 phys_page) {
    if (!b || phys_page >= PSX_RAM_PAGES) {
        return;
    }
    u16 prev = b->code_page_refs[phys_page];
    if (prev < UINT16_MAX) {
        b->code_page_refs[phys_page] = (u16)(prev + 1u);
    }
    if (prev == 0u) {
        b->code_page_mode[phys_page] = (u8)PSX_PAGE_TRAP;
        b->code_page_traps[phys_page] = 0u;
        if (b->fm_w) {
            fastmem_refresh_phys_page(b, phys_page);
        }
    }
}

void bus_code_page_unref(PsxBus *b, u32 phys_page) {
    if (!b || phys_page >= PSX_RAM_PAGES) {
        return;
    }
    u16 prev = b->code_page_refs[phys_page];
    if (prev == 0u) {
        return;
    }
    b->code_page_refs[phys_page] = (u16)(prev - 1u);
    if (prev == 1u) {
        b->code_page_mode[phys_page] = (u8)PSX_PAGE_TRAP;
        b->code_page_traps[phys_page] = 0u;
        if (b->fm_w) {
            fastmem_refresh_phys_page(b, phys_page);
        }
    }
}
