#include <3ds.h>
#include <citro3d.h>

#include "psx3ds/system.h"

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    bool is_n3ds = false;
    APT_CheckNew3DS(&is_n3ds);
    if (is_n3ds) {
        osSetSpeedupEnable(true);
    }

    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);

    PsxConfig cfg;
    psx_config_defaults(&cfg, is_n3ds);
    plat_log("psx3ds 3DS skeleton initialized (n3ds=%d)\n", (int)is_n3ds);

    C3D_Fini();
    gfxExit();
    return 0;
}
