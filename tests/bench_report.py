#!/usr/bin/env python3
"""Benchmark Reporter & Regression Gate for psx3ds (ARCHITECTURE.md §11).

Reads JSON output from `psx3ds-bench` (or executes `psx3ds-bench` across
`tests/roms/benchmarks/*.exe`), prints a formatted summary table, and optionally
gates against a baseline JSON report with a configurable regression threshold.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, List


def extract_runs(doc: Any) -> List[Dict[str, Any]]:
    if isinstance(doc, dict):
        for key in ("passes", "runs"):
            if key in doc and isinstance(doc[key], list):
                return [r for r in doc[key] if isinstance(r, dict)]
        if "emu_fps" in doc:
            return [doc]
    if isinstance(doc, list):
        out: List[Dict[str, Any]] = []
        for item in doc:
            out.extend(extract_runs(item))
        return out
    return []


def run_bench_binary(bench_bin: Path, roms: List[Path], frames: int, bios: Path | None, hle: bool) -> List[Dict[str, Any]]:
    all_runs: List[Dict[str, Any]] = []
    for rom in roms:
        cmd = [str(bench_bin), "--exe", str(rom), "--frames", str(frames)]
        if hle:
            cmd.append("--hle")
        elif bios:
            cmd.extend(["--bios", str(bios)])
        proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
        if proc.returncode != 0:
            sys.stderr.write(f"psx3ds-bench failed on {rom} (rc={proc.returncode}):\n{proc.stderr}\n")
            sys.exit(1)
        doc = json.loads(proc.stdout)
        all_runs.extend(extract_runs(doc))
    return all_runs


def print_markdown_table(runs: List[Dict[str, Any]]) -> None:
    print("| ROM | CPU | GPU | Emu FPS | Host ms/f | MIPS/f | Blks | Code KiB | IBTC % | TexHit % |")
    print("|---|---|---|---:|---:|---:|---:|---:|---:|---:|")
    for r in runs:
        rom_name = Path(str(r.get("rom", "bios"))).name
        print(
            f"| {rom_name} "
            f"| {r.get('cpu_engine', '?')} "
            f"| {r.get('gpu_backend', '?')} "
            f"| {float(r.get('emu_fps', 0.0)):7.2f} "
            f"| {float(r.get('host_ms_per_frame', 0.0)):9.3f} "
            f"| {int(r.get('mips_insns_per_frame', 0)):6d} "
            f"| {int(r.get('blocks_compiled', 0)):4d} "
            f"| {float(r.get('code_kib', 0.0)):8.2f} "
            f"| {float(r.get('ibtc_hit_pct', 0.0)):6.2f} "
            f"| {float(r.get('gpu_tex_hit_pct', r.get('texcache_hit_pct', 0.0))):8.2f} |"
        )


def check_regressions(baseline: List[Dict[str, Any]], current: List[Dict[str, Any]], max_reg_pct: float) -> int:
    base_map = {
        (Path(str(r.get("rom", ""))).name, r.get("cpu_engine"), r.get("gpu_backend")): r
        for r in baseline
    }
    regressions = 0
    for cur in current:
        key = (Path(str(cur.get("rom", ""))).name, cur.get("cpu_engine"), cur.get("gpu_backend"))
        base = base_map.get(key)
        if not base:
            continue
        base_fps = float(base.get("emu_fps", 0.0))
        cur_fps = float(cur.get("emu_fps", 0.0))
        if base_fps > 0.0:
            drop_pct = 100.0 * (base_fps - cur_fps) / base_fps
            if drop_pct > max_reg_pct:
                print(
                    f"REGRESSION {key[0]} [{key[1]}/{key[2]}]: "
                    f"emu_fps {base_fps:.2f} -> {cur_fps:.2f} (-{drop_pct:.2f}% > {max_reg_pct:.2f}%)"
                )
                regressions += 1
    return 1 if regressions > 0 else 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Generate and compare psx3ds benchmark reports.")
    ap.add_argument("input_json", nargs="?", type=Path, help="Existing benchmark JSON file (or '-' for stdin)")
    ap.add_argument("--bench-bin", type=Path, help="Run psx3ds-bench binary across benchmark ROMs")
    ap.add_argument("--roms-dir", type=Path, default=Path("tests/roms/benchmarks"), help="Benchmark ROM directory")
    ap.add_argument("--bios", type=Path, help="Optional BIOS path when running --bench-bin")
    ap.add_argument("--hle", action="store_true", help="Use standalone HLE mode when running --bench-bin")
    ap.add_argument("--frames", type=int, default=120, help="Frames per benchmark pass (default: 120)")
    ap.add_argument("--out-json", type=Path, help="Save aggregated JSON report to path")
    ap.add_argument("--baseline", type=Path, help="Compare against baseline JSON and fail on regression")
    ap.add_argument("--max-regression-pct", type=float, default=5.0, help="Max allowed FPS drop %% (default: 5.0)")
    args = ap.parse_args()

    runs: List[Dict[str, Any]] = []
    if args.bench_bin:
        roms = sorted(args.roms_dir.glob("*.exe"))
        if not roms:
            sys.stderr.write(f"No .exe benchmarks found in {args.roms_dir}\n")
            return 1
        runs = run_bench_binary(args.bench_bin, roms, args.frames, args.bios, args.hle)
    elif args.input_json:
        raw = sys.stdin.read() if str(args.input_json) == "-" else args.input_json.read_text(encoding="utf-8")
        runs = extract_runs(json.loads(raw))
    else:
        ap.print_help()
        return 2

    if args.out_json:
        args.out_json.write_text(json.dumps({"suite": "psx3ds-bench", "runs": runs}, indent=2) + "\n", encoding="utf-8")

    print_markdown_table(runs)

    if args.baseline:
        base_runs = extract_runs(json.loads(args.baseline.read_text(encoding="utf-8")))
        return check_regressions(base_runs, runs, args.max_regression_pct)

    return 0


if __name__ == "__main__":
    sys.exit(main())
