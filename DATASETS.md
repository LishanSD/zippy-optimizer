# Zippy Optimizer: Experiment Dataset Matrix

## Overview
To rigorously evaluate the Zippy top-k aggregation algorithm and our proposed extensions (Extension A and Extension B), we utilize a matrix of diverse synthetic datasets. Relying on a single dataset risks overfitting and fails to expose the specific vulnerabilities of the baseline uniform-sampling approach. 

To match the scale of the original VLDB paper, our primary testbed operates on **200 Million rows** with **30 Million unique groups**, scaling up to **400 Million rows** for stress testing.

This document outlines five distinct dataset archetypes, the Python commands used to generate them, and the expected algorithmic outcomes for each.

---

## 1. Vanilla Zipfian (Baseline Control)
**Purpose:** To verify that our extensions do not introduce prohibitive overhead on clean data. On standard, highly skewed Zipfian data without malicious outliers, Baseline Zippy is already highly optimal. We use this to prove that the index-building pre-passes for Ext-A and Ext-B are practically "free."

**Generation Command:**
```bash
python python/generate_data.py \
    --output data/D1_vanilla.bin \
    --n-rows 200000000 \
    --n-groups 30000000 \
    --zipf-alpha 1.2 \
    --rare-group-fraction 0.0
```

## 2. Extension A Target
**Purpose:** Designed to break the baseline uniform sampler using "frequent-but-rare" groups. These are groups that possess a handful of rows (e.g., 5 rows) and moderately high values. Extension B's min-heap might miss them because no *single* row is a massive extreme outlier, but Extension A's stratified sampling index will easily identify their underrepresentation and boost them.

**Generation Command:**
```bash
python python/generate_data.py \
    --output data/D2_ext_a_target.bin \
    --n-rows 200000000 \
    --n-groups 30000000 \
    --zipf-alpha 1.1 \
    --rare-group-fraction 0.05 \
    --rare-group-rows 5 \
    --rare-group-value-multiplier 500
```

## 3. Extension B Target
**Purpose:** Designed to test extreme sparsity. This dataset contains a tiny number of groups (e.g., ~300) that only have 1 row each, but that single row holds a massive value. Extension A might miss them because they are too rare to sample effectively, but Extension B's measure-column min-heap will capture them instantly.

**Generation Command:**
```bash
python python/generate_data.py \
    --output data/D3_ext_b_target.bin \
    --n-rows 200000000 \
    --n-groups 30000000 \
    --zipf-alpha 1.1 \
    --rare-group-fraction 0.00001 \
    --rare-group-rows 1 \
    --rare-group-value-multiplier 1000000
```

## 4. Chaos (Ext-AB Target)
**Purpose:** Simulates a realistic, noisy production environment containing *both* types of adversarial outliers (moderate-frequency/moderate-value and extreme-sparsity/extreme-value). This proves the necessity of the combined Ext-AB mode.

**Generation Command:**
```bash
python python/generate_data.py \
    --output data/D4_chaos.bin \
    --n-rows 200000000 \
    --n-groups 30000000 \
    --zipf-alpha 1.05 \
    --rare-group-fraction 0.02 \
    --rare-group-rows 3 \
    --rare-group-value-multiplier 50000
```

## 5. The "Scale-Up" Sweep
**Purpose:** Used to generate performance scaling plots (Execution Time vs. Row Count). We keep the distribution fixed (using the Chaos archetype) but push the data volume up to 400M rows to observe L3 cache thrashing on the brute-force baseline.

**Generation Commands:**
```bash
# 100 Million Rows
python python/generate_data.py --output data/scale_100M.bin --n-rows 100000000 --n-groups 15000000 --zipf-alpha 1.05 --rare-group-fraction 0.02 --rare-group-rows 3 --rare-group-value-multiplier 50000

# 200 Million Rows (Re-use D4_chaos.bin)

# 400 Million Rows
python python/generate_data.py --output data/scale_400M.bin --n-rows 400000000 --n-groups 55000000 --zipf-alpha 1.05 --rare-group-fraction 0.02 --rare-group-rows 3 --rare-group-value-multiplier 50000
```

---

## Expected Outcomes

When running the 5 algorithm modes (`brute-force`, `baseline`, `ext-a`, `ext-b`, `ext-ab`) across this matrix, we expect the following results:

### 1. Brute-Force
*   **Behavior:** Scales linearly with dataset size. Will consistently take exactly 1 pass.
*   **Outcome:** Slower overall execution on 200M+ rows due to massive L3 cache thrashing, but immune to data skew/outliers. Serves solely as the ground-truth accuracy benchmark.

