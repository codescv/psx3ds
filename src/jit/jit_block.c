#include "jit_internal.h"

#include <string.h>

void jit_purge_ibtc(JitState *j) {
    if (!j || !j->ibtc) {
        return;
    }
    void *slow = (void *)jit_dispatch_slow;
    for (u32 i = 0u; i < JIT_IBTC_ENTRIES; ++i) {
        j->ibtc[i].tag = IBTC_EMPTY_TAG;
        j->ibtc[i].entry = slow;
    }
}

void jit_refresh_pins(PsxSystem *sys) {
    if (!sys || !sys->jit.tab_ram) {
        return;
    }
    JitState *j = &sys->jit;
    j->tab_ram[BLOCKTAB_RAM_IDX(0xA0u)] = (void *)jit_bios_vec_stub;
    j->tab_ram[BLOCKTAB_RAM_IDX(0xB0u)] = (void *)jit_bios_vec_stub;
    j->tab_ram[BLOCKTAB_RAM_IDX(0xC0u)] = (void *)jit_bios_vec_stub;

    bool psexe_armed = (sys->cpu.psexe_pending != 0u) || sys->bios.psexe_pending;
    u32 shell_idx = BLOCKTAB_RAM_IDX(PSX_SHELL_PC);
    if (psexe_armed) {
        if (j->tab_ram[shell_idx] != (void *)jit_psexe_stub) {
            if (j->tab_ram[shell_idx] != (void *)jit_compile_stub) {
                /* Kill any previously compiled block on page 0x30 before pinning */
                u32 shell_page = (PSX_SHELL_PC & PSX_RAM_MASK) >> 12;
                if (j->page_head[shell_page] != JIT_PAGE_NONE) {
                    /* Unlink page 0x30 blocks first; recursive refresh will set pin */
                    j->tab_ram[shell_idx] = (void *)jit_compile_stub;
                    jit_invalidate_page(sys, shell_page);
                }
            }
            j->tab_ram[shell_idx] = (void *)jit_psexe_stub;
            jit_purge_ibtc(j);
        }
    } else if (j->tab_ram[shell_idx] == (void *)jit_psexe_stub) {
        j->tab_ram[shell_idx] = (void *)jit_compile_stub;
    }
}

/* ============================================================================
 * Sub-page (64-byte line) SMC filter
 *
 * Soundness contract: a RAM write [w, w+len) invalidates nothing iff no live
 * block's guest instruction range (or statically peeked load-delay target word)
 * overlaps it. When >= 1 block on page p overlaps, the WHOLE page p is wiped via
 * jit_invalidate_page(), which also kills every direct-linked caller (arm_jit
 * only emits `B entry` when target pages are a subset of caller pages, so any
 * caller of a block living on p is itself on p).
 * ============================================================================ */

#define JIT_RAM_OFF_MASK ((u32)PSX_RAM_SIZE - 1u)

static inline u64 jit_line_mask(u32 lo, u32 hi) {
    u32 a = lo >> 6;
    u32 b = hi >> 6;
    u64 upper = (b >= 63u) ? ~0ULL : ((1ULL << (b + 1u)) - 1ULL);
    u64 lower = (1ULL << a) - 1ULL;
    return upper & ~lower;
}

/* Modular (2 MiB wrap-around) interval overlap; lengths must be < PSX_RAM_SIZE */
static inline bool jit_ram_overlap(u32 a, u32 alen, u32 b, u32 blen) {
    return (((b - a) & JIT_RAM_OFF_MASK) < alen) || (((a - b) & JIT_RAM_OFF_MASK) < blen);
}

static void jit_mask_mark(JitState *j, u32 phys, u32 bytes) {
    phys &= JIT_RAM_OFF_MASK;
    while (bytes > 0u) {
        u32 p = (phys >> 12) & (PSX_RAM_PAGES - 1u);
        u32 lo = phys & 0xFFFu;
        u32 chunk = 4096u - lo;
        if (chunk > bytes) {
            chunk = bytes;
        }
        j->page_code_mask[p] |= jit_line_mask(lo, lo + chunk - 1u);
        phys = (phys + chunk) & JIT_RAM_OFF_MASK;
        bytes -= chunk;
    }
}

