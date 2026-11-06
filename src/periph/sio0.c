#include "psx3ds/sio.h"

void sio_init(PsxSystem *sys) {
    (void)sys;
}

void sio_reset(PsxSystem *sys) {
    (void)sys;
}

void sio_state(PsxSystem *sys, StateIO *st) {
    (void)sys;
    (void)st;
}

void sio_shutdown(PsxSystem *sys) {
    (void)sys;
}

void sio_set_pad(PsxSystem *sys, int port, const PsxPadState *pad) {
    (void)sys;
    (void)port;
    (void)pad;
}
