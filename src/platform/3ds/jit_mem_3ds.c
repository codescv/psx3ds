#include <3ds.h>
#include <malloc.h>
#include <string.h>

#include "psx3ds/jit.h"
#include "psx3ds/jit_mem.h"
#include "psx3ds/platform.h"

/*
 * Nintendo 3DS ARM11 MPCore W^X / Executable Code Memory Provider
 * Strictly implements ARCHITECTURE.md §5.12 & docs/toolchain_env.md §6.1:
 *  - Opens real process handle via svcGetProcessId + svcOpenProcess (NEVER CUR_PROCESS_HANDLE)
 *  - Walks free VA window >= 0x04000000 via svcQueryMemory (never hardcodes heap 0x08000000)
 *  - Checks `rc == 0` STRICTLY (NEVER `R_SUCCEEDED(rc)`, which is falsely true in Azahar HLE!)
 *  - Never executes privileged CP15 `c7, c5, 0` in EL0; uses `svcFlushProcessDataCache` +
 *    EL0-safe DSB (`c7, c10, 4`) + FlushPrefetchBuffer (`c7, c5, 4`)
 */

static Handle g_jit_proc = 0;
static bool   g_jit_mapped_mirror = false;
static bool   g_jit_prot_rwx = false;
static bool   g_jit_allow_hle_exec = true;

void jit_mem_3ds_set_allow_hle_exec(bool allow) {
    g_jit_allow_hle_exec = allow;
}

static inline void ctr_el0_dsb_isb(void) {
    u32 zero = 0u;
    __asm__ volatile(
        "mcr p15, 0, %0, c7, c10, 4\n\t" /* Data Synchronization Barrier (EL0-safe) */
        "mcr p15, 0, %0, c7, c5, 4\n\t"  /* FlushPrefetchBuffer / ISB (EL0-safe) */
        :
        : "r"(zero)
        : "memory");
}

/*
 * 128 KiB (32,768 instructions) EL0-safe NOP eviction sled in .text.
 * 4x the 32 KiB ARM11 MPCore I-cache: deterministically evicts stale cache lines
 * on full JIT cache flushes when Luma SVC 0x94 is unavailable, without touching EL1.
 */
__attribute__((noinline, aligned(32)))
static void psx3ds_icache_evict_sled(void) {
    __asm__ volatile(
        ".rept 32768\n\t"
        "nop\n\t"
        ".endr\n\t"
        :
        :
        : "memory");
}

static u32 find_free_mirror_va(u32 size, uintptr_t rw_addr) {
    const u32 step = 0x00100000u; /* 1 MiB alignment */
    u32 va = 0x04000000u;
    const u32 va_limit = 0x14000000u;

    while (va < va_limit && (va_limit - va) >= size) {
        /* Avoid colliding with the backing RW buffer itself */
        if (!((uintptr_t)(va + size) <= rw_addr || (uintptr_t)va >= (rw_addr + (uintptr_t)size))) {
            va += step;
            continue;
        }
        MemInfo mi;
        PageInfo pi;
        memset(&mi, 0, sizeof(mi));
        memset(&pi, 0, sizeof(pi));
        Result qrc = svcQueryMemory(&mi, &pi, va);
        if (qrc != 0) {
            va += step;
            continue;
        }
        if (mi.state == MEMSTATE_FREE) {
            u32 region_end = mi.base_addr + mi.size;
            if (region_end > va && (region_end - va) >= size) {
                return va;
            }
        }
        u32 next_va = mi.base_addr + mi.size;
        next_va = (next_va + (step - 1u)) & ~(step - 1u);
        if (next_va <= va) {
            va += step;
        } else {
            va = next_va;
        }
    }
    return 0u;
}

static bool probe_hle_exec_buffer(u8 *probe_addr) {
    /*
     * Emit minimal 2-word ARM leaf `{ mov r0, #42 ; bx lr }` in the dedicated
     * trailing probe page (`rw + size - 64u`) — NEVER at `rw + 0` where arm_jit
     * emits its first PS1 block (otherwise Azahar's Dynarmic caches `{mov r0,#42; bx lr}`
     * at `rw + 0`!).
     */
    u32 *code = (u32 *)(void *)probe_addr;
    code[0] = 0xE3A0002Au; /* mov r0, #42 */
    code[1] = 0xE12FFF1Eu; /* bx lr */

    (void)svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)(uintptr_t)probe_addr, 64u);
    ctr_el0_dsb_isb();

    int (*probe_fn)(void) = (int (*)(void))(uintptr_t)probe_addr;
    int res = probe_fn();

    code[0] = 0u;
    code[1] = 0u;
    (void)svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)(uintptr_t)probe_addr, 64u);
    ctr_el0_dsb_isb();

    return (res == 42);
}

