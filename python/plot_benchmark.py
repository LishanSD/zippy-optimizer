#!/usr/bin/env python3
"""
Plot Zippy benchmark results from the CSV produced by run_benchmark.py.

Creates four plots analogous to the paper's figures:
  Fig A – Distribution sensitivity   (throughput by distribution)
  Fig B – Dataset-size sensitivity    (speedup vs N)
  Fig C – k sensitivity               (speedup vs k)
  Fig D – Aggregate sensitivity       (throughput + pruning by aggregate)
"""

import argparse
import os
import sys
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import numpy as np

DIST_ORDER = ["uniform", "sorted", "heavy-hitter", "zipf", "self-similar", "moving-cluster"]
AGG_ORDER  = ["sum", "count", "max", "min"]

COLORS = {
    "sum":           "#1f77b4",
    "count":         "#ff7f0e",
    "max":           "#2ca02c",
    "min":           "#d62728",
    "brute-force":   "#888888",
    "baseline":      "#1f77b4",
}


def load_csv(csv_path: str) -> pd.DataFrame:
    df = pd.read_csv(csv_path)
    # Coerce types
    df["n_rows"] = pd.to_numeric(df["n_rows"], errors="coerce")
    df["throughput_Mrows_s"] = pd.to_numeric(df["throughput_Mrows_s"], errors="coerce")
    df["partitions_pruned_pct"] = pd.to_numeric(df["partitions_pruned_pct"], errors="coerce")
    df["total_passes"] = pd.to_numeric(df["total_passes"], errors="coerce")
    df["fa_hit_rate"] = pd.to_numeric(df["fa_hit_rate"], errors="coerce")
    return df


def speedup_df(df: pd.DataFrame) -> pd.DataFrame:
    """Add speedup column: baseline_throughput / bf_throughput, grouped by key."""
    bf  = df[df["mode"] == "brute-force"].copy()
    bl  = df[df["mode"] == "baseline"].copy()

    key_cols = ["experiment", "dataset", "n_rows", "n_groups", "group_dist", "k", "agg"]
    # Only keep key cols + throughput
    bf_small = bf[key_cols + ["throughput_Mrows_s"]].rename(
        columns={"throughput_Mrows_s": "bf_tp"}
    )
    bl_small = bl.merge(bf_small, on=key_cols, how="left")
    bl_small["speedup"] = bl_small["throughput_Mrows_s"] / bl_small["bf_tp"].replace(0, float("nan"))
    return bl_small


def fig_distribution_sensitivity(df: pd.DataFrame, out_dir: str):
    """Fig A – Throughput (BF vs Baseline) per distribution."""
    sub = df[df["experiment"] == "dist_sensitivity"].copy()
    if sub.empty:
        print("  No dist_sensitivity data — skipping Fig A")
        return

    dists    = [d for d in DIST_ORDER if d in sub["group_dist"].unique()]
    x        = np.arange(len(dists))
    width    = 0.35

    bf_tp = []
    bl_tp = []
    bl_speedup = []
    for d in dists:
        r_bf = sub[(sub["group_dist"] == d) & (sub["mode"] == "brute-force")]
        r_bl = sub[(sub["group_dist"] == d) & (sub["mode"] == "baseline")]
        bf_tp.append(r_bf["throughput_Mrows_s"].mean() if not r_bf.empty else 0)
        bl_tp.append(r_bl["throughput_Mrows_s"].mean() if not r_bl.empty else 0)
        sp = (r_bl["throughput_Mrows_s"].mean() / r_bf["throughput_Mrows_s"].mean()
              if (not r_bf.empty and not r_bl.empty and r_bf["throughput_Mrows_s"].mean() > 0)
              else 0)
        bl_speedup.append(sp)

    fig, ax = plt.subplots(figsize=(10, 5))
    bars_bf = ax.bar(x - width/2, bf_tp, width, label="Brute-Force", color="#888888", alpha=0.8)
    bars_bl = ax.bar(x + width/2, bl_tp, width, label="Zippy (baseline)", color="#1f77b4", alpha=0.8)

    # Annotate speedup above baseline bar
    for i, sp in enumerate(bl_speedup):
        if sp > 0:
            ax.text(x[i] + width/2, bl_tp[i] * 1.02, f"{sp:.1f}×",
                    ha="center", va="bottom", fontsize=8, fontweight="bold", color="#1f77b4")

    ax.set_xticks(x)
    ax.set_xticklabels(dists, rotation=15, ha="right")
    ax.set_ylabel("Throughput (M rows/sec)")
    ax.set_title("Fig A — Distribution Sensitivity\n"
                 f"(N={sub['n_rows'].iloc[0]/1e6:.0f}M  k={sub['k'].iloc[0]}  agg=SUM)")
    ax.legend()
    ax.yaxis.set_major_formatter(ticker.FormatStrFormatter("%.2f"))
    ax.grid(axis="y", linestyle="--", alpha=0.4)
    plt.tight_layout()
    path = os.path.join(out_dir, "fig_A_distribution_sensitivity.png")
    plt.savefig(path, dpi=150)
    plt.close()
    print(f"  Saved {path}")


