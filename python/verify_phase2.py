#!/usr/bin/env python3
"""Cross-check C++ brute-force output against Python exact groupby.sum().

Handles ties at the k-th boundary correctly: if multiple groups share the
same aggregate as the k-th group, any selection among them is valid.
"""
import json
import numpy as np
import sys

def verify(bin_path, n_rows, json_path, k):
    # Read binary dataset
    dt = np.dtype([("group_id", "<u8"), ("value", "<f8")])
    data = np.fromfile(bin_path, dtype=dt)
    assert len(data) == n_rows, f"Row count mismatch: {len(data)} vs {n_rows}"

    # Python ground truth: exact groupby sum
    agg = {}
    for row in data:
        gid = int(row["group_id"])
        agg[gid] = agg.get(gid, 0.0) + float(row["value"])

    sorted_agg = sorted(agg.items(), key=lambda x: -x[1])

    # Read C++ JSON output
    with open(json_path) as f:
        cpp_result = json.load(f)
    cpp_topk = [(r["group_id"], r["aggregate"]) for r in cpp_result["top_k_results"]]
    cpp_dict = dict(cpp_topk)

    print(f"\n=== Verifying {bin_path} (k={k}) ===")
    print(f"Python top-{k} (first 5): {sorted_agg[:5]}...")
    print(f"C++    top-{k} (first 5): {cpp_topk[:5]}...")

    # The k-th boundary value: aggregate of the k-th Python group
    boundary_val = sorted_agg[k - 1][1] if k <= len(sorted_agg) else 0.0

    # All groups strictly above the boundary MUST be in both sets
    py_above = {gid for gid, val in sorted_agg if val > boundary_val}
    cpp_above = {gid for gid, val in cpp_topk if val > boundary_val}

    if py_above != cpp_above:
        print(f"FAIL: Groups strictly above boundary ({boundary_val:.2f}) differ!")
        print(f"  Missing: {py_above - cpp_above}")
        print(f"  Extra:   {cpp_above - py_above}")
        return False

    # Groups AT the boundary: C++ may pick any subset of tied groups.
    # Just verify that C++ picked from the correct pool.
    py_at_boundary = {gid for gid, val in agg.items() if abs(val - boundary_val) < 1e-6}
    cpp_at_boundary = {gid for gid, val in cpp_topk if abs(val - boundary_val) < 1e-6}

    if not cpp_at_boundary.issubset(py_at_boundary):
        print(f"FAIL: C++ picked boundary groups not in Python's pool!")
        print(f"  Invalid: {cpp_at_boundary - py_at_boundary}")
        return False

    # Verify aggregate values match for all non-boundary groups
    max_diff = 0.0
    for gid, cpp_val in cpp_topk:
        if abs(cpp_val - boundary_val) < 1e-6:
            continue  # boundary ties — skip value check
        py_val = agg[gid]
        diff = abs(py_val - cpp_val)
        max_diff = max(max_diff, diff)
        if diff > 1e-6:
            print(f"FAIL: Aggregate mismatch for group {gid}: "
                  f"py={py_val:.6f} cpp={cpp_val:.6f} diff={diff:.2e}")
            return False

    # Verify correct count
    if len(cpp_topk) != min(k, len(agg)):
        print(f"FAIL: Expected {min(k, len(agg))} results, got {len(cpp_topk)}")
        return False

    n_tied = len(cpp_at_boundary)
    print(f"PASS: {k} groups correct. {len(py_above)} strictly above boundary, "
          f"{n_tied} at boundary ({boundary_val:.2f}). Max diff: {max_diff:.2e}")
    return True

ok = True
ok &= verify("data/tiny.bin", 1012, "results/tiny_bf.json", 5)
ok &= verify("data/S0.bin", 10089, "results/S0_bf.json", 10)

print(f"\n{'ALL PASSED' if ok else 'FAILURES DETECTED'}")
sys.exit(0 if ok else 1)
