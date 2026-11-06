#ifndef PSX3DS_SPSC_H
#define PSX3DS_SPSC_H

/*
 * Lock-free Single-Producer Single-Consumer (SPSC) ring buffer helpers.
 *
 * ARM11v6K rule (ARCHITECTURE.md §10.1, §13.11):
 *   Never emit ARMv7 `dmb` mnemonics. GCC's `__atomic_*` built-ins with
 *   `__ATOMIC_ACQUIRE` / `__ATOMIC_RELEASE` emit the ARMv6K CP15 data memory
 *   barrier (`mcr p15, 0, r0, c7, c10, 5`) on T1/T2 and zero-cost compiler
 *   barriers on x86_64 T3.
 *
 * Capacity `cap` MUST be a power of two (and <= 2^31). Monotonic 32-bit
 * `head` (producer write cursor) and `tail` (consumer read cursor) are masked
 * on slot access (`idx & (cap - 1)`), allowing full `cap` occupancy.
 */

#include "psx3ds/types.h"

/* Raw index-level SPSC operations for external ring arrays (PsxSpu, CdReadahead) */
PSX_INLINE void spsc_index_reset(u32 *head, u32 *tail) {
    __atomic_store_n(head, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(tail, 0u, __ATOMIC_RELAXED);
}

/* Producer-side: returns number of items currently queued */
PSX_INLINE u32 spsc_producer_count(const u32 *head, const u32 *tail) {
    u32 h = __atomic_load_n(head, __ATOMIC_RELAXED);
    u32 t = __atomic_load_n(tail, __ATOMIC_ACQUIRE);
    return h - t;
}

/* Consumer-side: returns number of items ready to pop */
PSX_INLINE u32 spsc_consumer_count(const u32 *head, const u32 *tail) {
    u32 h = __atomic_load_n(head, __ATOMIC_ACQUIRE);
    u32 t = __atomic_load_n(tail, __ATOMIC_RELAXED);
    return h - t;
}

/* Producer-side: claim next slot if space available; caller writes buf[slot] then calls spsc_commit_push */
PSX_INLINE bool spsc_begin_push(const u32 *head, const u32 *tail, u32 cap, u32 *out_slot) {
    u32 h = __atomic_load_n(head, __ATOMIC_RELAXED);
    u32 t = __atomic_load_n(tail, __ATOMIC_ACQUIRE);
    if ((h - t) >= cap) {
        return false;
    }
    *out_slot = h & (cap - 1u);
    return true;
}

PSX_INLINE void spsc_commit_push(u32 *head) {
    u32 h = __atomic_load_n(head, __ATOMIC_RELAXED);
    __atomic_store_n(head, h + 1u, __ATOMIC_RELEASE);
}

/* Consumer-side: inspect next slot if non-empty; caller reads buf[slot] then calls spsc_commit_pop */
PSX_INLINE bool spsc_begin_pop(const u32 *head, const u32 *tail, u32 cap, u32 *out_slot) {
    u32 t = __atomic_load_n(tail, __ATOMIC_RELAXED);
    u32 h = __atomic_load_n(head, __ATOMIC_ACQUIRE);
    if (h == t) {
        return false;
    }
    *out_slot = t & (cap - 1u);
    return true;
}

PSX_INLINE void spsc_commit_pop(u32 *tail) {
    u32 t = __atomic_load_n(tail, __ATOMIC_RELAXED);
    __atomic_store_n(tail, t + 1u, __ATOMIC_RELEASE);
}

/* Type-safe self-contained SPSC ring generator with cacheline-isolated cursors */
#define PSX_SPSC_DECLARE(Name, ElemType, CapPow2)                                      \
    _Static_assert(((CapPow2) != 0u) && (((CapPow2) & ((CapPow2) - 1u)) == 0u),        \
                   #Name " capacity must be a power of two");                          \
    typedef struct Name {                                                              \
        alignas(PSX_CACHELINE) u32 head;                                               \
        alignas(PSX_CACHELINE) u32 tail;                                               \
        alignas(PSX_CACHELINE) ElemType buf[CapPow2];                                  \
    } Name;                                                                            \
    PSX_INLINE void Name##_init(Name *q) {                                             \
        spsc_index_reset(&q->head, &q->tail);                                          \
    }                                                                                  \
    PSX_INLINE u32 Name##_count(const Name *q) {                                       \
        return spsc_consumer_count(&q->head, &q->tail);                                \
    }                                                                                  \
    PSX_INLINE bool Name##_empty(const Name *q) {                                      \
        return Name##_count(q) == 0u;                                                  \
    }                                                                                  \
    PSX_INLINE bool Name##_full(const Name *q) {                                       \
        return spsc_producer_count(&q->head, &q->tail) >= (CapPow2);                   \
    }                                                                                  \
    PSX_INLINE bool Name##_push(Name *q, ElemType v) {                                 \
        u32 slot = 0;                                                                  \
        if (!spsc_begin_push(&q->head, &q->tail, (CapPow2), &slot)) {                  \
            return false;                                                              \
        }                                                                              \
        q->buf[slot] = v;                                                              \
        spsc_commit_push(&q->head);                                                    \
        return true;                                                                   \
    }                                                                                  \
    PSX_INLINE bool Name##_pop(Name *q, ElemType *out) {                               \
        u32 slot = 0;                                                                  \
        if (!spsc_begin_pop(&q->head, &q->tail, (CapPow2), &slot)) {                   \
            return false;                                                              \
        }                                                                              \
        *out = q->buf[slot];                                                           \
        spsc_commit_pop(&q->tail);                                                     \
        return true;                                                                   \
    }

#endif /* PSX3DS_SPSC_H */
