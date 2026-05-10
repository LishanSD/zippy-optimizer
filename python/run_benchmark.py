#!/usr/bin/env python3
"""
Comprehensive benchmark for the Zippy top-k optimizer.

Reproduces the key experiments from Siddiqui et al., PVLDB 17(4), 2023:
  Exp 1 – Distribution sensitivity  (6 synthetic distributions, fixed N/k/agg)
  Exp 2 – Aggregate sensitivity      (SUM / COUNT / MAX / MIN, fixed N/dist/k)
  Exp 3 – k sensitivity              (k = 1..100, fixed N/dist/agg)
  Exp 4 – Dataset-size sensitivity   (N = 1M..200M, fixed dist/k/agg)

For each (dataset, mode) pair the script:
  • Calls the C++ binary, parses JSON output
  • Computes throughput (rows/sec) = n_rows / total_duration_ms × 1000
  • Verifies baseline top-k matches brute-force (by sorted aggregate-value multiset)
  • Computes FA hit rate = |FA ∩ true-top-k| / k   (requires --output-fa-groups)
  • Writes a CSV row per run

Usage:
    python python/run_benchmark.py              # runs everything
    python python/run_benchmark.py --quick      # 1M rows only, zipf only
    python python/run_benchmark.py --sizes 10M 50M 200M
"""

import argparse
import csv
import json
import os
import subprocess
import sys
import time
from dataclasses import dataclass, field, fields, asdict
from pathlib import Path
from typing import List, Optional, Tuple

# ── Paths ─────────────────────────────────────────────────────────────────────
REPO_ROOT   = Path(__file__).resolve().parent.parent
BINARY      = REPO_ROOT / "build" / "zippy"
DATA_DIR    = REPO_ROOT / "data" / "benchmark"
RESULTS_DIR = REPO_ROOT / "results" / "benchmark"
CSV_PATH    = RESULTS_DIR / "benchmark_results.csv"

GEN_SCRIPT  = REPO_ROOT / "python" / "generate_benchmark_datasets.py"


# ── Experiment configuration ──────────────────────────────────────────────────
ALL_DISTRIBUTIONS = ["uniform", "sorted", "heavy-hitter", "zipf", "self-similar", "moving-cluster"]
ALL_AGGREGATES    = ["sum", "count", "max", "min"]
K_VALUES          = [1, 5, 10, 20, 50, 100]
SIZE_TABLE        = {
    "1M":   (1_000_000,     100_000),
    "5M":   (5_000_000,     500_000),
    "10M":  (10_000_000,  1_000_000),
    "50M":  (50_000_000,  7_500_000),   # ratio 0.15 → matches generated file
    "200M": (200_000_000, 30_000_000),  # ratio 0.15 → matches generated file
}

# Default parameters (paper Section 6 defaults where applicable)
DEFAULT_K         = 50
DEFAULT_AGG       = "sum"
DEFAULT_DIST      = "zipf"
DEFAULT_SIZE      = "10M"

FA_CAPACITY       = 50_000    # C_f — matches paper's default ~50K
N_PARTITIONS      = 10_000    # CA partitions
PAPER_SAMPLE_SIZE = 100_000   # paper Section 6: s = 100k (fixed regardless of N)
DELTA             = 0.05      # tolerance Δ (controls formula-based minimum)
BETA_CI           = 0.95      # Hoeffding CI confidence
ALPHA_LOCALITY    = 0.20      # locality threshold α₀ (paper Appendix A)


def paper_sample_frac(n_rows: int) -> float:
    """Target s=100K samples regardless of N (paper Section 6 default)."""
    return max(0.0001, min(0.10, PAPER_SAMPLE_SIZE / n_rows))


# ── Result record ─────────────────────────────────────────────────────────────
@dataclass
class BenchResult:
    experiment:          str = ""
    dataset:             str = ""
    n_rows:              int = 0
    n_groups:            int = 0
    group_dist:          str = ""
    k:                   int = 0
    agg:                 str = ""
    mode:                str = ""
    is_optimizable:      bool = True
    total_duration_ms:   float = 0.0
    sample_duration_ms:  float = 0.0
    pass1_duration_ms:   float = 0.0
    pass2plus_duration_ms: float = 0.0
    throughput_Mrows_s:  float = 0.0    # 10^6 rows/sec (total including sampling)
    pass1_throughput_Mrows_s: float = 0.0  # pass1-only throughput (shows cache efficiency)
    total_passes:        int = 0
    partitions_pruned_pct: float = 0.0
    fa_candidates_count: int = 0
    fa_hit_rate:         float = -1.0   # -1 = not computed
    l_k_lower_bound:     float = 0.0
    cs_above_lk:         int = 0
    partitions_exact_agg: int = 0
    partitions_logical:  int = 0
    partitions_physical: int = 0
    correct:             bool = True    # baseline top-k matches brute-force
    error:               str = ""       # non-empty if run failed