### 2. Baseline Zippy
*   **Behavior:** Highly sensitive to dataset topology.
*   **Outcome:** Will perform beautifully on `D1_vanilla` (1 pass, high pruning). Will fail drastically on `D2`, `D3`, and `D4`. By missing the adversarial outliers during its 1% uniform sample, it will set a weak `topKBound`, fail to prune coarse partitions, and fall into expensive, multi-pass loops resulting in execution times slower than Brute-Force.

### 3. Extension A (Stratified Sampling)
*   **Behavior:** Pre-builds a group occurrence index.
*   **Outcome:** Will completely solve `D2_ext_a_target` by boosting the underrepresented high-value groups into the FA table, achieving 1-pass convergence. May still drop to 2 passes on `D3_ext_b_target` if the groups are too rare for stratified sampling to catch effectively.

### 4. Extension B (Measure Column Index)
*   **Behavior:** Pre-builds a top-$m$ min-heap.
*   **Outcome:** Will completely solve `D3_ext_b_target` by sniping the extreme needle-in-a-haystack outliers, achieving 1-pass convergence. May struggle on `D2` if the outliers' individual row values aren't high enough to break into the top-$m$ heap, relying instead on aggregate sum.

### 5. Extension AB (Combined)
*   **Behavior:** Runs both pre-passes and intelligently routes VIP groups.
*   **Outcome:** The ultimate champion. Will achieve near 100% Pass-1 pruning and fastest execution times across `D1`, `D2`, `D3`, and `D4`. It proves that the two extensions perfectly complement each other to patch all holes in the baseline algorithm without sacrificing throughput.

---

## 6. Automation Script

To easily generate all the datasets required for the experiment matrix, save the following code as `generate_datasets.sh` in the root of the project, make it executable (`chmod +x generate_datasets.sh`), and run it.

```bash
#!/bin/bash

# Ensure the data directory exists
mkdir -p data

echo "===================================================="
echo " Generating VLDB-Scale Experiment Dataset Matrix"
echo " Note: This will generate several massive binary files"
echo " requiring roughly 25GB of disk space combined."
echo "===================================================="

echo "[1/7] Generating D1_vanilla (200M rows, 30M groups)..."
python python/generate_data.py --output data/D1_vanilla.bin --n-rows 200000000 --n-groups 30000000 --zipf-alpha 1.2 --rare-group-fraction 0.0

echo "[2/7] Generating D2_ext_a_target (200M rows, 30M groups)..."
python python/generate_data.py --output data/D2_ext_a_target.bin --n-rows 200000000 --n-groups 30000000 --zipf-alpha 1.1 --rare-group-fraction 0.05 --rare-group-rows 5 --rare-group-value-multiplier 500

echo "[3/7] Generating D3_ext_b_target (200M rows, 30M groups)..."
python python/generate_data.py --output data/D3_ext_b_target.bin --n-rows 200000000 --n-groups 30000000 --zipf-alpha 1.1 --rare-group-fraction 0.00001 --rare-group-rows 1 --rare-group-value-multiplier 1000000

echo "[4/7] Generating D4_chaos (200M rows, 30M groups)..."
python python/generate_data.py --output data/D4_chaos.bin --n-rows 200000000 --n-groups 30000000 --zipf-alpha 1.05 --rare-group-fraction 0.02 --rare-group-rows 3 --rare-group-value-multiplier 50000

echo "[5/7] Generating scale_100M (100M rows, 15M groups)..."
python python/generate_data.py --output data/scale_100M.bin --n-rows 100000000 --n-groups 15000000 --zipf-alpha 1.05 --rare-group-fraction 0.02 --rare-group-rows 3 --rare-group-value-multiplier 50000

echo "[6/7] Generating scale_300M (300M rows, 37M groups)..."
python python/generate_data.py --output data/scale_300M.bin --n-rows 300000000 --n-groups 37000000 --zipf-alpha 1.05 --rare-group-fraction 0.02 --rare-group-rows 3 --rare-group-value-multiplier 50000

echo "[7/7] Generating scale_400M (400M rows, 55M groups)..."
python python/generate_data.py --output data/scale_400M.bin --n-rows 400000000 --n-groups 55000000 --zipf-alpha 1.05 --rare-group-fraction 0.02 --rare-group-rows 3 --rare-group-value-multiplier 50000

echo "===================================================="
echo " All datasets generated successfully in the /data directory!"
echo "===================================================="
```