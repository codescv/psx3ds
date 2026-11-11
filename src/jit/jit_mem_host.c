#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "psx3ds/jit.h"
#include "psx3ds/jit_mem.h"
#include "psx3ds/config.h"

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

PsxResult jit_mem_alloc(JitMem *m, u32 size) {
    if (!m || size == 0u) {
        return PSX_ERR_BAD_ARG;
    }

    u32 aligned_size = (size + 4095u) & ~4095u;
    m->rw = NULL;
    m->rx = NULL;
    m->size = 0u;
    m->mode = JIT_MEM_NONE;
    m->luma_icache_svc = false;

#if PSX3DS_HAVE_JIT
    void *mapped = mmap(NULL, (size_t)aligned_size,
                        PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped != MAP_FAILED) {
        m->rw = (u8 *)mapped;
        m->rx = (u8 *)mapped;
        m->size = aligned_size;
        m->mode = JIT_MEM_HOST_MMAP;
        return PSX_OK;
    }
#endif

    void *rw_buf = NULL;
    if (posix_memalign(&rw_buf, 4096u, (size_t)aligned_size) != 0 || !rw_buf) {
        return PSX_ERR_NOMEM;
    }
    memset(rw_buf, 0, (size_t)aligned_size);
    m->rw = (u8 *)rw_buf;
    m->rx = NULL;
    m->size = aligned_size;
    m->mode = JIT_MEM_NONE;
    return PSX_OK;
}

void jit_mem_free(JitMem *m) {
    if (!m) {
        return;
    }
    if (m->rw) {
        if (m->mode == JIT_MEM_HOST_MMAP) {
            munmap(m->rw, (size_t)m->size);
        } else {
            free(m->rw);
        }
    }
    m->rw = NULL;
    m->rx = NULL;
    m->size = 0u;
    m->mode = JIT_MEM_NONE;
    m->luma_icache_svc = false;
}

void jit_mem_commit_dcache(JitMem *m, const void *rw_begin, u32 bytes) {
#if PSX3DS_HAVE_JIT
    if (!m || !m->rx || !m->rw || !rw_begin || bytes == 0u) {
        return;
    }
    /*
     * Hosts have no cheap "invalidate entire I-cache" from user space, so the
     * ranged __builtin___clear_cache (D-clean + I-invalidate of exactly these
     * bytes; a no-op on x86) is done here and jit_mem_flush_icache() is a no-op.
     */
    char *rx_begin = (char *)JIT_RW2RX(m, rw_begin);
    __builtin___clear_cache(rx_begin, rx_begin + bytes);
#else
    (void)m;
    (void)rw_begin;
    (void)bytes;
#endif
}

void jit_mem_flush_icache(JitMem *m) {
    /* Ranged I-cache maintenance already done per block in jit_mem_commit_dcache. */
    (void)m;
}

void jit_mem_commit(JitMem *m, const void *rw_begin, u32 bytes) {
    jit_mem_commit_dcache(m, rw_begin, bytes);
    jit_mem_flush_icache(m);
}

void jit_mem_icache_flush_all(JitMem *m) {
#if PSX3DS_HAVE_JIT
    if (!m || !m->rx || m->size == 0u) {
        return;
    }
    __builtin___clear_cache((char *)m->rx, (char *)m->rx + m->size);
#else
    (void)m;
#endif
}
