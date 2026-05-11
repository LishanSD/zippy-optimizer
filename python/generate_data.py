#!/usr/bin/env python3
"""
python/generate_data.py — Synthetic dataset generator for Zippy top-k experiments.

Generates binary datasets with configurable Zipf skew and optional rare high-value
groups (adversarial pattern for Extension A/B evaluation).

Memory-Optimized: Uses chunking to stream data to disk, preventing RAM exhaustion
on VLDB-scale datasets (200M+ rows).
"""

import argparse
import json
import os
import sys
import numpy as np
from tqdm import tqdm  # Added tqdm

CHUNK_SIZE = 10_000_000  # Generate 10M rows at a time to keep RAM under 1GB


def generate_dataset(
    output_path: str,
    n_rows: int,
    n_groups: int,
    zipf_alpha: float,
    value_distribution: str = "exponential",
    value_scale: float = 100.0,
    rare_group_fraction: float = 0.0,
    rare_group_rows: int = 3,
    rare_group_value_multiplier: float = 100.0,
    seed: int = 42,
) -> dict:

    rng = np.random.default_rng(seed)

    # ── 1. Pre-compute Weights (if needed) ──────────────────────────────────
    weights = None
    if zipf_alpha <= 1.0:
        print(
            f"  Pre-computing weights for zipf_alpha={zipf_alpha} (this takes a moment)...")
        ranks = np.arange(1, n_groups + 1, dtype=np.float64)
        weights = 1.0 / np.power(ranks, zipf_alpha)
        weights /= weights.sum()

    # ── 2. Pre-generate Rare Groups ─────────────────────────────────────────
    n_rare = int(n_groups * rare_group_fraction)
    extra_group_ids = []
    extra_values = []

    if n_rare > 0:
        rare_ids = np.arange(n_groups, n_groups + n_rare, dtype=np.uint64)
        for gid in rare_ids:
            n_rare_actual = rng.integers(1, rare_group_rows + 1)
            extra_group_ids.extend([gid] * n_rare_actual)
            extra_values.extend(
                [value_scale * rare_group_value_multiplier] * n_rare_actual)

        extra_group_ids = np.array(extra_group_ids, dtype=np.uint64)
        extra_values = np.array(extra_values, dtype=np.float64)

        # Shuffle the rare pool
        perm = rng.permutation(len(extra_group_ids))
        extra_group_ids = extra_group_ids[perm]
        extra_values = extra_values[perm]

    total_rare_rows = len(extra_group_ids) if n_rare > 0 else 0
    actual_total_rows = n_rows + total_rare_rows

    # ── 3. Stream Chunks to Disk ────────────────────────────────────────────
    os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)

    rows_generated = 0
    rare_idx = 0

    print("\nStreaming rows to disk:")
    # Open file and initialize tqdm progress bar
    with open(output_path, "wb") as f, tqdm(total=n_rows, unit="row", unit_scale=True, colour='green') as pbar:
        while rows_generated < n_rows:
            current_chunk = min(CHUNK_SIZE, n_rows - rows_generated)

            # Generate base group IDs
            if zipf_alpha > 1.0:
                ranks = rng.zipf(zipf_alpha, size=current_chunk)
                chunk_gids = ((ranks - 1) % n_groups).astype(np.uint64)
            else:
                chunk_gids = rng.choice(
                    n_groups, size=current_chunk, p=weights).astype(np.uint64)

            # Generate base values
            if value_distribution == "exponential":
                chunk_vals = rng.exponential(
                    scale=value_scale, size=current_chunk)
            elif value_distribution == "uniform":
                chunk_vals = rng.uniform(
                    0.0, value_scale * 2, size=current_chunk)
            elif value_distribution == "constant":
                chunk_vals = np.full(
                    current_chunk, value_scale, dtype=np.float64)

            # Inject a proportional amount of rare rows into this chunk
            if total_rare_rows > 0:
                fraction = current_chunk / n_rows
                rare_in_chunk = int(total_rare_rows * fraction)

                # If last chunk, grab all remaining rare rows
                if rows_generated + current_chunk == n_rows:
                    rare_in_chunk = total_rare_rows - rare_idx

                if rare_in_chunk > 0:
                    chunk_gids = np.concatenate(
                        [chunk_gids, extra_group_ids[rare_idx: rare_idx + rare_in_chunk]])
                    chunk_vals = np.concatenate(
                        [chunk_vals, extra_values[rare_idx: rare_idx + rare_in_chunk]])
                    rare_idx += rare_in_chunk

            # Mask out UINT64_MAX
            uint64_max = np.iinfo(np.uint64).max
            chunk_gids = np.where(chunk_gids == uint64_max,
                                  np.uint64(0), chunk_gids)

            # Shuffle the chunk
            perm = rng.permutation(len(chunk_gids))
            chunk_gids = chunk_gids[perm]
            chunk_vals = chunk_vals[perm]

            # Write to disk
            dt = np.dtype([("group_id", "<u8"), ("value", "<f8")])
            data = np.empty(len(chunk_gids), dtype=dt)
            data["group_id"] = chunk_gids
            data["value"] = chunk_vals

            f.write(data.tobytes())

            # Update loop variables and progress bar
            rows_generated += current_chunk
            pbar.update(current_chunk)

    print("\n  Done writing to disk.")

    # ── 4. Verify and Metadata ──────────────────────────────────────────────
    expected_size = actual_total_rows * 16
    actual_size = os.path.getsize(output_path)
    assert actual_size == expected_size, f"File size mismatch: expected {expected_size}, got {actual_size}"

    metadata = {
        "n_rows": actual_total_rows,
        "n_groups_base": n_groups,
        "n_groups_total": n_groups + n_rare,
        "n_rare_groups": n_rare,
        "zipf_alpha": zipf_alpha,
        "value_distribution": value_distribution,
        "value_scale": value_scale,
        "rare_group_value_multiplier": rare_group_value_multiplier,
        "seed": seed,
        "file_size_bytes": actual_size,
        "output_path": output_path,
    }
    return metadata


