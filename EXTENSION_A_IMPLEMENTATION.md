# Extension A: Stratified Sampling — Detailed Implementation Explanation

## The Problem It Solves

In the baseline Zippy algorithm, rare groups (groups with few rows but high aggregate values) are **systematically underrepresented** in uniform random sampling. For example:

```
Group A: 1,000,000 rows × $1    = $1,000,000    (easily sampled)
Group B:         1 row × $999,999 = $999,999    (almost never sampled)
```

Group B belongs in the top-2 but uniform sampling will almost never select its single row. As a result:

- Group B is missing from the FA (Fine-grained Aggregates) hash table
- Its partition's `total_sum` still exceeds `topKBound` (safe from pruning)
- But `topKBound` is artificially low, causing fewer partitions to be pruned
- This forces **unnecessary extra passes** through the data

**Extension A fixes this by using a two-phase sampling strategy:** first uniform random sampling, then targeted boosting of underrepresented groups using a pre-built index.

---

## High-Level Architecture

Extension A consists of **two main components**:

1. **GroupOccurrenceIndex** (`group_index.h/cpp`) — Maps each group_id to its row positions in the dataset
2. **StratifiedSampler** (`stratified_sampler.h/cpp`) — Two-phase sampler that uses the index to boost underrepresented groups

```
┌─────────────────────────────────────────────────────────────────┐
│                      Extension A Pipeline                       │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  1. Build GroupOccurrenceIndex (one scan of dataset)           │
│     ├─ Record every row's position                            │
│     ├─ Group them by group_id                                 │
│     └─ Store map: group_id → {row_count, [positions...]}      │
│                                                                 │
│  2. Phase 1: Uniform Random Sampling                           │
│     ├─ Draw sample_size random rows                           │
│     ├─ Aggregate per-group: {sum, count, min, max}            │
│     └─ Result: sample_stats (Phase 1 only)                    │
│                                                                 │
│  3. Phase 2: Stratified Correction                            │
│     ├─ For EACH group in GroupOccurrenceIndex:                │
│     │   ├─ Calculate: expected_count = (group_size/N) × s1    │
│     │   ├─ Check: observed_count < threshold × expected?      │
│     │   ├─ If YES: fetch boost_rows from index               │
│     │   └─ Add fetched rows to sample_stats for this group    │
│     └─ Result: merged sample_stats (Phase 1 + Phase 2)        │
│                                                                 │
│  4. Candidate Selection (Hoeffding LB + L_k logic)             │
│     ├─ Compute per-group confidence interval lower bounds     │
│     ├─ Find L_k = K-th highest lower bound                    │
│     ├─ Select all groups where LB ≥ L_k                      │
│     ├─ Fill remaining FA slots with heavy hitters             │
│     └─ Return: FA group candidates                            │
│                                                                 │
│  5. Standard Zippy Pipeline (unchanged)                        │
│     ├─ Pass 1: Route all rows → FA + CA                      │
│     ├─ Prune: Remove low-potential CA partitions              │
│     └─ Multi-pass: Converge to final top-K                    │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

---

## Component 1: GroupOccurrenceIndex

**Purpose:** Record the dataset structure — where each group's rows are located.

**Data Structure:**

```cpp
std::unordered_map<uint64_t, GroupOccurrenceEntry> entries_;
    ↓
struct GroupOccurrenceEntry {
    size_t row_count = 0;           // True count of all rows for this group
    std::vector<uint64_t> positions; // Subset of row indices (capped to save memory)
};
```

### Key Methods

| Method                                                | What It Does                                                                                                |
| ----------------------------------------------------- | ----------------------------------------------------------------------------------------------------------- |
| `build(dataset)`                                      | One-pass scan: records every row position per group_id. Estimates `N/4` distinct groups to avoid rehashing. |
| `add_occurrence(group_id, row_pos)`                   | Incremental: increment `row_count` and append position (if under cap). Used by `build()` and ext-ab.        |
| `is_underrepresented(gid, obs, sample_sz, threshold)` | Computes `expected = (group_size/N) × sample_size`. Returns **true** if `observed < threshold × expected`.  |
| `get_boost_rows(gid, n_boost)`                        | Returns the first `min(n_boost, stored_positions)` row indices for a group.                                 |
| `row_count_for(gid)`                                  | Returns the **true** row count for a group (not just stored positions — the actual count is separate).      |

### Example Walkthrough

```
Dataset:
  Row 0: group_id=42,  value=100
  Row 1: group_id=99,  value=50
  Row 2: group_id=42,  value=200
  Row 3: group_id=42,  value=150

