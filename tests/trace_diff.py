#!/usr/bin/env python3
"""JIT / Cached-Interpreter Architectural State Oracle Diff Tool (ARCHITECTURE.md §11).

Compares `--trace-hash` streams (`<cycles> <fnv1a64_hex> <pc_hex>`) either from two
saved trace files OR by running a reference command (`--engine interp`) against a
candidate command (`--engine cached` or `qemu-arm -cpu arm11mpcore ... --engine jit`).
"""

from __future__ import annotations

import argparse
import shlex
import subprocess
import sys
from pathlib import Path
from typing import List, Tuple

TraceEntry = Tuple[int, str, str]


def parse_trace_lines(text: str) -> List[TraceEntry]:
    entries: List[TraceEntry] = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) < 3:
            continue
        try:
            cyc = int(parts[0], 10)
        except ValueError:
            continue
        entries.append((cyc, parts[1].lower(), parts[2].upper()))
    return entries


def run_trace_cmd(cmd: List[str], hash_every: int, extra_args: List[str]) -> List[TraceEntry]:
    full_cmd = cmd + ["--trace-hash", "-", "--hash-every", str(hash_every)] + extra_args
    proc = subprocess.run(full_cmd, capture_output=True, text=True, check=False)
    if proc.returncode not in (0, 3):
        sys.stderr.write(f"Command failed (rc={proc.returncode}): {' '.join(full_cmd)}\n")
        if proc.stderr:
            sys.stderr.write(proc.stderr)
        sys.exit(2)
    return parse_trace_lines(proc.stdout)


def diff_traces(ref: List[TraceEntry], cand: List[TraceEntry]) -> int:
    n = min(len(ref), len(cand))
    if n == 0:
        print(f"WARN: empty trace stream (ref={len(ref)} entries, cand={len(cand)} entries)")
        return 0 if len(ref) == len(cand) else 1

    for idx in range(n):
        rcyc, rhash, rpc = ref[idx]
        ccyc, chash, cpc = cand[idx]
        if rhash != chash or rpc != cpc:
            print(
                f"DIVERGENCE at checkpoint #{idx}:\n"
                f"  REF : cycle={rcyc:12d}  pc=0x{rpc}  hash=0x{rhash}\n"
                f"  CAND: cycle={ccyc:12d}  pc=0x{cpc}  hash=0x{chash}"
            )
            if idx > 0:
                pcyc, phash, ppc = ref[idx - 1]
                print(f"  LAST GOOD (#{idx - 1}): cycle={pcyc:12d}  pc=0x{ppc}  hash=0x{phash}")
            return 1

    if len(ref) != len(cand):
        print(
            f"LENGTH MISMATCH after {n} matching checkpoints: "
            f"ref={len(ref)} vs cand={len(cand)}"
        )
        return 1

    print(f"MATCH: {n} checkpoints identical (final cycle={ref[-1][0]}, pc=0x{ref[-1][2]})")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Compare psx3ds CPU/JIT architectural trace hashes.")
    ap.add_argument("--ref-file", type=Path, help="Pre-recorded reference trace-hash file")
    ap.add_argument("--cand-file", type=Path, help="Pre-recorded candidate trace-hash file")
    ap.add_argument("--ref-cmd", type=str, help="Reference CLI command (e.g. 'build-host/psx3ds-cli --cpu interp')")
    ap.add_argument("--cand-cmd", type=str, help="Candidate CLI command (e.g. 'qemu-arm -cpu arm11mpcore build-arm11/psx3ds-cli --cpu jit')")
    ap.add_argument("--exe", type=Path, help="PS-X EXE to run when using --ref-cmd / --cand-cmd")
    ap.add_argument("--bios", type=Path, help="Optional BIOS image path")
    ap.add_argument("--hle", action="store_true", help="Run in standalone HLE BIOS mode")
    ap.add_argument("--frames", type=int, default=60, help="Frame limit for live runs (default: 60)")
    ap.add_argument("--hash-every", type=int, default=10000, help="Cycle interval per hash checkpoint")
    args = ap.parse_args()

    if args.ref_file and args.cand_file:
        ref = parse_trace_lines(args.ref_file.read_text(encoding="utf-8"))
        cand = parse_trace_lines(args.cand_file.read_text(encoding="utf-8"))
        return diff_traces(ref, cand)

    if args.ref_cmd and args.cand_cmd:
        common: List[str] = ["--frames", str(args.frames)]
        if args.hle:
            common.append("--hle")
        elif args.bios:
            common.extend(["--bios", str(args.bios)])
        if args.exe:
            common.extend(["--exe", str(args.exe)])

        ref = run_trace_cmd(shlex.split(args.ref_cmd), args.hash_every, common)
        cand = run_trace_cmd(shlex.split(args.cand_cmd), args.hash_every, common)
        return diff_traces(ref, cand)

    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