static bool jit_peek_word(const PsxSystem *sys, u32 pc, u32 *out) {
    if (jit_pc_is_ram(sys, pc)) {
        if (!sys->bus.ram) {
            return false;
        }
        memcpy(out, sys->bus.ram + (pc & JIT_RAM_OFF_MASK), 4u);
        return true;
    }
    if (jit_pc_is_bios(pc)) {
        if (!sys->bus.bios) {
            return false;
        }
        memcpy(out, sys->bus.bios + ((pc & 0x1FFFFFFFu) - PSX_BIOS_PHYS), 4u);
        return true;
    }
    return false;
}

/* Guest byte length of the block's own instruction words (+4 for a peeked fallthrough) */
static inline u32 jit_block_code_len(const JitBlock *b) {
    u32 n = (b->n_insns != 0u) ? (u32)b->n_insns : 1u;
    u32 len = n * 4u;
    if ((b->flags & (JB_PEEKED_TARGET | JB_CACHED_OP)) == JB_PEEKED_TARGET) {
        len += 4u;
    }
    return len;
}

/*
 * Static branch target peeked by scan_check_ds_load (RAM only). The branch word
 * itself lies inside the block's own (trapped) range, so while the block is live
 * the word in RAM is exactly the one the compiler saw.
 */
static bool jit_block_peek_branch_target(const PsxSystem *sys, const JitBlock *b, u32 *out_phys) {
    if ((b->flags & (JB_PEEKED_TARGET | JB_CACHED_OP)) != JB_PEEKED_TARGET || b->n_insns < 2u) {
        return false;
    }
    u32 br_pc = b->pc + ((u32)b->n_insns - 2u) * 4u;
    u32 insn = 0u;
    if (!jit_peek_word(sys, br_pc, &insn)) {
        return false;
    }
    u32 op = insn >> 26;
    u32 t;
    if (op == 0x02u || op == 0x03u) {
        t = ((br_pc + 4u) & 0xF0000000u) | ((insn & 0x03FFFFFFu) << 2);
    } else if (op >= 0x01u && op <= 0x07u) {
        t = (br_pc + 4u) + ((u32)(s32)(s16)(insn & 0xFFFFu) << 2);
    } else {
        return false;
    }
    if (!jit_pc_is_ram(sys, t)) {
        return false;
    }
    *out_phys = t & JIT_RAM_OFF_MASK;
    return true;
}

void jit_block_mark_code(PsxSystem *sys, const JitBlock *b) {
    if (!sys || !b) {
        return;
    }
    JitState *j = &sys->jit;
    if (jit_pc_is_ram(sys, b->pc)) {
        jit_mask_mark(j, b->pc & JIT_RAM_OFF_MASK, jit_block_code_len(b));
    }
    u32 t = 0u;
    if (jit_block_peek_branch_target(sys, b, &t)) {
        jit_mask_mark(j, t, 4u);
    }
}

static bool jit_block_overlaps(const PsxSystem *sys, const JitBlock *b, u32 w, u32 wlen) {
    if (jit_pc_is_ram(sys, b->pc) &&
        jit_ram_overlap(b->pc & JIT_RAM_OFF_MASK, jit_block_code_len(b), w, wlen)) {
        return true;
    }
    u32 t = 0u;
    if (jit_block_peek_branch_target(sys, b, &t) && jit_ram_overlap(t, 4u, w, wlen)) {
        return true;
    }
    return false;
}

