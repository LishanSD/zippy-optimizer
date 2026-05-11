import json
import os
import glob
import pandas as pd

RESULTS_DIR = "results"
OUTPUT_FILE = "results/PERFORMANCE_TABLES.md"


def load_extended_results():
    """Reads JSON files and extracts deep metrics for tabular display."""
    data = []
    filepaths = glob.glob(os.path.join(RESULTS_DIR, "*.json"))

    if not filepaths:
        print(f"No JSON files found in {RESULTS_DIR}/")
        return pd.DataFrame()

    for file in filepaths:
        filename = os.path.basename(file)
        parts = filename.replace(".json", "").rsplit("_", 1)
        dataset_name = parts[0] if len(parts) > 1 else "unknown"

        with open(file, 'r') as f:
            res = json.load(f)
            metrics = res.get("metrics", {})
            n_rows = res.get("n_rows", 0)

            data.append({
                "Dataset": dataset_name,
                "Mode": res.get("mode", "unknown"),
                "Rows": f"{n_rows:,}",  # Added comma formatting for millions
                "Passes": metrics.get("total_passes", 0),
                "Pruned (%)": round(metrics.get("partitions_pruned_pct", 0.0) * 100, 2),
                "Top-K Bound": f"{metrics.get('topKBound_after_pass1', 0):,.0f}",
                "Total Time (ms)": f"{metrics.get('total_duration_ms', 0):.1f}",
                "Index Build (ms)": f"{metrics.get('index_build_duration_ms', 0):.1f}",
                "Sample Time (ms)": f"{metrics.get('sample_duration_ms', 0):.1f}",
                "Pass 1 (ms)": f"{metrics.get('pass1_duration_ms', 0):.1f}",
                "Pass 2+ (ms)": f"{metrics.get('pass2plus_duration_ms', 0):.1f}"
            })

    return pd.DataFrame(data)


def generate_markdown():
    try:
        import tabulate
    except ImportError:
        print("❌ Error: Missing required library 'tabulate'.")
        print("Please run: pip install tabulate")
        return

    df = load_extended_results()
    if df.empty:
        return

    # Sort data for clean presentation
    mode_order = {"brute-force": 0, "baseline": 1,
                  "ext-a": 2, "ext-b": 3, "ext-ab": 4}
    df['Mode_Rank'] = df['Mode'].map(mode_order)
    df = df.sort_values(by=['Dataset', 'Mode_Rank']
                        ).drop(columns=['Mode_Rank'])

    # 1. Primary Execution Summary Table (Added "Rows" here)
    summary_cols = ["Dataset", "Rows", "Mode", "Passes",
                    "Pruned (%)", "Top-K Bound", "Total Time (ms)"]
    summary_df = df[summary_cols]

    # 2. Timing Breakdown Table
    timing_cols = ["Dataset", "Mode",
                   "Index Build (ms)", "Sample Time (ms)", "Pass 1 (ms)", "Pass 2+ (ms)", "Total Time (ms)"]
    timing_df = df[timing_cols]

    # Write to Markdown file
    with open(OUTPUT_FILE, 'w') as f:
        f.write("# Zippy Optimizer Experiment Results\n\n")

        f.write("## 1. Execution Summary\n")
        f.write(
            "This table highlights the convergence efficiency and overall execution time.\n\n")
        f.write(summary_df.to_markdown(index=False))
        f.write("\n\n---\n\n")

        f.write("## 2. Timing Breakdown\n")
        f.write("This table breaks down the computational phases to demonstrate the overhead versus savings of the extensions.\n\n")
        f.write(timing_df.to_markdown(index=False))
        f.write("\n")

    print(
        f"✅ Markdown tables successfully generated and saved to: {OUTPUT_FILE}")
    print("You can copy/paste these directly into your documentation!")


if __name__ == "__main__":
    generate_markdown()