def main():
    parser = argparse.ArgumentParser(
        description="Generate synthetic Zipf-distributed datasets.")
    parser.add_argument("--output", required=True,
                        help="Path to binary output file")
    parser.add_argument("--n-rows", type=int, required=True,
                        help="Number of base rows")
    parser.add_argument("--n-groups", type=int, required=True,
                        help="Number of unique groups")
    parser.add_argument("--zipf-alpha", type=float, default=1.2)
    parser.add_argument("--value-distribution", default="exponential",
                        choices=["exponential", "uniform", "constant"])
    parser.add_argument("--value-scale", type=float, default=100.0)
    parser.add_argument("--rare-group-fraction", type=float, default=0.0)
    parser.add_argument("--rare-group-rows", type=int, default=3)
    parser.add_argument("--rare-group-value-multiplier",
                        type=float, default=100.0)
    parser.add_argument("--seed", type=int, default=42)
    args = parser.parse_args()

    if args.zipf_alpha <= 0:
        parser.error("--zipf-alpha must be > 0")

    print(
        f"Generating dataset: {args.n_rows:,} rows, {args.n_groups:,} groups, alpha={args.zipf_alpha}")

    metadata = generate_dataset(
        output_path=args.output,
        n_rows=args.n_rows,
        n_groups=args.n_groups,
        zipf_alpha=args.zipf_alpha,
        value_distribution=args.value_distribution,
        value_scale=args.value_scale,
        rare_group_fraction=args.rare_group_fraction,
        rare_group_rows=args.rare_group_rows,
        rare_group_value_multiplier=args.rare_group_value_multiplier,
        seed=args.seed,
    )

    print(f"Written: {metadata['output_path']}")
    print(f"  Total Rows:    {metadata['n_rows']:,}")
    print(f"  Total Groups:  {metadata['n_groups_total']:,}")
    print(f"  Rare Groups:   {metadata['n_rare_groups']:,}")
    print(
        f"  File size:     {metadata['file_size_bytes'] / (1024*1024):.1f} MB")


if __name__ == "__main__":
    main()
