# Zippy Implementation vs. Paper/Patent — Mismatch Report

**Date:** 2026-05-11  
**Audited against:**
- Siddiqui et al., *Cache-Efficient Top-k Aggregation over High Cardinality Large Datasets*, PVLDB 17(4): 644–656, 2023 (`p644-siddiqui-paper.pdf`)
- US Patent **12,380,098 B2** (Microsoft / Siddiqui et al., issued Aug. 5, 2025) (`patent-application.pdf`)

**Source files reviewed:**
`src/zippy.cpp`, `src/zippy.h`, `src/sampler.cpp`, `src/stratified_sampler.cpp`,
`src/data_structures.h`, `src/main.cpp`, `src/utils.h`, `src/measure_index.cpp`

---

## Task 01 — All Mismatches

---

### M1 — Uniform sampler uses sampling WITH replacement (not Bernoulli)

**Paper/Patent reference:** Algorithm 2 and patent col. 9 specify Bernoulli sampling — each row is independently included with probability `p = s/N`.

**Code location:** `src/sampler.cpp:153–173`

```cpp
std::uniform_int_distribution<size_t> dist(0, n_rows - 1);
for (size_t i = 0; i < sample_size_target; ++i) {
    const auto& row = dataset[dist(rng)];   // random index, WITH replacement
    ...
}
```

Random access with replacement means the same row can be sampled multiple times, inflating counts for popular groups. The Hoeffding CI formula uses `n_i'` (observed count in sample), so over-counting biases CIs downward and makes the L_k gate more permissive than the paper intends.

**Inconsistency:** The stratified sampler (`src/stratified_sampler.cpp:166–174`) correctly uses Bernoulli — the two samplers are inconsistent with each other and with the paper.

---

### M2 — Locality test is whole-partition, not segmented

**Paper/Patent reference:** Patent col. 11 / paper §4.3 specify `l = Σ_s (d_s / c_s) / t` computed over **segments of s = 100,000 rows**, where `d_s` and `c_s` are distinct groups and row count within the segment window. Default `α₀ = 0.20`.

**Code location:** `src/zippy.cpp:113–115`

```cpp
const double locality = static_cast<double>(d) / static_cast<double>(part.count);
if (locality < cfg.alpha_locality) return PartitionDecision::EXACT;
```

Uses global `d/count` over the entire partition. The `cfg.segment_size` field exists in `src/zippy.h:25` but is **never read anywhere in `zippy.cpp`**. Segmented locality detects temporal clustering within the partition that the single global ratio misses entirely.

---

### M3 — PHYSICAL partitioning branch is a no-op (identical to LOGICAL)

**Paper/Patent reference:** Patent claims 3/4 and 13/14 specify physical row materialization: *"buffers one partition per cache line ... writes the partition data to the memory once full using non-temporal store instructions"* (col. 12). Physical partitioning eliminates multi-pass re-scanning.

**Code location:** `src/zippy.cpp:381–385` (identical in `ext_a` and `ext_b`)

```cpp
} else {
    // LOGICAL or PHYSICAL: same execution path in this prototype
    // (single-threaded — physical row movement provides no speedup).
    const size_t child_id = ...;
    update_partition_from_row(children[child_id], row.group_id, row.value);
}
```

Both LOGICAL and PHYSICAL take the exact same code path. The `partitions_physical` metric is counted in output JSON but the physical advantage (avoiding re-scans through row materialization) is never realized.

---

### M4 — Ranked partition processing not used in multi-pass loop

**Paper/Patent reference:** Patent col. 8 specifies: *"rank the surviving non-candidate-group partitions in order of decreasing estimated sum per group (`psum/d`)"*. Processing highest-potential partitions first enables earlier topKBound convergence and termination.

**Code location:** `src/zippy.cpp:330–334`

`CATable::ranked_surviving_partitions(AggFunc)` exists in `src/data_structures.h:408–415` but is **never called**. Both pass-1 survivors and multi-pass child partitions use unordered `surviving_partitions()` iteration.

---

### M5 — FA hit rate is never computed

