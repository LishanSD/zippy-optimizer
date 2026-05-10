# Extension B: Measure Column Index Optimization

## Overview
Standard Zippy relies on uniform random sampling (typically 1%) to identify the top-k heavy hitting groups. While mathematically sound for standard Zipfian distributions, uniform sampling fails on **"needle in a haystack" adversarial data** — datasets where a tiny number of extremely high-value rows exist. 

**Extension B** fixes this by introducing a lightweight pre-pass. It maintains a min-heap of the top-m largest individual row values, finds their owning groups, and **force-injects** them into the fast FA table *before* standard sampling occurs. This guarantees that rare outliers are caught, resulting in a much stronger `topKBound`, massive pass-1 pruning, and faster convergence.

---

## 1. Generating the "Monster" Adversarial Dataset

To properly benchmark Extension B against the baseline, we need a dataset large enough to spill out of the CPU's L3 cache, containing mathematically buried outliers. 

We generate a **100 Million row dataset** with **10 Million unique groups**, but we configure exactly 100 rare groups (`rare-group-fraction = 0.00001`). Each of these groups contains only 1 row, but its value is multiplied by 1,000,000. 

Run the following Python command to generate `monster_adv.bin`:

```bash
python python/generate_data.py \
    --output data/monster_adv.bin \
    --n-rows 100000000 \
    --n-groups 10000000 \
    --zipf-alpha 1.1 \
    --rare-group-fraction 0.00001 \
    --rare-group-rows 1 \
    --rare-group-value-multiplier 1000000
```

*Note: This generates a ~1.6 GB binary file. Ensure you have sufficient disk space.*

---

## 2. Running the Benchmarks

With the monster dataset generated, we race the three algorithmic modes against each other. Ensure your C++ binary is compiled with `-O2` or `-O3` before running.

### Mode 1: Brute-Force (The Ground Truth)
The naive approach. It hash-aggregates every group in one pass and sorts them. It heavily thrashes the CPU cache but guarantees the mathematically correct top-k.

```bash
./build/zippy \
    --input data/monster_adv.bin \
    --n-rows 100000000 \
    --k 50 \
    --mode brute-force \
    --output results/monster_bf.json \
    --verbose
```

### Mode 2: Baseline Zippy (Paper Algorithm)
The standard algorithm. Because it only samples 1% of the data, it has a 99% chance of missing each of the 100 extreme outliers. Missing them results in a weak `topKBound`, failing to prune partitions in Pass 1, and forcing an expensive Pass 2 scan.

```bash
./build/zippy \
    --input data/monster_adv.bin \
    --n-rows 100000000 \
    --k 50 \
    --mode baseline \
    --output results/monster_baseline.json \
    --verbose
```
*Expected Result: Fails to prune efficiently, falls into 2+ passes, and runs slower than brute-force.*

### Mode 3: Extension B (Measure Index)
Our optimized algorithm. The ~180ms min-heap pre-pass perfectly captures all 100 outliers. It force-injects them into FA, establishes a massive `topKBound`, and mathematically eliminates the rest of the dataset in a single pass.

```bash
./build/zippy \
    --input data/monster_adv.bin \
    --n-rows 100000000 \
    --k 50 \
    --mode ext-b \
    --output results/monster_extb.json \
    --verbose
```
*Expected Result: 100% partition pruning, single-pass convergence, and executes significantly faster than both brute-force and baseline.*