#include "periph_internal.h"
#include "psx3ds/arena.h"
#include "psx3ds/bus.h"
#include "psx3ds/state.h"

#include <string.h>

/* SIO_STAT bit definitions (0x1F801044) */
#define SIO_STAT_TX_RDY1   (1u << 0) /* TX FIFO ready for next byte */
#define SIO_STAT_RX_RDY    (1u << 1) /* RX FIFO not empty */
#define SIO_STAT_TX_RDY2   (1u << 2) /* TX shift register idle */
#define SIO_STAT_PARITY    (1u << 3)
#define SIO_STAT_OVERRUN   (1u << 4)
#define SIO_STAT_FRAMING   (1u << 5)
#define SIO_STAT_ACK       (1u << 7) /* /ACK (DSR) input level: 1 = asserted low */
#define SIO_STAT_IRQ       (1u << 9) /* Latched IRQ7 request */

/* SIO_CTRL bit definitions (0x1F80104A) */
#define SIO_CTRL_TXEN      (1u << 0)
#define SIO_CTRL_DTR       (1u << 1) /* /CS output: 1 = device selected */
#define SIO_CTRL_RXEN      (1u << 2)
#define SIO_CTRL_ACK       (1u << 4) /* Write-only: clear IRQ & error flags */
#define SIO_CTRL_RESET     (1u << 6) /* Write-only: soft-reset SIO0 */
#define SIO_CTRL_TX_IRQ_EN (1u << 10)
#define SIO_CTRL_RX_IRQ_EN (1u << 11)
#define SIO_CTRL_ACK_IRQ   (1u << 12) /* /ACK (DSR) interrupt enable */
#define SIO_CTRL_PORT_SEL  (1u << 13) /* 0 = Slot 1, 1 = Slot 2 */

static void sio0_rx_push(PsxSio *sio, u8 val) {
    u8 count = (u8)(sio->rx_w - sio->rx_r);
    if (count < 8u) {
        sio->rx_fifo[sio->rx_w & 7u] = val;
        sio->rx_w++;
    } else {
        sio->rx_fifo[(sio->rx_w - 1u) & 7u] = val;
        sio->stat |= SIO_STAT_OVERRUN;
    }
    sio->stat |= SIO_STAT_RX_RDY;
}

static u8 sio0_rx_pop(PsxSio *sio) {
    if (sio->rx_r == sio->rx_w) {
        sio->stat &= ~SIO_STAT_RX_RDY;
        return 0xFFu;
    }
    u8 val = sio->rx_fifo[sio->rx_r & 7u];
    sio->rx_r++;
    if (sio->rx_r == sio->rx_w) {
        sio->rx_r = 0u;
        sio->rx_w = 0u;
        sio->stat &= ~SIO_STAT_RX_RDY;
    }
    return val;
}

static void sio0_deselect(PsxSystem *sys) {
    PsxSio *sio = &sys->sio;
    if (sio->selected && sio->port < 2u) {
        joypad_reset_port(sys, sio->port);
        memcard_reset_transfer(&sio->card[sio->port]);
    }
    sio->selected   = false;
    sio->device     = 0u;
    sio->seq        = 0u;
    sio->tx_pending = false;
    sio->stat &= ~SIO_STAT_ACK;
    sched_cancel(sys, EV_SIO0);
}

static void sio0_soft_reset(PsxSystem *sys) {
    PsxSio *sio = &sys->sio;
    sio0_deselect(sys);
    joypad_reset_port(sys, 0u);
    joypad_reset_port(sys, 1u);
    memcard_reset_transfer(&sio->card[0]);
    memcard_reset_transfer(&sio->card[1]);

    sio->mode    = 0x0000u;
    sio->ctrl    = 0x0000u;
    sio->baud    = 0x0088u;
    sio->stat    = SIO_STAT_TX_RDY1 | SIO_STAT_TX_RDY2;
    sio->tx_data = 0xFFu;
    memset(sio->rx_fifo, 0xFF, sizeof(sio->rx_fifo));
    sio->rx_r = 0u;
    sio->rx_w = 0u;
    sio->port = 0u;
}

