#include "psx3ds/jit_mem.h"

PsxResult jit_mem_alloc(JitMem *m, u32 size) {
    if (!m) {
        return PSX_ERR_BAD_ARG;
    }
    m->rw = NULL;
    m->rx = NULL;
    m->size = size;
    m->mode = JIT_MEM_NONE;
    m->luma_icache_svc = false;
    return PSX_ERR_NOT_IMPL;
}

void jit_mem_free(JitMem *m) {
    if (!m) {
        return;
    }
    m->rw = NULL;
    m->rx = NULL;
    m->size = 0u;
    m->mode = JIT_MEM_NONE;
}

void jit_mem_commit(JitMem *m, const void *rw_begin, u32 bytes) {
    (void)m;
    (void)rw_begin;
    (void)bytes;
}

void jit_mem_icache_flush_all(JitMem *m) {
    (void)m;
}
