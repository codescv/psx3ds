# T2 Toolchain: ARM11v6K + VFPv2 Linux static cross-build for QEMU (-cpu arm11mpcore)
cmake_minimum_required(VERSION 3.16)

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(_psx3ds_sdk_default "$ENV{HOME}/.local/psx3ds-sdk")
if(DEFINED ENV{PSX3DS_SDK_ROOT})
    set(_psx3ds_sdk_default "$ENV{PSX3DS_SDK_ROOT}")
endif()

if(DEFINED ENV{ARM11_LINUX_CC} AND EXISTS "$ENV{ARM11_LINUX_CC}")
    set(CMAKE_C_COMPILER "$ENV{ARM11_LINUX_CC}")
else()
    find_program(_arm11_cc
        NAMES arm-linux-gnueabihf-gcc
        HINTS "${_psx3ds_sdk_default}/bin" "${_psx3ds_sdk_default}/armv6-eabihf/bin"
        REQUIRED
    )
    set(CMAKE_C_COMPILER "${_arm11_cc}")
endif()

set(CMAKE_ASM_COMPILER "${CMAKE_C_COMPILER}")

set(_arm11_arch_flags "-march=armv6k -mtune=mpcore -mfpu=vfpv2 -mfloat-abi=hard -marm")
set(CMAKE_C_FLAGS_INIT   "${_arm11_arch_flags}")
set(CMAKE_ASM_FLAGS_INIT "${_arm11_arch_flags} -x assembler-with-cpp")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")

find_program(PSX3DS_QEMU_ARM
    NAMES qemu-arm
    HINTS "${_psx3ds_sdk_default}/bin"
)
if(PSX3DS_QEMU_ARM)
    set(CMAKE_CROSSCOMPILING_EMULATOR "${PSX3DS_QEMU_ARM};-cpu;arm11mpcore")
endif()

set(CMAKE_FIND_ROOT_PATH "${_psx3ds_sdk_default}/armv6-eabihf/arm-buildroot-linux-gnueabihf/sysroot")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
