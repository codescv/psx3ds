#include "psx3ds/cdrom.h"

struct CdImage {
    u32 sector_count;
};

bool cdrom_image_read_sector_stub(CdImage *img, u32 lba, u8 out[2352]);

bool cdrom_image_read_sector_stub(CdImage *img, u32 lba, u8 out[2352]) {
    (void)img;
    (void)lba;
    (void)out;
    return false;
}
