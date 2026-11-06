#include "psx3ds/system.h"

#include <stdio.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    PsxConfig cfg;
    psx_config_defaults(&cfg, true);
#if defined(PSX3DS_BENCH_DEFAULT)
    fprintf(stderr, "psx3ds-bench skeleton (PSX_ERR_NOT_IMPL)\n");
#else
    fprintf(stderr, "psx3ds-cli skeleton (PSX_ERR_NOT_IMPL)\n");
#endif
    return 1;
}