**Paper/Patent reference:** Paper §5, Table 2 uses FA hit rate as the primary quality metric for the sampling step — it measures what fraction of the true top-k appear in the sampled FA candidates.

**Code location:** `src/utils.h:31`

```cpp
double fa_hit_rate = -1;   // -1 = not computed (needs --output-fa-groups)
```

The field is always `-1` in JSON output for all run modes (`brute-force`, `baseline`, `ext-a`, `ext-b`). The Python benchmark computes it externally by diffing against brute-force, but the C++ engine never self-reports it.

---

### M6 — `n_groups` is hardcoded 0 in all JSON outputs

**Paper/Patent reference:** Paper §5 experiments report dataset cardinality as a primary independent variable.

**Code location:** `src/main.cpp:109`

```cpp
write_output_json(output_path, mode, k, n_rows,
                  /* n_groups estimate */ 0,   // always 0
                  ...);
```

The value is available after sampling (`sample.sample_stats.size()` gives a lower-bound estimate) or exactly from the brute-force pass.

---

### M7 — `ext-ab` mode (Extensions A + B combined) is not implemented

**Code location:** `src/main.cpp:99–101`

```cpp
} else if (mode == "ext-ab") {
    fprintf(stderr, "ERROR: ext-ab mode not yet implemented (Phase 7)\n");
    return 1;
}
```

Both Extension A (`ext-a`) and Extension B (`ext-b`) are individually implemented and wired. The combination — build `GroupOccurrenceIndex` for stratified sampling (A) while also injecting extreme-value groups from the measure index (B) — is absent.

---

### M8 — No multi-core parallelism

**Paper/Patent reference:** Patent claim 1 is the independent claim requiring *"creating a first cache resident data structure ... in a cache of **each core** of the multicore processor"* and *"merging the exact aggregate values **for the plurality of cores**."* Paper §4.4 describes per-core FA tables and a cross-core merge step. Figure 10 shows near-linear scaling up to 20 cores.

**Code:** Entirely single-threaded. No `std::thread`, no per-core partial aggregates, no SIMD, no `#include <thread>` anywhere in the project.

---

### M9 — Skew validation gate omitted for MAX/MIN

**Paper/Patent reference:** Algorithm 2's skew analysis step (patent claim 2/12) is not restricted to any aggregate type.

**Code location:** `src/sampler.cpp:234`

```cpp
if (agg_func == AggFunc::SUM || agg_func == AggFunc::COUNT) {
```

The 1%-share skew gate fires only for SUM and COUNT. For MAX and MIN, `is_optimizable` can only go false via the `|tempGroups| > fa_capacity` check, meaning the gate provides no protection on adversarial MAX/MIN distributions.

---

### M10 — Acklam approximation is copy-pasted across two files

The `inverse_standard_normal_cdf` function (40 lines of constants + logic) is **duplicated** in both `src/sampler.cpp:13–65` and `src/stratified_sampler.cpp:34–80`, with a comment: *"extract to a shared header if a third consumer appears."* Three independent consumers now exist (uniform sampler, stratified sampler, and any future `ext-ab` implementation).

---

### M11 — Pass 2+ loop body is triplicated

The inner multi-pass scan loop is copied verbatim across:
- `src/zippy.cpp:369–416` (`run_zippy_baseline`)
- `src/zippy.cpp:585–631` (`run_zippy_ext_a`)
- `src/zippy.cpp:800–848` (`run_zippy_ext_b`)

Approximately 80 lines of identical logic with only variable-name prefix differences. Any future bug fix or paper-conformance change must be applied to all three copies manually.

---

### M12 — MIN upper bound in `CAPartition` uses partition MAX (loose bound)

**Code location:** `src/data_structures.h:337`

```cpp
case AggFunc::MIN:   return max_value;  // group's MIN ≤ any row in partition ≤ partition MAX
```

Patent col. 13 states *"maximum value sets an upper bound for max and min aggregates"* — so this is technically correct per the patent. However, for top-K by MIN (largest group minimum), the partition `max_value` is a loose upper bound that causes under-pruning. The partition's `min_value` is not usable as a tighter UB either (it would be the best-case group minimum), so this is an inherent limitation of partition-level tracking for MIN.

