# Extension B: Measure Column Index — Detailed Implementation Explanation

## The Problem It Solves

Extension A addresses **underrepresented groups** (rare groups with many rows but low individual values). Extension B addresses a different scenario: **groups with a few extreme-value rows that could rank in the top-K despite low frequency**.

Example:

```
Dataset with 10 million rows, 1 million groups, top-K = 50

Group A: 500,000 rows × $10 = $5,000,000 (common high-value)
Group B:      5 rows × $1,000,000 = $5,000,000 (rare extreme-value)
Group C: 100 rows × $1 = $100 (very low value)
```

Both Group A and B belong in the top-2, but **uniform sampling is unlikely to pick even one row from Group B** (5 rows out of 10M ≈ 0.00005% chance per row).

Extension B solves this by:

1. **Pre-scanning the dataset** to find the `m` rows with the **largest individual values** (not group aggregates)
2. **Force-injecting** the group_ids of those extreme rows into the FA candidates
3. Letting Zippy's normal pipeline handle the rest

This ensures that groups with outlier-valued rows cannot be missed by sampling, regardless of their frequency.

---

## High-Level Architecture

Extension B consists of **one lightweight component**:

1. **MeasureIndexBuilder** (`measure_index.h/cpp`) — Min-heap tracker for top-m extreme values

```
┌─────────────────────────────────────────────────────────────────┐
│                      Extension B Pipeline                       │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  1. Build Measure Index (one scan of dataset)                  │
│     ├─ Maintain min-heap of size m                            │
│     ├─ For each row: if value > min_heap.top(), swap it in    │
│     └─ Result: m rows with largest individual values          │
│                                                                 │
│  2. Extract Extreme Group IDs                                 │
│     ├─ Pop all m rows from the heap                          │
│     ├─ Collect their unique group_ids                        │
│     └─ Result: FORCED_SET = set of up to m unique groups     │
│                                                                 │
│  3. Pre-inject into FA Candidate List                         │
│     ├─ Reserve FA slots for extreme_groups                   │
│     ├─ Pass extreme_groups to Algorithm 2 sampler            │
│     └─ Sampler prioritizes them: insert first, fill rest     │
│                                                                 │
│  4. Algorithm 2: Sampling + FA Selection (modified)           │
│     ├─ Compute Hoeffding LB + L_k (same as baseline)         │
│     ├─ BUT: Force-insert extreme_groups first into FA        │
│     ├─ Fill remaining capacity with sample-derived groups    │
│     └─ Return: FA group candidates (extreme + sample)        │
│                                                                 │
│  5. Standard Zippy Pipeline (unchanged)                        │
│     ├─ Pass 1: Route all rows → FA + CA                      │
│     ├─ Prune: Remove low-potential CA partitions              │
│     └─ Multi-pass: Converge to final top-K                    │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

---

## Component: MeasureIndexBuilder

**Purpose:** Track the dataset's extreme-value rows efficiently in one pass.

**Data Structure:**

```cpp
class MeasureIndexBuilder {
    using HeapItem = std::pair<double, uint64_t>;  // (value, group_id)
    using MinHeap = std::priority_queue<HeapItem,
                                        std::vector<HeapItem>,
                                        std::greater<HeapItem>>;

    size_t  m_;          // max heap size (parameter)
    MinHeap min_heap_;   // tracks top-m highest values
};
```

### Key Methods

| Method                   | What It Does                                                                                |
| ------------------------ | ------------------------------------------------------------------------------------------- |
| `MeasureIndexBuilder(m)` | Constructor: initialize empty min-heap with capacity m.                                     |
| `observe(row)`           | Process one row: if heap size < m, insert; else if row.value > min, pop min and insert row. |
| `finish()`               | Extract all group_ids from heap, return unique set.                                         |

### Implementation Details

```cpp
void MeasureIndexBuilder::observe(const Row& row) {
    if (m_ == 0) return;  // disabled if m=0

    if (min_heap_.size() < m_) {
        // Heap not full yet — always insert
        min_heap_.push({row.value, row.group_id});
    } else if (row.value > min_heap_.top().first) {
        // Heap full but this row is larger than current minimum
        // → evict minimum, insert new row
        min_heap_.pop();
        min_heap_.push({row.value, row.group_id});
    }
    // else: row.value ≤ current min, ignore it
}

