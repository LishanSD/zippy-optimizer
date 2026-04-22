# Zippy — Cache-Efficient Top-K Aggregation with Index Extensions

A C++17 implementation of the **Zippy** algorithm for cache-efficient top-k aggregation over high-cardinality datasets, extended with two novel index-based optimizations proposed (but not implemented) in the original paper.

> **Paper:** *"Cache-Efficient Top-k Aggregation over High Cardinality Large Datasets"*
> — Siddiqui et al., PVLDB 17(4): 644–656, 2023

---

## What Problem Does This Solve?

Consider a query like:

```sql
SELECT salesperson, SUM(revenue)
FROM transactions
GROUP BY salesperson
ORDER BY SUM(revenue) DESC
LIMIT 50;
```

On large datasets (200M+ rows, 30M unique groups), the naive approach — computing exact aggregates for every group — causes massive **CPU cache thrashing**. All those groups can't fit in L1/L2 cache simultaneously, and each cache miss is ~100× slower than a hit.

**Zippy's insight:** Real-world data follows power-law (Zipf) distributions. A small number of groups hold most of the aggregate value. By identifying those top groups early via sampling, Zippy tracks them exactly in fast cache memory (FA) while using coarse partition-level bounds (CA) to prune the rest — often eliminating 90%+ of groups after a single pass.

## Our Extensions

The original paper identifies two enhancements as future work but never implements them. This project builds both:

| Extension | What It Does | Why It Helps |
|-----------|-------------|--------------|
| **A — Stratified Sampling** | Builds a group occurrence index, then boosts underrepresented rare groups in the sample | Rare high-value groups (1 row worth $999K) are almost never sampled uniformly. The index ensures they get into FA. |
| **B — Measure Column Index** | Scans for rows with extreme individual values via a min-heap, and force-injects their groups into FA | Directly captures "needle in a haystack" groups that uniform sampling misses entirely. |

Both extensions improve **efficiency** (fewer passes, more pruning) without affecting **correctness** — Zippy's pruning is always mathematically safe.

---

## Algorithm Modes

The binary supports five modes via `--mode <name>`. All modes produce the **same correct top-k result** — they differ only in how efficiently they get there.

### `brute-force` — Ground Truth Reference
The simplest approach: hash-aggregates **every group** in one pass using an `unordered_map`, then picks the top-k via `partial_sort`. This is the correctness anchor — all other modes are validated against its output. It's slow on large datasets because the hash map doesn't fit in CPU cache, causing constant cache misses.

**When to use:** Generating ground-truth results; verifying other modes produce correct output.

### `baseline` — Standard Zippy (Paper Algorithm)
Implements the full Zippy algorithm from the paper:
1. **Sample** — randomly selects ~1% of rows and identifies the likely top-k groups (FA candidates)
2. **Pass 1** — scans all rows; FA candidates get exact aggregation, everything else goes to coarse CA partitions
3. **Prune** — eliminates CA partitions whose upper bound is below the k-th FA value (typically 90%+ pruned)
4. **Pass 2+** — re-scans only surviving partitions to resolve remaining groups

Uses **uniform random sampling** to identify FA candidates. This works well when the top groups are frequent (high Zipf skew), but misses rare high-value groups.

**When to use:** Standard top-k queries on skewed data without rare-group adversarial patterns.

### `ext-a` — Extension A: Stratified Sampling
Enhances baseline Zippy by replacing uniform sampling with **stratified sampling** powered by a pre-built group occurrence index:
1. **Build index** — single pre-pass to count occurrences of each group
2. **Stratified sample** — groups appearing less than expected (underrepresented) get their rows **boosted** in the sample, ensuring rare groups are included
3. **Rest of Zippy** — identical to baseline from Pass 1 onward

Targets the weakness where uniform sampling misses groups with very few rows (e.g., a salesperson with only 3 transactions, each worth $1M).

**When to use:** Datasets with rare high-value groups (non-zero `--rare-group-fraction`).