---

## Summary Table

| # | Mismatch | Paper/Patent Reference | Severity | Corresponding Fix |
|---|----------|----------------------|----------|-------------------|
| M1 | Uniform sampler: with-replacement vs Bernoulli | Algorithm 2, patent col. 9 | **High** — biases Hoeffding CIs | Fix 1 |
| M2 | Locality: whole-partition vs segmented | §4.3, patent col. 11 | **Medium** — wrong partition decisions | Fix 2 |
| M3 | PHYSICAL branch is a no-op (= LOGICAL) | Claims 3/4, 13/14 | **Medium** — performance claim invalid | Requires parallelism |
| M4 | Partition ranking not used in multi-pass | Algorithm 4, patent col. 8 | **Medium** — missed early termination | Fix 3 |
| M5 | FA hit rate never computed | Paper §5, Table 2 | Medium — missing key metric | Fix 4 |
| M6 | `n_groups` always 0 in JSON | Paper §5 experiments | Low | Fix 5 |
| M7 | `ext-ab` not implemented | Paper §7 | Medium | Fix 6 |
| M8 | No multi-core parallelism | Patent claim 1, §4.4 | **Critical** — core patent claim absent | Large effort |
| M9 | Skew gate skips MAX/MIN | Algorithm 2, claim 2/12 | Low | Fix 7 |
| M10 | Acklam function duplicated | — | Code quality | Fix 8 |
| M11 | Pass 2+ loop triplicated | — | Code quality | Fix 9 |
| M12 | MIN upper bound is partition MAX (loose) | Patent col. 13 | Low — correct but loose | Acceptable as-is |

---

## Task 02 — Improvements to Fill the Gaps

Ordered by correctness impact.

---

### Fix 1 — Switch uniform sampler to Bernoulli sampling *(High Priority)*

**File:** `src/sampler.cpp`

Replace the fixed-count random-with-replacement loop with a Bernoulli pass to match the paper and make the two samplers consistent:

```cpp
// Replace this (WRONG — samples WITH replacement):
std::uniform_int_distribution<size_t> dist(0, n_rows - 1);
for (size_t i = 0; i < sample_size_target; ++i) {
    const auto& row = dataset[dist(rng)];
    ...
}

// With this (correct — Bernoulli, each row independently included with prob p):
const double p_row = static_cast<double>(sample_size_target)
                   / static_cast<double>(n_rows);
std::bernoulli_distribution coin(p_row);
for (const auto& row : dataset) {
    if (!coin(rng)) continue;
    auto& stats = result.sample_stats[row.group_id];
    stats.sum   += row.value;
    stats.count += 1.0;
    if (row.value < stats.min_val) stats.min_val = row.value;
    if (row.value > stats.max_val) stats.max_val = row.value;
    ++result.sample_size_actual;
}
```

**Why this matters:** The Hoeffding CI formula (`ε = (b−a)·√(ln(2/(1−β))/(2·n_i'))`) assumes `n_i'` is the number of *distinct* rows sampled for group i, not the number of times any row from i was drawn. With-replacement sampling violates this assumption.

---

### Fix 2 — Implement segmented locality test *(Medium Priority)*

**File:** `src/zippy.cpp` — `classify_partition()` and the Pass 1 scan

The segmented locality test requires tracking per-segment distinct counts during the Pass 1 scan. Minimal implementation:

1. Add `std::vector<FMSketch> segment_sketches` and `std::vector<uint64_t> segment_counts` to `CAPartition`.
2. In the Pass 1 scan loop, bucket rows into segments of `cfg.segment_size` by their row index.
3. In `classify_partition`, compute `l = (1/t) Σ_s (fm_s.estimate() / segment_counts[s])` across `t` segments.

