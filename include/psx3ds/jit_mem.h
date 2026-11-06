#ifndef PSX3DS_JIT_MEM_H
#define PSX3DS_JIT_MEM_H

#include "psx3ds/types.h"

/*
 * W^X / executable code-memory provider interface (ARCHITECTURE.md §5.12).
 * Implemented by src/jit/jit_mem_host.c on hosts (T2/T3) and
 * src/platform/3ds/jit_mem_3ds.c on Nintendo 3DS (T1).
 */
typedef enum JitMemMode {
    JIT_MEM_NONE = 0,
    JIT_MEM_MIRROR_RWRX,
    JIT_MEM_RWX_INPLACE,
    JIT_MEM_HOST_MMAP
} JitMemMode;

typedef struct JitMem {
    u8        *rw;
    u8        *rx;
    u32        size;
    JitMemMode mode;
    bool       luma_icache_svc;
} JitMem;

PsxResult jit_mem_alloc(JitMem *m, u32 size);
void      jit_mem_free(JitMem *m);
void      jit_mem_commit(JitMem *m, const void *rw_begin, u32 bytes);
void      jit_mem_icache_flush_all(JitMem *m);

#define JIT_RW2RX(m, p) ((u8 *)(p) + ((m)->rx - (m)->rw))

#endif /* PSX3DS_JIT_MEM_H */
