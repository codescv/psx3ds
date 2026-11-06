#include "psx3ds/scheduler.h"

void sched_init(Scheduler *s) {
    (void)s;
}

void sched_reset(PsxSystem *sys) {
    (void)sys;
}

void sched_state(PsxSystem *sys, StateIO *st) {
    (void)sys;
    (void)st;
}

void sched_shutdown(PsxSystem *sys) {
    (void)sys;
}

void sched_set_handler(Scheduler *s, PsxEventId id, PsxEventFn fn) {
    (void)s;
    (void)id;
    (void)fn;
}

void sched_schedule(PsxSystem *sys, PsxEventId id, u32 delta_cycles) {
    (void)sys;
    (void)id;
    (void)delta_cycles;
}

void sched_cancel(PsxSystem *sys, PsxEventId id) {
    (void)sys;
    (void)id;
}

psx_tick_t sched_now(const PsxSystem *sys) {
    (void)sys;
    return 0u;
}

void sched_run(PsxSystem *sys) {
    (void)sys;
}