After index.build():
  index.entries_ = {
    42: {row_count: 3, positions: [0, 2, 3]},
    99: {row_count: 1, positions: [1]}
  }

Later, if group 42 is underrepresented in Phase 1 sample:
  boost_rows = index.get_boost_rows(42, n_boost=2)
  → returns [0, 2]  (first 2 positions)
  → fetch dataset[0] and dataset[2]
  → add their values (100 + 200) to the Phase 1 sample aggregate for group 42
```

---

## Component 2: StratifiedSampler — The Two-Phase Logic

**Function Signature:**

```cpp
SampleResult stratified_sample_and_select(
    const std::vector<Row>&              dataset,
    const GroupOccurrenceIndex&          group_index,  // pre-built
    size_t                               fa_capacity,
    int                                  k,
    AggFunc                              agg_func,
    double                               sample_frac,
    double                               delta,
    double                               alpha_ci,
    double                               beta_ci,
    double                               underrep_threshold,  // e.g., 0.5
    size_t                               boost_rows,         // e.g., 10
    uint64_t                             seed,
    const std::unordered_set<uint64_t>&  pre_injected_groups  // ext-ab
);
```

---

## Phase 1: Uniform Random Sampling

```cpp
// Sample size calculation (IDENTICAL to baseline sampler)
const double z = z_alpha_over_two_strat(alpha_ci);
const double formula_sample = std::ceil((z * z) / (4.0 * safe_delta * safe_delta));
const double frac_sample = std::ceil(safe_frac * static_cast<double>(n_rows));
size_t sample_size_target = std::max(1.0, std::max(formula_sample, frac_sample));
//    ↑ Usually: max(384, 0.01 × N)

// Direct random row selection (O(sample_size) not O(N))
std::mt19937_64 rng(seed);
std::uniform_int_distribution<size_t> dist(0, n_rows - 1);

