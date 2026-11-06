cmake_minimum_required(VERSION 3.16)
project(psx3ds LANGUAGES C ASM)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/options.cmake")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON) # -std=gnu11

# -----------------------------------------------------------------------------
# Target classification (mirrors include/psx3ds/config.h)
# -----------------------------------------------------------------------------
if(NINTENDO_3DS)
    set(PSX3DS_IS_3DS TRUE)
    set(PSX3DS_IS_ARMV6K TRUE)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|armv6)")
    set(PSX3DS_IS_3DS FALSE)
    set(PSX3DS_IS_ARMV6K TRUE)
else()
    set(PSX3DS_IS_3DS FALSE)
    set(PSX3DS_IS_ARMV6K FALSE)
endif()

if(NOT PSX3DS_IS_ARMV6K)
    set(PSX3DS_ENABLE_JIT OFF CACHE BOOL "ARM11 dynarec is ARM32-only" FORCE)
endif()
if(NOT PSX3DS_IS_3DS)
    set(PSX3DS_ENABLE_HW_GPU OFF CACHE BOOL "citro3d backend is 3DS-only" FORCE)
endif()

# -----------------------------------------------------------------------------
# Compiler & linker flags (per ARCHITECTURE.md §2)
# -----------------------------------------------------------------------------
set(PSX3DS_COMMON_C_FLAGS
    -fno-strict-aliasing
    -fno-common
    -Wall
    -Wextra
    -Werror=implicit-function-declaration
    -Werror=return-type
)

set(PSX3DS_COMPILE_DEFS
    "PSX3DS_CYCLE_MULT_DEFAULT=${PSX3DS_CYCLE_MULT_DEFAULT}"
)
if(NOT PSX3DS_ENABLE_JIT)
    list(APPEND PSX3DS_COMPILE_DEFS PSX3DS_DISABLE_JIT=1)
endif()
if(NOT PSX3DS_ENABLE_HW_GPU)
    list(APPEND PSX3DS_COMPILE_DEFS PSX3DS_DISABLE_HW_GPU=1)
endif()
if(PSX3DS_SMALL_MEM)
    list(APPEND PSX3DS_COMPILE_DEFS PSX3DS_FORCE_SMALL_MEM=1)
endif()

if(PSX3DS_IS_3DS)
    # T1: Nintendo 3DS (.elf / .3dsx)
    set(PSX3DS_TARGET_C_FLAGS
        -O3
        -flto=auto
        -fomit-frame-pointer
        -mfpu=vfpv2
        -marm
        -fdata-sections
    )
    set(PSX3DS_TARGET_ASM_FLAGS
        -mfpu=vfpv2
        -marm
    )
    set(PSX3DS_TARGET_LINK_FLAGS
        -flto=auto
        -Wl,--gc-sections
    )
    set(PSX3DS_TARGET_LIBS citro3d ctru m)
elseif(PSX3DS_IS_ARMV6K)
    # T2: ARM11v6K Linux QEMU harness
    set(PSX3DS_TARGET_C_FLAGS
        -O3
        -fomit-frame-pointer
        -march=armv6k
        -mtune=mpcore
        -mfpu=vfpv2
        -mfloat-abi=hard
        -marm
    )
    set(PSX3DS_TARGET_ASM_FLAGS
        -x assembler-with-cpp
        -march=armv6k
        -mtune=mpcore
        -mfpu=vfpv2
        -mfloat-abi=hard
        -marm
    )
    set(PSX3DS_TARGET_LINK_FLAGS -static)
    set(PSX3DS_TARGET_LIBS m pthread)
else()
    # T3: x86_64 host CLI & unit-test harness
    set(PSX3DS_TARGET_C_FLAGS -O2 -g)
    set(PSX3DS_TARGET_ASM_FLAGS "")
    set(PSX3DS_TARGET_LINK_FLAGS "")
    if(PSX3DS_ENABLE_ASAN)
        list(APPEND PSX3DS_TARGET_C_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer)
        list(APPEND PSX3DS_TARGET_LINK_FLAGS -fsanitize=address,undefined)
    endif()
    set(PSX3DS_TARGET_LIBS m pthread)
endif()