### `ext-b` — Extension B: Measure Column Index
Enhances baseline Zippy by pre-scanning the value column to find **extreme individual values** and force-injecting their groups into FA:
1. **Build index** — single pre-pass maintaining a min-heap of the top-m largest row values
2. **Force-inject** — groups owning those extreme values are added to FA before sampling fills remaining slots
3. **Rest of Zippy** — identical to baseline from Pass 1 onward

Targets "needle in a haystack" groups: a single row with value $10M makes its group a guaranteed top-k member, yet uniform sampling has only a 1% chance of seeing it.

**When to use:** Datasets where individual row values vary by orders of magnitude.

### `ext-ab` — Combined Extensions A + B
Applies **both** extensions simultaneously:
1. Build both indices (group occurrence + measure column) in a single pre-pass
2. Force-inject extreme-value groups (Extension B) into FA first
3. Fill remaining FA slots via stratified sampling (Extension A)
4. Run standard Zippy from Pass 1 onward

This is the most robust mode — it handles both rare-group and extreme-value adversarial patterns.

**When to use:** Production workloads or benchmarking the combined improvement.

---

## Project Structure

```
zippy-optimizer/
├── src/                          C++ core engine
│   ├── main.cpp                  CLI entry point, argument parsing, data loading
│   ├── zippy.h / zippy.cpp       Zippy algorithm (baseline implemented through Phase 4C)
│   ├── data_structures.h         FATable, CATable, FMSketch, hash functions
│   ├── sampler.h / sampler.cpp   Uniform random sampler (Phase 4)
│   ├── group_index.h / .cpp      Extension A: GroupOccurrenceIndex (Phase 5)
│   ├── stratified_sampler.h/.cpp Extension A: stratified sampling (Phase 5)
│   ├── measure_index.h / .cpp    Extension B: min-heap MeasureIndex (Phase 6)
│   ├── utils.h                   Timer, metrics, JSON output
│   └── test_data_structures.cpp  Unit tests for core data structures
│
├── python/                       Python orchestration layer
│   ├── generate_data.py          Synthetic dataset generator (Zipf + rare groups)
│   ├── run_experiments.py        Parameter sweeps, calls C++ binary (Phase 8)
│   ├── plot_results.py           Matplotlib plots for results (Phase 8)
│   ├── compare_phase4c_results.py Compare baseline vs brute-force outputs (Phase 4C)
│   ├── verify_correctness.py     Checks all modes match brute-force (Phase 7)
│   └── verify_phase2.py          Cross-checks brute-force vs Python pandas
│
├── data/                         Generated binary datasets (.gitignored if large)
├── results/                      JSON output from experiment runs
├── plots/                        PNG output from plot_results.py
├── build/                        Compiled binaries
│   ├── zippy.exe                 Main Zippy binary
│   └── test_ds.exe               Data structure unit tests
├── CMakeLists.txt                Build configuration
├── AGENTS.md                     Full algorithm spec + implementation guide
└── README.md                     This file
```

---

## Current Implementation Status

The project follows an 8-phase implementation plan (see `AGENTS.md` §5.1).

| Phase | Description | Status |
|-------|-------------|--------|
| 1 | Build system + data generator + CLI skeleton | ✅ Complete |
| 2 | Brute-force baseline (correctness anchor) | ✅ Complete |
| 3 | Core data structures (FATable, FMSketch, CATable) | ✅ Complete |
| 4A | Sampler — uniform sampling + candidate selection (Algorithm 2) | ✅ Complete |
| 4B | Single-pass FA/CA routing + pruning (Algorithms 3 + 4, pass 1) | ✅ Complete |
| 4C | MergeAndPrune + multi-pass convergence loop (full Algorithm 1) | ✅ Complete |
| 5 | Extension A — Stratified sampling via group index | 🔲 Planned |
| 6 | Extension B — Measure column index (min-heap) | 🔲 Planned |
| 7 | Combined mode (A + B) + full correctness sweep | 🔲 Planned |
| 8 | Experiment matrix + plots + documentation | 🔲 Planned |

### What's Working Now