# ── Binary invocation ─────────────────────────────────────────────────────────
def run_zippy(
    data_path: Path,
    n_rows: int,
    k: int,
    mode: str,
    agg: str,
    out_json: Path,
    fa_capacity: int = FA_CAPACITY,
    n_partitions: int = N_PARTITIONS,
    sample_frac: float = -1.0,   # -1 → adaptive (paper_sample_frac)
    delta: float = DELTA,
    beta_ci: float = BETA_CI,
    alpha_locality: float = ALPHA_LOCALITY,
    output_fa_groups: bool = False,
    timeout: int = 600,
) -> Tuple[Optional[dict], str]:
    """Run the C++ binary and return (parsed_json, stderr_output)."""
    if sample_frac < 0:
        sample_frac = paper_sample_frac(n_rows)
    cmd = [
        str(BINARY),
        "--input",        str(data_path),
        "--n-rows",       str(n_rows),
        "--k",            str(k),
        "--mode",         mode,
        "--agg",          agg,
        "--output",       str(out_json),
        "--fa-capacity",  str(fa_capacity),
        "--n-partitions", str(n_partitions),
        "--sample-frac",  str(sample_frac),
        "--delta",        str(delta),
        "--beta-ci",      str(beta_ci),
        "--alpha-locality", str(alpha_locality),
    ]
    if output_fa_groups:
        cmd.append("--output-fa-groups")

    try:
        result = subprocess.run(
            cmd, capture_output=True, text=True, timeout=timeout
        )
        if result.returncode != 0:
            return None, f"exit {result.returncode}: {result.stderr.strip()}"
        with open(out_json) as f:
            return json.load(f), result.stderr
    except subprocess.TimeoutExpired:
        return None, f"TIMEOUT after {timeout}s"
    except Exception as e:
        return None, str(e)


def compute_throughput(n_rows: int, duration_ms: float) -> float:
    if duration_ms <= 0:
        return 0.0
    return (n_rows / (duration_ms / 1000.0)) / 1e6  # M rows/sec


def top_k_values_sorted(top_k_results: list) -> list:
    """Return sorted list of aggregate values for set-equality check."""
    return sorted([r["aggregate"] for r in top_k_results], reverse=True)


def compute_fa_hit_rate(fa_group_ids: list, bf_top_k: list, k: int) -> float:
    """Fraction of true top-k groups found in FA."""
    if not fa_group_ids or k <= 0:
        return -1.0
    true_top_k_ids = {r["group_id"] for r in bf_top_k[:k]}
    fa_set = set(fa_group_ids)
    hits = len(true_top_k_ids & fa_set)
    return hits / min(k, len(true_top_k_ids))


# ── Dataset generation ────────────────────────────────────────────────────────
def ensure_dataset(dist: str, n_rows: int, n_groups: int,
                   zipf_alpha: float = 0.5) -> Optional[Path]:
    size_tag   = f"{n_rows // 1_000_000}M" if n_rows >= 1_000_000 else f"{n_rows // 1_000}K"
    groups_tag = f"{n_groups // 1_000_000}M" if n_groups >= 1_000_000 else f"{n_groups // 1_000}K"
    fname = f"{dist}_{size_tag}_G{groups_tag}.bin"
    path  = DATA_DIR / fname

    if path.exists():
        return path

    print(f"  Generating {fname}...")
    alpha_arg = ["--zipf-alpha", str(zipf_alpha)] if dist == "zipf" else []
    cmd = [
        sys.executable, str(GEN_SCRIPT),
        "--data-dir", str(DATA_DIR),
        "--sizes", str(n_rows),
        "--groups-ratio", str(n_groups / n_rows),
        "--distributions", dist,
        "--skip-existing",
    ] + alpha_arg
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  ERROR generating {fname}: {r.stderr[:300]}")
        return None
    return path if path.exists() else None