# -----------------------------------------------------------------------------
# PICA200 shader assembly & binary embedding (T1 only)
# -----------------------------------------------------------------------------
set(PSX3DS_SHADER_GENERATED_SRCS "")
if(PSX3DS_IS_3DS AND PSX3DS_ENABLE_HW_GPU)
    find_program(PSX3DS_PICASSO_EXE NAMES picasso HINTS "$ENV{DEVKITPRO}/tools/bin" REQUIRED)
    find_program(PSX3DS_BIN2S_EXE   NAMES bin2s   HINTS "$ENV{DEVKITPRO}/tools/bin" REQUIRED)

    set(PSX3DS_SHBIN_FILE "${CMAKE_CURRENT_BINARY_DIR}/psx_gpu.shbin")
    set(PSX3DS_SHBIN_HDR  "${CMAKE_CURRENT_BINARY_DIR}/psx_gpu_shbin.h")
    set(PSX3DS_SHBIN_ASM  "${CMAKE_CURRENT_BINARY_DIR}/psx_gpu_shbin.S")

    add_custom_command(
        OUTPUT "${PSX3DS_SHBIN_FILE}"
        COMMAND "${PSX3DS_PICASSO_EXE}" -o "${PSX3DS_SHBIN_FILE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_hw/psx_gpu.v.pica"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_hw/psx_gpu.v.pica"
        COMMENT "Assembling PICA200 vertex shader psx_gpu.shbin"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${PSX3DS_SHBIN_ASM}" "${PSX3DS_SHBIN_HDR}"
        COMMAND /bin/sh -c "\"${PSX3DS_BIN2S_EXE}\" -a 4 -H \"${PSX3DS_SHBIN_HDR}\" \"${PSX3DS_SHBIN_FILE}\" > \"${PSX3DS_SHBIN_ASM}\""
        DEPENDS "${PSX3DS_SHBIN_FILE}"
        COMMENT "Embedding psx_gpu.shbin via bin2s"
        VERBATIM
    )
    set_source_files_properties("${PSX3DS_SHBIN_ASM}" PROPERTIES LANGUAGE ASM)
    list(APPEND PSX3DS_SHADER_GENERATED_SRCS "${PSX3DS_SHBIN_ASM}" "${PSX3DS_SHBIN_HDR}")
endif()

# -----------------------------------------------------------------------------
# Unified core OBJECT library (all 6 slices — no circular archive order bugs)
# -----------------------------------------------------------------------------
set(PSX3DS_CORE_SRCS
    # Slice A: core
    src/core/arena.c
    src/core/bus.c
    src/core/fastmem.c
    src/core/scheduler.c
    src/core/irq.c
    src/core/dma.c
    src/core/timers.c
    src/core/cpu_interp.c
    src/core/cop0.c
    src/core/gte.c
    src/core/gte_tables.c

    # Slice B: block cache & cached interpreter (all targets)
    src/jit/jit_block.c
    src/jit/cpu_cached.c

    # Slice C: GP0/GP1 front-end + software rasterizer
    src/gpu/gpu.c
    src/gpu/gpu_soft.c
    src/gpu/gpu_soft_span.c
    src/gpu/gpu_vram.c

    # Slice E: peripherals
    src/periph/spu.c
    src/periph/spu_adpcm.c
    src/periph/spu_reverb.c
    src/periph/spu_xa.c
    src/periph/cdrom.c
    src/periph/cdrom_chd.c
    src/periph/cdrom_image.c
    src/periph/cdrom_readahead.c
    src/periph/mdec.c
    src/periph/sio0.c
    src/periph/joypad.c
    src/periph/memcard.c

    # Slice F: system, BIOS/HLE, PS-EXE, savestate, stats, bench
    src/sys/system.c
    src/sys/bios.c
    src/sys/hle_bios.c
    src/sys/psexe.c
    src/sys/savestate.c
    src/sys/stats.c
    src/sys/bench.c
)

if(PSX3DS_ENABLE_JIT)
    list(APPEND PSX3DS_CORE_SRCS
        src/jit/arm_jit.c
        src/jit/arm_emit.c
        src/jit/jit_regalloc.c
        src/jit/jit_gte.c
        src/jit/jit_thunks.S
    )
endif()

if(PSX3DS_IS_ARMV6K)
    list(APPEND PSX3DS_CORE_SRCS
        src/gpu/gpu_soft_span_armv6.S
        src/periph/spu_adpcm_armv6.S
        src/periph/mdec_idct_armv6.S
    )
endif()

if(PSX3DS_IS_3DS AND PSX3DS_ENABLE_HW_GPU)
    list(APPEND PSX3DS_CORE_SRCS
        src/gpu_hw/gpu_citro3d.c
        src/gpu_hw/gpu_texcache.c
        src/gpu_hw/gpu_morton.c
        ${PSX3DS_SHADER_GENERATED_SRCS}
    )
else()
    list(APPEND PSX3DS_CORE_SRCS
        src/gpu_hw/gpu_hw_stub.c
    )
endif()

