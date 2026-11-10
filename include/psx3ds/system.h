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

typedef void (*PsxTtyCallback)(const char *data, u32 len, void *user);

typedef struct PsxExeHeader {
    char magic[8];
    u32  pc0;
    u32  gp0;
    u32  t_addr;
    u32  t_size;
    u32  d_addr;
    u32  d_size;
    u32  b_addr;
    u32  b_size;
    u32  s_addr;
    u32  s_size;
} PsxExeHeader;

typedef struct PsxCdBootInfo {
    bool         valid;          /* boot EXE located + header parsed */
    bool         has_pvd;        /* LBA 16 "\x01CD001\x01" present */
    bool         has_system_cnf; /* SYSTEM.CNF found in root directory */
    char         volume[33];
    char         boot_path[128]; /* normalized, e.g. "PSP\GAME.EXE" (no "cdrom:\", no ";1") */
    bool         cnf_kernel_cfg; /* SYSTEM.CNF also sets TCB + EVENT */
    u32          exe_lba;
    u32          exe_size;
    PsxExeHeader hdr;
    u32          sig_off;        /* text offset of sig[] (last non-zero 16 B of the EXE) */
    u8           sig[16];        /* native-boot load-complete detection */
} PsxCdBootInfo;

#define PSX_HLE_MAX_FDS 8u

typedef struct PsxHleFile {
    bool used;
    u32  lba;
    u32  size;
    u32  pos;
} PsxHleFile;

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

    /* Extended TTY capture & standalone HLE kernel state */
    u32            until_pattern_len;
    char          *tty_buf;
    u32            tty_buf_len;
    u32            tty_buf_cap;
    u32            tty_outer_sp;
    PsxTtyCallback tty_cb;
    void          *tty_cb_user;
    u8            *bios_rom_backup;
    u32            hle_hook_entry_int;
    u32            hle_saved_gpr[32];
    u32            hle_saved_hi;
    u32            hle_saved_lo;
    u32            hle_saved_epc;
    u32            hle_saved_sr;
    bool           hle_in_intr_hook;
    u16            hle_pad_auto_ack;
    u16            hle_rcnt_auto_ack;
    u32            hle_pad_buf1_addr;
    u32            hle_pad_buf2_addr;
    u32            hle_pad_dest_addr; /* B0:15 OutdatedPadInitAndStart button_dest */

    /* ISO9660 CD-ROM boot (v0.2.2) */
    bool           cd_boot;        /* a data disc with a bootable EXE is mounted */
    bool           cdboot_pending; /* sideload cdboot EXE at the 0x80030000 shell hook */
    bool           cdboot_skip_shell; /* return from 0x80030000 shell so BIOS boots the disc */
    bool           cdboot_watch;      /* native BIOS CD boot in progress (watchdog armed) */
    u32            cdboot_watch_frames;
    PsxCdBootInfo  cdboot;
    PsxHleFile     hle_files[PSX_HLE_MAX_FDS]; /* HLE kernel cdrom: file handles */
} PsxBios;

void bios_init(PsxSystem *sys);
void bios_reset(PsxSystem *sys);
void bios_state(PsxSystem *sys, StateIO *st);
void bios_shutdown(PsxSystem *sys);
bool bios_pc_hook(PsxSystem *sys, u32 pc);

void bios_tty_putc(PsxSystem *sys, char c);
void bios_tty_clear(PsxSystem *sys);
void bios_set_tty_callback(PsxSystem *sys, PsxTtyCallback cb, void *user);
void bios_set_stop_pattern(PsxSystem *sys, const char *substr);

/* ---------------------------------------------------------------------------
 * BIOS image selection (v0.2.2)
 *   psx_load_bios(sys, "builtin")  -> embedded 512 KiB OpenBIOS (always available)
 *   psx_load_bios(sys, "auto")     -> external 512 KiB BIOS found by
 *                                     psx_bios_find_external(), else built-in OpenBIOS
 *   psx_load_bios(sys, "hle"/NULL) -> standalone BIOS-less HLE kernel
 * ------------------------------------------------------------------------- */
