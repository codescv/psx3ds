#ifndef PSX3DS_TYPES_H
#define PSX3DS_TYPES_H

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
               "psx3ds requires a little-endian target");
#endif

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

/* 32-bit wrapping CPU-cycle timestamp (wrap-safe via (s32)(a - b) < 0) */
typedef u32 psx_tick_t;

#define PSX_HOT         __attribute__((hot))
#define PSX_COLD        __attribute__((cold, noinline))
#define PSX_USED        __attribute__((used))
#define PSX_UNUSED      __attribute__((unused))
#define PSX_INLINE      static inline __attribute__((always_inline))
#define PSX_LIKELY(x)   __builtin_expect(!!(x), 1)
#define PSX_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define PSX_ALIGNED(n)  __attribute__((aligned(n)))
#define PSX_CACHELINE   32 /* ARM11 MPCore L1 cache line size in bytes */

#define PSX_ASSERT_OFF(T, f, off) \
    _Static_assert(offsetof(T, f) == (off), #T "." #f " offset drifted")

typedef enum PsxResult {
    PSX_OK           =  0,
    PSX_ERR_NOT_IMPL = -1,
    PSX_ERR_IO       = -2,
    PSX_ERR_NOMEM    = -3,
    PSX_ERR_BAD_ARG  = -4,
    PSX_ERR_FORMAT   = -5
} PsxResult;

/* Forward declarations so every header compiles cleanly in any include order */
typedef struct PsxSystem PsxSystem;
typedef struct StateIO   StateIO;
typedef struct PsxCpu    PsxCpu;
typedef struct PsxBus    PsxBus;
typedef struct Scheduler Scheduler;

#endif /* PSX3DS_TYPES_H */
