#include "psx3ds/scheduler.h"
#include "psx3ds/system.h"

#include <string.h>

void sched_init(Scheduler *s) {
    if (!s) {
        return;
    }
    memset(s, 0, sizeof(*s));
}

void sched_reset(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    Scheduler *s = &sys->sched;
    PsxEventFn saved_fn[EV_COUNT];
    memcpy(saved_fn, s->fn, sizeof(saved_fn));

    memset(s, 0, sizeof(*s));
    memcpy(s->fn, saved_fn, sizeof(saved_fn));

    sys->cpu.slice_len = 0;
    sys->cpu.cycles_left = 0;
    sys->cpu.cycle_deficit = 0;
    sys->cpu.idle_skip = 0u;
}

void sched_state(PsxSystem *sys, StateIO *st) {
    if (!sys || !st) {
        return;
    }
    Scheduler *s = &sys->sched;
    state_u32(st, &s->now);
    state_u32(st, &s->slice_start);
    state_u64(st, &s->total);
    state_u16(st, &s->active_mask);
    state_u8(st, &s->count);
    if (!st->writing && s->count > (u8)EV_COUNT) {
        s->count = (u8)EV_COUNT;
    }

    for (u32 i = 0u; i < (u32)EV_COUNT; i++) {
        u32 delta = st->writing ? (u32)(s->q[i].when - s->now) : 0u;
        state_u32(st, &delta);
        state_u8(st, &s->q[i].id);
        if (!st->writing) {
            s->q[i].when = s->now + delta;
        }
    }

    for (u32 i = 0u; i < (u32)EV_COUNT; i++) {
        u32 delta = st->writing ? (u32)(s->when_by_id[i] - s->now) : 0u;
        state_u32(st, &delta);
        if (!st->writing) {
            s->when_by_id[i] = ((s->active_mask & (1u << i)) != 0u) ? (s->now + delta) : 0u;
        }
    }
}

void sched_shutdown(PsxSystem *sys) {
    (void)sys;
}

void sched_set_handler(Scheduler *s, PsxEventId id, PsxEventFn fn) {
    if (!s || (u32)id >= (u32)EV_COUNT) {
        return;
    }
    s->fn[id] = fn;
}

psx_tick_t sched_now(const PsxSystem *sys) {
    const Scheduler *s = &sys->sched;
    const PsxCpu *c = &sys->cpu;
    s32 consumed = c->slice_len - c->cycles_left;
    if (consumed < 0) {
        consumed = 0;
    }
    return s->slice_start + (psx_tick_t)(u32)consumed;
}

void sched_cancel(PsxSystem *sys, PsxEventId id) {
    if (!sys || (u32)id >= (u32)EV_COUNT) {
        return;
    }
    Scheduler *s = &sys->sched;
    u16 bit = (u16)(1u << (u32)id);
    if ((s->active_mask & bit) == 0u) {
        return;
    }
    s->active_mask = (u16)(s->active_mask & ~bit);
    s->when_by_id[id] = 0u;

    for (u8 i = 0u; i < s->count; i++) {
        if (s->q[i].id == (u8)id) {
            s->count--;
            for (u8 j = i; j < s->count; j++) {
                s->q[j] = s->q[j + 1u];
            }
            s->q[s->count].when = 0u;
            s->q[s->count].id = 0u;
            break;
        }
    }
}

void sched_schedule(PsxSystem *sys, PsxEventId id, u32 delta_cycles) {
    if (!sys || (u32)id >= (u32)EV_COUNT) {
        return;
    }
    Scheduler *s = &sys->sched;
    PsxCpu *c = &sys->cpu;

    if ((s->active_mask & (1u << (u32)id)) != 0u) {
        sched_cancel(sys, id);
    }

    psx_tick_t cur = sched_now(sys);
    psx_tick_t when = cur + delta_cycles;

    /* Insert in wrap-safe ascending order of when; equal timestamps stay FIFO */
    u8 pos = s->count;
    while (pos > 0u && tick_before(when, s->q[pos - 1u].when)) {
        s->q[pos] = s->q[pos - 1u];
        pos--;
    }
    s->q[pos].when = when;
    s->q[pos].id = (u8)id;
    s->count++;
    s->when_by_id[id] = when;
    s->active_mask |= (u16)(1u << (u32)id);

    /* Shorten active CPU slice if this event falls before slice end */
    if (c->cycles_left > 0) {
        s32 rem = (s32)(when - cur);
        if (rem < 0) {
            rem = 0;
        }
        if (rem < c->cycles_left) {
            s32 diff = c->cycles_left - rem;
            c->cycles_left = rem;
            c->slice_len -= diff;
        }
    }
}

void sched_run(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    Scheduler *s = &sys->sched;
    PsxCpu *c = &sys->cpu;

    s32 raw_consumed = c->slice_len - c->cycles_left;
    if (raw_consumed < 0) {
        raw_consumed = 0;
    }
    u32 consumed = (u32)raw_consumed;
    s32 deficit = c->cycle_deficit;
    if (c->cycles_left < 0) {
        deficit += -c->cycles_left;
    }

    if (c->idle_skip != 0u) {
        c->idle_skip = 0u;
        if (s->count > 0u) {
            s32 jump = (s32)(s->q[0].when - s->slice_start);
            if (jump > (s32)consumed) {
                consumed = (u32)jump;
                deficit = 0;
                sys->stats.idle_skips++;
            }
        }
    }

    s->now = s->slice_start + consumed;
    s->total += (u64)consumed;
    sys->stats.cycles_total += (u64)consumed;
    c->total_cycles += (u64)consumed;

    /* Anchor slice at s->now before callbacks so sched_now() & sched_schedule() are exact */
    s->slice_start = s->now;
    c->slice_len = 0;
    c->cycles_left = 0;
    c->cycle_deficit = 0;

    u32 safety = 0u;
    while (s->count > 0u && !tick_before(s->now, s->q[0].when)) {
        u8 id = s->q[0].id;
        psx_tick_t when = s->q[0].when;

        s->count--;
        for (u8 i = 0u; i < s->count; i++) {
            s->q[i] = s->q[i + 1u];
        }
        s->q[s->count].when = 0u;
        s->q[s->count].id = 0u;
        s->active_mask = (u16)(s->active_mask & ~(1u << id));
        s->when_by_id[id] = 0u;

        sys->stats.events_fired[id & 15u]++;
        if (s->fn[id]) {
            s->fn[id](sys, (u32)(s->now - when));
        }

        if (sys->quit || sys->frame_done || ++safety >= 1024u) {
            break;
        }
    }

    cpu_check_irq(sys);
    if (c->irq_pending != 0u) {
        cpu_dispatch_irq(sys);
    }

    s32 next = (s->count > 0u) ? (s32)(s->q[0].when - s->now) : 0x10000;
    if (next > 0x10000) {
        next = 0x10000;
    }
    if (next < 1) {
        next = 1;
    }

    if (deficit > 0) {
        if (next > deficit) {
            next -= deficit;
            deficit = 0;
        } else {
            deficit -= (next - 1);
            next = 1;
        }
    }

    c->cycle_deficit = deficit;
    c->slice_len = next;
    c->cycles_left = next;
    s->slice_start = s->now;
}
