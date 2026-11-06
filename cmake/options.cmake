option(PSX3DS_ENABLE_ASAN   "Enable ASan/UBSan on host x86_64 builds" OFF)
option(PSX3DS_ENABLE_JIT    "Build ARM11v6K dynamic recompiler"       ON)
option(PSX3DS_ENABLE_HW_GPU "Build citro3d hardware GPU backend"      ON)
option(PSX3DS_SMALL_MEM     "Force Old-3DS memory budget profile"     OFF)
set(PSX3DS_CYCLE_MULT_DEFAULT 448 CACHE STRING
    "Default 8.8 fixed-point CPU cycles per MIPS instruction (448 = 1.75)")