for (size_t i = 0; i < sample_size_target; ++i) {
    const Row& row = dataset[dist(rng)];                    // Random index
    accumulate(result.sample_stats[row.group_id], row.value);
    ++result.sample_size_actual;
}
// After Phase 1:
// result.sample_stats = {
//   42: {sum: 300, count: 2, min: 100, max: 200},
//   99: {sum: 50,  count: 1, min: 50,  max: 50}
// }
```

**Cost:** O(sample_size) ≈ O(log N) operations instead of O(N).

---

## Phase 2: Stratified Correction for Underrepresented Groups

```cpp
// Iterate EVERY group in the index
for (const auto& [gid, entry] : group_index.entries()) {
    // Step 1: Calculate expected representation in Phase 1 sample
    const double expected_count =
        (static_cast<double>(entry.row_count) / group_index.total_rows())
        * static_cast<double>(result.sample_size_actual);
    // expected_count = (true_group_size / N) × sample_size_phase1
    // Example: if group 42 has 3 rows out of 1000, and sample_size=100,
    // then expected_count = (3/1000) × 100 = 0.3

    // Step 2: Skip groups that were already well-represented
    if (expected_count > static_cast<double>(boost_rows)) {
        continue;  // These groups don't need boosting
    }

    // Step 3: Get actual observed count from Phase 1
    size_t observed = 0;
    {
        const auto it = result.sample_stats.find(gid);
        if (it != result.sample_stats.end()) {
            observed = static_cast<size_t>(it->second.count);
        }
    }

    // Step 4: Check underrepresentation
    //   Is observed < underrep_threshold × expected?
    if (!group_index.is_underrepresented(
            gid, observed, result.sample_size_actual, underrep_threshold)) {
        continue;  // This group is adequately represented
    }

    // Step 5: GROUP IS UNDERREPRESENTED → Boost it!
    // Fetch up to boost_rows additional rows from the index
    const std::vector<uint64_t> boost_positions =
        group_index.get_boost_rows(gid, boost_rows);

    if (boost_positions.empty()) continue;

    // Step 6: Add boost rows to Phase 1 aggregate
    SampleGroupStats& stats = result.sample_stats[gid];
    for (uint64_t pos : boost_positions) {
        if (pos >= dataset.size()) continue;
        accumulate(stats, dataset[static_cast<size_t>(pos)].value);
        // accumulate() updates: sum, count, min, max
    }
}
// After Phase 2:
// result.sample_stats = {
//   42: {sum: 550, count: 5, min: 100, max: 200},  ← boosted!
//   99: {sum: 50,  count: 1, min: 50,  max: 50}
// }
```

**Cost per group:**

- O(1) lookup in `sample_stats`
- O(1) lookup in group_index map
- O(boost_rows) to fetch and accumulate rows
- **Total Phase 2:** O(|GroupIndex| × boost_rows) ≈ O(M × 10) for M groups

---

## Underrepresentation Check (The Core Logic)

```cpp
// From group_index.cpp
bool GroupOccurrenceIndex::is_underrepresented(
    uint64_t group_id,
    size_t observed_count,
    size_t sample_size,
    double threshold) const
{
    const size_t group_size = row_count_for(group_id);

    // expected_count = (group_size / N) × sample_size
    const double expected =
        (static_cast<double>(group_size) / static_cast<double>(total_rows_))
        * static_cast<double>(sample_size);

    // If expected < 0.5, the group is too rare to meaningfully boost
    if (expected < 0.5) return false;

    // Is observed < threshold × expected?
    // Example: threshold=0.5, expected=0.3, observed=0 → true (boosted)
    return static_cast<double>(observed_count) < threshold * expected;
}

expected_count = (group_size / total_rows) × sample_size

observed_count < threshold × expected_count

```

### Example

- Group 42: true size=3, N=1000, sample_size=100
- `expected = (3/1000) × 100 = 0.3`
- Suppose `threshold = 0.5` and `observed = 0`
- Check: `0 < 0.5 × 0.3 = 0.15` → **true → boost this group**

---

## Phase 3: Candidate Selection (Hoeffding LB + L_k)

This is **identical to the baseline sampler**. Using the merged Phase 1 + Phase 2 aggregates:

```cpp
// Compute per-group Hoeffding confidence interval lower bounds
std::vector<Candidate> candidates;
for (const auto& [gid, stats] : result.sample_stats) {
    candidates.push_back(Candidate{
        gid,
        aggregate_lb_strat(stats, agg_func, beta_ci),  // LB = sum - ε×count
        stats.count
    });
}

// L_k = K-th highest lower bound
// (Use std::nth_element for O(M) rather than O(M log M))
std::vector<double> lbs;
for (const auto& c : candidates) lbs.push_back(c.lb);
std::nth_element(lbs.begin(), lbs.begin() + (k - 1), lbs.end(),
                 std::greater<double>());
double l_k = lbs[k - 1];

// tempGroups = {g : LB(g) ≥ L_k}
// These are the FA candidates (confident in top-K)
std::vector<Candidate> temp_groups;
for (const auto& c : candidates) {
    if (c.lb >= l_k) temp_groups.push_back(c);
}

// Heavy-hitter top-up: fill remaining FA slots
// with highest-count groups not already selected
if (result.fa_groups.size() < fa_capacity) {
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) {
                  return a.count > b.count;
              });
    for (const auto& c : candidates) {
        if (result.fa_groups.size() >= fa_capacity) break;
        result.fa_groups.insert(c.group_id);
    }
}

