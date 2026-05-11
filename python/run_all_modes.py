#!/usr/bin/env python3
"""
Build Zippy, run all five modes for one dataset/query, and write a single
combined JSON report containing per-mode outputs plus cross-mode comparisons.
"""

import argparse
import json
import os
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, List, Optional, Tuple


MODES = ["brute-force", "baseline", "ext-a", "ext-b", "ext-ab"]
ROW_SIZE_BYTES = 16
CPP_SOURCES = [
    "src/main.cpp",
    "src/zippy.cpp",
    "src/sampler.cpp",
    "src/group_index.cpp",
    "src/stratified_sampler.cpp",
    "src/measure_index.cpp",
]


def exe_name() -> str:
    return "zippy.exe" if os.name == "nt" else "zippy"


def infer_n_rows(input_path: Path) -> int:
    size_bytes = input_path.stat().st_size
    if size_bytes % ROW_SIZE_BYTES != 0:
        raise ValueError(
            f"Dataset size {size_bytes} is not divisible by {ROW_SIZE_BYTES}; "
            "the file does not look like a valid Zippy binary dataset."
        )
    return size_bytes // ROW_SIZE_BYTES


def build_zippy(repo_root: Path, compiler: str) -> List[str]:
    build_dir = repo_root / "build"
    build_dir.mkdir(parents=True, exist_ok=True)

    output_path = build_dir / exe_name()
    cmd = [
        compiler,
        "-std=c++17",
        "-O2",
        "-Wall",
        "-Wextra",
        "-o",
        str(output_path),
        *[str(repo_root / src) for src in CPP_SOURCES],
        f"-I{repo_root / 'src'}",
    ]
    subprocess.run(cmd, check=True, cwd=repo_root)
    return cmd


def mode_output_path(output_path: Path, mode: str) -> Path:
    stem = output_path.stem
    suffix = output_path.suffix or ".json"
    return output_path.with_name(f"{stem}.{mode}{suffix}")


def run_mode(
    repo_root: Path,
    exe_path: Path,
    mode: str,
    args: argparse.Namespace,
    extra_args: List[str],
    temp_output: Path,
) -> Tuple[List[str], dict]:
    cmd = [
        str(exe_path),
        "--input",
        args.input,
        "--n-rows",
        str(args.n_rows_resolved),
        "--k",
        str(args.k),
        "--mode",
        mode,
        "--agg",
        args.agg,
        "--output",
        str(temp_output),
    ]
    if args.verbose:
        cmd.append("--verbose")
    cmd.extend(extra_args)

    subprocess.run(cmd, check=True, cwd=repo_root)

    with temp_output.open("r", encoding="utf-8") as f:
        data = json.load(f)

    return cmd, data


def group_id_set(report: dict) -> List[int]:
    return sorted(item["group_id"] for item in report["top_k_results"])


def aggregate_multiset(report: dict) -> List[float]:
    return sorted((item["aggregate"] for item in report["top_k_results"]), reverse=True)


def exact_pairs(report: dict) -> List[dict]:
    return report["top_k_results"]


def speedup(reference_ms: float, current_ms: float) -> Optional[float]:
    if current_ms <= 0.0:
        return None
    return reference_ms / current_ms


def summarize_comparisons(reports: Dict[str, dict]) -> dict:
    brute = reports["brute-force"]
    brute_metrics = brute["metrics"]
    brute_ms = float(brute_metrics.get("total_duration_ms", 0.0))

    per_mode = {}
    ordered = []
    for mode in MODES:
        report = reports[mode]
        metrics = report["metrics"]
        total_ms = float(metrics.get("total_duration_ms", 0.0))
        ordered.append((mode, total_ms))

        per_mode[mode] = {
            "matches_exact_top_k_results": exact_pairs(report) == exact_pairs(brute),
            "matches_group_id_set": group_id_set(report) == group_id_set(brute),
            "matches_aggregate_multiset": aggregate_multiset(report) == aggregate_multiset(brute),
            "total_duration_ms": total_ms,
            "duration_delta_vs_bruteforce_ms": total_ms - brute_ms,
            "speedup_vs_bruteforce": speedup(brute_ms, total_ms),
            "topKBound_after_pass1": metrics.get("topKBound_after_pass1"),
            "partitions_pruned_pct": metrics.get("partitions_pruned_pct"),
            "total_passes": metrics.get("total_passes"),
        }

    sorted_by_duration = sorted(ordered, key=lambda item: item[1])
    non_bf = [item for item in sorted_by_duration if item[0] != "brute-force"]

    return {
        "reference_mode": "brute-force",
        "per_mode": per_mode,
        "fastest_mode_overall": sorted_by_duration[0][0] if sorted_by_duration else None,
        "fastest_mode_non_bruteforce": non_bf[0][0] if non_bf else None,
        "modes_by_total_duration_ms": [
            {"mode": mode, "total_duration_ms": total_ms}
            for mode, total_ms in ordered
        ],
        "modes_ranked_by_total_duration_ms": [
            {"mode": mode, "total_duration_ms": total_ms}
            for mode, total_ms in sorted_by_duration
        ],
    }