# ── Single experiment ─────────────────────────────────────────────────────────
def run_experiment(
    exp_name: str,
    dist: str,
    n_rows: int,
    n_groups: int,
    k: int,
    agg: str,
    bf_top_k: Optional[list] = None,   # pass pre-computed BF result to avoid re-running
    zipf_alpha: float = 0.5,
) -> Tuple[BenchResult, BenchResult, Optional[list]]:
    """
    Run both brute-force and baseline on the same dataset.
    Returns (bf_result, bl_result, bf_top_k_list).
    """
    data_path = ensure_dataset(dist, n_rows, n_groups, zipf_alpha)
    if data_path is None:
        err = BenchResult(experiment=exp_name, dataset=f"{dist}_{n_rows}", error="dataset missing")
        return err, err, None

    tag = f"{exp_name}_{dist}_{n_rows}_{k}_{agg}"
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)

    def make_result(mode: str, data: Optional[dict], stderr: str) -> BenchResult:
        r = BenchResult(
            experiment=exp_name, dataset=data_path.name,
            n_rows=n_rows, n_groups=n_groups,
            group_dist=dist, k=k, agg=agg, mode=mode,
        )
        if data is None:
            r.error = stderr
            return r
        m = data["metrics"]
        r.is_optimizable     = m.get("is_optimizable", True)
        r.total_duration_ms  = m.get("total_duration_ms", 0.0)
        r.sample_duration_ms = m.get("sample_duration_ms", 0.0)
        r.pass1_duration_ms  = m.get("pass1_duration_ms", 0.0)
        r.pass2plus_duration_ms = m.get("pass2plus_duration_ms", 0.0)
        r.total_passes       = m.get("total_passes", 0)
        r.partitions_pruned_pct = m.get("partitions_pruned_pct", 0.0)
        r.fa_candidates_count = m.get("fa_candidates_count", 0)
        r.l_k_lower_bound    = m.get("l_k_lower_bound", 0.0)
        r.cs_above_lk        = m.get("cs_above_lk", 0)
        r.partitions_exact_agg = m.get("partitions_exact_agg", 0)
        r.partitions_logical = m.get("partitions_logical", 0)
        r.partitions_physical = m.get("partitions_physical", 0)
        r.throughput_Mrows_s = compute_throughput(n_rows, r.total_duration_ms)
        if mode == "baseline" and r.pass1_duration_ms > 0:
            r.pass1_throughput_Mrows_s = compute_throughput(n_rows, r.pass1_duration_ms)
        return r

    # ── Brute-force ──────────────────────────────────────────────────────────
    if bf_top_k is None:
        bf_json = RESULTS_DIR / f"bf_{tag}.json"
        bf_data, bf_err = run_zippy(data_path, n_rows, k, "brute-force", agg, bf_json)
        bf_result = make_result("brute-force", bf_data, bf_err)
        bf_top_k  = bf_data["top_k_results"] if bf_data else None
    else:
        # Re-use pre-computed BF (different k or agg — caller must pass correct one)
        bf_json = RESULTS_DIR / f"bf_{tag}.json"
        bf_data, bf_err = run_zippy(data_path, n_rows, k, "brute-force", agg, bf_json)
        bf_result = make_result("brute-force", bf_data, bf_err)
        bf_top_k  = bf_data["top_k_results"] if bf_data else bf_top_k

    # ── Baseline ─────────────────────────────────────────────────────────────
    bl_json   = RESULTS_DIR / f"bl_{tag}.json"
    bl_data, bl_err = run_zippy(data_path, n_rows, k, "baseline", agg, bl_json,
                                 output_fa_groups=True)
    bl_result = make_result("baseline", bl_data, bl_err)

    # Correctness: compare sorted aggregate values (tie-safe)
    if bf_top_k and bl_data:
        bf_vals = top_k_values_sorted(bf_top_k)
        bl_vals = top_k_values_sorted(bl_data["top_k_results"])
        bl_result.correct = (
            len(bf_vals) == len(bl_vals) and
            all(abs(a - b) < 1e-3 for a, b in zip(bf_vals, bl_vals))
        )
        # FA hit rate
        fa_ids = bl_data.get("fa_group_ids", [])
        bl_result.fa_hit_rate = compute_fa_hit_rate(fa_ids, bf_top_k, k)

    return bf_result, bl_result, bf_top_k


# ── Print helpers ─────────────────────────────────────────────────────────────
def fmt_speedup(bf: BenchResult, bl: BenchResult) -> str:
    if bf.throughput_Mrows_s <= 0 or bl.throughput_Mrows_s <= 0:
        return "N/A"
    s = bl.throughput_Mrows_s / bf.throughput_Mrows_s
    return f"{s:.2f}x"