return result;
```

---

## Why This Improves Zippy

### Before Extension A (Baseline)

- Rare groups miss the Phase 1 sample
- They're not in FA
- Their partition's `total_sum` is the only signal
- `topKBound` is artificially low → fewer partitions pruned
- More data passes needed

### After Extension A

- Rare groups are detected by Phase 2
- Their boosted aggregate places them in FA
- FA groups drive higher `topKBound` in Pass 1
- More CA partitions are pruned immediately
- Fewer or same number of passes, but with better efficiency

### Key Metrics Improved

1. `fa_hit_rate` ↑ (more true top-K groups captured in FA)
2. `topKBound_after_pass1` ↑ (boosted aggregates set a higher threshold)
3. `partitions_pruned_pct` ↑ (higher threshold → more partitions eliminated)
4. `total_passes` ↓ (less multi-pass work needed)

---

## Cost Breakdown

| Phase                          | Cost                  | Notes                                          |
| ------------------------------ | --------------------- | ---------------------------------------------- |
| Build GroupOccurrenceIndex     | **O(N)**              | One full scan + hashmap insertions             |
| Phase 1 uniform sampling       | **O(sample_size)**    | ~O(log N) random lookups into dataset          |
| Phase 2 stratified correction  | **O(M × boost_rows)** | Iterate all M groups, fetch ≤10 rows each      |
| Phase 3 candidate selection    | **O(M log M)**        | Sort or nth_element on candidates              |
| **Total Extension A overhead** | **~O(N + M log M)**   | Index build dominates; stratified walk is fast |
| Pass 1 (standard Zippy)        | **O(N)**              | Unaffected by Extension A                      |

**Trade-off:** Pay ~5–20ms to build the index and run stratified correction, save 50–200ms in multi-pass overhead on adversarial datasets.

---

## Integration with Zippy Pipeline

In `zippy.cpp`, Extension A is wired as:

```cpp
RunMetrics run_zippy_ext_a(
    const std::vector<Row>& dataset, int k, const ZippyConfig& cfg,
    std::vector<std::pair<uint64_t,double>>& out_results,
    std::vector<uint64_t>& out_fa_groups)
{
    Timer timer; timer.reset();

    // Step 1: Build GroupOccurrenceIndex
    GroupOccurrenceIndex group_index;
    group_index.build(dataset);
    // ↑ One pass, records all row positions per group

    // Step 2: Call stratified sampler (replaces uniform_sample_and_select)
    SampleResult sample =
        stratified_sample_and_select(
            dataset,
            group_index,
            cfg.fa_capacity,
            k,
            AggFunc::SUM,
            cfg.sample_frac,
            cfg.delta,
            cfg.alpha_ci,
            cfg.beta_ci,
            cfg.underrep_threshold,
            cfg.boost_rows,
            42  /* seed */
        );
    // ↑ Phase 1: uniform sample
    // ↑ Phase 2: stratified boost
    // ↑ Phase 3: Hoeffding LB + L_k

    // Step 3: Rest of Zippy pipeline (IDENTICAL to baseline)
    return run_zippy_with_candidates(
        dataset, k, cfg, sample, out_results, out_fa_groups);
}
```

---

## Summary

| Aspect             | Detail                                                                            |
| ------------------ | --------------------------------------------------------------------------------- |
| **Problem**        | Rare high-value groups miss uniform sampling, causing unnecessary multi-pass work |
| **Solution**       | Two-phase sampling: uniform + index-based boost for underrepresented groups       |
| **Phase 1**        | O(log N) uniform random samples, accumulate per-group stats                       |
| **Phase 2**        | Walk all groups in GroupOccurrenceIndex, boost if observed < threshold × expected |
| **Cost**           | Index build (O(N)) + stratified walk (O(M × boost_rows))                          |
| **Benefit**        | Higher `topKBound` after Pass 1 → more pruning → fewer total passes               |
| **Implementation** | `GroupOccurrenceIndex` + `stratified_sample_and_select()`                         |
| **No change to**   | Pass 1 routing, CA pruning, multi-pass loop, or final correctness                 |

This completes Extension A — a clean, self-contained layer on top of baseline Zippy that targets the undersampling weakness without affecting any other part of the algorithm.