- **Data generation** — synthetic Zipf datasets with configurable skew, rare-group injection
- **Brute-force top-k** — ground-truth comparator using hash map + partial sort
- **FATable** — open-addressing hash table with linear probing (50% load factor, 16-byte entries for cache-line alignment)
- **FMSketch** — 32-bucket stochastic averaging Flajolet-Martin distinct count
- **CATable** — partition-level aggregate tracking with pruning and ranking

---

## Prerequisites

- **C++ compiler:** GCC 10+ or Clang 12+ with C++17 support (MSVC also works)
- **Python 3.8+** with `numpy` (for data generation and verification)
- **Optional:** `matplotlib`, `pandas` (for experiment plotting in Phase 8)

```bash
pip install numpy matplotlib pandas
```

---

## Quick Start

### 1. Build the C++ binary

**Using g++ directly (simplest):**
```bash
g++ -std=c++17 -O2 -o build/zippy src/main.cpp src/zippy.cpp src/sampler.cpp src/group_index.cpp src/stratified_sampler.cpp src/measure_index.cpp -Isrc/
```

**Using CMake:**
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
cd ..
```

### 2. Generate a test dataset

```bash
# Tiny dataset (1K rows, 50 groups, with rare high-value groups)
python python/generate_data.py \
    --output data/tiny.bin \
    --n-rows 1000 --n-groups 50 --zipf-alpha 1.2 \
    --rare-group-fraction 0.1 --rare-group-rows 3 \
    --rare-group-value-multiplier 100

# Larger test dataset (10K rows)
python python/generate_data.py \
    --output data/S0.bin \
    --n-rows 10000 --n-groups 500 --zipf-alpha 1.2 \
    --rare-group-fraction 0.1 --rare-group-rows 3 \
    --rare-group-value-multiplier 100
```

### 3. Run brute-force top-k

```bash
./build/zippy \
    --input data/tiny.bin \
    --n-rows 1012 \
    --k 5 \
    --mode brute-force \
    --output results/tiny_bf.json
```

### 4. Verify correctness against Python

```bash
python python/verify_phase2.py
```

### 5. Run data structure unit tests

```bash
g++ -std=c++17 -O2 -o build/test_ds src/test_data_structures.cpp -Isrc/
./build/test_ds
```

### 6. Run sampler unit test (Phase 4A gate)

```bash
g++ -std=c++17 -O2 -o build/test_sampler src/test_sampler.cpp src/sampler.cpp -Isrc/
./build/test_sampler
```

### 7. Run Phase 4B gate test (single-pass baseline on S0)

```bash
g++ -std=c++17 -O2 -o build/test_phase4b src/test_phase4b.cpp src/zippy.cpp src/sampler.cpp -Isrc/
./build/test_phase4b
```

### 8. Run Phase 4C correctness gate (baseline vs brute-force)

```bash
g++ -std=c++17 -O2 -o build/test_phase4c src/test_phase4c.cpp src/zippy.cpp src/sampler.cpp -Isrc/
./build/test_phase4c --input data/S0.bin --n-rows 10089 --k 10
```

### 9. Full baseline validation workflow (S0 + S1)

Use this when you want to confirm the full Phase 4C baseline behavior end-to-end.

```bash
# 1) Build zippy
g++ -std=c++17 -O2 -o build/zippy src/main.cpp src/zippy.cpp src/sampler.cpp src/group_index.cpp src/stratified_sampler.cpp src/measure_index.cpp -Isrc/

# 2) Ensure S0 and S1 exist (S1 may take some time to generate)
python python/generate_data.py --output data/S0.bin --n-rows 10000 --n-groups 500 --zipf-alpha 1.2 --rare-group-fraction 0.1 --rare-group-rows 3 --rare-group-value-multiplier 100
python python/generate_data.py --output data/S1.bin --n-rows 10000000 --n-groups 1000000 --zipf-alpha 1.2 --rare-group-fraction 0.0

# 3) Run brute-force and baseline on S0
./build/zippy --input data/S0.bin --n-rows 10089 --k 10 --mode brute-force --output results/S0_bf.json
./build/zippy --input data/S0.bin --n-rows 10089 --k 10 --mode baseline --output results/S0_baseline.json