static bool jit_page_has_overlap(const PsxSystem *sys, u32 p, u32 w, u32 wlen) {
    const JitState *j = &sys->jit;
    u16 cur = j->page_head[p];
    while (cur != JIT_PAGE_NONE) {
        const JitBlock *b = &j->blocks[cur];
        if (jit_block_overlaps(sys, b, w, wlen)) {
            return true;
        }
        cur = b->next_in_page[(b->phys_page[0] == (u16)p) ? 0 : 1];
    }
    return false;
}

bool jit_block_add_ram_page(PsxSystem *sys, u16 block_idx, u16 phys_page) {
    if (!sys || phys_page >= PSX_RAM_PAGES) {
        return false;
    }
    JitState *j = &sys->jit;
    if (!j->blocks || block_idx >= j->max_blocks) {
        return false;
    }
    JitBlock *b = &j->blocks[block_idx];
    if (b->phys_page[0] == phys_page || b->phys_page[1] == phys_page) {
        return true;
    }
    int slot = -1;
    if (b->phys_page[0] == JIT_PAGE_NONE) {
        slot = 0;
    } else if (b->phys_page[1] == JIT_PAGE_NONE) {
        slot = 1;
    } else {
        return false;
    }

    b->phys_page[slot] = phys_page;
    b->next_in_page[slot] = j->page_head[phys_page];
    j->page_head[phys_page] = block_idx;
    jit_ref_code_page(&sys->bus, phys_page);
    return true;
}

void jit_init(PsxSystem *sys) {
    if (!sys || !sys->arena.base) {
        return;
    }
    JitState *j = &sys->jit;
    memset(j, 0, sizeof(*j));

    u32 cfg_blocks = sys->cfg.jit_max_blocks;
    if (cfg_blocks == 0u) {
        cfg_blocks = sys->cfg.small_mem ? 32768u : 65536u;
    }
    if (cfg_blocks > JIT_SAFE_MAX_BLOCKS) {
        cfg_blocks = JIT_SAFE_MAX_BLOCKS;
    }
    j->max_blocks = cfg_blocks;

    j->blocks = (JitBlock *)arena_alloc(
        &sys->arena, j->max_blocks * (u32)sizeof(JitBlock), 32u);
    j->tab_ram = (void **)arena_alloc(
        &sys->arena, BLOCKTAB_RAM_WORDS * (u32)sizeof(void *), 32u);
    j->tab_bios = (void **)arena_alloc(
        &sys->arena, BLOCKTAB_BIOS_WORDS * (u32)sizeof(void *), 32u);
    j->ibtc = (JitIbtcEntry *)arena_alloc(
        &sys->arena, JIT_IBTC_ENTRIES * (u32)sizeof(JitIbtcEntry), 32u);

    for (u32 p = 0u; p < PSX_RAM_PAGES; ++p) {
        j->page_head[p] = JIT_PAGE_NONE;
    }

    u32 code_bytes = sys->cfg.jit_code_bytes;
    if (code_bytes == 0u) {
        if (PSX3DS_PLATFORM_HOST) {
            code_bytes = 32u * 1024u * 1024u;
        } else {
            code_bytes = sys->cfg.small_mem ? (6u * 1024u * 1024u) : (16u * 1024u * 1024u);
        }
    }
    (void)jit_mem_alloc(&j->mem, code_bytes);
    j->code_ptr = j->mem.rw;
    j->code_end = j->mem.rw ? (j->mem.rw + j->mem.size) : NULL;

    sys->cpu.ibtc.p = j->ibtc;
    sys->cpu.blocktab_ram.p = j->tab_ram;
    sys->cpu.blocktab_bios.p = j->tab_bios;
    if (!sys->cpu.sys.p) {
        sys->cpu.sys.p = sys;
    }

    jit_flush_all(sys);
    sys->stats.jit_flushes = 0u;
    sys->stats.jit_mem_mode = (u32)j->mem.mode;
}