def fig_size_sensitivity(df: pd.DataFrame, out_dir: str):
    """Fig B – Speedup vs dataset size."""
    sub = df[df["experiment"] == "size_sensitivity"].copy()
    if sub.empty:
        print("  No size_sensitivity data — skipping Fig B")
        return

    sdf = speedup_df(sub)
    sdf = sdf.sort_values("n_rows")

    sizes_M   = sdf["n_rows"] / 1e6
    speedups  = sdf["speedup"].clip(lower=0)
    bl_tp     = sdf["throughput_Mrows_s"]

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 5))

    # Speedup
    ax1.plot(sizes_M, speedups, "o-", color="#1f77b4", linewidth=2, markersize=7)
    ax1.axhline(1.0, color="gray", linestyle="--", linewidth=1, label="Brute-Force (1×)")
    ax1.set_xlabel("Dataset size (M rows)")
    ax1.set_ylabel("Speedup vs Brute-Force")
    ax1.set_title("Fig B — Speedup vs Dataset Size")
    ax1.legend()
    ax1.grid(linestyle="--", alpha=0.4)
    ax1.set_xscale("log")
    # Annotate points
    for _, row in sdf.iterrows():
        ax1.annotate(f"{row['speedup']:.1f}×",
                     (row["n_rows"]/1e6, row["speedup"]),
                     textcoords="offset points", xytext=(4, 4), fontsize=8)

    # Absolute throughput
    bf_sub = sub[sub["mode"] == "brute-force"].sort_values("n_rows")
    ax2.plot(bf_sub["n_rows"]/1e6, bf_sub["throughput_Mrows_s"],
             "s--", color="#888888", label="Brute-Force", linewidth=2)
    ax2.plot(sdf["n_rows"]/1e6, bl_tp,
             "o-", color="#1f77b4", label="Zippy", linewidth=2)
    ax2.set_xlabel("Dataset size (M rows)")
    ax2.set_ylabel("Throughput (M rows/sec)")
    ax2.set_title("Throughput vs Dataset Size")
    ax2.legend()
    ax2.grid(linestyle="--", alpha=0.4)
    ax2.set_xscale("log")

    plt.tight_layout()
    path = os.path.join(out_dir, "fig_B_size_sensitivity.png")
    plt.savefig(path, dpi=150)
    plt.close()
    print(f"  Saved {path}")


def fig_k_sensitivity(df: pd.DataFrame, out_dir: str):
    """Fig C – Speedup vs k (mirrors Figure 7b of the paper)."""
    sub = df[df["experiment"] == "k_sensitivity"].copy()
    if sub.empty:
        print("  No k_sensitivity data — skipping Fig C")
        return

    sdf = speedup_df(sub).sort_values("k")

    fig, ax = plt.subplots(figsize=(8, 5))
    ax.plot(sdf["k"], sdf["speedup"], "o-", color="#1f77b4", linewidth=2, markersize=7,
            label="SUM speedup")
    ax.axhline(1.0, color="gray", linestyle="--", linewidth=1)

    # Also plot pruning rate
    ax2 = ax.twinx()
    ax2.plot(sdf["k"], sdf["partitions_pruned_pct"] * 100, "s:", color="#ff7f0e",
             linewidth=2, markersize=7, label="Pruned %")
    ax2.set_ylabel("Partitions pruned (%)", color="#ff7f0e")
    ax2.tick_params(axis="y", labelcolor="#ff7f0e")

    ax.set_xlabel("k (top-k value)")
    ax.set_ylabel("Speedup vs Brute-Force")
    ax.set_title(f"Fig C — Speedup vs k\n"
                 f"(N={sub['n_rows'].iloc[0]/1e6:.0f}M  dist=zipf  agg=SUM)")
    lines1, lbl1 = ax.get_legend_handles_labels()
    lines2, lbl2 = ax2.get_legend_handles_labels()
    ax.legend(lines1 + lines2, lbl1 + lbl2)
    ax.grid(linestyle="--", alpha=0.4)
    plt.tight_layout()
    path = os.path.join(out_dir, "fig_C_k_sensitivity.png")
    plt.savefig(path, dpi=150)
    plt.close()
    print(f"  Saved {path}")


