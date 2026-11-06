#ifndef PSX3DS_ARENA_H
#define PSX3DS_ARENA_H

#include "psx3ds/types.h"

/*
 * Single bump allocator (ARCHITECTURE.md §3.8, §12).
 * Allocated once in psx_system_init(); zero dynamic heap allocation thereafter.
 */
typedef struct PsxArena {
    u8 *base;
    u32 size;
    u32 used;
} PsxArena;

PsxResult arena_init(PsxArena *a, u32 bytes);
void      arena_reset(PsxArena *a);
void      arena_destroy(PsxArena *a);
void     *arena_alloc(PsxArena *a, u32 bytes, u32 align);

#endif /* PSX3DS_ARENA_H */