static void sio0_write_ctrl(PsxSystem *sys, u16 val) {
    PsxSio *sio = &sys->sio;

    if ((val & SIO_CTRL_RESET) != 0u) {
        sio0_soft_reset(sys);
        return;
    }

    if ((val & SIO_CTRL_ACK) != 0u) {
        sio->stat &= ~(SIO_STAT_IRQ | SIO_STAT_ACK |
                       SIO_STAT_PARITY | SIO_STAT_OVERRUN | SIO_STAT_FRAMING);
    }

    u8   new_port = ((val & SIO_CTRL_PORT_SEL) != 0u) ? 1u : 0u;
    bool new_dtr  = ((val & SIO_CTRL_DTR) != 0u);

    if (!new_dtr) {
        sio0_deselect(sys);
        sio->port = new_port;
    } else {
        if (!sio->selected || new_port != sio->port) {
            if (sio->selected && sio->port < 2u) {
                joypad_reset_port(sys, sio->port);
                memcard_reset_transfer(&sio->card[sio->port]);
            }
            sio->selected   = true;
            sio->port       = new_port;
            sio->device     = 0u;
            sio->seq        = 0u;
            sio->tx_pending = false;
            sio->stat &= ~SIO_STAT_ACK;
            sched_cancel(sys, EV_SIO0);
            joypad_reset_port(sys, new_port);
            memcard_reset_transfer(&sio->card[new_port]);
        }
    }

    sio->ctrl = (u16)(val & ~(SIO_CTRL_ACK | SIO_CTRL_RESET));
}

static void sio0_transfer_byte(PsxSystem *sys, u8 tx) {
    PsxSio *sio = &sys->sio;
    sio->tx_data = tx;

    /* Must have DTR asserted or TXEN enabled to clock the bus */
    if (!sio->selected && (sio->ctrl & SIO_CTRL_TXEN) == 0u) {
        return;
    }

    u8   rx   = 0xFFu;
    bool more = false;

    if (sio->selected && sio->port < 2u) {
        u8 seq = sio->seq;
        if (seq == 0u) {
            if (tx == 0x01u) {
                sio->device = 1u; /* Joypad */
                rx          = joypad_transfer_byte(sys, sio->port, 0u, tx, &more);
            } else if (tx == 0x81u) {
                sio->device = 2u; /* Memory Card */
                rx          = memcard_transfer_byte(&sio->card[sio->port], 0u, tx, &more);
            } else {
                sio->device = 0u;
                rx          = 0xFFu;
                more        = false;
            }
            if (!more) {
                sio->device = 0u;
            }
        } else if (sio->device == 1u) {
            rx = joypad_transfer_byte(sys, sio->port, seq, tx, &more);
            if (!more) {
                sio->device = 0u;
            }
        } else if (sio->device == 2u) {
            rx = memcard_transfer_byte(&sio->card[sio->port], seq, tx, &more);
            if (!more) {
                sio->device = 0u;
            }
        }

        if (sio->seq < 250u) {
            sio->seq++;
        }
    }

    sio0_rx_push(sio, rx);
    sio->stat |= (SIO_STAT_TX_RDY1 | SIO_STAT_TX_RDY2);
    sio->stat &= ~SIO_STAT_ACK;

    u32 baud_cycles = (u32)(sio->baud ? sio->baud : 0x0088u) * 8u;
    if (baud_cycles < 64u) {
        baud_cycles = 64u;
    }

    if (more) {
        sio->tx_pending = true;
        sched_schedule(sys, EV_SIO0, baud_cycles + PSX_SIO0_ACK_DELAY_CYCLES);
    } else {
        sio->tx_pending = false;
        if ((sio->ctrl & (SIO_CTRL_TX_IRQ_EN | SIO_CTRL_RX_IRQ_EN)) != 0u) {
            sched_schedule(sys, EV_SIO0, baud_cycles);
        } else {
            sched_cancel(sys, EV_SIO0);
        }
    }
}

void sio0_event_cb(PsxSystem *sys, u32 late_cycles) {
    (void)late_cycles;
    if (!sys) {
        return;
    }
    PsxSio *sio = &sys->sio;

    if (sio->tx_pending) {
        /* Leading edge of peripheral /ACK pulse */
        sio->tx_pending = false;
        sio->stat |= SIO_STAT_ACK;

        if ((sio->ctrl & (SIO_CTRL_ACK_IRQ | SIO_CTRL_RX_IRQ_EN | SIO_CTRL_TX_IRQ_EN)) != 0u) {
            if ((sio->stat & SIO_STAT_IRQ) == 0u) {
                sio->stat |= SIO_STAT_IRQ;
                irq_raise(sys, IRQ_SIO0);
            }
        }
        /* Schedule trailing edge to release /ACK input line */
        sched_schedule(sys, EV_SIO0, PSX_SIO0_ACK_DELAY_CYCLES);
    } else if ((sio->stat & SIO_STAT_ACK) != 0u) {
        /* Trailing edge of /ACK pulse; SIO_STAT_IRQ remains latched until CTRL.ACK */
        sio->stat &= ~SIO_STAT_ACK;
    } else if ((sio->ctrl & (SIO_CTRL_TX_IRQ_EN | SIO_CTRL_RX_IRQ_EN)) != 0u) {
        if ((sio->stat & SIO_STAT_IRQ) == 0u) {
            sio->stat |= SIO_STAT_IRQ;
            irq_raise(sys, IRQ_SIO0);
        }
    }
}

