import json
import os
import glob
import pandas as pd
import matplotlib.pyplot as plt
import numpy as np

# Configuration
RESULTS_DIR = "results"
PLOTS_DIR = "plots"

# Ensure plots directory exists
os.makedirs(PLOTS_DIR, exist_ok=True)

# Colors for our modes (VLDB paper style)
MODE_COLORS = {
    "brute-force": "#7f8c8d",  # Gray
    "baseline": "#e74c3c",     # Red
    "ext-a": "#f39c12",        # Orange
    "ext-b": "#3498db",        # Blue
    "ext-ab": "#2ecc71"        # Green
}

# Line styles for the scaling plot
MODE_MARKERS = {
    "brute-force": "s",  # Square
    "baseline": "o",     # Circle
    "ext-a": "^",        # Triangle
    "ext-b": "D",        # Diamond
    "ext-ab": "*"        # Star
}


def load_results():
    """Reads all JSON files in the results directory ."""
    data = []
    filepaths = glob.glob(os.path.join(RESULTS_DIR, "*.json"))

    if not filepaths:
        print(
            f"No JSON files found in {RESULTS_DIR}/. Run your C++ benchmarks first!")
        return pd.DataFrame()

    for file in filepaths:
        filename = os.path.basename(file)
        parts = filename.replace(".json", "").rsplit("_", 1)
        dataset_name = parts[0] if len(parts) > 1 else "unknown"

        with open(file, 'r') as f:
            res = json.load(f)
            metrics = res.get("metrics", {})

            data.append({
                "dataset": dataset_name,
                "mode": res.get("mode", "unknown"),
                "n_rows": res.get("n_rows", 0),
                "total_duration_ms": metrics.get("total_duration_ms", 0),
                "total_passes": metrics.get("total_passes", 0),
                "pruned_pct": metrics.get("partitions_pruned_pct", 0.0) * 100,
            })

    return pd.DataFrame(data)


def plot_execution_time(df):
    """Generates a bar chart of total duration per mode, grouped by dataset."""
    plt.figure(figsize=(10, 6))

    # Filter out scale-up datasets
    archetype_df = df[~df['dataset'].str.startswith('scale')].copy()
    if archetype_df.empty:
        return

    # Pivot data for easy plotting
    pivot_df = archetype_df.pivot(
        index="dataset", columns="mode", values="total_duration_ms")

    # Sort columns to keep a consistent order
    modes_present = [m for m in ["brute-force", "baseline",
                                 "ext-a", "ext-b", "ext-ab"] if m in pivot_df.columns]
    pivot_df = pivot_df[modes_present]

    ax = pivot_df.plot(kind="bar",
                       color=[MODE_COLORS.get(m, "#000")
                              for m in modes_present],
                       edgecolor="black",
                       figsize=(10, 6),
                       width=0.7)

    plt.title("Execution Time by Dataset and Algorithm Mode",
              fontsize=14, fontweight='bold')
    plt.ylabel("Execution Time (ms)", fontsize=12)
    plt.xlabel("Dataset Archetype", fontsize=12)
    plt.xticks(rotation=0)
    plt.grid(axis='y', linestyle='--', alpha=0.7)
    plt.legend(title="Algorithm Mode")
    plt.tight_layout()

    out_path = os.path.join(PLOTS_DIR, "execution_time_comparison.png")
    plt.savefig(out_path, dpi=300)
    print(f"Saved plot: {out_path}")


def plot_pass_count(df):
    """Generates a bar chart showing the number of passes required to converge."""
    plt.figure(figsize=(10, 6))

    archetype_df = df[~df['dataset'].str.startswith('scale')].copy()
    # Remove brute-force from pass count since it's always 0/1 by definition
    archetype_df = archetype_df[archetype_df['mode'] != 'brute-force']

    if archetype_df.empty:
        return

    pivot_df = archetype_df.pivot(
        index="dataset", columns="mode", values="total_passes")

    modes_present = [m for m in ["baseline", "ext-a",
                                 "ext-b", "ext-ab"] if m in pivot_df.columns]
    pivot_df = pivot_df[modes_present]

    ax = pivot_df.plot(kind="bar",
                       color=[MODE_COLORS.get(m, "#000")
                              for m in modes_present],
                       edgecolor="black",
                       figsize=(10, 6),
                       width=0.6)

    plt.title("Total Data Passes Required for Convergence",
              fontsize=14, fontweight='bold')
    plt.ylabel("Number of Passes", fontsize=12)
    plt.xlabel("Dataset Archetype", fontsize=12)
    plt.xticks(rotation=0)

    # Force Y-axis to use integers
    plt.yticks(np.arange(0, pivot_df.max().max() + 2, 1))
    plt.grid(axis='y', linestyle='--', alpha=0.7)
    plt.legend(title="Algorithm Mode")
    plt.tight_layout()

    out_path = os.path.join(PLOTS_DIR, "pass_count_comparison.png")
    plt.savefig(out_path, dpi=300)
    print(f"Saved plot: {out_path}")


def plot_scaling_line_chart(df):
    """Generates a line chart showing execution time vs row count for the scaling datasets."""
    plt.figure(figsize=(10, 6))

    # Filter ONLY the scale-up datasets
    scale_df = df[df['dataset'].str.startswith('scale')].copy()
    if scale_df.empty:
        return

    # Pivot data: X-axis = n_rows, Y-axis = execution time, Lines = modes
    pivot_df = scale_df.pivot(
        index="n_rows", columns="mode", values="total_duration_ms")

    modes_present = [m for m in ["brute-force", "baseline",
                                 "ext-a", "ext-b", "ext-ab"] if m in pivot_df.columns]
    pivot_df = pivot_df[modes_present]

    # Convert index (rows) to millions for cleaner X-axis labels
    pivot_df.index = pivot_df.index / 1_000_000

    for mode in modes_present:
        plt.plot(pivot_df.index, pivot_df[mode],
                 marker=MODE_MARKERS.get(mode, "o"),
                 color=MODE_COLORS.get(mode, "#000"),
                 linewidth=2, markersize=8, label=mode)

    plt.title("Execution Time vs. Dataset Size (Scale-up)",
              fontsize=14, fontweight='bold')
    plt.ylabel("Execution Time (ms)", fontsize=12)
    plt.xlabel("Number of Rows (Millions)", fontsize=12)
    plt.grid(True, linestyle='--', alpha=0.7)
    plt.legend(title="Algorithm Mode")
    plt.tight_layout()

    out_path = os.path.join(PLOTS_DIR, "scaling_comparison.png")
    plt.savefig(out_path, dpi=300)
    print(f"Saved plot: {out_path}")


if __name__ == "__main__":
    # Ensure matplotlib is not running in interactive mode causing it to hang
    plt.ioff()

    df = load_results()
    if not df.empty:
        print("Loaded benchmark data:")
        print(df.to_string(index=False))
        print("-" * 40)

        plot_execution_time(df)
        plot_pass_count(df)
        plot_scaling_line_chart(df)  # NEW: Generate the scaling line chart

        print("Phase 8 plotting complete! Check the /plots directory.")
