#ifndef PSX3DS_SCHEDULER_H
#define PSX3DS_SCHEDULER_H

#include "psx3ds/types.h"

/*
 * Event-driven downcounter scheduler (ARCHITECTURE.md §4).
 * Fixed-capacity sorted array (<= 16 live events) with 32-bit wrap-safe ticks.
 */
typedef enum PsxEventId {
    EV_HBLANK = 0,
    EV_VBLANK,
    EV_TIMER0,
    EV_TIMER1,
    EV_TIMER2,
    EV_CDROM,
    EV_DMA,
    EV_SPU,
    EV_SIO0,
    EV_MDEC,
    EV_GPU,
    EV_IRQ_LATCH,
    EV_COUNT
} PsxEventId;

_Static_assert(EV_COUNT <= 16, "sorted-array scheduler capacity is 16");

typedef void (*PsxEventFn)(PsxSystem *sys, u32 late_cycles);

typedef struct Scheduler {
    psx_tick_t now;
    psx_tick_t slice_start;
    u8         count;
    struct {
        psx_tick_t when;
        u8         id;
    } q[EV_COUNT];
    psx_tick_t when_by_id[EV_COUNT];
    u16        active_mask;
    PsxEventFn fn[EV_COUNT];
    u64        total;
} Scheduler;

static inline bool tick_before(psx_tick_t a, psx_tick_t b) {
    return (s32)(a - b) < 0;
}

void       sched_init(Scheduler *s);
void       sched_reset(PsxSystem *sys);
void       sched_state(PsxSystem *sys, StateIO *st);
void       sched_shutdown(PsxSystem *sys);

void       sched_set_handler(Scheduler *s, PsxEventId id, PsxEventFn fn);
void       sched_schedule(PsxSystem *sys, PsxEventId id, u32 delta_cycles);
void       sched_cancel(PsxSystem *sys, PsxEventId id);
psx_tick_t sched_now(const PsxSystem *sys);
void       sched_run(PsxSystem *sys);

#endif /* PSX3DS_SCHEDULER_H */
