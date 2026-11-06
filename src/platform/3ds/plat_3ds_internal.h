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
    CTR_UI_QUIT_APP
} CtrUiAction;

typedef struct CtrUiState {
    u8   active_tab;      /* 0 = ROM Browser, 1 = Live HUD, 2 = Quick Controls */
    u8   state_slot;      /* 0..3 */
    u32  rom_count;
    u32  rom_sel;
    u32  rom_scroll;
    char rom_paths[128][256];
    char rom_labels[128][64];
    char bios_path[256];
    char bios_badge[32];
    char status_msg[64];
    u32  status_timer;
    u16  touch_extra_buttons; /* active-high L2/R2/L3/R3 touch pads for Old 3DS */
    bool game_loaded;
    char pending_boot_path[256];
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
void ctr_audio_step_core1(PsxSystem *sys);
void ctr_audio_shutdown(void);
u32  ctr_audio_queued_frames(void);

/* plat_3ds.c */
void      ctr_input_set_touch_buttons(u16 active_high_mask);
void      ctr_input_set_scale_mode(PsxVideoScale mode);
PsxResult ctr_capture_top_screen_ppm(const char *path);

/* ui_bottom.c */
void        ctr_ui_init(CtrUiState *ui);
void        ctr_ui_scan_roms(CtrUiState *ui);
void        ctr_ui_set_status(CtrUiState *ui, const char *msg);
CtrUiAction ctr_ui_update(CtrUiState *ui, PsxSystem *sys, const CtrHwInfo *hw,
                          u32 keys_down, u32 keys_held, const touchPosition *touch);
void        ctr_ui_render(const CtrUiState *ui, const PsxSystem *sys, const CtrHwInfo *hw);

#endif /* PSX3DS_PLAT_3DS_INTERNAL_H */
