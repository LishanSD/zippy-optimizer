#!/usr/bin/env python3
"""
Generate synthetic benchmark datasets for Zippy.

Implements the 6 distributions from Siddiqui et al. (PVLDB 17(4), 2023) Section 6.5,
derived from Gray et al. [16] (SIGMOD 1994):
  1. uniform       – groups chosen uniformly at random
  2. sorted        – sequential cyclic assignment (tests locality)
  3. heavy-hitter  – top 10% groups get 50% of tuples
  4. zipf          – power-law (exponent configurable; paper uses 0.5)
  5. self-similar  – 80-20 Pareto proportion
  6. moving-cluster – sliding window of 1024 groups

Binary format: N × 16 bytes, each (group_id: uint64 LE, value: float64 LE).
"""

import argparse
import json
import os
import sys
import time
import numpy as np
from pathlib import Path

DISTRIBUTIONS = ["uniform", "sorted", "heavy-hitter", "zipf", "self-similar", "moving-cluster"]


def generate_group_ids(dist: str, n_rows: int, n_groups: int,
                       rng: np.random.Generator, zipf_alpha: float = 0.5) -> np.ndarray:
    if dist == "uniform":
        return rng.integers(0, n_groups, size=n_rows, dtype=np.uint64)

    elif dist == "sorted":
        # Sequential cyclic: group 0, 1, 2, ..., n_groups-1, 0, 1, ...
        return (np.arange(n_rows, dtype=np.uint64) % np.uint64(n_groups))

    elif dist == "heavy-hitter":
        # 10% of groups account for 50% of tuples (heavy hitters); rest uniform.
        n_heavy = max(1, int(n_groups * 0.10))
        n_tuples_heavy = int(n_rows * 0.50)
        n_tuples_light = n_rows - n_tuples_heavy
        heavy_ids = rng.integers(0, n_heavy, size=n_tuples_heavy, dtype=np.uint64)
        light_ids = rng.integers(n_heavy, n_groups, size=n_tuples_light, dtype=np.uint64)
        ids = np.concatenate([heavy_ids, light_ids])
        return ids[rng.permutation(n_rows)]

    elif dist == "zipf":
        if zipf_alpha > 1.0:
            # numpy.zipf is fast for alpha > 1
            ranks = rng.zipf(zipf_alpha, size=n_rows)
            return ((ranks - 1) % n_groups).astype(np.uint64)
        else:
            # For alpha ≤ 1 use weighted CDF sampling.
            # Chunked to cap peak memory when n_groups is large.
            ranks = np.arange(1, n_groups + 1, dtype=np.float64)
            weights = 1.0 / np.power(ranks, max(zipf_alpha, 1e-9))
            weights /= weights.sum()
            chunk = 5_000_000
            parts = []
            remaining = n_rows
            while remaining > 0:
                take = min(chunk, remaining)
                parts.append(rng.choice(n_groups, size=take, p=weights).astype(np.uint64))
                remaining -= take
            return np.concatenate(parts)

    elif dist == "self-similar":
        # 80-20 rule: top 20% of groups get 80% of tuples.
        n_popular = max(1, int(n_groups * 0.20))
        n_tuples_popular = int(n_rows * 0.80)
        n_tuples_rare = n_rows - n_tuples_popular
        popular_ids = rng.integers(0, n_popular, size=n_tuples_popular, dtype=np.uint64)
        rare_ids = rng.integers(n_popular, n_groups, size=n_tuples_rare, dtype=np.uint64)
        ids = np.concatenate([popular_ids, rare_ids])
        return ids[rng.permutation(n_rows)]

    elif dist == "moving-cluster":
        # Sliding window of 1024 groups that sweeps linearly across the group space.
        window = min(1024, n_groups)
        i = np.arange(n_rows, dtype=np.int64)
        window_start = ((i * (n_groups - window)) // n_rows).astype(np.int64)
        offsets = rng.integers(0, window, size=n_rows, dtype=np.int64)
        return ((window_start + offsets) % n_groups).astype(np.uint64)

    else:
        raise ValueError(f"Unknown distribution: {dist!r}")


def generate_values(n_rows: int, value_dist: str, value_scale: float,
                    rng: np.random.Generator) -> np.ndarray:
    if value_dist == "uniform":
        return rng.uniform(0.0, value_scale, size=n_rows)
    elif value_dist == "exponential":
        return rng.exponential(scale=value_scale / 2.0, size=n_rows)
    else:
        raise ValueError(f"Unknown value distribution: {value_dist!r}")


def generate_dataset(out_path: str, n_rows: int, n_groups: int, group_dist: str,
                     value_dist: str = "uniform", value_scale: float = 100.0,
                     zipf_alpha: float = 0.5, seed: int = 42) -> dict:
    t0 = time.time()
    rng = np.random.default_rng(seed)

    group_ids = generate_group_ids(group_dist, n_rows, n_groups, rng, zipf_alpha)
    values = generate_values(n_rows, value_dist, value_scale, rng)

    # Mask FA sentinel (UINT64_MAX)
    group_ids = np.where(group_ids == np.iinfo(np.uint64).max,
                         np.uint64(0), group_ids)

    dt = np.dtype([("group_id", "<u8"), ("value", "<f8")])
    data = np.empty(n_rows, dtype=dt)
    data["group_id"] = group_ids
    data["value"] = values

    Path(out_path).parent.mkdir(parents=True, exist_ok=True)
    data.tofile(out_path)

    file_size = os.path.getsize(out_path)
    assert file_size == n_rows * 16, f"size mismatch: {file_size} != {n_rows*16}"

    # Count unique groups without allocating a full sorted copy
    n_unique = int(np.unique(group_ids).size)
    elapsed = time.time() - t0

    return {
        "n_rows": n_rows,
        "n_groups_requested": n_groups,
        "n_groups_actual": n_unique,
        "group_dist": group_dist,
        "zipf_alpha": zipf_alpha,
        "value_dist": value_dist,
        "value_scale": value_scale,
        "seed": seed,
        "file_size_bytes": file_size,
        "file_size_mb": round(file_size / (1024 ** 2), 1),
        "gen_time_sec": round(elapsed, 1),
        "path": out_path,
    }


def main():
    parser = argparse.ArgumentParser(
        description="Generate Zippy benchmark datasets (6 distributions, multiple sizes)."
    )
    parser.add_argument("--data-dir", default="data/benchmark")
    parser.add_argument("--sizes", nargs="+", type=int,
                        default=[1_000_000, 5_000_000, 10_000_000, 50_000_000],
                        help="N-rows values to generate (default: 1M 5M 10M 50M)")
    parser.add_argument("--groups-ratio", type=float, default=0.1,
                        help="n_groups = round(n_rows * groups_ratio). Default 0.1 → 10 rows/group avg.")
    parser.add_argument("--distributions", nargs="+", default=DISTRIBUTIONS,
                        choices=DISTRIBUTIONS)
    parser.add_argument("--value-dist", default="uniform",
                        choices=["uniform", "exponential"])
    parser.add_argument("--value-scale", type=float, default=100.0)
    parser.add_argument("--zipf-alpha", type=float, default=0.5,
                        help="Zipf exponent for the 'zipf' distribution. Default 0.5 (paper).")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--skip-existing", action="store_true",
                        help="Skip if output file already exists")
    parser.add_argument("--manifest", default=None,
                        help="Path to save manifest JSON (default: <data-dir>/manifest.json)")
    args = parser.parse_args()

    Path(args.data_dir).mkdir(parents=True, exist_ok=True)
    manifest_path = args.manifest or os.path.join(args.data_dir, "manifest.json")

    manifest = []
    total_skipped = 0

    for n_rows in args.sizes:
        n_groups = max(100, round(n_rows * args.groups_ratio))
        size_tag = f"{n_rows // 1_000_000}M" if n_rows >= 1_000_000 else f"{n_rows // 1_000}K"
        groups_tag = f"{n_groups // 1_000_000}M" if n_groups >= 1_000_000 else f"{n_groups // 1_000}K"

        for dist in args.distributions:
            fname = f"{dist}_{size_tag}_G{groups_tag}.bin"
            out_path = os.path.join(args.data_dir, fname)

            if args.skip_existing and os.path.exists(out_path):
                file_size = os.path.getsize(out_path)
                print(f"  SKIP  {fname}  ({file_size / (1024**2):.0f} MB)")
                manifest.append({
                    "n_rows": n_rows, "n_groups_requested": n_groups,
                    "group_dist": dist, "path": out_path,
                    "file_size_bytes": file_size,
                    "file_size_mb": round(file_size / (1024**2), 1),
                    "skipped": True,
                })
                total_skipped += 1
                continue

            print(f"\n[{dist}] N={n_rows:,}  G={n_groups:,}  →  {fname}")
            meta = generate_dataset(
                out_path=out_path,
                n_rows=n_rows,
                n_groups=n_groups,
                group_dist=dist,
                value_dist=args.value_dist,
                value_scale=args.value_scale,
                zipf_alpha=args.zipf_alpha,
                seed=args.seed,
            )
            print(f"  {meta['file_size_mb']} MB  {meta['n_groups_actual']:,} unique groups"
                  f"  ({meta['gen_time_sec']}s)")
            manifest.append({**meta, "skipped": False})

    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=2)

    total_generated = len(manifest) - total_skipped
    total_mb = sum(m.get("file_size_mb", 0) for m in manifest)
    print(f"\nDone. Generated {total_generated} datasets, skipped {total_skipped}.")
    print(f"Total disk: {total_mb:.0f} MB")
    print(f"Manifest: {manifest_path}")


if __name__ == "__main__":
    main()