PsxResult jit_mem_alloc(JitMem *m, u32 size) {
    if (!m) {
        return PSX_ERR_BAD_ARG;
    }
    if (g_jit_proc != 0) {
        jit_mem_free(m);
    }

    size = (size < 0x2000u) ? 0x2000u : ((size + 0xFFFu) & ~0xFFFu);
    memset(m, 0, sizeof(*m));
    m->size = size;
    m->mode = JIT_MEM_NONE;

    /*
     * v0.2.3: 16 KiB alignment (>= 8 KiB per-way size of the 32 KiB 4-way ARM11
     * MPCore VIPT L1 caches) so `rw` and the 1 MiB-aligned `rx` mirror share the
     * same cache colour (VA bits [13:12]); otherwise an odd-4KiB `rw` aliases.
     */
    u8 *rw = (u8 *)memalign(0x4000u, size);
    if (!rw) {
        return PSX_ERR_NOMEM;
    }
    memset(rw, 0, size);
    m->rw = rw;

    /*
     * Probe Luma3DS vs Citra/Azahar HLE:
     * Note that Azahar 2126+ emulates Luma's svcGetSystemInfo(0x10000), returning 0,
     * while also exposing Citra's 0x20000 info type and leaving r0 == proc (> 0) when
     * SVC 0x70 is masked on .3dsx (real 3DS kernels ALWAYS return negative Result < 0 on error).
     */
    s64 luma_ver = 0;
    s64 citra_info = 0;
    bool has_luma_info = (svcGetSystemInfo(&luma_ver, 0x10000, 0) == 0);
    bool has_citra_info = (svcGetSystemInfo(&citra_info, 0x20000, 0) == 0);
    bool real_luma = has_luma_info && !has_citra_info;
    m->luma_icache_svc = false;
    m->is_hle_emu = !real_luma;

    u32 pid = 0u;
    Handle proc = 0;
    Result rc_prot = -1;
    bool have_proc = (svcGetProcessId(&pid, CUR_PROCESS_HANDLE) == 0) &&
                     (svcOpenProcess(&proc, pid) == 0) &&
                     (proc != 0);

    if (have_proc) {
        /* 1. Try W^X dual-mapped RW + RX mirror (preferred on Luma3DS / .cia) */
        u32 mirror_va = find_free_mirror_va(size, (uintptr_t)rw);
        if (mirror_va != 0u) {
            Result rc_map = svcControlProcessMemory(
                proc, mirror_va, (u32)(uintptr_t)rw, size,
                MEMOP_MAP, MEMPERM_READ | MEMPERM_EXECUTE);
            if (rc_map == 0) { /* MUST check == 0 strictly, NEVER R_SUCCEEDED! */
                MemInfo mi;
                PageInfo pi;
                memset(&mi, 0, sizeof(mi));
                memset(&pi, 0, sizeof(pi));
                if (svcQueryMemory(&mi, &pi, mirror_va) == 0 &&
                    mi.state != MEMSTATE_FREE &&
                    (mi.perm & MEMPERM_EXECUTE) != 0) {
                    m->rx = (u8 *)(uintptr_t)mirror_va;
                    m->mode = JIT_MEM_MIRROR_RWRX;
                    m->luma_icache_svc = real_luma;
                    g_jit_proc = proc;
                    g_jit_mapped_mirror = true;
                    g_jit_prot_rwx = false;
                    plat_log("[jit_mem_3ds] mode=MIRROR_RWRX rw=%p rx=%p size=%u luma=%d\n",
                             (void *)m->rw, (void *)m->rx, (unsigned)size, (int)real_luma);
                    return PSX_OK;
                }
                (void)svcControlProcessMemory(proc, mirror_va, (u32)(uintptr_t)rw,
                                              size, MEMOP_UNMAP, 0);
            }
        }

        /* 2. Fallback: in-place RWX protection via MEMOP_PROT */
        rc_prot = svcControlProcessMemory(
            proc, (u32)(uintptr_t)rw, 0u, size,
            MEMOP_PROT, MEMPERM_READ | MEMPERM_WRITE | MEMPERM_EXECUTE);
        if (rc_prot == 0) { /* MUST check == 0 strictly! */
            m->rx = rw;
            m->mode = JIT_MEM_RWX_INPLACE;
            m->luma_icache_svc = real_luma;
            g_jit_proc = proc;
            g_jit_mapped_mirror = false;
            g_jit_prot_rwx = true;
            plat_log("[jit_mem_3ds] mode=RWX_INPLACE(PROT) rw=%p size=%u luma=%d\n",
                     (void *)m->rw, (unsigned)size, (int)real_luma);
            return PSX_OK;
        }

        (void)svcCloseHandle(proc);
        proc = 0;
    }

    /*
     * 3. HLE emulator fallback (Azahar / Citra .3dsx without SVC 0x70):
     *    Attempted ONLY when we know we are inside an HLE emulator:
     *      - !real_luma (Citra/Azahar 0x20000 present or Luma 0x10000 absent), OR
     *      - rc_prot > 0 (masked HLE SVC 0x70 left positive proc handle in r0;
     *        real 3DS W^X kernels always return a negative Result < 0 on SVC failure).
     */
    bool is_hle_emu = (!real_luma) || (have_proc && rc_prot > 0);
    m->is_hle_emu = is_hle_emu;
    if (is_hle_emu && g_jit_allow_hle_exec && probe_hle_exec_buffer(rw + size - 64u)) {
        m->rx = rw;
        m->size = size - 0x1000u; /* Reserve trailing probe page; keep [rw, rw+m->size) virgin */
        m->mode = JIT_MEM_RWX_INPLACE;
        m->luma_icache_svc = false;
        g_jit_mapped_mirror = false;
        g_jit_prot_rwx = false;
        plat_log("[jit_mem_3ds] mode=RWX_INPLACE(HLE) rw=%p size=%u\n",
                 (void *)m->rw, (unsigned)m->size);
        return PSX_OK;
    }

    /*
     * 4. Safe fallback: keep m->rw allocated for CPU_CACHED block cache,
     *    set m->rx = NULL and m->mode = JIT_MEM_NONE.
     */
    m->rx = NULL;
    m->mode = JIT_MEM_NONE;
    m->luma_icache_svc = false;
    g_jit_mapped_mirror = false;
    g_jit_prot_rwx = false;
    plat_log("[jit_mem_3ds] mode=NONE(CPU_CACHED) rw=%p size=%u luma=%d\n",
             (void *)m->rw, (unsigned)size, (int)real_luma);
    return PSX_OK;
}

