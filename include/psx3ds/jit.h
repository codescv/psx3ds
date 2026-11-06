#ifndef PSX3DS_JIT_H
#define PSX3DS_JIT_H

#include "psx3ds/config.h"
#include "psx3ds/cpu.h"
#include "psx3ds/jit_mem.h"
#include "psx3ds/types.h"

#define BLOCKTAB_RAM_WORDS   0x80000u
#define BLOCKTAB_BIOS_WORDS  0x20000u
#define BLOCKTAB_RAM_IDX(pc)  (((u32)(pc) & 0x1FFFFFu) >> 2)
#define BLOCKTAB_BIOS_IDX(pc) ((((u32)(pc) & 0x1FFFFFFFu) - 0x1FC00000u) >> 2)

#define JIT_IBTC_ENTRIES     1024u
#define IBTC_EMPTY_TAG       1u
#define JIT_MAX_BLOCK_INSNS  128u
#define JIT_MAX_BLOCK_BYTES  4096u
#define JIT_PAGE_NONE        0xFFFFu

enum JitBlockFlags {
    JB_IDLE_LOOP     = (1u << 0),
    JB_ENDS_LOAD_DS  = (1u << 1),
    JB_HAS_GTE       = (1u << 2),
    JB_DEAD          = (1u << 3),
    JB_PEEKED_TARGET = (1u << 4)
};

typedef struct JitBlock {
    PsxPtrSlot entry;           /* host RX entry (or CachedOp* for cpu_cached); 8 bytes */
    u32        pc;              /* guest virtual start PC                               */
    u32        code_bytes;
    u16        n_insns;
    u16        cycles;          /* scaled by cfg.cycle_mult                             */
    u16        phys_page[2];    /* RAM pages touched (0xFFFF = none)                    */
    u16        next_in_page[2]; /* intrusive per-page singly linked lists (0xFFFF = end)*/
    u16        flags;           /* JB_* bitmask                                         */
    u16        _pad;
} JitBlock;

_Static_assert(sizeof(JitBlock) == 32, "JitBlock must be 32 bytes on ARM32 and x86_64");

typedef struct PSX_ALIGNED(8) JitIbtcEntry {
    u32   tag;
    void *entry;
} JitIbtcEntry;

#if PSX3DS_HAVE_JIT
_Static_assert(sizeof(JitIbtcEntry) == 8, "JitIbtcEntry must be 8 bytes for ARM LDM");
#endif

typedef struct JitState {
    JitBlock     *blocks;
    u32           n_blocks;
    u32           max_blocks;
    u16           page_head[512];
    JitIbtcEntry *ibtc;
    void        **tab_ram;
    void        **tab_bios;
    JitMem        mem;
    u8           *code_ptr;
    u8           *code_end;
    u32           generation;
    /* Sub-page SMC filter: bit k of page_code_mask[p] set => some live (or recently
     * killed) block's guest code / peeked target word lies in 64-byte line k of
     * 4 KiB physical RAM page p. Conservative superset; cleared on page wipe. */
    u64           page_code_mask[512];
} JitState;

/* Lifecycle & execution engines (built on ALL targets) */
void  jit_init(PsxSystem *sys);
void  jit_reset(PsxSystem *sys);
void  jit_state(PsxSystem *sys, StateIO *st);
void  jit_shutdown(PsxSystem *sys);

void  jit_run(PsxSystem *sys);
void  cpu_cached_run(PsxSystem *sys);

/* SMC invalidation & cache flush (called by bus/DMA/savestate on ALL targets) */
void  jit_invalidate_page(PsxSystem *sys, u32 phys_page);
void  jit_invalidate_range(PsxSystem *sys, u32 phys, u32 bytes);
void  jit_flush_all(PsxSystem *sys);

/* Dispatcher/compiler C hooks (PSX_USED protects against LTO stripping when called from .S) */
PSX_USED void *jit_lookup_slow(PsxCpu *cpu, u32 pc);
PSX_USED void *jit_compile_at_pc(PsxCpu *cpu);
int            jit_enter(PsxCpu *cpu);

/* Assembly/sentinel trampoline symbols (jit_thunks.S when PSX3DS_HAVE_JIT, else jit_block.c) */
extern char jit_dispatch[];
extern char jit_dispatch_slow[];
extern char jit_compile_stub[];
extern char jit_bios_vec_stub[];
extern char jit_psexe_stub[];
extern char jit_exit[];

#endif /* PSX3DS_JIT_H */
