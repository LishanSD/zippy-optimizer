# Paper-vs-Implementation Audit

Reference: Siddiqui et al., *Cache-Efficient Top-k Aggregation over High Cardinality Large Datasets*, PVLDB 17(4): 644–656, 2023 (`p644-siddiqui-paper.pdf`).

Audited against: `src/zippy.cpp`, `src/zippy.h`, `src/sampler.cpp`, `src/sampler.h`, `src/data_structures.h`, `src/main.cpp`.

## TL;DR
The skeleton is right, but several core algorithmic pieces from the paper are **stubbed, replaced, or missing**. It's a single-threaded, SUM-only prototype — not a faithful Zippy implementation yet.

## What matches the paper

- **Cache-resident structures (§4.1.1):** `FATable` is single-level open-addressing + linear probing at 50% load with 16-byte entries (`data_structures.h:90-204`). `CATable` has per-partition `sum`, `count`, `max`, `min`, `FMSketch` (`data_structures.h:256-352`). Matches the paper.
- **Multi-pass framework outline (Algorithm 1):** sample → pass 1 → merge/prune → loop. Structurally present in `zippy.cpp:162-333`.
- **Sample size formula** `s ≥ Z²_{α/2} / (4Δ²)` implemented at `sampler.cpp:100-101`.
- **FM sketch** uses stochastic averaging (32 buckets) — a reasonable variant of [15].
- **Hash family separation** for FA / partition / FM / child-partition. Good.

## Concrete deviations from the paper

### 1. Algorithm 2 (FA candidate selection) is gutted
Paper (Algorithm 2, lines 11–33): compute Hoeffding CI for each group, take K-th highest **lower bound** L_k, set `tempGroups = {g : LB(g) ≥ L_k}`, **if |tempGroups| > C_F → return isOptimizable=false** (fall back).

Implementation (`sampler.cpp:86`, `:151`): two literal `// TODO` comments. `beta_ci` is unused. Candidates are just sorted by sample `sum` and the top `fa_capacity` are picked. No CIs, no L_k, no skew-validation gate. `is_optimizable` is only set false when the sample is empty.

**Consequence:** the implementation never actually validates that the data is skewed enough to optimize — it always proceeds. The paper's whole "if not optimizable, fall back" branch is effectively dead.

### 2. Only SUM is implemented
Paper targets monotonic aggregates — SUM, COUNT, MAX, MIN with Y ≥ 0 (§2). MergeAndPrune uses partition `total_sum` for SUM/AVG/COUNT and `max_value` / `min_value` for MAX/MIN.

Implementation (`zippy.cpp:82`): `union_values.push_back(part.total_sum); // SUM UB`. No MAX / MIN / COUNT path. CLI has no aggregate-function flag. `CAPartition` tracks max/min but they're never read for pruning. The README and AGENTS.md both claim full Algorithm 1; the code only does SUM.

### 3. Algorithm 3 — locality test and logical-vs-physical decision are missing
Paper (Algorithm 3 lines 5–24, §4.3): segment-based locality `l = Σ_s d_s/c_s / t`, threshold α₀, then choose logical vs physical via `E = C_p/Q` vs `T_c`.

Implementation (`zippy.cpp:274-307`): pass 2+ only checks `fm.estimate() < fa_capacity`. No segment scan, no α₀ test, no E / T_c comparison. Always logical (re-filter the row stream). Physical partitioning + non-temporal stores from Algorithm 3 lines 33–37 are entirely absent.

### 4. No parallelism (§4.4)
Paper is explicitly shared-nothing: each core samples its chunk, runs local FA / CA, syncs at MergeAndPrune. Implementation is fully single-threaded. No threads, no per-core partial aggregates, no input blocking. Pass 2+ re-scans the **entire dataset** every pass instead of scanning unprocessed partitions.

### 5. Pass 1 pruning bound is FA-only
Paper Algorithm 4 line 12: `topKBound = K-th highest among {exactAggregates ∪ childPartition UBs}`.

Implementation (`zippy.cpp:230-235`): `topKBound` = K-th FA value only, **before** building children. This is *safe but under-prunes* compared to the paper. Pass 2+ does use the union (`zippy.cpp:74-85`), so the deviation is only in pass 1.

### 6. Confidence-interval skew validation absent
Algorithm 2 lines 16–25 (Hoeffding bounds, β/2 percentiles for max/min) — none of it. `cfg.beta_ci` flows in but is silently ignored.

### 7. Extensions A & B not implemented
Phases 5–7 in README are 🔲. `main.cpp:92-100` returns errors for `ext-a` / `ext-b` / `ext-ab`. The header files exist, but the modes don't run.

## Bottom line
What's labeled "Phase 4C complete / full Algorithm 1" is more honestly:

> Algorithm 1 outline + Algorithm 2 with point estimates instead of Hoeffding + Algorithm 3 with the locality/partitioning logic stripped + Algorithm 4 SUM-only + no parallelism.

The brute-force comparator and the cache-resident data structures are paper-faithful; the algorithm proper is significantly thinned.

## Suggested fix order (by impact)

1. **Implement Hoeffding LB + L_k gate in Algorithm 2** (`sampler.cpp`) — finishes the candidate-selection logic the paper centers on.
2. **Generalize MergeAndPrune to SUM / COUNT / MAX / MIN** (`zippy.cpp` + CLI flag).
3. **Add the locality test + adaptive logical/physical decision** in pass 2+ (`zippy.cpp`, new helpers).
4. Parallelism (§4.4) — biggest engineering lift, lowest priority for a correctness-focused prototype.