# 4) Run brute-force and baseline on S1
./build/zippy --input data/S1.bin --n-rows 10000000 --k 50 --mode brute-force --output results/S1_bf.json
./build/zippy --input data/S1.bin --n-rows 10000000 --k 50 --mode baseline --output results/S1_baseline.json --verbose

# 5) Compare top-k ID sets + print baseline metrics
python python/compare_phase4c_results.py
```

Expected outcome for Phase 4C baseline:
- S0 and S1 top-k ID sets match brute-force exactly.
- S1 should typically converge in ~1–2 passes with high pruning on skewed data.

---

## CLI Reference

```
./build/zippy [OPTIONS]

Required:
  --input <path>          Path to binary dataset file
  --n-rows <int>          Number of rows in dataset
  --k <int>               Number of top results to return
  --output <path>         Path to write JSON results file

Algorithm selection:
  --mode <string>         One of: brute-force | baseline | ext-a | ext-b | ext-ab

Zippy tuning parameters:
  --fa-capacity <int>     FA hash table capacity (default: 50000)
  --n-partitions <int>    CA partition count (default: 10000)
  --sample-frac <float>   Uniform sample fraction (default: 0.01)
  --delta <float>         Sampling tolerance Δ (default: 0.05)

Extension A parameters:
  --underrep-threshold <float>  Underrepresentation threshold (default: 0.5)
  --boost-rows <int>            Rows to fetch per boosted group (default: 10)

Extension B parameters:
  --measure-m <int>       Extreme-value rows to track (default: 500)

Output options:
  --verbose               Print per-pass stats to stderr
  --output-fa-groups      Include FA group IDs in JSON output
```

---

## Dataset Format

Binary format, 16 bytes per row, little-endian, no header:

```
[ group_id : uint64 (8 bytes) | value : float64 (8 bytes) ]
```

Total file size = `n_rows × 16` bytes.

---

## Output Format

JSON with top-k results and performance metrics:

```json
{
  "mode": "brute-force",
  "k": 5,
  "n_rows": 1012,
  "top_k_results": [
    {"group_id": 51, "aggregate": 30000.0},
    {"group_id": 53, "aggregate": 30000.0}
  ],
  "metrics": {
    "total_duration_ms": 0.988,
    "total_passes": 0,
    "partitions_pruned_pct": 0.0,
    "fa_hit_rate": -1.0
  }
}
```

---

## Key Metrics

| Metric | Description |
|--------|-------------|
| `fa_hit_rate` | Fraction of true top-k groups placed in FA by sampling |
| `partitions_pruned_pct` | Fraction of CA partitions pruned after Pass 1 |
| `total_passes` | Data passes until convergence (lower = better) |
| `total_duration_ms` | Wall-clock time (excludes dataset loading) |
| `topKBound_after_pass1` | Pruning threshold after first pass (higher = more pruning) |

---

## How Zippy Works (Simplified)

```
┌─────────────────────────────────────────────────────────┐
│ Phase 0: Sample 1% of rows → identify candidate groups  │
│          (Extensions A/B improve this step)              │
├──────────────────────────┬──────────────────────────────┤
│ Phase 1: Full data scan  │                              │
│                          │                              │
│  For each row:           │  After scan:                 │
│  ┌─ group in FA? ──────┐ │  topKBound = k-th FA value  │
│  │ YES: exact sum += val│ │                              │
│  │ NO:  CA[hash] += val │ │  Prune: if CA[p] < bound   │
│  └──────────────────────┘ │  → eliminate partition p     │
├──────────────────────────┴──────────────────────────────┤
│ Phase 2+: Re-scan only surviving partitions              │
│           → aggregate individual groups → confirm top-k  │
└─────────────────────────────────────────────────────────┘
```

---

## References

- **Paper:** Siddiqui et al., "Cache-Efficient Top-k Aggregation over High Cardinality Large Datasets", PVLDB 17(4), 2023. DOI: [10.14778/3636218.3636222](https://doi.org/10.14778/3636218.3636222)
- **Patent:** US 12380098 / Application 20250103591 (implementation details)
- **Full specification:** See [`AGENTS.md`](AGENTS.md) for algorithm pseudocode, data structure specs, invariants, and the complete implementation plan.
