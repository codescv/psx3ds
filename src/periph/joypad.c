#include "periph_internal.h"

static u8 g_pad_cmd[2];

void joypad_reset_port(PsxSystem *sys, u32 port) {
    if (!sys || port >= 2u) {
        return;
    }
    g_pad_cmd[port] = 0u;
}

u8 joypad_transfer_byte(PsxSystem *sys, u32 port, u8 seq, u8 tx, bool *more) {
    if (more) {
        *more = false;
    }
    if (!sys || port >= 2u) {
        return 0xFFu;
    }

    const PsxPadState *pad = &sys->pads[port];
    PsxSio            *sio = &sys->sio;
    if (pad->connected == 0u) {
        return 0xFFu;
    }

    bool in_cfg    = (sio->pad_config[port] != 0u);
    bool is_analog = (sio->pad_mode[port] != 0u) || (pad->analog_mode != 0u);
    u8   id_lo     = in_cfg ? 0xF3u : (is_analog ? 0x73u : 0x41u);

    if (seq == 0u) {
        if (tx != 0x01u) {
            return 0xFFu;
        }
        g_pad_cmd[port] = 0u;
        if (more) {
            *more = true;
        }
        return 0xFFu;
    }

    if (seq == 1u) {
        g_pad_cmd[port] = tx;
        if (tx == 0x42u || tx == 0x43u) {
            if (more) {
                *more = true;
            }
            return id_lo;
        }
        if (in_cfg && (tx == 0x44u || tx == 0x45u || tx == 0x4Du)) {
            if (more) {
                *more = true;
            }
            return 0xF3u;
        }
        return 0xFFu;
    }

    u8 cmd = g_pad_cmd[port];

    if (seq == 2u) {
        if (more) {
            *more = true;
        }
        return 0x5Au;
    }

    switch (cmd) {
    case 0x42u: /* Read Buttons (+ Analog Axes in 0x73/0xF3 mode) */
        switch (seq) {
        case 3u:
            if (more) {
                *more = true;
            }
            return (u8)(pad->buttons & 0xFFu);
        case 4u:
            if (more) {
                *more = (is_analog || in_cfg);
            }
            return (u8)(pad->buttons >> 8);
        case 5u:
            if (more) {
                *more = true;
            }
            return pad->rx;
        case 6u:
            if (more) {
                *more = true;
            }
            return pad->ry;
        case 7u:
            if (more) {
                *more = true;
            }
            return pad->lx;
        case 8u:
            if (more) {
                *more = false;
            }
            return pad->ly;
        default:
            return 0xFFu;
        }

    case 0x43u: /* Enter / Exit DualShock Config Mode */
        switch (seq) {
        case 3u:
            sio->pad_config[port] = (tx & 1u);
            if (more) {
                *more = true;
            }
            return (u8)(pad->buttons & 0xFFu);
        case 4u:
            if (more) {
                *more = (is_analog || in_cfg);
            }
            return (u8)(pad->buttons >> 8);
        case 5u:
            if (more) {
                *more = true;
            }
            return is_analog ? pad->rx : 0x00u;
        case 6u:
            if (more) {
                *more = true;
            }
            return is_analog ? pad->ry : 0x00u;
        case 7u:
            if (more) {
                *more = true;
            }
            return is_analog ? pad->lx : 0x00u;
        case 8u:
            if (more) {
                *more = false;
            }
            return is_analog ? pad->ly : 0x00u;
        default:
            return 0xFFu;
        }

    case 0x44u: /* Set Analog Mode / LED (Config Mode) */
        if (seq == 3u) {
            sio->pad_mode[port] = (tx & 1u);
        }
        if (more) {
            *more = (seq < 8u);
        }
        return 0x00u;

    case 0x45u: /* Query DualShock Model / LED Status (Config Mode) */ {
        static const u8 k_model[6] = { 0x01u, 0x02u, 0x00u, 0x02u, 0x01u, 0x00u };
        u32 idx = (u32)seq - 3u;
        if (idx >= 6u) {
            return 0x00u;
        }
        if (more) {
            *more = (seq < 8u);
        }
        if (idx == 2u) {
            return (sio->pad_mode[port] != 0u) ? 0x01u : 0x00u;
        }
        return k_model[idx];
    }

    case 0x4Du: /* Map Rumble Motors (Config Mode) */ {
        static const u8 k_rumble[6] = { 0x00u, 0x01u, 0xFFu, 0xFFu, 0xFFu, 0xFFu };
        u32 idx = (u32)seq - 3u;
        if (idx >= 6u) {
            return 0xFFu;
        }
        if (more) {
            *more = (seq < 8u);
        }
        return k_rumble[idx];
    }

    default:
        return 0xFFu;
    }
}
