#ifndef PSX3DS_IRQ_H
#define PSX3DS_IRQ_H

#include "psx3ds/hw_regs.h"
#include "psx3ds/types.h"

typedef enum PsxIrqLine {
    IRQ_VBLANK = PSX_IRQ_VBLANK,
    IRQ_GPU    = PSX_IRQ_GPU,
    IRQ_CDROM  = PSX_IRQ_CDROM,
    IRQ_DMA    = PSX_IRQ_DMA,
    IRQ_TMR0   = PSX_IRQ_TMR0,
    IRQ_TMR1   = PSX_IRQ_TMR1,
    IRQ_TMR2   = PSX_IRQ_TMR2,
    IRQ_SIO0   = PSX_IRQ_SIO0,
    IRQ_SIO1   = PSX_IRQ_SIO1,
    IRQ_SPU    = PSX_IRQ_SPU,
    IRQ_PIO    = PSX_IRQ_PIO
} PsxIrqLine;

typedef struct PsxIrq {
    u32 stat; /* 0x1F801070 I_STAT (write-0-to-clear) */
    u32 mask; /* 0x1F801074 I_MASK */
} PsxIrq;

void irq_init(PsxSystem *sys);
void irq_reset(PsxSystem *sys);
void irq_state(PsxSystem *sys, StateIO *st);
void irq_shutdown(PsxSystem *sys);

void irq_raise(PsxSystem *sys, u32 irq_bit);

#endif /* PSX3DS_IRQ_H */
