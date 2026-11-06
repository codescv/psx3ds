#include "psx3ds/sio.h"

u8 joypad_transfer_stub(u8 tx);

u8 joypad_transfer_stub(u8 tx) {
    (void)tx;
    return 0xFFu;
}