if(PSX3DS_IS_3DS)
    list(APPEND PSX3DS_CORE_SRCS
        src/platform/3ds/plat_3ds.c
        src/platform/3ds/jit_mem_3ds.c
        src/platform/3ds/audio_ndsp.c
        src/platform/3ds/ui_bottom.c
        src/platform/3ds/font8x8.c
        src/platform/3ds/threads_3ds.c
    )
else()
    list(APPEND PSX3DS_CORE_SRCS
        src/jit/jit_mem_host.c
        src/platform/host/plat_host.c
        src/platform/host/video_ppm.c
        src/platform/host/audio_wav.c
    )
endif()

add_library(psx3ds_core_objs OBJECT ${PSX3DS_CORE_SRCS})
target_include_directories(psx3ds_core_objs PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}"
)
if(PSX3DS_IS_3DS AND DEFINED DEVKITPRO)
    target_include_directories(psx3ds_core_objs SYSTEM PUBLIC
        "${DEVKITPRO}/libctru/include"
        "${DEVKITPRO}/portlibs/3ds/include"
    )
endif()
target_compile_definitions(psx3ds_core_objs PUBLIC ${PSX3DS_COMPILE_DEFS})
target_compile_options(psx3ds_core_objs PRIVATE
    $<$<COMPILE_LANGUAGE:C>:${PSX3DS_COMMON_C_FLAGS} ${PSX3DS_TARGET_C_FLAGS}>
    $<$<COMPILE_LANGUAGE:ASM>:${PSX3DS_TARGET_ASM_FLAGS}>
)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/chd.cmake")

# Helper function to configure any executable linking psx3ds_core_objs
function(psx3ds_configure_executable target_name)
    target_include_directories(${target_name} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_BINARY_DIR}"
    )
    target_compile_definitions(${target_name} PRIVATE ${PSX3DS_COMPILE_DEFS})
    target_compile_options(${target_name} PRIVATE
        $<$<COMPILE_LANGUAGE:C>:${PSX3DS_COMMON_C_FLAGS} ${PSX3DS_TARGET_C_FLAGS}>
        $<$<COMPILE_LANGUAGE:ASM>:${PSX3DS_TARGET_ASM_FLAGS}>
    )
    target_link_options(${target_name} PRIVATE ${PSX3DS_TARGET_LINK_FLAGS})
    target_link_libraries(${target_name} PRIVATE ${PSX3DS_TARGET_LIBS})
endfunction()

# -----------------------------------------------------------------------------
# Primary Executables
# -----------------------------------------------------------------------------
if(PSX3DS_IS_3DS)
    add_executable(psx3ds
        src/platform/3ds/main_3ds.c
        $<TARGET_OBJECTS:psx3ds_core_objs>
    )
    psx3ds_configure_executable(psx3ds)

    ctr_generate_smdh(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/psx3ds.smdh"
        NAME "psx3ds"
        DESCRIPTION "PlayStation 1 Emulator for Nintendo 3DS"
        AUTHOR "psx3ds"
    )
    ctr_create_3dsx(psx3ds
        SMDH "${CMAKE_CURRENT_BINARY_DIR}/psx3ds.smdh"
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/psx3ds.3dsx"
    )
else()
    add_executable(psx3ds-cli
        src/platform/host/main_cli.c
        $<TARGET_OBJECTS:psx3ds_core_objs>
    )
    psx3ds_configure_executable(psx3ds-cli)

    add_executable(psx3ds-bench
        src/platform/host/main_cli.c
        $<TARGET_OBJECTS:psx3ds_core_objs>
    )
    psx3ds_configure_executable(psx3ds-bench)
    target_compile_definitions(psx3ds-bench PRIVATE PSX3DS_BENCH_DEFAULT=1)
endif()

# -----------------------------------------------------------------------------
# Unit Tests (CTest; also cross-compiled on T1/T2 to validate ABI & offsets)
# -----------------------------------------------------------------------------
enable_testing()

function(psx3ds_add_unit_test test_name test_src)
    add_executable(${test_name}
        ${test_src}
        $<TARGET_OBJECTS:psx3ds_core_objs>
    )
    psx3ds_configure_executable(${test_name})
    if(NOT PSX3DS_IS_3DS)
        add_test(NAME ${test_name} COMMAND ${test_name})
    endif()
endfunction()

file(GLOB PSX3DS_UNIT_TEST_SRCS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/test_*.c"
)
list(SORT PSX3DS_UNIT_TEST_SRCS)
foreach(_test_src IN LISTS PSX3DS_UNIT_TEST_SRCS)
    get_filename_component(_test_name "${_test_src}" NAME_WE)
    psx3ds_add_unit_test("${_test_name}" "${_test_src}")
endforeach()