void jit_mem_free(JitMem *m) {
    if (!m) {
        return;
    }
    if (g_jit_proc != 0) {
        if (g_jit_mapped_mirror && m->rx && m->rw && m->size > 0u) {
            (void)svcControlProcessMemory(g_jit_proc,
                                          (u32)(uintptr_t)m->rx,
                                          (u32)(uintptr_t)m->rw,
                                          m->size, MEMOP_UNMAP, 0);
        } else if (g_jit_prot_rwx && m->rw && m->size > 0u) {
            (void)svcControlProcessMemory(g_jit_proc,
                                          (u32)(uintptr_t)m->rw,
                                          0u, m->size,
                                          MEMOP_PROT, MEMPERM_READ | MEMPERM_WRITE);
        }
        (void)svcCloseHandle(g_jit_proc);
        g_jit_proc = 0;
    }
    g_jit_mapped_mirror = false;
    g_jit_prot_rwx = false;

    if (m->rw) {
        free(m->rw);
    }
    memset(m, 0, sizeof(*m));
}

void jit_mem_commit_dcache(JitMem *m, const void *rw_begin, u32 bytes) {
    if (!m || !rw_begin || bytes == 0u || m->mode == JIT_MEM_NONE) {
        return;
    }
    /* 32-byte ARM11 cache-line aligned [start, end) in m->rw */
    uintptr_t start = (uintptr_t)rw_begin & ~(uintptr_t)31u;
    uintptr_t end = ((uintptr_t)rw_begin + (uintptr_t)bytes + 31u) & ~(uintptr_t)31u;
    u32 len = (u32)(end - start);

    /* Clean D-cache of the RW view (and the RX alias) out to FCRAM. */
    (void)svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)start, len);
    if (m->rx && m->rx != m->rw) {
        uintptr_t rx_start = (uintptr_t)m->rx + (start - (uintptr_t)m->rw);
        (void)svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)rx_start, len);
    }
    /* DSB: the clean is complete before any later I-cache invalidate / fetch. */
    ctr_el0_dsb_isb();
}

void jit_mem_flush_icache(JitMem *m) {
    if (!m || m->mode == JIT_MEM_NONE) {
        return;
    }
    /*
     * v0.2.3: invalidate the ARM11 L1 I-cache + BTB. Without this a freshly
     * emitted block could execute stale I-cache lines (e.g. zero words decoded
     * as `andeq r0, r0, r0`), skipping `blx`/`pop` and corrupting SP before
     * jit_exit (real New 3DS prefetch abort at PC = 0x08067E70).
     * Luma3DS svcInvalidateEntireInstructionCache (SVC 0x94) clobbers r0-r3, r12.
     * v0.2.5: called ONCE per jit_compile_at_pc (after all eagerly compiled blocks
     * were D-cache cleaned by jit_mem_commit_dcache), not once per block.
     */
    if (m->luma_icache_svc) {
        __asm__ volatile("svc 0x94\n\t" ::: "r0", "r1", "r2", "r3", "r12", "memory");
    } else if (!m->is_hle_emu) {
        psx3ds_icache_evict_sled();
    }
    /* DSB + ISB (EL0-safe) */
    ctr_el0_dsb_isb();
}

void jit_mem_commit(JitMem *m, const void *rw_begin, u32 bytes) {
    if (!m || !rw_begin || bytes == 0u || m->mode == JIT_MEM_NONE) {
        return;
    }
    jit_mem_commit_dcache(m, rw_begin, bytes);
    jit_mem_flush_icache(m);
}

void jit_mem_icache_flush_all(JitMem *m) {
    if (!m || m->mode == JIT_MEM_NONE || !m->rw || m->size == 0u) {
        return;
    }
    (void)svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)(uintptr_t)m->rw, m->size);
    if (m->luma_icache_svc) {
        __asm__ volatile("svc 0x94\n\t" ::: "r0", "r1", "r2", "r3", "r12", "memory");
    } else {
        psx3ds_icache_evict_sled();
    }
    ctr_el0_dsb_isb();
}
