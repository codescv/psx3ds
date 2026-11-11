#ifndef PSX3DS_PLAT_3DS_INTERNAL_H
#define PSX3DS_PLAT_3DS_INTERNAL_H

#include <3ds.h>

#include "psx3ds/system.h"

extern const u8 psx3ds_font8x8[128][8];

typedef struct CtrHwInfo {
    bool is_n3ds;
    u32  cpu_mhz;
    u32  syscore_pct;
    bool ndsp_ok;
    bool spu_core1_active;
    bool cd_core2_active;
    int  cd_worker_core;
} CtrHwInfo;

typedef enum CtrUiAction {
    CTR_UI_NONE = 0,
    CTR_UI_BOOT_PATH,
    CTR_UI_TOGGLE_GPU,
    CTR_UI_CYCLE_SCALE,
    CTR_UI_CYCLE_FRAMESKIP,
    CTR_UI_TOGGLE_REVERB,
    CTR_UI_SAVE_STATE,
    CTR_UI_LOAD_STATE,
    CTR_UI_RESET_SYSTEM,
    CTR_UI_OPEN_BROWSER,
    CTR_UI_QUIT_APP,
    CTR_UI_TOGGLE_BIOS_MODE,
    CTR_UI_TOGGLE_CD_BOOT
} CtrUiAction;

#define CTR_UI_TAB_ROMS 0u
#define CTR_UI_TAB_HUD  1u
#define CTR_UI_TAB_CTRL 2u
#define CTR_UI_TAB_LOG  3u
#define CTR_UI_TAB_COUNT 4u

#define CTR_BIOS_MODE_AUTO 0u /* external SD BIOS if found, else built-in OpenBIOS */
#define CTR_BIOS_MODE_HLE  1u /* BIOS-less HLE kernel (direct boot) */

#define CTR_UI_FRAME_RING  32u

typedef struct CtrUiState {
    u8   active_tab;      /* CTR_UI_TAB_*: 0 ROMS, 1 HUD, 2 CTRL, 3 LOG */
    u8   bios_mode;       /* CTR_BIOS_MODE_* */
    bool cd_fast_boot;    /* -> PsxConfig.cd_fast_boot: sideload disc EXE vs native BIOS CD boot */
    u8   state_slot;      /* 0..3 */
    u32  rom_count;
    u32  rom_sel;
    u32  rom_scroll;
    char rom_paths[128][256];
    char rom_labels[128][64];
    char bios_path[256];  /* external SD BIOS path, or "builtin" */
    char bios_badge[32];  /* e.g. "SCPH1001", "OPENBIOS" */
    u32  log_scroll;      /* LOG tab: lines scrolled up from the newest (0 = follow tail) */
    char status_msg[64];
    u32  status_timer;
    u16  touch_extra_buttons; /* active-high L2/R2/L3/R3 touch pads for Old 3DS */
    bool game_loaded;
    char pending_boot_path[256];
    /* v0.2.6: rolling 32-frame paced frame-time window for the HUD (even + odd frames) */
    u32  frame_us_ring[CTR_UI_FRAME_RING];
    u32  cpu_us_ring[CTR_UI_FRAME_RING];
    u32  gpu_us_ring[CTR_UI_FRAME_RING];
    u8   frame_ring_pos;
    u8   frame_ring_count;
} CtrUiState;

/* jit_mem_3ds.c */
void jit_mem_3ds_set_allow_hle_exec(bool allow);

/* threads_3ds.c */
void ctr_threads_init_hardware(CtrHwInfo *hw);
void ctr_threads_start_workers(PsxSystem *sys, CtrHwInfo *hw);
void ctr_threads_stop_workers(PsxSystem *sys, CtrHwInfo *hw);

/* audio_ndsp.c */
bool ctr_audio_init(PsxSystem *sys);
void ctr_audio_bind_system(PsxSystem *sys);
void ctr_audio_render_core1(PsxSystem *sys); /* Core 1: SPU render only, never NDSP */
void ctr_audio_step_core1(PsxSystem *sys);   /* Core 0: NDSP wavebuf pump (+ render fallback) */
void ctr_audio_pump_core0(PsxSystem *sys);   /* alias of ctr_audio_step_core1 */
void ctr_audio_get_diag(u32 *out_wb_submitted, u32 *out_wb_active, u32 *out_pcm_peak);
void ctr_audio_shutdown(void);
u32  ctr_audio_queued_frames(void);

/* plat_3ds.c */
void      ctr_input_set_touch_buttons(u16 active_high_mask);
void      ctr_input_set_scale_mode(PsxVideoScale mode);
PsxResult ctr_capture_top_screen_ppm(const char *path);
PsxResult ctr_capture_bottom_screen_ppm(const char *path); /* 320x240 touch UI */

/* On-screen log ring (plat_log + guest TTY), mirrored to sdmc:/psx3ds/psx3ds.log */
#define CTR_LOG_DUMP_PATH "sdmc:/psx3ds/psx3ds_dump.log"
u32  ctr_log_count(void);
u32  ctr_log_get(u32 idx_from_oldest, char *out, u32 cap); /* returns strlen(out) */
void ctr_log_clear(void);
bool ctr_log_dump(const char *path);
void ctr_log_tty_putc(char c); /* line-buffers guest TTY as "[tty] ..." lines */

/* ui_bottom.c */
void        ctr_ui_init(CtrUiState *ui);
void        ctr_ui_scan_roms(CtrUiState *ui);
void        ctr_ui_set_status(CtrUiState *ui, const char *msg);
void        ctr_ui_reset_frame_stats(CtrUiState *ui);
void        ctr_ui_record_frame_time(CtrUiState *ui, u32 paced_us, u32 cpu_us, u32 gpu_us);
CtrUiAction ctr_ui_update(CtrUiState *ui, PsxSystem *sys, const CtrHwInfo *hw,
                          u32 keys_down, u32 keys_held, const touchPosition *touch);
void        ctr_ui_render(const CtrUiState *ui, const PsxSystem *sys, const CtrHwInfo *hw);

#endif /* PSX3DS_PLAT_3DS_INTERNAL_H */