def parse_args() -> Tuple[argparse.Namespace, List[str]]:
    parser = argparse.ArgumentParser(
        description=(
            "Build zippy, run all five modes for one dataset/query, and write "
            "a single combined JSON report."
        )
    )
    parser.add_argument("--input", required=True, help="Dataset path passed to zippy --input")
    parser.add_argument(
        "--n-rows",
        type=int,
        help="Optional explicit row count. If omitted, inferred from file_size / 16.",
    )
    parser.add_argument("--k", type=int, default=50, help="Top-k size (default: 50)")
    parser.add_argument(
        "--agg",
        default="sum",
        choices=["sum", "count", "max", "min"],
        help="Aggregate function passed to zippy --agg (default: sum)",
    )
    parser.add_argument("--output", required=True, help="Combined JSON report path")
    parser.add_argument("--compiler", default="g++", help="Compiler used for the build step (default: g++)")
    parser.add_argument(
        "--skip-build",
        action="store_true",
        help="Reuse an existing build/zippy(.exe) instead of rebuilding",
    )
    parser.add_argument(
        "--keep-mode-json",
        action="store_true",
        help="Keep the intermediate per-mode JSON files alongside the combined report",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Pass --verbose through to each zippy run",
    )

    args, extra = parser.parse_known_args()
    return args, extra


def main() -> int:
    args, extra_args = parse_args()
    repo_root = Path(__file__).resolve().parent.parent
    input_path = (repo_root / args.input).resolve() if not Path(args.input).is_absolute() else Path(args.input)
    if not input_path.exists():
        raise FileNotFoundError(f"Missing dataset: {input_path}")

    args.n_rows_resolved = args.n_rows if args.n_rows is not None else infer_n_rows(input_path)
    output_path = (repo_root / args.output).resolve() if not Path(args.output).is_absolute() else Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    exe_path = repo_root / "build" / exe_name()
    build_cmd = None
    if not args.skip_build:
        build_cmd = build_zippy(repo_root, args.compiler)
    elif not exe_path.exists():
        raise FileNotFoundError(f"Missing executable: {exe_path}")

    reports: Dict[str, dict] = {}
    commands: Dict[str, List[str]] = {}
    temp_paths: List[Path] = []

    for mode in MODES:
        temp_output = mode_output_path(output_path, mode)
        temp_paths.append(temp_output)
        cmd, report = run_mode(repo_root, exe_path, mode, args, extra_args, temp_output)
        commands[mode] = cmd
        reports[mode] = report

    comparisons = summarize_comparisons(reports)

    combined = {
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "build": {
            "skipped": args.skip_build,
            "compiler": args.compiler,
            "command": build_cmd,
            "executable": str(exe_path),
        },
        "query": {
            "input": args.input,
            "n_rows": args.n_rows_resolved,
            "n_rows_inferred": args.n_rows is None,
            "k": args.k,
            "agg": args.agg,
            "extra_zippy_args": extra_args,
        },
        "comparisons": comparisons,
        "modes": reports,
        "commands": commands,
    }

    with output_path.open("w", encoding="utf-8") as f:
        json.dump(combined, f, indent=2)
        f.write("\n")

    if not args.keep_mode_json:
        for path in temp_paths:
            path.unlink(missing_ok=True)

    print(f"Combined report written to {output_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
