#!/usr/bin/env bash
# =============================================================================
# psx3ds Automated PS-EXE / Golden Test Runner (Slice F expands in Wave 1)
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    echo "Usage: $0 [--build-dir DIR] [--engine interp|cached|jit] [--list]"
    exit 0
fi

if [[ "${1:-}" == "--list" ]]; then
    find "${PSX3DS_ROMS_DIR:-${REPO_ROOT}/tests/roms}" -type f -name '*.exe' | sort
    exit 0
fi

echo "rom_runner.sh: skeleton stub (populated by coder-sys once psx3ds-cli lands)"
exit 0
