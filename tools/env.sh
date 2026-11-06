#!/usr/bin/env bash
# =============================================================================
# psx3ds Toolchain & Verification Environment Setup
# Usage:
#   source tools/env.sh
# =============================================================================

# Resolve repository root regardless of current working directory
if [ -n "${BASH_SOURCE[0]:-}" ]; then
  _PSX3DS_ENV_FILE="${BASH_SOURCE[0]}"
elif [ -n "${ZSH_VERSION:-}" ]; then
  # shellcheck disable=SC2296
  _PSX3DS_ENV_FILE="${(%):-%x}"
else
  _PSX3DS_ENV_FILE="$0"
fi

export PSX3DS_ROOT="$(cd "$(dirname "$_PSX3DS_ENV_FILE")/.." && pwd)"
export PSX3DS_SDK_ROOT="${PSX3DS_SDK_ROOT:-$HOME/.local/psx3ds-sdk}"

# 1. Official Nintendo 3DS Homebrew SDK (devkitPro / devkitARM / libctru / citro3d)
export DEVKITPRO="$PSX3DS_SDK_ROOT/devkitpro"
export DEVKITARM="$DEVKITPRO/devkitARM"
export CTR_CMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/3DS.cmake"

# Canonical 3DS ARM11 MPCore bare-metal compile flags (matches 3DS.cmake / 3ds_rules)
export CTR_ARCH_FLAGS="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft"
export CTR_CFLAGS="$CTR_ARCH_FLAGS -mword-relocations -ffunction-sections -fdata-sections -D__3DS__"

# 2. ARM11v6K + VFPv2 Linux Cross-Toolchain & QEMU User-Mode Harness
# Pure ARMv6 + VFPv2 glibc sysroot so `qemu-arm -cpu arm11mpcore` runs full C/C++
# + mmap(PROT_EXEC) JIT + ARMv6 SIMD while strictly trapping illegal ARMv7 instructions.
export ARM11_SYSROOT="$PSX3DS_SDK_ROOT/armv6-eabihf/arm-buildroot-linux-gnueabihf/sysroot"
export QEMU_LD_PREFIX="$ARM11_SYSROOT"
export ARM11_LINUX_CC="$PSX3DS_SDK_ROOT/bin/arm-linux-gnueabihf-gcc"
export ARM11_LINUX_CXX="$PSX3DS_SDK_ROOT/bin/arm-linux-gnueabihf-g++"
export ARM11_LINUX_CFLAGS="-march=armv6k -mtune=mpcore -mfpu=vfpv2 -mfloat-abi=hard -marm"
export QEMU_CPU="arm11mpcore"
export QEMU_ARM11_CMD="qemu-arm -cpu arm11mpcore"

# 3. PS1 Open-Source Test ROMs & OpenBIOS Paths
export PSX3DS_ROMS_DIR="$PSX3DS_ROOT/tests/roms"
export PSX3DS_OPENBIOS="$PSX3DS_ROMS_DIR/bios/openbios.bin"

# Idempotently prepend SDK directories to PATH
_psx3ds_prepend_path() {
  case ":$PATH:" in
    *":$1:"*) ;;
    *) PATH="$1:$PATH" ;;
  esac
}

_psx3ds_prepend_path "$PSX3DS_SDK_ROOT/armv6-eabihf/bin"
_psx3ds_prepend_path "$DEVKITARM/bin"
_psx3ds_prepend_path "$DEVKITPRO/tools/bin"
_psx3ds_prepend_path "$PSX3DS_SDK_ROOT/bin"
export PATH

unset _PSX3DS_ENV_FILE
unset -f _psx3ds_prepend_path
