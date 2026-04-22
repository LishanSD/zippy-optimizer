#!/usr/bin/env python3
"""
python/generate_data.py — Synthetic dataset generator for Zippy top-k experiments.

Generates binary datasets with configurable Zipf skew and optional rare high-value
groups (adversarial pattern for Extension A/B evaluation).

Binary format: contiguous 16-byte rows, each (group_id: uint64 LE, value: float64 LE).
No header. Total file size = n_rows × 16 bytes.

See AGENTS.md Section 9 for full specification.
"""

import argparse
import json
import os
import sys
import numpy as np


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
    """
    Generate a synthetic dataset and write it as a binary file.

    Parameters
    ----------
    output_path : str
        Path to write the binary dataset file.
    n_rows : int
        Number of base rows (before rare-group injection).
    n_groups : int
        Number of unique groups in the base Zipf distribution.
    zipf_alpha : float
        Zipf skew parameter. Higher = more skew. Must be > 0.
        For alpha > 1.0: uses numpy's fast Zipf sampler.
        For alpha <= 1.0: uses weighted sampling (slower but correct).
    value_distribution : str
        One of "exponential", "uniform", "constant".
    value_scale : float
        Mean/scale of value distribution. All values > 0.
    rare_group_fraction : float
        Fraction of n_groups to create as rare high-value groups (0.0 = none).
    rare_group_rows : int
        Maximum rows per rare group (actual count is random in [1, rare_group_rows]).
    rare_group_value_multiplier : float
        Rare group values = value_scale × this multiplier.
    seed : int
        Random seed for reproducibility.

    Returns
    -------
    dict
        Metadata about the generated dataset.
    """
    rng = np.random.default_rng(seed)

    # ── Generate Zipf-distributed group IDs bounded to [0, n_groups - 1] ────
    if zipf_alpha > 1.0:
        # numpy.zipf requires alpha > 1.0
        # zipf() returns ranks in [1, ∞) — subtract 1 and take modulo
        ranks = rng.zipf(zipf_alpha, size=n_rows)
        group_ids = ((ranks - 1) % n_groups).astype(np.uint64)
    else:
        # For alpha <= 1.0 (e.g., dataset S5 with alpha=0.8), use weighted sampling.
        # Weights proportional to 1/rank^alpha give a valid Zipf-like distribution.
        print(f"  Using weighted sampling for zipf_alpha={zipf_alpha} (<= 1.0)...")
        ranks = np.arange(1, n_groups + 1, dtype=np.float64)
        weights = 1.0 / np.power(ranks, zipf_alpha)
        weights /= weights.sum()
        group_ids = rng.choice(n_groups, size=n_rows, p=weights).astype(np.uint64)

    # ── Generate positive values ────────────────────────────────────────────
    if value_distribution == "exponential":
        values = rng.exponential(scale=value_scale, size=n_rows)
    elif value_distribution == "uniform":
        values = rng.uniform(0.0, value_scale * 2, size=n_rows)
    elif value_distribution == "constant":
        values = np.full(n_rows, value_scale, dtype=np.float64)
    else:
        raise ValueError(f"Unknown value_distribution: {value_distribution}")

    # ── Inject rare high-value groups (adversarial pattern for extensions) ──
    n_rare = int(n_groups * rare_group_fraction)
    if n_rare > 0:
        # Rare groups get IDs in [n_groups, n_groups + n_rare) — distinct from normal
        rare_ids = np.arange(n_groups, n_groups + n_rare, dtype=np.uint64)
        extra_group_ids = []
        extra_values = []
        for gid in rare_ids:
            n_rare_actual = rng.integers(1, rare_group_rows + 1)
            extra_group_ids.extend([gid] * n_rare_actual)
            extra_values.extend(
                [value_scale * rare_group_value_multiplier] * n_rare_actual
            )

        group_ids = np.concatenate(
            [group_ids, np.array(extra_group_ids, dtype=np.uint64)]
        )
        values = np.concatenate(
            [values, np.array(extra_values, dtype=np.float64)]
        )

        # Shuffle so rare groups are not clustered at the end
        perm = rng.permutation(len(group_ids))
        group_ids = group_ids[perm]
        values = values[perm]
    else:
        n_rare = 0

    # ── Mask group_ids to exclude UINT64_MAX (FA sentinel) ──────────────────
    uint64_max = np.iinfo(np.uint64).max
    group_ids = np.where(group_ids == uint64_max, np.uint64(0), group_ids)

    # ── Write binary file ───────────────────────────────────────────────────
    # Use numpy structured array for fast, correct little-endian output.
    # Each element is 16 bytes: (uint64 LE, float64 LE), written contiguously.
    actual_n_rows = len(group_ids)
    dt = np.dtype([("group_id", "<u8"), ("value", "<f8")])
    data = np.empty(actual_n_rows, dtype=dt)
    data["group_id"] = group_ids
    data["value"] = values

    os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)
    data.tofile(output_path)

    # ── Verify file size ────────────────────────────────────────────────────
    expected_size = actual_n_rows * 16
    actual_size = os.path.getsize(output_path)
    assert actual_size == expected_size, (
        f"File size mismatch: expected {expected_size}, got {actual_size}"
    )

    # ── Compute and return metadata ─────────────────────────────────────────
    n_unique = len(np.unique(group_ids))
    metadata = {
        "n_rows": actual_n_rows,
        "n_groups_base": n_groups,
        "n_groups_total": n_groups + n_rare,
        "n_groups_actual_unique": int(n_unique),
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
        description="Generate synthetic Zipf-distributed datasets for Zippy experiments."
    )
    parser.add_argument("--output", required=True, help="Path to binary output file")
    parser.add_argument("--n-rows", type=int, required=True, help="Number of base rows")
    parser.add_argument("--n-groups", type=int, required=True, help="Number of unique groups")
    parser.add_argument("--zipf-alpha", type=float, default=1.2,
                        help="Zipf skew parameter (default: 1.2). Must be > 0.")
    parser.add_argument("--value-distribution", default="exponential",
                        choices=["exponential", "uniform", "constant"],
                        help="Value distribution (default: exponential)")
    parser.add_argument("--value-scale", type=float, default=100.0,
                        help="Mean/scale of value distribution (default: 100.0)")
    parser.add_argument("--rare-group-fraction", type=float, default=0.0,
                        help="Fraction of groups to make rare high-value (default: 0.0)")
    parser.add_argument("--rare-group-rows", type=int, default=3,
                        help="Max rows per rare group (default: 3)")
    parser.add_argument("--rare-group-value-multiplier", type=float, default=100.0,
                        help="Multiplier for rare group values (default: 100.0)")
    parser.add_argument("--seed", type=int, default=42,
                        help="Random seed (default: 42)")
    args = parser.parse_args()

    if args.zipf_alpha <= 0:
        parser.error("--zipf-alpha must be > 0")

    print(f"Generating dataset: {args.n_rows} rows, {args.n_groups} groups, "
          f"zipf_alpha={args.zipf_alpha}, rare_frac={args.rare_group_fraction}")

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
    print(f"  Rows:          {metadata['n_rows']}")
    print(f"  Unique groups: {metadata['n_groups_actual_unique']}")
    print(f"  Rare groups:   {metadata['n_rare_groups']}")
    print(f"  File size:     {metadata['file_size_bytes']:,} bytes "
          f"({metadata['file_size_bytes'] / (1024*1024):.1f} MB)")
    print(json.dumps(metadata, indent=2))


if __name__ == "__main__":
    main()
