#ifndef PSX3DS_SYSTEM_H
#define PSX3DS_SYSTEM_H

#include "psx3ds/arena.h"
#include "psx3ds/bus.h"
#include "psx3ds/cdrom.h"
#include "psx3ds/config.h"
#include "psx3ds/cpu.h"
#include "psx3ds/dma.h"
#include "psx3ds/gpu.h"
#include "psx3ds/gpu_hw.h"
#include "psx3ds/gpu_soft.h"
#include "psx3ds/gte.h"
#include "psx3ds/irq.h"
#include "psx3ds/jit.h"
#include "psx3ds/mdec.h"
#include "psx3ds/platform.h"
#include "psx3ds/scheduler.h"
#include "psx3ds/sio.h"
#include "psx3ds/spu.h"
#include "psx3ds/state.h"
#include "psx3ds/stats.h"
#include "psx3ds/timers.h"
#include "psx3ds/types.h"

typedef struct PsxBios {
    bool loaded;
    bool hle;
    u32  exe_pc;
    u32  exe_gp;
    u32  exe_sp;
    char exe_path[256];
    bool psexe_pending;
    char tty_line[256];
    u32  tty_len;
    char until_pattern[64];
    bool until_matched;
    u32  hle_heap_ptr;
    u32  hle_events[32][4];
} PsxBios;

void bios_init(PsxSystem *sys);
void bios_reset(PsxSystem *sys);
void bios_state(PsxSystem *sys, StateIO *st);
void bios_shutdown(PsxSystem *sys);
bool bios_pc_hook(PsxSystem *sys, u32 pc);

typedef struct PsxSystem {
    PsxConfig   cfg;
    PsxStats    stats;
    PsxArena    arena;

    PsxCpu      cpu; /* 32-byte aligned; cpu.sys.p points back to this PsxSystem */

    PsxBus      bus;
    Scheduler   sched;
    PsxIrq      irq;
    PsxDma      dma;
    PsxTimers   timers;
    PsxGte      gte;
    JitState    jit;
    PsxGpu      gpu;
    PsxSpu      spu;
    PsxCdrom    cdrom;
    PsxMdec     mdec;
    PsxSio      sio;
    PsxBios     bios;

    u32         cpu_mode; /* resolved PsxCpuEngine */
    bool        frame_done;
    bool        quit;
    PsxPadState pads[2];
} PsxSystem;

PsxResult psx_system_init(PsxSystem *sys, const PsxConfig *cfg);
void      psx_system_reset(PsxSystem *sys);
PsxResult psx_load_bios(PsxSystem *sys, const char *path);
PsxResult psx_load_exe(PsxSystem *sys, const char *path);
PsxResult psx_load_cd(PsxSystem *sys, const char *path);
void      psx_run_frame(PsxSystem *sys);
PsxResult psx_state_save(PsxSystem *sys, StateIO *st);
PsxResult psx_state_load(PsxSystem *sys, StateIO *st);
void      psx_system_shutdown(PsxSystem *sys);

#endif /* PSX3DS_SYSTEM_H */