def print_table(rows: List[Tuple[BenchResult, BenchResult]], title: str):
    print(f"\n{'='*100}")
    print(f"  {title}")
    print(f"{'='*100}")
    hdr = (f"{'Dataset':<32} {'k':>4} {'agg':>5} "
           f"{'BF Mrows/s':>11} {'BL Mrows/s':>11} {'Speedup':>8} "
           f"{'Passes':>6} {'Pruned%':>8} {'FA_hit%':>8} "
           f"{'Optim':>6} {'OK':>3}")
    print(hdr)
    print("-" * 100)
    for bf, bl in rows:
        if bf.error or bl.error:
            print(f"  ERROR: {bf.error or bl.error}")
            continue
        ok = "✓" if bl.correct else "✗"
        opt = "Y" if bl.is_optimizable else "N"
        fa_pct = f"{bl.fa_hit_rate*100:.0f}%" if bl.fa_hit_rate >= 0 else "N/A"
        print(
            f"{bl.dataset:<32} {bl.k:>4} {bl.agg:>5} "
            f"{bf.throughput_Mrows_s:>11.3f} {bl.throughput_Mrows_s:>11.3f} {fmt_speedup(bf,bl):>8} "
            f"{bl.total_passes:>6} {bl.partitions_pruned_pct*100:>7.1f}% {fa_pct:>8} "
            f"{opt:>6} {ok:>3}"
        )


# ── CSV writer ────────────────────────────────────────────────────────────────
def append_csv(result_rows: List[BenchResult]):
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    write_header = not CSV_PATH.exists()
    field_names = [f.name for f in fields(BenchResult)]
    with open(CSV_PATH, "a", newline="") as f:
        w = csv.DictWriter(f, fieldnames=field_names)
        if write_header:
            w.writeheader()
        for r in result_rows:
            w.writerow(asdict(r))


# ── Experiments ───────────────────────────────────────────────────────────────
def exp_distribution_sensitivity(sizes: List[str], verbose: bool = True) -> List[Tuple]:
    """Exp 1: All 6 distributions at a fixed N/k/agg — shows which distributions benefit."""
    print("\n" + "="*60)
    print("Experiment 1: Distribution Sensitivity")
    print(f"  k={DEFAULT_K}, agg={DEFAULT_AGG}")
    print("="*60)

    # Use DEFAULT_SIZE (10M) for a meaningful comparison independent of size sweep
    size_tag = DEFAULT_SIZE if DEFAULT_SIZE in SIZE_TABLE else (sizes[0] if sizes else "10M")
    n_rows, n_groups = SIZE_TABLE[size_tag]

    pairs = []
    all_results = []
    for dist in ALL_DISTRIBUTIONS:
        alpha = 0.5 if dist == "zipf" else 1.2
        print(f"\n  [{dist}] N={n_rows:,}  G={n_groups:,}")
        bf, bl, _ = run_experiment(
            "dist_sensitivity", dist, n_rows, n_groups,
            DEFAULT_K, DEFAULT_AGG, zipf_alpha=alpha
        )
        pairs.append((bf, bl))
        all_results.extend([bf, bl])
        status = "✓" if bl.correct else "✗WRONG"
        opt    = "(fallback)" if not bl.is_optimizable else ""
        print(f"    BF {bf.throughput_Mrows_s:.3f} Mrows/s | "
              f"BL {bl.throughput_Mrows_s:.3f} Mrows/s | "
              f"speedup={fmt_speedup(bf,bl)} | "
              f"pruned={bl.partitions_pruned_pct*100:.1f}% | "
              f"passes={bl.total_passes} {opt} {status}")

    if verbose:
        print_table(pairs, "Exp 1: Distribution Sensitivity")
    append_csv(all_results)
    return pairs


