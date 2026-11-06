#if !defined(__3DS__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "psx3ds/arena.h"
#include "psx3ds/config.h"

#include <stdlib.h>
#include <string.h>
#if PSX3DS_PLATFORM_3DS
#include <malloc.h>
#endif

PsxResult arena_init(PsxArena *a, u32 bytes) {
    if (!a || bytes == 0u || bytes > (UINT32_MAX - 4095u)) {
        return PSX_ERR_BAD_ARG;
    }
    u32 aligned_size = (bytes + 4095u) & ~4095u;
    void *mem = NULL;
#if PSX3DS_PLATFORM_3DS
    mem = memalign(4096u, aligned_size);
#else
    if (posix_memalign(&mem, 4096u, aligned_size) != 0) {
        mem = NULL;
    }
#endif
    if (!mem) {
        a->base = NULL;
        a->size = 0u;
        a->used = 0u;
        return PSX_ERR_NOMEM;
    }
    memset(mem, 0, aligned_size);
    a->base = (u8 *)mem;
    a->size = aligned_size;
    a->used = 0u;
    return PSX_OK;
}

void arena_reset(PsxArena *a) {
    if (!a) {
        return;
    }
    if (a->base && a->used > 0u) {
        memset(a->base, 0, a->used);
    }
    a->used = 0u;
}

void arena_destroy(PsxArena *a) {
    if (!a) {
        return;
    }
    free(a->base);
    a->base = NULL;
    a->size = 0u;
    a->used = 0u;
}

void *arena_alloc(PsxArena *a, u32 bytes, u32 align) {
    if (!a || !a->base) {
        abort();
    }
    if (align == 0u) {
        align = 8u;
    }
    if ((align & (align - 1u)) != 0u) {
        abort();
    }
    uintptr_t cur = (uintptr_t)a->base + (uintptr_t)a->used;
    uintptr_t mask = (uintptr_t)(align - 1u);
    uintptr_t aligned = (cur + mask) & ~mask;
    u64 pad = (u64)(aligned - cur);
    u64 alloc_sz = (u64)(bytes ? bytes : 1u);
    u64 end = (u64)a->used + pad + alloc_sz;
    if (end > (u64)a->size) {
        abort();
    }
    a->used = (u32)end;
    void *ptr = (void *)aligned;
    memset(ptr, 0, (size_t)alloc_sz);
    return ptr;
}