#define PSX_BIOS_BUILTIN_NAME "builtin"
const u8 *psx_builtin_openbios(u32 *out_size);
/* Scans sdmc:/psx3ds/bios, sdmc:/psx, sdmc:/roms/psx, sdmc:/bios, sdmc:/ (3DS) or
 * ./, ./bios, tests/roms/bios (host) case-insensitively for a 524,288-byte
 * scph*.bin / psxonpsp*.bin / bios*.bin / openbios.bin. On success fills path_out and an
 * upper-case badge (e.g. "SCPH5501", "OPENBIOS") and returns true. */
bool psx_bios_find_external(char *path_out, u32 path_cap, char *badge_out, u32 badge_cap);

/* ---------------------------------------------------------------------------
 * ISO9660 CD-ROM boot (src/sys/cdboot.c, v0.2.2)
 * ------------------------------------------------------------------------- */

PsxResult psx_cdboot_probe(PsxSystem *sys, PsxCdBootInfo *out);
PsxResult psx_cdboot_inject(PsxSystem *sys, const PsxCdBootInfo *info);
bool      psx_cdboot_find_file(PsxSystem *sys, const char *path, u32 *out_lba, u32 *out_size);
bool      psx_cdboot_read_data(PsxSystem *sys, u32 lba, u32 offset, u8 *dst, u32 len);

void      psx_cdboot_arm_bios(PsxSystem *sys);
void      psx_cdboot_watchdog(PsxSystem *sys); /* once per frame */
PsxResult psx_cdboot_load_exe(PsxSystem *sys, u32 exe_lba, u32 exe_size, PsxExeHeader *hdr_out);

PsxResult psx_exe_parse_header(const u8 *buf, u32 buf_len, PsxExeHeader *out);
void      psx_exe_write_ram(PsxSystem *sys, u32 phys_start, const u8 *src, u32 len);
void      psx_exe_apply_entry(PsxSystem *sys, const PsxExeHeader *hdr, u32 sp_val);
PsxResult psx_exe_inject_now(PsxSystem *sys);

void hle_bios_install_rom_stub(PsxSystem *sys);
void hle_bios_install_ram_vectors(PsxSystem *sys);
bool hle_bios_dispatch(PsxSystem *sys, u32 vec);

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

    /* Cached GPU backend handles for zero-leak runtime HW/SW switching */
    const GpuBackend *soft_gpu_be;
    void             *soft_gpu_self;
    const GpuBackend *hw_gpu_be;
    void             *hw_gpu_self;
} PsxSystem;

PsxResult psx_system_init(PsxSystem *sys, const PsxConfig *cfg);
void      psx_system_reset(PsxSystem *sys);
PsxResult psx_load_bios(PsxSystem *sys, const char *path);
PsxResult psx_load_exe(PsxSystem *sys, const char *path);
PsxResult psx_load_cd(PsxSystem *sys, const char *path);
void      psx_run_frame(PsxSystem *sys);
PsxResult psx_state_save(PsxSystem *sys, StateIO *st);
PsxResult psx_state_load(PsxSystem *sys, StateIO *st);
PsxResult psx_state_save_file(PsxSystem *sys, const char *path);
PsxResult psx_state_load_file(PsxSystem *sys, const char *path);
PsxResult psx_system_set_gpu_backend(PsxSystem *sys, PsxGpuBackendKind kind);
void      psx_system_shutdown(PsxSystem *sys);

void psx_stats_accumulate(PsxStats *accum, const PsxStats *frame_stats);
u32  psx_stats_format_json(const PsxSystem *sys, const PsxStats *accum,
                           const char *rom_name, u64 wall_us, u32 frames_ran,
                           char *buf, u32 cap);

typedef struct PsxBenchConfig {
    const char *bios_path;
    bool        hle_bios;
    const char *exe_path;
    const char *cd_path;
    u32         frames;
    bool        all_engines;
    bool        all_gpus;
} PsxBenchConfig;

PsxResult psx_bench_run_json(const PsxBenchConfig *bcfg, char *out_json, u32 out_cap);

#endif /* PSX3DS_SYSTEM_H */