def exp_aggregate_sensitivity(sizes: List[str], verbose: bool = True) -> List[Tuple]:
    """Exp 2: SUM / COUNT / MAX / MIN — shows per-aggregate pruning effectiveness."""
    print("\n" + "="*60)
    print("Experiment 2: Aggregate Sensitivity")
    print(f"  dist={DEFAULT_DIST}, k={DEFAULT_K}")
    print("="*60)

    size_tag = DEFAULT_SIZE if DEFAULT_SIZE in SIZE_TABLE else (sizes[0] if sizes else "10M")
    n_rows, n_groups = SIZE_TABLE[size_tag]

    pairs = []
    all_results = []
    for agg in ALL_AGGREGATES:
        print(f"\n  [{agg}] N={n_rows:,}  G={n_groups:,}")
        bf, bl, _ = run_experiment(
            "agg_sensitivity", DEFAULT_DIST, n_rows, n_groups,
            DEFAULT_K, agg, zipf_alpha=1.2
        )
        pairs.append((bf, bl))
        all_results.extend([bf, bl])
        status = "✓" if bl.correct else "✗WRONG"
        print(f"    BF {bf.throughput_Mrows_s:.3f} | BL {bl.throughput_Mrows_s:.3f} | "
              f"speedup={fmt_speedup(bf,bl)} | pruned={bl.partitions_pruned_pct*100:.1f}% "
              f"| passes={bl.total_passes} {status}")

    if verbose:
        print_table(pairs, "Exp 2: Aggregate Sensitivity")
    append_csv(all_results)
    return pairs


def exp_k_sensitivity(sizes: List[str], verbose: bool = True) -> List[Tuple]:
    """Exp 3: Varying k — mirrors Figure 7b of the paper."""
    print("\n" + "="*60)
    print("Experiment 3: k Sensitivity")
    print(f"  dist={DEFAULT_DIST}, agg={DEFAULT_AGG}")
    print("="*60)

    size_tag = DEFAULT_SIZE if DEFAULT_SIZE in SIZE_TABLE else (sizes[0] if sizes else "10M")
    n_rows, n_groups = SIZE_TABLE[size_tag]

    pairs = []
    all_results = []
    for k in K_VALUES:
        print(f"\n  [k={k}] N={n_rows:,}  G={n_groups:,}")
        bf, bl, _ = run_experiment(
            "k_sensitivity", DEFAULT_DIST, n_rows, n_groups,
            k, DEFAULT_AGG, zipf_alpha=1.2
        )
        pairs.append((bf, bl))
        all_results.extend([bf, bl])
        status = "✓" if bl.correct else "✗WRONG"
        print(f"    BF {bf.throughput_Mrows_s:.3f} | BL {bl.throughput_Mrows_s:.3f} | "
              f"speedup={fmt_speedup(bf,bl)} | pruned={bl.partitions_pruned_pct*100:.1f}% {status}")

    if verbose:
        print_table(pairs, "Exp 3: k Sensitivity")
    append_csv(all_results)
    return pairs


def exp_size_sensitivity(sizes: List[str], verbose: bool = True) -> List[Tuple]:
    """Exp 4: Varying dataset size — mirrors Figure 8b of the paper."""
    print("\n" + "="*60)
    print("Experiment 4: Dataset-Size Sensitivity")
    print(f"  dist={DEFAULT_DIST}, k={DEFAULT_K}, agg={DEFAULT_AGG}")
    print("="*60)

    pairs = []
    all_results = []
    for size_tag in sizes:
        if size_tag not in SIZE_TABLE:
            print(f"  Unknown size tag {size_tag!r}, skipping")
            continue
        n_rows, n_groups = SIZE_TABLE[size_tag]
        print(f"\n  [{size_tag}] N={n_rows:,}  G={n_groups:,}")
        bf, bl, _ = run_experiment(
            "size_sensitivity", DEFAULT_DIST, n_rows, n_groups,
            DEFAULT_K, DEFAULT_AGG, zipf_alpha=1.2
        )
        pairs.append((bf, bl))
        all_results.extend([bf, bl])
        status = "✓" if bl.correct else "✗WRONG"
        print(f"    BF {bf.throughput_Mrows_s:.3f} Mrows/s | "
              f"BL {bl.throughput_Mrows_s:.3f} Mrows/s | "
              f"speedup={fmt_speedup(bf,bl)} | "
              f"pruned={bl.partitions_pruned_pct*100:.1f}% | "
              f"passes={bl.total_passes} {status}")

    if verbose:
        print_table(pairs, "Exp 4: Dataset-Size Sensitivity")
    append_csv(all_results)
    return pairs