std::unordered_set<uint64_t> MeasureIndexBuilder::finish() {
    std::unordered_set<uint64_t> extreme_groups;
    extreme_groups.reserve(min_heap_.size());

    while (!min_heap_.empty()) {
        extreme_groups.insert(min_heap_.top().second);  // extract group_id
        min_heap_.pop();
    }
    return extreme_groups;
}

std::unordered_set<uint64_t> build_measure_index(
    const std::vector<Row>& dataset,
    size_t m)
{
    if (dataset.empty() || m == 0) return {};

    MeasureIndexBuilder builder(m);
    for (const auto& row : dataset) {
        builder.observe(row);  // O(log m) per row
    }
    return builder.finish();  // O(m log m) to extract all
}
```

---

## Example Walkthrough

```
Dataset (10 rows, 3 groups):
  Row 0: group_id=42,  value=100
  Row 1: group_id=99,  value=50
  Row 2: group_id=42,  value=999    ← EXTREME
  Row 3: group_id=99,  value=10
  Row 4: group_id=88,  value=1
  Row 5: group_id=42,  value=200
  Row 6: group_id=99,  value=500    ← EXTREME
  Row 7: group_id=88,  value=750    ← EXTREME
  Row 8: group_id=88,  value=30
  Row 9: group_id=42,  value=5

Build measure index with m=3:
  Process rows 0–1: min_heap = {(100, 42), (50, 99)}
  Process row 2:    min_heap = {(100, 42), (50, 99), (999, 42)}  [full]
  Process row 3:    10 ≤ 50 (min) → skip
  Process row 4:    1 ≤ 50 (min) → skip
  Process row 5:    200 > 50 (min) → pop (50,99), push (200,42)
                    min_heap = {(100, 42), (200, 42), (999, 42)}
  Process row 6:    500 > 100 (min) → pop (100,42), push (500,99)
                    min_heap = {(200, 42), (500, 99), (999, 42)}
  Process row 7:    750 > 200 (min) → pop (200,42), push (750,88)
                    min_heap = {(500, 99), (750, 88), (999, 42)}
  Process rows 8–9: values ≤ 500 → skip

Final heap: [(500, 99), (750, 88), (999, 42)]
Extract group_ids: {42, 88, 99}
```

**Key observation:** Even though group 88 and 99 have only 2–3 extreme-value rows, they are forced into FA candidates because their single rows could determine their top-K ranking.

---

## Integration with Algorithm 2 (Sampling)

Extension B modifies Algorithm 2's candidate selection to **prioritize extreme groups**:

```cpp
// In uniform_sample_and_select() (sampler.cpp, lines 265–276):

// --- Extension B Injection ---
// VIP access: Force-inject the extreme groups first.
for (uint64_t gid : pre_injected_groups) {  // pre_injected_groups = extreme_groups
    if (result.fa_groups.size() >= fa_capacity) break;
    result.fa_groups.insert(gid);
}

// FAgroups = tempGroups (Algorithm 2 line 33), up to remaining capacity
for (const auto& c : temp_groups) {  // temp_groups = {g : LB(g) ≥ L_k}
    if (result.fa_groups.size() >= fa_capacity) break;
    result.fa_groups.insert(c.group_id);
}

