#include "arm_emit.h"

void arm_emit_reset(Emitter *e, void *buf, u32 max_bytes) {
    if (!e) {
        return;
    }
    e->begin = (u32 *)buf;
    e->cur = (u32 *)buf;
    e->end = (u32 *)((u8 *)buf + max_bytes);
    e->overflow = false;
}