# ── Main ──────────────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(
        description="Run the Zippy benchmark suite.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--quick", action="store_true",
                        help="Quick mode: only 1M rows, zipf+heavy-hitter, k=10,50")
    parser.add_argument("--sizes", nargs="+",
                        default=["1M", "5M", "10M", "50M"],
                        choices=list(SIZE_TABLE.keys()),
                        help="Dataset sizes for size-sensitivity experiment. "
                             "Add '200M' for paper-scale (needs ~15 min).")
    parser.add_argument("--exp", nargs="+",
                        choices=["dist", "agg", "k", "size", "all"],
                        default=["all"],
                        help="Experiments to run")
    parser.add_argument("--no-plots", action="store_true",
                        help="Skip plot generation")
    args = parser.parse_args()

    # Validate binary
    if not BINARY.exists():
        print(f"ERROR: Binary not found at {BINARY}")
        print("Build with:  g++ -std=c++17 -O3 -o build/zippy src/main.cpp "
              "src/zippy.cpp src/sampler.cpp src/group_index.cpp "
              "src/stratified_sampler.cpp src/measure_index.cpp -Isrc/")
        sys.exit(1)

    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    DATA_DIR.mkdir(parents=True, exist_ok=True)

    if args.quick:
        sizes = ["1M"]
        run_exps = {"dist": False, "agg": True, "k": True, "size": True}
        # Only 2 distributions + 2 k values in quick mode
        global ALL_DISTRIBUTIONS, K_VALUES
        ALL_DISTRIBUTIONS = ["zipf", "heavy-hitter"]
        K_VALUES = [10, 50]
    else:
        sizes = args.sizes
        exps = set(args.exp)
        run_exps = {
            "dist": "all" in exps or "dist" in exps,
            "agg":  "all" in exps or "agg"  in exps,
            "k":    "all" in exps or "k"    in exps,
            "size": "all" in exps or "size" in exps,
        }

    print("=" * 70)
    print("  Zippy Benchmark Suite")
    print(f"  Binary:  {BINARY}")
    print(f"  Data:    {DATA_DIR}")
    print(f"  Results: {RESULTS_DIR}")
    print(f"  Sizes:   {sizes}")
    print("=" * 70)

    t_start = time.time()

    all_pairs: List[Tuple] = []

    if run_exps["dist"]:
        all_pairs.extend(exp_distribution_sensitivity(sizes))

    if run_exps["agg"]:
        all_pairs.extend(exp_aggregate_sensitivity(sizes))

    if run_exps["k"]:
        all_pairs.extend(exp_k_sensitivity(sizes))

    if run_exps["size"]:
        all_pairs.extend(exp_size_sensitivity(sizes))

    elapsed = time.time() - t_start

    # ── Final summary ─────────────────────────────────────────────────────────
    n_correct   = sum(1 for _, bl in all_pairs if bl.correct and not bl.error)
    n_optimized = sum(1 for _, bl in all_pairs if bl.is_optimizable and not bl.error)
    n_total     = sum(1 for _, bl in all_pairs if not bl.error)

    speedups = [
        bl.throughput_Mrows_s / bf.throughput_Mrows_s
        for bf, bl in all_pairs
        if not bl.error and not bf.error
        and bf.throughput_Mrows_s > 0 and bl.throughput_Mrows_s > 0
        and bl.is_optimizable
    ]
    pruning_rates = [
        bl.partitions_pruned_pct
        for _, bl in all_pairs
        if not bl.error and bl.is_optimizable
    ]

    print("\n" + "="*70)
    print("  BENCHMARK SUMMARY")
    print("="*70)
    print(f"  Total runs completed : {n_total * 2} (BF + BL)")
    print(f"  Correct results      : {n_correct}/{n_total}")
    print(f"  Zippy optimized      : {n_optimized}/{n_total}")
    if speedups:
        print(f"  Speedup (vs BF)      : min={min(speedups):.2f}x  "
              f"median={sorted(speedups)[len(speedups)//2]:.2f}x  "
              f"max={max(speedups):.2f}x")
    if pruning_rates:
        avg_p = sum(pruning_rates) / len(pruning_rates)
        print(f"  Pruning rate (avg)   : {avg_p*100:.1f}%")
    print(f"  Results CSV          : {CSV_PATH}")
    print(f"  Total wall time      : {elapsed:.0f}s")

    # ── Invoke plotter ────────────────────────────────────────────────────────
    if not args.no_plots:
        plotter = REPO_ROOT / "python" / "plot_benchmark.py"
        if plotter.exists() and CSV_PATH.exists():
            print("\nGenerating plots...")
            subprocess.run([sys.executable, str(plotter),
                            "--csv", str(CSV_PATH),
                            "--out-dir", str(RESULTS_DIR)],
                           check=False)


if __name__ == "__main__":
    main()
