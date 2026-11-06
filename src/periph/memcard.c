#include "psx3ds/sio.h"

u8 memcard_transfer_stub(PsxMemcard *mc, u8 tx);

u8 memcard_transfer_stub(PsxMemcard *mc, u8 tx) {
    (void)mc;
    (void)tx;
    return 0xFFu;
}