```cpp
// In classify_partition, replace the current single-ratio check:
// OLD:
const double locality = static_cast<double>(d) / static_cast<double>(part.count);

// NEW (segmented):
double locality_sum = 0.0;
int active_segments = 0;
for (size_t s = 0; s < part.segment_sketches.size(); ++s) {
    if (part.segment_counts[s] == 0) continue;
    locality_sum += static_cast<double>(part.segment_sketches[s].estimate())
                  / static_cast<double>(part.segment_counts[s]);
    ++active_segments;
}
const double locality = active_segments > 0
    ? locality_sum / static_cast<double>(active_segments) : 1.0;
```

This makes `cfg.segment_size` actually functional.

---

### Fix 3 — Use ranked partition ordering in multi-pass loop *(Medium Priority)*

**File:** `src/zippy.cpp:330–334`

One-line change in Pass 1 survivor collection:

```cpp
// OLD — unordered:
for (size_t pid : ca.surviving_partitions()) {

// NEW — ranked by estimated per-group aggregate (descending):
for (size_t pid : ca.ranked_surviving_partitions(cfg.agg_func)) {
```

The same change should also be applied when building `active_partitions` from `merged.surviving_partitions` at each pass iteration. This matches Algorithm 4 / patent col. 8 and allows earlier topKBound convergence.

---

### Fix 4 — Compute `fa_hit_rate` in all run modes *(Medium Priority)*

**File:** `src/zippy.cpp` — end of `run_zippy_baseline`, `run_zippy_ext_a`, `run_zippy_ext_b`

Add after `out_results = top_k_from_exact(...)` in each function:

```cpp
// Compute FA hit rate: fraction of true top-k groups that were FA candidates.
// Requires out_fa_groups to be populated.
if (!out_fa_groups.empty() && !out_results.empty()) {
    std::unordered_set<uint64_t> fa_set(out_fa_groups.begin(), out_fa_groups.end());
    size_t hits = 0;
    for (const auto& [gid, val] : out_results) {
        if (fa_set.count(gid)) ++hits;
    }
    metrics.fa_hit_rate = static_cast<double>(hits)
                        / static_cast<double>(out_results.size());
}
```

This removes the permanent `-1` from the JSON output and makes the metric self-reported by the engine.

---

### Fix 5 — Emit a real `n_groups` estimate *(Low Priority)*

**File:** `src/main.cpp:109`

```cpp
// Replace the hardcoded 0:
// OLD:
write_output_json(output_path, mode, k, n_rows, /* n_groups estimate */ 0, ...);

// NEW — use sampled distinct count as a lower-bound estimate:
// (metrics.fa_candidates_count underestimates true cardinality; use sample_stats.size()
//  if it's forwarded, or fa_candidates_count as a proxy)
const size_t n_groups_est = (mode == "brute-force")
    ? results.size()                         // exact after brute-force
    : metrics.fa_candidates_count;           // lower bound from sample
write_output_json(output_path, mode, k, n_rows, n_groups_est, ...);
```

For a better estimate in baseline mode, expose `sample.sample_stats.size()` through `RunMetrics`.

---

### Fix 6 — Implement `ext-ab` mode *(Medium Priority)*

**Files:** `src/zippy.h`, `src/zippy.cpp`, `src/stratified_sampler.h/.cpp`, `src/main.cpp`

**Step 1** — Add `pre_injected_groups` parameter to `stratified_sample_and_select` (mirrors the existing parameter in `uniform_sample_and_select`):

```cpp
// src/stratified_sampler.h
SampleResult stratified_sample_and_select(
    const std::vector<Row>& dataset,
    const GroupOccurrenceIndex& group_index,
    size_t fa_capacity, int k, AggFunc agg_func,
    double sample_frac, double delta, double alpha_ci, double beta_ci,
    double underrep_threshold, size_t boost_rows, uint64_t seed,
    const std::unordered_set<uint64_t>& pre_injected_groups = {});  // NEW
```

**Step 2** — Implement `run_zippy_ext_ab` in `src/zippy.cpp`:

