#!/usr/bin/env bash
# =============================================================================
# psx3ds Automated PS-EXE / Golden Test Runner (ARCHITECTURE.md §11)
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${REPO_ROOT}/build-host"
ENGINE="interp"
FRAMES="300"
FILTER="*"
QEMU_CMD=""
UPDATE_GOLDEN=0
LIST_ONLY=0
HLE_MODE=0

usage() {
    cat <<EOF
Usage: $0 [options]
  --build-dir <DIR>       Build directory containing psx3ds-cli (default: build-host)
  --engine <MODE>         CPU engine: interp | cached | jit | auto (default: interp)
  --frames <N>            Frame budget per non-TTY visual ROM (default: 300)
  --filter <GLOB>         Only run ROMs whose path matches GLOB (default: *)
  --qemu <CMD>            Prefix runner (e.g. "qemu-arm -cpu arm11mpcore")
  --hle                   Boot with standalone HLE BIOS instead of openbios.bin
  --update-golden         Write/refresh golden hashes & transcripts in tests/golden/
  --list                  List discovered PS-EXE test ROMs and exit
  --help, -h              Show this help message
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)     BUILD_DIR="$2"; shift 2 ;;
        --engine|--cpu)  ENGINE="$2"; shift 2 ;;
        --frames)        FRAMES="$2"; shift 2 ;;
        --filter)        FILTER="$2"; shift 2 ;;
        --qemu)          QEMU_CMD="$2"; shift 2 ;;
        --hle)           HLE_MODE=1; shift ;;
        --update-golden) UPDATE_GOLDEN=1; shift ;;
        --list)          LIST_ONLY=1; shift ;;
        --help|-h)       usage; exit 0 ;;
        *)               echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
done

ROMS_DIR="${PSX3DS_ROMS_DIR:-${REPO_ROOT}/tests/roms}"
GOLDEN_DIR="${REPO_ROOT}/tests/golden"

if [[ "${LIST_ONLY}" -eq 1 ]]; then
    find "${ROMS_DIR}" -type f -name '*.exe' | sort
    exit 0
fi

CLI_BIN="${BUILD_DIR}/psx3ds-cli"
if [[ ! -x "${CLI_BIN}" ]]; then
    echo "Error: psx3ds-cli not found or not executable at ${CLI_BIN}" >&2
    exit 1
fi

BIOS_ARGS=()
if [[ "${HLE_MODE}" -eq 1 ]]; then
    BIOS_ARGS=(--hle)
else
    OPENBIOS="${PSX3DS_OPENBIOS:-${ROMS_DIR}/bios/openbios.bin}"
    BIOS_ARGS=(--bios "${OPENBIOS}")
fi

TMP_WORK="$(mktemp -d /tmp/psx3ds_rom_runner.XXXXXX)"
trap 'rm -rf "${TMP_WORK}"' EXIT

if [[ "${UPDATE_GOLDEN}" -eq 1 ]]; then
    mkdir -p "${GOLDEN_DIR}"
fi

PASS=0
FAIL=0
SKIP=0

mapfile -t ROM_LIST < <(find "${ROMS_DIR}" -type f -name '*.exe' | sort)

for rom in "${ROM_LIST[@]}"; do
    if [[ "${rom}" != ${FILTER} ]]; then
        SKIP=$((SKIP + 1))
        continue
    fi
    rel="${rom#${ROMS_DIR}/}"
    stem="$(basename "${rom}" .exe)"
    ppm_out="${TMP_WORK}/${stem}.ppm"
    tty_out="${TMP_WORK}/${stem}.txt"

    extra_flags=(--frames "${FRAMES}")
    case "${stem}" in
        psxtest_cpu|psxtest_cpx|psxtest_gte)
            extra_flags=(--stop-on-tty "Done" --frames 7200)
            ;;
        gte-test-all|gte-fuzz|cop|otc-test|chain-looping|chopping|dpcr|timers)
            extra_flags=(--stop-on-tty "PASS" --frames 3600)
            ;;
    esac

    runner=()
    if [[ -n "${QEMU_CMD}" ]]; then
        read -r -a runner <<< "${QEMU_CMD}"
    fi
    runner+=("${CLI_BIN}" "${BIOS_ARGS[@]}" --cpu "${ENGINE}" --exe "${rom}"
             "${extra_flags[@]}" --dump-ppm "${ppm_out}" --tty-out "${tty_out}")

    set +e
    "${runner[@]}" >/dev/null 2>"${TMP_WORK}/${stem}.err"
    rc=$?
    set -e

    if [[ "${rc}" -ne 0 && "${rc}" -ne 3 ]]; then
        echo "[FAIL] ${rel} (exit=${rc})"
        FAIL=$((FAIL + 1))
        continue
    fi

    ppm_sha="$(sha256sum "${ppm_out}" | awk '{print $1}')"
    if [[ "${UPDATE_GOLDEN}" -eq 1 ]]; then
        echo "${ppm_sha}" > "${GOLDEN_DIR}/${stem}.sha256"
        cp "${tty_out}" "${GOLDEN_DIR}/${stem}.txt"
        echo "[GOLD] ${rel} (${ppm_sha:0:12})"
        PASS=$((PASS + 1))
    elif [[ -f "${GOLDEN_DIR}/${stem}.sha256" ]]; then
        expect_sha="$(tr -d '[:space:]' < "${GOLDEN_DIR}/${stem}.sha256")"
        if [[ "${ppm_sha}" != "${expect_sha}" ]]; then
            echo "[FAIL] ${rel} frame hash mismatch (${ppm_sha:0:12} != ${expect_sha:0:12})"
            FAIL=$((FAIL + 1))
        else
            echo "[PASS] ${rel} (${ppm_sha:0:12})"
            PASS=$((PASS + 1))
        fi
    else
        echo "[RUN ] ${rel} (frame=${ppm_sha:0:12}, exit=${rc})"
        PASS=$((PASS + 1))
    fi
done

echo "Summary: PASS=${PASS} FAIL=${FAIL} SKIP=${SKIP}"
[[ "${FAIL}" -eq 0 ]]