u32 sio0_mmio_read(PsxSystem *sys, u32 addr, u32 width) {
    if (!sys) {
        return 0u;
    }
    PsxSio *sio = &sys->sio;
    u32     off = addr - PSX_REG_SIO0_BASE;

    switch (off) {
    case 0x00u: /* 0x1F801040 SIO_RX_DATA */ {
        u8 b0 = sio0_rx_pop(sio);
        if (width == 1u) {
            return (u32)b0;
        }
        /* Preview remaining FIFO bytes (or 0xFF) in upper bytes */
        u8 b1 = (sio->rx_r != sio->rx_w) ? sio->rx_fifo[sio->rx_r & 7u] : 0xFFu;
        if (width == 2u) {
            return (u32)b0 | ((u32)b1 << 8);
        }
        return (u32)b0 | ((u32)b1 << 8) | 0xFFFF0000u;
    }

    case 0x04u: /* 0x1F801044 SIO_STAT */ {
        u32 timer = (u32)(sio->baud ? sio->baud : 0x0088u) & 0xFFFFu;
        u32 st    = (sio->stat & 0x07FFu) | (timer << 11);
        return (width == 2u) ? (st & 0xFFFFu) : st;
    }

    case 0x06u: /* High halfword of SIO_STAT */
        return ((u32)(sio->baud ? sio->baud : 0x0088u) & 0xFFFFu) >> 5;

    case 0x08u: /* 0x1F801048 SIO_MODE (+ 0x104A SIO_CTRL on 32-bit read) */
        if (width == 4u) {
            return (u32)sio->mode | ((u32)sio->ctrl << 16);
        }
        return (width == 1u) ? (u32)(sio->mode & 0xFFu) : (u32)sio->mode;

    case 0x0Au: /* 0x1F80104A SIO_CTRL */
        return (width == 1u) ? (u32)(sio->ctrl & 0xFFu) : (u32)sio->ctrl;

    case 0x0Bu: /* High byte of SIO_CTRL */
        return (u32)(sio->ctrl >> 8);

    case 0x0Cu: /* 0x1F80104C reserved (+ 0x104E SIO_BAUD on 32-bit read) */
        return (width == 4u) ? ((u32)sio->baud << 16) : 0u;

    case 0x0Eu: /* 0x1F80104E SIO_BAUD */
        return (width == 1u) ? (u32)(sio->baud & 0xFFu) : (u32)sio->baud;

    default:
        return 0xFFu;
    }
}

void sio0_mmio_write(PsxSystem *sys, u32 addr, u32 val, u32 width) {
    if (!sys) {
        return;
    }
    PsxSio *sio = &sys->sio;
    u32     off = addr - PSX_REG_SIO0_BASE;

    switch (off) {
    case 0x00u: /* 0x1F801040 SIO_TX_DATA */
        sio0_transfer_byte(sys, (u8)(val & 0xFFu));
        break;

    case 0x04u: /* 0x1F801044 SIO_STAT (read-only) */
        break;

    case 0x08u: /* 0x1F801048 SIO_MODE */
        sio->mode = (u16)(val & 0x01FFu);
        if (width == 4u) {
            sio0_write_ctrl(sys, (u16)(val >> 16));
        }
        break;

    case 0x0Au: /* 0x1F80104A SIO_CTRL */
        if (width == 1u) {
            u16 merged = (u16)((sio->ctrl & 0xFF00u) | (val & 0x00FFu));
            sio0_write_ctrl(sys, merged);
        } else {
            sio0_write_ctrl(sys, (u16)val);
        }
        break;

    case 0x0Bu: /* High byte of SIO_CTRL */ {
        u16 merged = (u16)((sio->ctrl & 0x00FFu) | ((val & 0x00FFu) << 8));
        sio0_write_ctrl(sys, merged);
        break;
    }

    case 0x0Cu:
        if (width == 4u) {
            sio->baud = (u16)(val >> 16);
        }
        break;

    case 0x0Eu: /* 0x1F80104E SIO_BAUD */
        sio->baud = (u16)val;
        break;

    default:
        break;
    }
}

/* Minimal SIO1 (0x1F801050..0x1F80105F) serial port stub so link-cable polls never hang */
static u32 sio1_mmio_read(PsxSystem *sys, u32 addr, u32 width) {
    (void)sys;
    u32 off = addr - PSX_REG_SIO1_BASE;
    if (off == 0x04u) {
        u32 st = SIO_STAT_TX_RDY1 | SIO_STAT_TX_RDY2 | (0x0088u << 11);
        return (width == 2u) ? (st & 0xFFFFu) : st;
    }
    if (off == 0x0Eu) {
        return 0x0088u;
    }
    return 0u;
}

