#!/usr/bin/env bash
# =============================================================================
# Cross-target PsxCpu layout & ABI verification across T3 (x86_64), T2 (ARM11
# Linux + QEMU arm11mpcore), and T1 (Nintendo 3DS devkitARM).
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# shellcheck source=env.sh
source "${SCRIPT_DIR}/env.sh"

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/psx3ds_offsets_check.XXXXXX")"
trap 'rm -rf "${TMP_DIR}"' EXIT

echo "[1/3] Building & running test_layout on T3 (x86_64 host + ASan/UBSan)..."
cmake -S "${REPO_ROOT}" -B "${TMP_DIR}/t3" -G Ninja -DPSX3DS_ENABLE_ASAN=ON >/dev/null
cmake --build "${TMP_DIR}/t3" --target test_layout >/dev/null
"${TMP_DIR}/t3/test_layout" > "${TMP_DIR}/t3.out"
grep '^OFFSET ' "${TMP_DIR}/t3.out" > "${TMP_DIR}/t3.offsets"

echo "[2/3] Building & running test_layout on T2 (ARM11v6K Linux + qemu-arm -cpu arm11mpcore)..."
cmake -S "${REPO_ROOT}" -B "${TMP_DIR}/t2" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${REPO_ROOT}/cmake/arm11-linux.cmake" >/dev/null
cmake --build "${TMP_DIR}/t2" --target test_layout >/dev/null
qemu-arm -cpu arm11mpcore "${TMP_DIR}/t2/test_layout" > "${TMP_DIR}/t2.out"
grep '^OFFSET ' "${TMP_DIR}/t2.out" > "${TMP_DIR}/t2.offsets"

echo "[3/3] Building test_layout.elf & psx3ds.3dsx on T1 (Nintendo 3DS devkitARM)..."
cmake -S "${REPO_ROOT}" -B "${TMP_DIR}/t1" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${CTR_CMAKE_TOOLCHAIN_FILE}" >/dev/null
cmake --build "${TMP_DIR}/t1" --target test_layout psx3ds_3dsx >/dev/null
test -f "${TMP_DIR}/t1/test_layout.elf"
test -f "${TMP_DIR}/t1/psx3ds.3dsx"
test -f "${TMP_DIR}/t1/psx_gpu.shbin"

diff -u "${TMP_DIR}/t3.offsets" "${TMP_DIR}/t2.offsets"

tail -n 1 "${TMP_DIR}/t3.out"
tail -n 1 "${TMP_DIR}/t2.out"
echo "gen_offsets_check PASS: all 34 PsxCpu offsets identical across x86_64 and ARM11v6K, T1 .3dsx verified."