void jit_reset(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    JitState *j = &sys->jit;
    if (!j->tab_ram || !j->tab_bios || !j->ibtc) {
        return;
    }
    sys->cpu.ibtc.p = j->ibtc;
    sys->cpu.blocktab_ram.p = j->tab_ram;
    sys->cpu.blocktab_bios.p = j->tab_bios;
    if (!sys->cpu.sys.p) {
        sys->cpu.sys.p = sys;
    }
    jit_flush_all(sys);
    sys->stats.jit_flushes = 0u;
}

void jit_state(PsxSystem *sys, StateIO *st) {
    if (!sys || !st) {
        return;
    }
    u32 tag = 1u;
    state_u32(st, &tag);
    if (!st->writing) {
        jit_flush_all(sys);
    }
}

void jit_shutdown(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    JitState *j = &sys->jit;
    jit_mem_free(&j->mem);
    j->code_ptr = NULL;
    j->code_end = NULL;
}

void jit_flush_all(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    JitState *j = &sys->jit;
    if (!j->tab_ram || !j->tab_bios || !j->ibtc) {
        return;
    }

    sys->cpu.ibtc.p = j->ibtc;
    sys->cpu.blocktab_ram.p = j->tab_ram;
    sys->cpu.blocktab_bios.p = j->tab_bios;
    if (!sys->cpu.sys.p) {
        sys->cpu.sys.p = sys;
    }

    for (u32 p = 0u; p < PSX_RAM_PAGES; ++p) {
        u32 refs = sys->bus.code_page_refs[p];
        for (u32 r = 0u; r < refs; ++r) {
            jit_unref_code_page(&sys->bus, p);
        }
        sys->bus.code_page_refs[p] = 0u;
        j->page_head[p] = JIT_PAGE_NONE;
        j->page_code_mask[p] = 0ULL;
    }

    j->n_blocks = 0u;
    if (j->mem.mode == JIT_MEM_RWX_INPLACE && !j->mem.luma_icache_svc &&
        j->code_ptr != NULL && j->code_end > j->code_ptr) {
        u8 *aligned = (u8 *)(((uintptr_t)j->code_ptr + 31u) & ~(uintptr_t)31u);
        if (j->code_end > aligned && (size_t)(j->code_end - aligned) >= 0x100000u) {
            j->code_ptr = aligned;
        } else {
            j->code_ptr = j->mem.rw;
        }
    } else {
        j->code_ptr = j->mem.rw;
    }
    j->generation++;

    void *stub = (void *)jit_compile_stub;
    for (u32 i = 0u; i < BLOCKTAB_RAM_WORDS; ++i) {
        j->tab_ram[i] = stub;
    }
    for (u32 i = 0u; i < BLOCKTAB_BIOS_WORDS; ++i) {
        j->tab_bios[i] = stub;
    }

    jit_refresh_pins(sys);
    jit_purge_ibtc(j);

    if (sys->cpu.in_jit != 0u) {
        jit_mem_icache_flush_all(&j->mem);
        sys->cpu.icache_dirty = 0u;
    } else {
        sys->cpu.icache_dirty = 1u;
    }

    sys->stats.jit_blocks_live = 0u;
    sys->stats.jit_code_bytes = (j->mem.rw && j->code_ptr >= j->mem.rw)
                                    ? (u32)(j->code_ptr - j->mem.rw)
                                    : 0u;
    sys->stats.jit_flushes++;
    sys->stats.jit_mem_mode = (u32)j->mem.mode;
}