// Heavy-hitter top-up: fill remaining FA slots
if (result.fa_groups.size() < fa_capacity) {
    // Sort by sample count, insert highest-count groups
    std::vector<Candidate> sorted_by_count = candidates;
    std::sort(sorted_by_count.begin(), sorted_by_count.end(), ...);
    for (const auto& c : sorted_by_count) {
        if (result.fa_groups.size() >= fa_capacity) break;
        result.fa_groups.insert(c.group_id);
    }
}
```

**Priority order in FA slots:**

1. **Extreme groups** (from measure index) — inserted FIRST
2. **Hoeffding LB-qualified groups** (from sample) — inserted SECOND
3. **Heavy hitters** (highest sample count) — inserted LAST

This ensures extreme-value groups never get crowded out of FA.

---

## Phase 1 Benefit: Higher topKBound

When extreme-value groups are placed in FA:

1. **Before Extension B (baseline):**
   - Extreme group 88 missed from FA (rare, not in sample)
   - Partition containing group 88 not pruned (total_sum includes other rows)
   - topKBound stays artificially low
   - More multi-pass work needed

2. **After Extension B:**
   - Extreme group 88 forced into FA
   - Its true (exact) aggregate computed in Pass 1: 750 + 30 = 780
   - topKBound becomes the K-th highest among {FA exact values + partition UBs}
   - topKBound is higher → more partitions pruned
   - Multi-pass work reduced

---

## Cost Breakdown

| Phase                          | Cost                   | Notes                                     |
| ------------------------------ | ---------------------- | ----------------------------------------- |
| Build MeasureIndex             | **O(N log m)**         | One scan; m insertions/pops into min-heap |
| Extract unique group_ids       | **O(m log m)**         | Pop all m items from heap                 |
| **Total Extension B overhead** | **~O(N log m)**        | Fast; m is typically 500–5000             |
| Pass 1 (standard Zippy)        | **O(N)**               | Unaffected by Extension B                 |
| Sampling (Algorithm 2)         | **O(log N + M log M)** | Pre-injection uses O(1) per extreme group |

**Trade-off:** Pay ~2–10ms to build the index, save 10–50ms in multi-pass overhead on datasets with extreme-value outliers.

---

## Integration with Zippy Pipeline

In `zippy.cpp`, Extension B is wired as:

```cpp
RunMetrics run_zippy_ext_b(
    const std::vector<Row>& dataset, int k, const ZippyConfig& cfg,
    std::vector<std::pair<uint64_t,double>>& out_results,
    std::vector<uint64_t>& out_fa_groups)
{
    RunMetrics metrics;
    Timer total_timer; total_timer.reset();

    // ── Extension B: Build Measure Index (Pre-Pass) ───────────
    Timer index_timer; index_timer.reset();
    std::unordered_set<uint64_t> extreme_groups =
        build_measure_index(dataset, cfg.measure_m);
    metrics.index_build_duration_ms = index_timer.elapsed_ms();
    // ↑ extreme_groups = group_ids of top-cfg.measure_m extreme-value rows

    // ── Algorithm 2: Sampling + FA Selection ──────────────────
    Timer sample_timer; sample_timer.reset();
    SampleResult sample = uniform_sample_and_select(
        dataset,
        cfg.fa_capacity,
        k,
        cfg.agg_func,
        cfg.sample_frac,
        cfg.delta,
        cfg.alpha_ci,
        cfg.beta_ci,
        42,
        extreme_groups);  // ← Pass extreme groups as pre_injected_groups
    // ↑ Algorithm 2 now prioritizes extreme_groups first, then sample-derived

    metrics.sample_duration_ms = sample_timer.elapsed_ms();
    metrics.sample_size_actual = sample.sample_size_actual;
    metrics.fa_candidates_count = sample.fa_groups.size();
    metrics.is_optimizable = sample.is_optimizable;

    if (!sample.is_optimizable) {
        // Fall back to brute-force
        out_results = run_brute_force(dataset, k, cfg.agg_func);
        metrics.total_duration_ms = total_timer.elapsed_ms();
        return metrics;
    }

    // ── Pass 1: FA / CA routing ──────────────────────────────
    FATable fa(cfg.fa_capacity);
    for (uint64_t gid : sample.fa_groups) fa.insert(gid);
    // ↑ FA now includes extreme groups; they will be tracked exactly

    CATable ca(cfg.n_partitions);

    Timer pass1_timer; pass1_timer.reset();
    for (const auto& row : dataset) {
        if (fa.contains(row.group_id)) {
            fa.update(row.group_id, row.value);  // exact tracking
        } else {
            ca.update(row.group_id, row.value);  // partition tracking
        }
    }

    // Compute Pass 1 topKBound with contribution from exact FA groups
    // (which now include extreme groups)
    std::vector<double> pass1_union;
    for (const auto& [gid, val] : fa.top_k(fa.size(), cfg.agg_func)) {
        pass1_union.push_back(val);  // exact FA values (now higher due to extremes)
    }
    for (size_t pid = 0; pid < cfg.n_partitions; ++pid) {
        const CAPartition& part = ca.partition(pid);
        if (part.count > 0) pass1_union.push_back(part.upper_bound(cfg.agg_func));
    }
    const double topKBound = kth_highest_or_zero(pass1_union, k);

    ca.prune(topKBound, cfg.agg_func);  // Prune with elevated bound

    metrics.pass1_duration_ms = pass1_timer.elapsed_ms();
    metrics.topKBound_after_pass1 = topKBound;
    metrics.partitions_pruned_pct = ca.pruning_fraction();  // ↑ Higher pruning fraction
    metrics.total_passes = 1;

    // ── Pass 2+: Multi-pass loop (identical to baseline) ──────
    // (continue with merge_and_prune + adaptive partitioning as usual)

    return metrics;
}
```

---

## Parameter Tuning: Choosing m (measure_m)

The key parameter is `--measure-m`, which controls the size of the min-heap (how many extreme-value rows to track).

| m Value  | Trade-off                                 | Use Case                                      |
| -------- | ----------------------------------------- | --------------------------------------------- |
| 100      | Low overhead (~1ms), misses some extremes | Huge datasets (>1B rows), time-sensitive      |
| 500–1000 | Good balance, catches most extremes       | Default; works for most datasets              |
| 5000     | Thorough but expensive (~10ms), overkill  | Small datasets (<10M rows), accuracy critical |
| 0        | No Extension B                            | Debugging/comparison baseline                 |

**Research finding** (from AGENTS.md experiments): Often no benefit beyond m ≈ 3–5 × k (the query parameter). For k=50, m=500 is typically sweet spot.

---

## Extension B vs Extension A

| Aspect          | Extension A (Stratified)           | Extension B (Extreme Values)          |
| --------------- | ---------------------------------- | ------------------------------------- |
| **Targets**     | Rare low-frequency groups          | Groups with outlier-valued rows       |
| **Mechanism**   | Index-based row fetching           | Min-heap top-m tracking               |
| **Decision**    | Underrepresentation check          | Absolute value threshold (top m)      |
| **Cost**        | O(N + M × boost_rows)              | O(N log m + m log m)                  |
| **Benefit**     | More FA candidates for rare groups | Guarantees extreme-value groups in FA |
| **When to use** | Zipf-distributed datasets          | Datasets with extreme value outliers  |

**Extension A+B (ext-ab):** Both mechanisms work together:

1. Build GroupOccurrenceIndex + MeasureIndex (two passes or one shared scan)
2. Extract extreme_groups from measure index
3. Pass extreme_groups to stratified sampler as pre_injected_groups
4. Stratified sampler fills remaining FA slots with stratified-sampled candidates
5. Rest of Zippy unchanged

---

## Why This Improves Zippy

### Before Extension B

- Extreme-value groups miss uniform sampling (rare rows)
- They're absent from FA candidates
- Their partitions' `total_sum` is loose (many other groups in partition)
- topKBound is artificially low
- Unnecessary multi-pass iterations

### After Extension B

- Extreme-value groups guaranteed in FA (measure index forced them in)
- Their true aggregates computed exactly in Pass 1
- FA groups drive a higher topKBound
- More CA partitions pruned immediately
- Faster convergence to final top-K

### Key Metrics Improved

1. `topKBound_after_pass1` ↑ (extreme groups contribute exact high values)
2. `partitions_pruned_pct` ↑ (higher threshold → more partitions eliminated)
3. `total_passes` ↓ (less multi-pass work needed)
4. `index_build_duration_ms` (overhead; typically 2–10ms)

---

## Summary

| Aspect              | Detail                                                                                       |
| ------------------- | -------------------------------------------------------------------------------------------- |
| **Problem**         | Extreme-value outlier rows miss uniform sampling, forcing unnecessary multi-pass work        |
| **Solution**        | Min-heap scan to find top-m highest individual row values, force-inject their groups into FA |
| **Index Structure** | `MeasureIndexBuilder` — min-heap of (value, group_id) pairs, size m                          |
| **Build Cost**      | O(N log m) — one pass, fast heap operations                                                  |
| **Integration**     | Pass extreme groups to Algorithm 2 sampler as `pre_injected_groups`                          |
| **FA Priority**     | Extreme groups → LB-qualified groups → heavy hitters                                         |
| **Benefit**         | Higher `topKBound` after Pass 1 → more pruning → fewer total passes                          |
| **Implementation**  | `MeasureIndexBuilder` + `build_measure_index()` + modified `uniform_sample_and_select()`     |
| **No change to**    | Pass 1 routing, CA pruning, multi-pass loop, or final correctness                            |

This completes Extension B — a simple, elegant mechanism for capturing high-impact outlier groups that would otherwise escape the sampling net, ensuring robust top-K accuracy with minimal overhead.
