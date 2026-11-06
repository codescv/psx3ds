#include <3ds.h>
#include <stdlib.h>
#include <string.h>

#include "plat_3ds_internal.h"

/* CD readahead producer step (src/periph/cdrom_readahead.c, declared in periph_internal.h) */
u32 cdrom_ra_pump(PsxSystem *sys, u32 max_sectors);

struct PlatThread {
    Thread t;
    int    actual_core;
};

static PlatThread  *g_spu_worker = NULL;
static PlatThread  *g_cd_worker = NULL;
static volatile int g_workers_running = 0;

void ctr_threads_init_hardware(CtrHwInfo *hw) {
    if (!hw) {
        return;
    }
    memset(hw, 0, sizeof(*hw));

    bool is_n3ds = false;
    if (R_SUCCEEDED(APT_CheckNew3DS(&is_n3ds)) && is_n3ds) {
        hw->is_n3ds = true;
        osSetSpeedupEnable(true); /* Enable 804 MHz ARM11 + 2 MiB L2 cache */
        hw->cpu_mhz = 804u;
    } else {
        hw->is_n3ds = false;
        hw->cpu_mhz = 268u;
    }

    /* Unlock Core 1 (SysCore): try 80% time limit, fallback to universally-accepted 30% */
    if (R_SUCCEEDED(APT_SetAppCpuTimeLimit(80))) {
        hw->syscore_pct = 80u;
    } else if (R_SUCCEEDED(APT_SetAppCpuTimeLimit(30))) {
        hw->syscore_pct = 30u;
    } else {
        hw->syscore_pct = 0u;
    }

    plat_log("[threads_3ds] n3ds=%d cpu_mhz=%u syscore_limit=%u%%\n",
             (int)hw->is_n3ds, (unsigned)hw->cpu_mhz, (unsigned)hw->syscore_pct);
}

PlatThread *plat_thread_create(void (*fn)(void *), void *arg, u32 stack, int prio, int core) {
    if (!fn) {
        return NULL;
    }
    if (stack < 16384u) {
        stack = 16384u;
    }
    stack = (stack + 7u) & ~7u;

    s32 main_prio = 0x30;
    (void)svcGetThreadPriority(&main_prio, CUR_THREAD_HANDLE);

    s32 target_prio = (s32)prio;
    if (target_prio < 0x18) {
        target_prio = 0x18;
    }
    if (target_prio > 0x3F) {
        target_prio = 0x3F;
    }
    /* On Core 1 (SysCore), libctru requires worker prio >= main_prio - 1 on some firmwares */
    if (core == 1 && target_prio < main_prio - 2) {
        target_prio = (main_prio > 0x19) ? (main_prio - 1) : main_prio;
    }

    int chosen_core = core;
    Thread t = threadCreate(fn, arg, stack, target_prio, chosen_core, false);
    if (!t && chosen_core == 2) {
        /* Fallback from N3DS Core 2 to Core 1 (O3DS or restricted affinity) */
        chosen_core = 1;
        s32 retry_prio = (main_prio < 0x3E) ? (main_prio + 1) : 0x3F;
        t = threadCreate(fn, arg, stack, retry_prio, chosen_core, false);
    }
    if (!t && chosen_core != -2) {
        chosen_core = -2;
        t = threadCreate(fn, arg, stack, main_prio, chosen_core, false);
    }
    if (!t) {
        return NULL;
    }

    PlatThread *pt = (PlatThread *)calloc(1u, sizeof(*pt));
    if (!pt) {
        threadJoin(t, U64_MAX);
        threadFree(t);
        return NULL;
    }
    pt->t = t;
    pt->actual_core = chosen_core;
    return pt;
}

void plat_thread_join(PlatThread *th) {
    if (!th) {
        return;
    }
    if (th->t) {
        threadJoin(th->t, U64_MAX);
        threadFree(th->t);
    }
    free(th);
}

void plat_sleep_us(u32 us) {
    svcSleepThread((s64)us * 1000LL);
}

void plat_yield(void) {
    svcSleepThread(0);
}

/*
 * Core 1: 500 us cadence (2 kHz) comfortably covers 44.1 kHz in 64-frame render chunks;
 * ctr_audio_step_core1() itself only renders time Core 0 has already queued.
 */
static void spu_core1_worker(void *arg) {
    PsxSystem *sys = (PsxSystem *)arg;
    while (__atomic_load_n(&g_workers_running, __ATOMIC_ACQUIRE) != 0) {
        if (sys && !__atomic_load_n(&sys->quit, __ATOMIC_ACQUIRE)) {
            ctr_audio_step_core1(sys);
        }
        plat_sleep_us(500u);
    }
}

/*
 * Core 2 (N3DS) / Core 1 (O3DS): pump the CD readahead ring directly so the loop exits
 * as soon as g_workers_running drops (no dependency on sys->quit).
 */
static void cd_core2_worker(void *arg) {
    PsxSystem *sys = (PsxSystem *)arg;
    while (__atomic_load_n(&g_workers_running, __ATOMIC_ACQUIRE) != 0) {
        u32 n = 0u;
        if (sys && !__atomic_load_n(&sys->quit, __ATOMIC_ACQUIRE)) {
            n = cdrom_ra_pump(sys, 8u);
        }
        plat_sleep_us(n != 0u ? 500u : 2000u);
    }
}

void ctr_threads_start_workers(PsxSystem *sys, CtrHwInfo *hw) {
    ctr_threads_stop_workers(sys, hw);
    if (!sys || !hw) {
        return;
    }
    __atomic_store_n(&g_workers_running, 1, __ATOMIC_RELEASE);

    s32 main_prio = 0x30;
    (void)svcGetThreadPriority(&main_prio, CUR_THREAD_HANDLE);

    if (hw->syscore_pct > 0u) {
        s32 spu_prio = (main_prio > 0x19) ? (main_prio - 1) : main_prio;
        g_spu_worker = plat_thread_create(spu_core1_worker, sys, 32768u, spu_prio, 1);
        hw->spu_core1_active = (g_spu_worker != NULL);
    }

    int target_cd_core = hw->is_n3ds ? 2 : 1;
    s32 cd_prio = hw->is_n3ds ? main_prio : ((main_prio < 0x3E) ? (main_prio + 1) : 0x3F);
    g_cd_worker = plat_thread_create(cd_core2_worker, sys, 32768u, cd_prio, target_cd_core);
    if (g_cd_worker) {
        hw->cd_worker_core = g_cd_worker->actual_core;
        hw->cd_core2_active = (g_cd_worker->actual_core == 2);
    } else {
        hw->cd_worker_core = -1;
        hw->cd_core2_active = false;
    }

    plat_log("[threads_3ds] workers started: spu_core1=%d cd_core=%d\n",
             (int)hw->spu_core1_active, hw->cd_worker_core);
}

void ctr_threads_stop_workers(PsxSystem *sys, CtrHwInfo *hw) {
    if (__atomic_load_n(&g_workers_running, __ATOMIC_ACQUIRE) == 0 &&
        !g_spu_worker && !g_cd_worker) {
        return;
    }
    (void)sys;
    __atomic_store_n(&g_workers_running, 0, __ATOMIC_RELEASE);
    if (g_spu_worker) {
        plat_thread_join(g_spu_worker);
        g_spu_worker = NULL;
    }
    if (g_cd_worker) {
        plat_thread_join(g_cd_worker);
        g_cd_worker = NULL;
    }
    if (hw) {
        hw->spu_core1_active = false;
        hw->cd_core2_active = false;
        hw->cd_worker_core = -1;
    }
}
