#ifndef PSX3DS_SIO_H
#define PSX3DS_SIO_H

#include "psx3ds/hw_regs.h"
#include "psx3ds/platform.h"
#include "psx3ds/types.h"

#define PSX_MEMCARD_BYTES (128u * 1024u)

typedef struct PsxMemcard {
    u8   *data; /* 128 KiB */
    u8    flag, state, cmd, sector_hi, sector_lo, checksum;
    u32   pos;
    bool  dirty, present;
    char  path[256];
} PsxMemcard;

typedef struct PsxSio {
    u16        mode, ctrl, baud;
    u32        stat;
    u8         tx_data, rx_fifo[8];
    u8         rx_r, rx_w;
    bool       tx_pending, selected;
    u8         device; /* 0 = none, 1 = pad, 2 = card */
    u8         port, seq;
    u8         pad_mode[2], pad_config[2];
    PsxMemcard card[2];
} PsxSio;

void sio_init(PsxSystem *sys);
void sio_reset(PsxSystem *sys);
void sio_state(PsxSystem *sys, StateIO *st);
void sio_shutdown(PsxSystem *sys);

void sio_set_pad(PsxSystem *sys, int port, const PsxPadState *pad);

#endif /* PSX3DS_SIO_H */
