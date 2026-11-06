#include <3ds.h>

#include "psx3ds/platform.h"

PlatThread *plat_thread_create(void (*fn)(void *), void *arg, u32 stack, int prio, int core) {
    (void)fn;
    (void)arg;
    (void)stack;
    (void)prio;
    (void)core;
    return NULL;
}

void plat_thread_join(PlatThread *th) {
    (void)th;
}

void plat_sleep_us(u32 us) {
    svcSleepThread((s64)us * 1000LL);
}

void plat_yield(void) {
    svcSleepThread(0);
}