def fig_aggregate_sensitivity(df: pd.DataFrame, out_dir: str):
    """Fig D – Breakdown by aggregate (Table 2 analog + bar chart)."""
    sub = df[df["experiment"] == "agg_sensitivity"].copy()
    if sub.empty:
        print("  No agg_sensitivity data — skipping Fig D")
        return

    aggs = [a for a in AGG_ORDER if a in sub["agg"].unique()]
    sdf  = speedup_df(sub)

    fig, axes = plt.subplots(1, 3, figsize=(14, 5))

    # Subplot 1: Speedup per aggregate
    sp_vals = [sdf[sdf["agg"] == a]["speedup"].mean() for a in aggs]
    colors  = [COLORS.get(a, "#1f77b4") for a in aggs]
    axes[0].bar(aggs, sp_vals, color=colors, alpha=0.85)
    axes[0].axhline(1.0, color="gray", linestyle="--")
    for i, v in enumerate(sp_vals):
        axes[0].text(i, v * 1.02, f"{v:.2f}×", ha="center", fontsize=9, fontweight="bold")
    axes[0].set_ylabel("Speedup vs Brute-Force")
    axes[0].set_title("Speedup by Aggregate")
    axes[0].grid(axis="y", linestyle="--", alpha=0.4)

    # Subplot 2: Pruning rate per aggregate
    pr_vals = [sdf[sdf["agg"] == a]["partitions_pruned_pct"].mean() * 100 for a in aggs]
    axes[1].bar(aggs, pr_vals, color=colors, alpha=0.85)
    for i, v in enumerate(pr_vals):
        axes[1].text(i, v * 1.01, f"{v:.0f}%", ha="center", fontsize=9)
    axes[1].set_ylabel("Partitions Pruned (%)")
    axes[1].set_title("Pruning Rate by Aggregate")
    axes[1].set_ylim(0, 105)
    axes[1].grid(axis="y", linestyle="--", alpha=0.4)

    # Subplot 3: Passes to convergence
    pass_vals = [sdf[sdf["agg"] == a]["total_passes"].mean() for a in aggs]
    axes[2].bar(aggs, pass_vals, color=colors, alpha=0.85)
    for i, v in enumerate(pass_vals):
        axes[2].text(i, v * 1.01, f"{v:.1f}", ha="center", fontsize=9)
    axes[2].set_ylabel("Passes to Convergence")
    axes[2].set_title("Passes by Aggregate")
    axes[2].grid(axis="y", linestyle="--", alpha=0.4)

    plt.suptitle(f"Fig D — Aggregate Sensitivity  "
                 f"(N={sub['n_rows'].iloc[0]/1e6:.0f}M  dist=zipf  k={sub['k'].iloc[0]})",
                 y=1.02)
    plt.tight_layout()
    path = os.path.join(out_dir, "fig_D_aggregate_sensitivity.png")
    plt.savefig(path, dpi=150, bbox_inches="tight")
    plt.close()
    print(f"  Saved {path}")


def fig_cache_efficiency(df: pd.DataFrame, out_dir: str):
    """Fig E – Pass-1 throughput vs BF (shows pure cache efficiency, no sampling overhead)."""
    sub = df[df["experiment"] == "size_sensitivity"].copy()
    if sub.empty or "pass1_throughput_Mrows_s" not in sub.columns:
        print("  No pass1 data — skipping Fig E")
        return

    bl = sub[sub["mode"] == "baseline"].sort_values("n_rows").copy()
    bf = sub[sub["mode"] == "brute-force"].sort_values("n_rows").copy()
    bl = bl[bl["pass1_throughput_Mrows_s"] > 0]

    merged = bl.merge(
        bf[["n_rows", "throughput_Mrows_s"]].rename(columns={"throughput_Mrows_s": "bf_tp"}),
        on="n_rows", how="inner"
    )
    merged["pass1_speedup"] = merged["pass1_throughput_Mrows_s"] / merged["bf_tp"]
    merged["total_speedup"] = merged["throughput_Mrows_s"] / merged["bf_tp"]

    sizes_M = merged["n_rows"] / 1e6

    fig, ax = plt.subplots(figsize=(9, 5))
    ax.plot(sizes_M, merged["pass1_speedup"], "o-", color="#2ca02c", linewidth=2,
            markersize=8, label="Zippy Pass-1 speedup (cache efficiency)")
    ax.plot(sizes_M, merged["total_speedup"], "s--", color="#1f77b4", linewidth=2,
            markersize=8, label="Zippy total speedup (incl. sampling)")
    ax.axhline(1.0, color="gray", linestyle=":", linewidth=1.5, label="Break-even (1×)")

    for _, row in merged.iterrows():
        ax.annotate(f"{row['pass1_speedup']:.2f}×",
                    (row["n_rows"]/1e6, row["pass1_speedup"]),
                    textcoords="offset points", xytext=(4, 6), fontsize=8, color="#2ca02c")
        ax.annotate(f"{row['total_speedup']:.2f}×",
                    (row["n_rows"]/1e6, row["total_speedup"]),
                    textcoords="offset points", xytext=(4, -14), fontsize=8, color="#1f77b4")

    ax.set_xlabel("Dataset size (M rows)  [log scale]")
    ax.set_ylabel("Speedup vs Brute-Force")
    ax.set_xscale("log")
    ax.set_title("Fig E — Cache Efficiency: Pass-1 vs Total Speedup\n"
                 "(dist=zipf, k=50, SUM  |  Zippy FA=50K entries ≈ 1.6 MB, fits in L2 cache)")
    ax.legend()
    ax.grid(linestyle="--", alpha=0.4)
    ax.fill_between(sizes_M, 1.0, merged["pass1_speedup"], alpha=0.12, color="#2ca02c",
                    label="_nolegend_")

    # Annotation box
    ax.text(0.02, 0.97,
            "Sampling overhead is the gap between\n"
            "Pass-1 speedup and Total speedup.\n"
            "Parallelism (48 cores in the paper)\n"
            "eliminates this gap → 3–15× reported.",
            transform=ax.transAxes, fontsize=8, va="top",
            bbox=dict(boxstyle="round,pad=0.4", fc="lightyellow", ec="gray", alpha=0.9))

    plt.tight_layout()
    path = os.path.join(out_dir, "fig_E_cache_efficiency.png")
    plt.savefig(path, dpi=150)
    plt.close()
    print(f"  Saved {path}")


