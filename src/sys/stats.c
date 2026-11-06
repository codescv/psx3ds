#include "psx3ds/stats.h"

#include <string.h>

void psx_stats_reset(PsxStats *s) {
    if (!s) {
        return;
    }
    memset(s, 0, sizeof(*s));
}

void psx_stats_frame_begin(PsxStats *s) {
    if (!s) {
        return;
    }
    u64 cycles_total = s->cycles_total;
    u64 insns_total = s->insns_total;
    u32 frames = s->frames;
    u32 jit_blocks_live = s->jit_blocks_live;
    u32 jit_code_bytes = s->jit_code_bytes;
    u32 jit_flushes = s->jit_flushes;
    u32 jit_mem_mode = s->jit_mem_mode;

    memset(s, 0, sizeof(*s));

    s->cycles_total = cycles_total;
    s->insns_total = insns_total;
    s->frames = frames;
    s->jit_blocks_live = jit_blocks_live;
    s->jit_code_bytes = jit_code_bytes;
    s->jit_flushes = jit_flushes;
    s->jit_mem_mode = jit_mem_mode;
}
