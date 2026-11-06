#include "psx3ds/irq.h"

void irq_init(PsxSystem *sys) {
    (void)sys;
}

void irq_reset(PsxSystem *sys) {
    (void)sys;
}

void irq_state(PsxSystem *sys, StateIO *st) {
    (void)sys;
    (void)st;
}

void irq_shutdown(PsxSystem *sys) {
    (void)sys;
}

void irq_raise(PsxSystem *sys, u32 irq_bit) {
    (void)sys;
    (void)irq_bit;
}
