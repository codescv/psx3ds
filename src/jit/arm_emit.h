#ifndef PSX3DS_ARM_EMIT_H
#define PSX3DS_ARM_EMIT_H

#include "psx3ds/jit.h"

typedef struct Emitter {
    u32 *cur;
    u32 *begin;
    u32 *end;
    bool overflow;
} Emitter;

void arm_emit_reset(Emitter *e, void *buf, u32 max_bytes);
void jit_regalloc_reset(void);
bool jit_emit_gte_inline(Emitter *e, u32 opcode);

#endif /* PSX3DS_ARM_EMIT_H */