def print_summary_table(df: pd.DataFrame):
    """Print a Table-2 style breakdown."""
    bl = df[df["mode"] == "baseline"].copy()
    bf = df[df["mode"] == "brute-force"].copy()

    key = ["experiment", "n_rows", "group_dist", "k", "agg"]
    merged = bl.merge(
        bf[key + ["throughput_Mrows_s"]].rename(columns={"throughput_Mrows_s": "bf_tp"}),
        on=key, how="left"
    )
    merged["speedup"] = merged["throughput_Mrows_s"] / merged["bf_tp"].replace(0, float("nan"))

    print("\n" + "="*110)
    print("  FULL RESULTS TABLE")
    print("="*110)
    print(f"{'Exp':<18} {'dist':<15} {'N':>8} {'k':>4} {'agg':>5} "
          f"{'BF Mrows/s':>11} {'BL Mrows/s':>11} {'speedup':>8} "
          f"{'passes':>6} {'pruned%':>8} {'fa_hit%':>8} {'optim':>6} {'OK':>3}")
    print("-" * 110)
    for _, row in merged.iterrows():
        ok   = "✓" if row.get("correct", True) else "✗"
        opt  = "Y" if row.get("is_optimizable", True) else "N"
        fa_h = f"{row['fa_hit_rate']*100:.0f}%" if row.get("fa_hit_rate", -1) >= 0 else "N/A"
        sp   = f"{row['speedup']:.2f}×" if pd.notna(row.get("speedup")) else "N/A"
        print(
            f"{row['experiment']:<18} {row['group_dist']:<15} {row['n_rows']/1e6:>7.0f}M "
            f"{row['k']:>4} {row['agg']:>5} "
            f"{row['bf_tp']:>11.3f} {row['throughput_Mrows_s']:>11.3f} {sp:>8} "
            f"{row['total_passes']:>6} {row['partitions_pruned_pct']*100:>7.1f}% "
            f"{fa_h:>8} {opt:>6} {ok:>3}"
        )


def main():
    parser = argparse.ArgumentParser(description="Plot Zippy benchmark results.")
    parser.add_argument("--csv", required=True, help="Path to benchmark_results.csv")
    parser.add_argument("--out-dir", default="results/benchmark",
                        help="Directory to write PNG files")
    args = parser.parse_args()

    if not os.path.exists(args.csv):
        print(f"ERROR: {args.csv} not found")
        sys.exit(1)

    os.makedirs(args.out_dir, exist_ok=True)
    df = load_csv(args.csv)
    print(f"Loaded {len(df)} rows from {args.csv}")

    fig_distribution_sensitivity(df, args.out_dir)
    fig_size_sensitivity(df, args.out_dir)
    fig_k_sensitivity(df, args.out_dir)
    fig_aggregate_sensitivity(df, args.out_dir)
    fig_cache_efficiency(df, args.out_dir)
    print_summary_table(df)
    print(f"\nPlots saved in {args.out_dir}/")


if __name__ == "__main__":
    main()
