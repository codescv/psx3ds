#include "psx3ds/system.h"

void state_u8(StateIO *s, u8 *v) {
    (void)s;
    (void)v;
}

void state_u16(StateIO *s, u16 *v) {
    (void)s;
    (void)v;
}

void state_u32(StateIO *s, u32 *v) {
    (void)s;
    (void)v;
}

void state_u64(StateIO *s, u64 *v) {
    (void)s;
    (void)v;
}

void state_bool(StateIO *s, bool *v) {
    (void)s;
    (void)v;
}

void state_bytes(StateIO *s, void *data, u32 len) {
    (void)s;
    (void)data;
    (void)len;
}

void state_skip(StateIO *s, u32 len) {
    (void)s;
    (void)len;
}

PsxResult psx_state_save(PsxSystem *sys, StateIO *st) {
    (void)sys;
    (void)st;
    return PSX_ERR_NOT_IMPL;
}

PsxResult psx_state_load(PsxSystem *sys, StateIO *st) {
    (void)sys;
    (void)st;
    return PSX_ERR_NOT_IMPL;
}