void jit_invalidate_page(PsxSystem *sys, u32 phys_page) {
    if (!sys || phys_page >= PSX_RAM_PAGES) {
        return;
    }
    JitState *j = &sys->jit;
    if (!j->blocks || !j->tab_ram) {
        return;
    }

    u16 cur = j->page_head[phys_page];
    if (cur == JIT_PAGE_NONE) {
        return;
    }

    void *stub = (void *)jit_compile_stub;
    while (cur != JIT_PAGE_NONE) {
        JitBlock *b = &j->blocks[cur];
        int slot = (b->phys_page[0] == (u16)phys_page) ? 0 : 1;
        u16 next = b->next_in_page[slot];

        if (jit_pc_is_bios(b->pc) && j->tab_bios) {
            /* BIOS block registered on this RAM page via a peeked load-delay target */
            j->tab_bios[BLOCKTAB_BIOS_IDX(b->pc)] = stub;
        } else {
            j->tab_ram[BLOCKTAB_RAM_IDX(b->pc)] = stub;
        }

        if ((b->flags & JB_DEAD) == 0u) {
            b->flags |= JB_DEAD;
            if (sys->stats.jit_blocks_live > 0u) {
                sys->stats.jit_blocks_live--;
            }
            sys->stats.smc_invalidations++;
        }

        int other = 1 - slot;
        u16 other_page = b->phys_page[other];
        if (other_page != JIT_PAGE_NONE && other_page < PSX_RAM_PAGES) {
            u16 *pp = &j->page_head[other_page];
            while (*pp != JIT_PAGE_NONE) {
                if (*pp == cur) {
                    *pp = b->next_in_page[other];
                    break;
                }
                pp = &j->blocks[*pp].next_in_page[
                    (j->blocks[*pp].phys_page[0] == other_page) ? 0 : 1];
            }
            jit_unref_code_page(&sys->bus, other_page);
        }

        jit_unref_code_page(&sys->bus, phys_page);

        b->phys_page[0] = JIT_PAGE_NONE;
        b->phys_page[1] = JIT_PAGE_NONE;
        b->next_in_page[0] = JIT_PAGE_NONE;
        b->next_in_page[1] = JIT_PAGE_NONE;

        cur = next;
    }

    j->page_head[phys_page] = JIT_PAGE_NONE;
    j->page_code_mask[phys_page] = 0ULL;
    jit_refresh_pins(sys);
    jit_purge_ibtc(j);
    jit_request_exit_wrap(sys);
}

void jit_invalidate_range(PsxSystem *sys, u32 phys, u32 bytes) {
    if (!sys || bytes == 0u) {
        return;
    }
    JitState *j = &sys->jit;
    if (!j->blocks || !j->tab_ram) {
        return;
    }

    if (bytes >= PSX_RAM_SIZE) {
        for (u32 p = 0u; p < PSX_RAM_PAGES; ++p) {
            if (j->page_head[p] != JIT_PAGE_NONE) {
                jit_invalidate_page(sys, p);
            }
        }
        return;
    }

    u32 cur = phys & JIT_RAM_OFF_MASK;
    u32 remaining = bytes;
    while (remaining > 0u) {
        u32 p = (cur >> 12) & (PSX_RAM_PAGES - 1u);
        u32 lo = cur & 0xFFFu;
        u32 chunk = 4096u - lo;
        if (chunk > remaining) {
            chunk = remaining;
        }
        /* O(1) reject: page holds no code, or no code line touched by this write */
        if (j->page_head[p] != JIT_PAGE_NONE &&
            (j->page_code_mask[p] & jit_line_mask(lo, lo + chunk - 1u)) != 0ULL &&
            jit_page_has_overlap(sys, p, cur, chunk)) {
            jit_invalidate_page(sys, p);
        }
        cur = (cur + chunk) & JIT_RAM_OFF_MASK;
        remaining -= chunk;
    }
}

#if !PSX3DS_HAVE_JIT
char jit_dispatch[1];
char jit_dispatch_slow[1];
char jit_compile_stub[1];
char jit_bios_vec_stub[1];
char jit_psexe_stub[1];
char jit_exit[1];

void jit_run(PsxSystem *sys) {
    cpu_cached_run(sys);
}

void *jit_lookup_slow(PsxCpu *cpu, u32 pc) {
    (void)cpu;
    (void)pc;
    return (void *)jit_exit;
}

void *jit_compile_at_pc(PsxCpu *cpu) {
    (void)cpu;
    return (void *)jit_exit;
}

int jit_enter(PsxCpu *cpu) {
    (void)cpu;
    return PSX_EXIT_EVENTS;
}
#endif