static void sio1_mmio_write(PsxSystem *sys, u32 addr, u32 val, u32 width) {
    (void)sys;
    (void)addr;
    (void)val;
    (void)width;
}

void sio_init(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    PsxSio *sio = &sys->sio;
    memset(sio, 0, sizeof(*sio));

    u8 *mc0_buf = (u8 *)arena_alloc(&sys->arena, PSX_MEMCARD_BYTES, 32u);
    u8 *mc1_buf = (u8 *)arena_alloc(&sys->arena, PSX_MEMCARD_BYTES, 32u);

    memcard_init_slot(&sio->card[0], mc0_buf, sys->cfg.memcard_path[0]);
    memcard_init_slot(&sio->card[1], mc1_buf, sys->cfg.memcard_path[1]);

    /* Default controller state: Port 0 Digital Pad connected, Port 1 unplugged */
    if (sys->pads[0].buttons == 0u && sys->pads[0].connected == 0u) {
        sys->pads[0].buttons     = 0xFFFFu;
        sys->pads[0].lx          = 0x80u;
        sys->pads[0].ly          = 0x80u;
        sys->pads[0].rx          = 0x80u;
        sys->pads[0].ry          = 0x80u;
        sys->pads[0].analog_mode = 0u;
        sys->pads[0].connected   = 1u;
    }
    if (sys->pads[1].buttons == 0u && sys->pads[1].connected == 0u) {
        sys->pads[1].buttons     = 0xFFFFu;
        sys->pads[1].lx          = 0x80u;
        sys->pads[1].ly          = 0x80u;
        sys->pads[1].rx          = 0x80u;
        sys->pads[1].ry          = 0x80u;
        sys->pads[1].analog_mode = 0u;
        sys->pads[1].connected   = 0u;
    }

    bus_mmio_register(sys, PSX_REG_SIO0_BASE, 0x10u, sio0_mmio_read, sio0_mmio_write);
    bus_mmio_register(sys, PSX_REG_SIO1_BASE, 0x10u, sio1_mmio_read, sio1_mmio_write);
    sched_set_handler(&sys->sched, EV_SIO0, sio0_event_cb);

    sio_reset(sys);
}

void sio_reset(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    PsxSio *sio = &sys->sio;
    sio->pad_mode[0]   = 0u;
    sio->pad_mode[1]   = 0u;
    sio->pad_config[0] = 0u;
    sio->pad_config[1] = 0u;
    sio0_soft_reset(sys);
}

void sio_state(PsxSystem *sys, StateIO *st) {
    if (!sys || !st) {
        return;
    }
    PsxSio *sio = &sys->sio;

    state_u16(st, &sio->mode);
    state_u16(st, &sio->ctrl);
    state_u16(st, &sio->baud);
    state_u32(st, &sio->stat);
    state_u8(st, &sio->tx_data);
    state_bytes(st, sio->rx_fifo, 8u);
    state_u8(st, &sio->rx_r);
    state_u8(st, &sio->rx_w);
    state_bool(st, &sio->tx_pending);
    state_bool(st, &sio->selected);
    state_u8(st, &sio->device);
    state_u8(st, &sio->port);
    state_u8(st, &sio->seq);
    state_bytes(st, sio->pad_mode, 2u);
    state_bytes(st, sio->pad_config, 2u);

    for (u32 i = 0u; i < 2u; i++) {
        PsxMemcard *mc = &sio->card[i];
        state_u8(st, &mc->flag);
        state_u8(st, &mc->state);
        state_u8(st, &mc->cmd);
        state_u8(st, &mc->sector_hi);
        state_u8(st, &mc->sector_lo);
        state_u8(st, &mc->checksum);
        state_u32(st, &mc->pos);
        state_bool(st, &mc->dirty);
        state_bool(st, &mc->present);
        if (mc->data) {
            state_bytes(st, mc->data, PSX_MEMCARD_BYTES);
            if (!st->writing && mc->path[0] != '\0') {
                mc->dirty = true;
            }
        }
    }
}

void sio_shutdown(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    PsxSio *sio = &sys->sio;
    for (u32 i = 0u; i < 2u; i++) {
        if (sio->card[i].dirty && sio->card[i].path[0] != '\0') {
            (void)memcard_flush_file(&sio->card[i]);
        }
    }
}

void sio_set_pad(PsxSystem *sys, int port, const PsxPadState *pad) {
    if (!sys || !pad || port < 0 || port >= 2) {
        return;
    }
    sys->pads[port] = *pad;
}