```cpp
RunMetrics run_zippy_ext_ab(...) {
    // Phase 1: Build GroupOccurrenceIndex (Extension A)
    GroupOccurrenceIndex group_index;
    group_index.build(dataset);

    // Phase 2: Build Measure Index (Extension B)
    std::unordered_set<uint64_t> extreme_groups =
        build_measure_index(dataset, cfg.measure_m);

    // Phase 3: Stratified sample + inject extreme groups as pre_injected
    const SampleResult sample = stratified_sample_and_select(
        dataset, group_index, cfg.fa_capacity, k, cfg.agg_func,
        cfg.sample_frac, cfg.delta, cfg.alpha_ci, cfg.beta_ci,
        cfg.underrep_threshold, cfg.boost_rows, 42,
        extreme_groups);   // <-- Extension B injection

    // Phases 4+: identical to baseline pipeline
    ...
}
```

**Step 3** — Wire to `src/main.cpp`:
```cpp
} else if (mode == "ext-ab") {
    metrics = run_zippy_ext_ab(dataset, k, cfg, results, fa_groups);
}
```

---

### Fix 7 — Apply skew validation gate to MAX/MIN *(Low Priority)*

**File:** `src/sampler.cpp:234`

Remove the aggregate-function restriction:

```cpp
// OLD — skips MAX and MIN:
if (agg_func == AggFunc::SUM || agg_func == AggFunc::COUNT) {

// NEW — applies to all aggregates (use absolute values for generality):
{
    double total_agg = 0.0;
    std::vector<double> per_group_agg;
    per_group_agg.reserve(result.sample_stats.size());
    for (const auto& [gid, stats] : result.sample_stats) {
        (void)gid;
        const double v = std::abs(aggregate_lower_bound(stats, agg_func, beta_ci));
        per_group_agg.push_back(v);
        total_agg += v;
    }
    // rest of the check unchanged
```

Apply the same change in `src/stratified_sampler.cpp:272`.

---

### Fix 8 — Extract shared math utilities to a header *(Low Priority)*

**Action:** Create `src/math_utils.h` and move the duplicated functions there:

```cpp
// src/math_utils.h
#pragma once
#include <cmath>

inline double inverse_standard_normal_cdf(double p) { /* Acklam implementation */ }
inline double z_alpha_over_two(double alpha_ci) { ... }
inline double hoeffding_eps_per_tuple(double a, double b, double beta_ci,
                                      double n_i_prime) { ... }
```

Then remove the duplicate implementations from `src/sampler.cpp:13–75` and `src/stratified_sampler.cpp:34–96`, replacing with `#include "math_utils.h"`.

---

### Fix 9 — Extract pass 2+ loop to a shared helper *(Medium Priority)*

**File:** `src/zippy.cpp`

The ~80-line pass-2+ body is identical in all three run functions. Extract it:

```cpp
// New helper (in anonymous namespace in zippy.cpp):
static void run_multipass_loop(
    const std::vector<Row>& dataset,
    const FATable& fa,
    ExactAggregates& exact_aggregates,
    ChildPartitions& active_partitions,
    std::vector<std::unordered_set<size_t>>& active_history,
    const ZippyConfig& cfg,
    size_t k_size,
    RunMetrics& metrics);
```

Replace the triplicated copy in `run_zippy_baseline`, `run_zippy_ext_a`, and `run_zippy_ext_b` with a single call. This eliminates ~160 lines of duplicate code and ensures all three modes stay in sync with future changes.

---

## Recommended Fix Order

| Priority | Fix | Impact |
|----------|-----|--------|
| 1 | **Fix 1** — Bernoulli sampling | Correctness of Hoeffding CIs |
| 2 | **Fix 3** — Ranked partition ordering | Convergence speed, paper conformance |
| 3 | **Fix 4** — Compute FA hit rate | Key evaluation metric from paper §5 |
| 4 | **Fix 6** — Implement `ext-ab` | Completes the extension set |
| 5 | **Fix 9** — Deduplicate pass 2+ loop | Maintainability before adding parallelism |
| 6 | **Fix 2** — Segmented locality | Closer paper conformance for partitioning |
| 7 | **Fix 7** — Skew gate for MAX/MIN | Minor correctness |
| 8 | **Fix 8** — Extract math header | Code quality |
| 9 | **Fix 5** — `n_groups` estimate | Output completeness |
| 10 | **M8** — Multi-core parallelism | Largest engineering effort; required for patent claim 1 |
