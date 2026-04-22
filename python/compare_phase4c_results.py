#!/usr/bin/env python3
"""Compare Phase 4C baseline outputs against brute-force outputs."""

import argparse
import json
import sys
from pathlib import Path


def load_topk_set(path: Path) -> set[int]:
    data = json.loads(path.read_text())
    return {int(row["group_id"]) for row in data["top_k_results"]}


def compare_sets(name: str, brute_path: Path, baseline_path: Path) -> bool:
    brute_set = load_topk_set(brute_path)
    baseline_set = load_topk_set(baseline_path)

    missing = brute_set - baseline_set
    extra = baseline_set - brute_set
    if missing or extra:
        print(f"{name} mismatch")
        print(f"  Missing in baseline: {sorted(missing)}")
        print(f"  Extra in baseline:   {sorted(extra)}")
        return False

    print(f"{name} top-k set match OK")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compare Phase 4C baseline result sets against brute-force."
    )
    parser.add_argument("--s0-bf", default="results/S0_bf.json")
    parser.add_argument("--s0-baseline", default="results/S0_baseline.json")
    parser.add_argument("--s1-bf", default="results/S1_bf.json")
    parser.add_argument("--s1-baseline", default="results/S1_baseline.json")
    args = parser.parse_args()

    s0_bf = Path(args.s0_bf)
    s0_baseline = Path(args.s0_baseline)
    s1_bf = Path(args.s1_bf)
    s1_baseline = Path(args.s1_baseline)

    for p in (s0_bf, s0_baseline, s1_bf, s1_baseline):
        if not p.exists():
            print(f"Missing file: {p}")
            return 1

    ok = True
    ok &= compare_sets("S0", s0_bf, s0_baseline)
    ok &= compare_sets("S1", s1_bf, s1_baseline)

    if not ok:
        return 1

    s1_data = json.loads(s1_baseline.read_text())
    metrics = s1_data.get("metrics", {})
    print("PASS: S0 and S1 baseline top-k sets match with brute-force exactly.")
    print(
        "S1 total_passes=",
        metrics.get("total_passes"),
        "partitions_pruned_pct=",
        metrics.get("partitions_pruned_pct"),
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
