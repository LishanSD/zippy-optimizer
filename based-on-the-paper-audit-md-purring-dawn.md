# Plan: Paper-Audit-Driven Code Improvements to Zippy Optimizer

## Context

The PAPER_AUDIT.md identifies 8 concrete deviations between the current implementation and Siddiqui et al. (PVLDB 2023) + US Patent 12,380,098 B2. The goal is to close the most impactful gaps in order — from trivial correctness fixes to moderate-complexity algorithm completions — while keeping the prototype single-threaded (parallelism is out of scope). Each improvement maps directly to specific lines in the codebase.

---

## Improvement 1 — Hoeffding CI bounds + L_k gate (Algorithm 2) [HIGH]

**Audit ref:** Deviations #1, #6, #7 — `sampler.cpp:86`, `sampler.cpp:151`  
**Patent:** Claims 7/17 (central limitation)

### Problem
`uniform_sample_and_select()` picks FA candidates by sorting on raw sample SUM. `beta_ci` is silently ignored (`(void)beta_ci`). `is_optimizable` only goes false when the sample is empty — the skew-validation gate is dead.

### Changes to `src/sampler.cpp`

**Add a `hoeffding_epsilon()` helper** (inside the anonymous namespace):
```cpp
// Hoeffding bound for one group: ε = (b-a) * sqrt(ln(2/(1-β)) / (2 * n_g))
// n_g = sample count for this group; b-a = overall value range in sample.
static double hoeffding_epsilon(double range, double n_g, double beta) {
    if (n_g <= 0.0 || range <= 0.0) return std::numeric_limits<double>::max();
    return range * std::sqrt(std::log(2.0 / (1.0 - beta)) / (2.0 * n_g));
}
```

**Compute per-group Hoeffding lower bounds** after building `sample_stats`:
```
LB_g = (s_g / n_sample) * N  -  epsilon_g
```
where `N` = total dataset size, `n_sample` = `result.sample_size_actual`, `range` = global (max_val - min_val) across all sampled values.

**Sort candidates by Hoeffding LB (descending)** — not raw SUM.

**Compute L_k** = k-th highest LB among all sampled groups (needs `k` passed in as parameter). The function signature already has all the info it needs except `k` — add it as a parameter with a default.

**`is_optimizable` gate:**
- `C_s` = bytes for candidate set = `candidates_selected * sizeof(FAEntry)` (currently computed as `cs_bytes`)
- `C_f` = FA capacity in bytes = `fa_capacity * sizeof(FAEntry)`
- Keep `is_optimizable = false` when `C_s > C_f` (already present)
- **Add:** also set `is_optimizable = false` when `k > sample.fa_groups.size()` after L_k filtering  

**Heavy-hitter top-up fix (Deviation #7):** The fill pass that tops up remaining FA slots should sort by Hoeffding LB first (not sample COUNT). After implementing the LB vector above, re-sort by LB descending for the top-up pass.

**Files modified:** `src/sampler.h` (add `k` param, add `hoeffding_lb` field to `SampleGroupStats`), `src/sampler.cpp` (implement above).

---

## Improvement 2 — Multi-aggregate support: SUM / COUNT / MAX / MIN [MEDIUM]

**Audit ref:** Deviation #2 — `zippy.cpp:82`, `data_structures.h:257`  
**Patent:** col. 13 — `total_sum` is SUM UB; `max_value` is MAX UB; `count` is COUNT UB.

### Problem
`FAEntry` only stores `exact_sum`; `CATable::prune()` always uses `total_sum`; `merge_and_prune()` pushes `part.total_sum` for every aggregate type. `max_value`/`min_value`/`count` in `CAPartition` are tracked but never used for pruning.

### Changes to `src/data_structures.h`

Add an enum before `FAEntry`:
```cpp
enum class AggregateType { SUM, COUNT, MAX, MIN };
```

Rename `FAEntry::exact_sum` → `FAEntry::exact_agg` (or keep alias). Modify `FATable::update()` to accept `AggregateType` and branch:
- SUM: `exact_agg += val`
- COUNT: `exact_agg += 1.0`
- MAX: `exact_agg = std::max(exact_agg, val)` (init to `lowest()`)
- MIN: `exact_agg = std::min(exact_agg, val)` (init to `max()`)

Add `CAPartition::upper_bound(AggregateType t)` method:
```cpp
double upper_bound(AggregateType t) const {
    switch (t) {
        case AggregateType::SUM:   return total_sum;
        case AggregateType::COUNT: return static_cast<double>(count);
        case AggregateType::MAX:   return max_value;
        case AggregateType::MIN:   return min_value;
    }
}
```

Modify `CATable::prune(double bound, AggregateType t)`:
- For SUM/COUNT/MAX: prune when `part.upper_bound(t) < bound` (current direction)
- For MIN: prune when `part.upper_bound(t) > bound` (top-k MIN = smallest values → flip)

### Changes to `src/zippy.h`
Add `AggregateType agg_type = AggregateType::SUM;` to `ZippyConfig`.

### Changes to `src/zippy.cpp`
- Thread `cfg.agg_type` through `fa.update()`, `ca.prune()`, and the `union_values` push in `merge_and_prune()`.
- In `merge_and_prune()`: push `part.upper_bound(agg_type)` instead of hardcoded `part.total_sum`.
- For MIN, flip the `top_k_bound` comparison direction (use `kth_lowest_or_inf` for MIN).

### Changes to `src/main.cpp`
Add `--agg [sum|count|max|min]` flag that maps to `cfg.agg_type`.

---

## Improvement 3 — Pass 1 pruning under-tightness fix [EASY]

**Audit ref:** Deviation #5 — `zippy.cpp:230-235`  
**Patent:** Algorithm 4 line 12: `topKBound = K-th highest among {exactAggregates ∪ childPartition UBs}`

### Problem
After pass 1, `topKBound` is set to the k-th FA value only. CA partitions are pruned with this weaker bound before the union with CA UBs is computed.

### Fix in `src/zippy.cpp` (after the pass 1 data scan, before `ca.prune()`)

Replace:
```cpp
double topKBound = 0.0;
if (k_size > 0 && out_results.size() >= k_size)
    topKBound = out_results[k_size - 1].second;
ca.prune(topKBound);
```

With:
```cpp
// Union bound: FA exact values + all non-empty CA partition UBs
std::vector<double> union_vals;
union_vals.reserve(out_fa_groups.size() + cfg.n_partitions);
for (uint64_t gid : out_fa_groups)
    union_vals.push_back(fa.get(gid));
for (size_t pid = 0; pid < ca.n_partitions(); ++pid)
    if (ca.partition(pid).count > 0)
        union_vals.push_back(ca.partition(pid).upper_bound(cfg.agg_type));
double topKBound = kth_highest_or_zero(union_vals, k_size);
ca.prune(topKBound, cfg.agg_type);
```

This is a one-pass extra vector build — negligible overhead, tighter pruning.

---

## Improvement 4 — Segment-locality test + C_p/Q < T_c decision (Algorithm 3) [MEDIUM]

**Audit ref:** Deviation #3 — `zippy.cpp:274-307`  
**Patent:** Claims 3/4, 13/14; defaults `segment_size=100k`, `α₀=0.20`

### Problem
Pass 2+ always re-filters the full row stream (logical partitioning). No locality score is computed; no C_p/Q < T_c check. Physical partitioning (pre-sort rows into per-partition buffers + non-temporal stores) is absent.

### New helper in `src/zippy.cpp`

```cpp
// Compute segment-level locality: l = (1/t) * Σ_segments (d_s / c_s)
// d_s = distinct group_ids seen in segment, c_s = total rows in segment.
// Returns value in [0,1]. Low l → rows are temporally clustered by group.
static double compute_locality(
    const std::vector<Row>& dataset,
    const std::unordered_set<size_t>& active_partitions_set,
    size_t n_partitions,
    int level,
    size_t segment_size = 100'000)
```

Implementation: scan rows in chunks of `segment_size`; for each chunk, count distinct `group_id`s (via `std::unordered_set`) and total rows among rows that map to an active partition. Average `d_s / c_s` over all chunks.

### New physical-partitioning helper

```cpp
// Pre-partition surviving rows into per-partition vectors.
// Returns map<partition_id, vector<Row>>.
static std::unordered_map<size_t, std::vector<Row>> physical_partition(
    const std::vector<Row>& dataset,
    const std::unordered_set<size_t>& active_pids,
    size_t n_partitions,
    int level)
```

One scan, route each non-FA row whose partition is in `active_pids` to its buffer. For patent conformance, write uses `_mm_stream_si128` non-temporal stores on x86 when available (guarded by `#ifdef __SSE2__`); falls back to normal assignment otherwise.

### Decision in pass 2+ loop

Add to `ZippyConfig`: `double locality_threshold = 0.20; size_t locality_segment_size = 100'000;`

At the start of each pass 2+ iteration:
```cpp
const double locality = compute_locality(dataset, active_pid_set, cfg.n_partitions, level, cfg.locality_segment_size);
const bool use_physical = (locality < cfg.locality_threshold);
```

For the `C_p/Q < T_c` check:
- `C_p` = estimated distinct groups across all surviving partitions (sum of `fm.estimate()`)
- `Q` = number of surviving partitions
- `T_c` = minimum count among current FA candidates (minimum `fa.get(gid)` for FA groups)
- If `C_p / Q < T_c` → use logical; else use physical (overrides locality decision as secondary check)

When `use_physical`:
- Call `physical_partition()` to get per-partition row vectors
- For each partition, run exact aggregation or child-partition routing on its local buffer
- This eliminates the full dataset re-scan overhead per pass

---

## Files to Modify (summary)

| File | Improvement |
|------|------------|
| [src/sampler.h](src/sampler.h) | Add `k` param, add `hoeffding_lb` to `SampleGroupStats` |
| [src/sampler.cpp](src/sampler.cpp) | Impl #1: Hoeffding epsilon, LB-based sort, L_k gate, heavy-hitter fix |
| [src/data_structures.h](src/data_structures.h) | Impl #2: `AggregateType` enum, `FATable::update(type)`, `CAPartition::upper_bound()`, `CATable::prune(bound, type)` |
| [src/zippy.h](src/zippy.h) | Impl #2 + #4: add `agg_type`, `locality_threshold`, `locality_segment_size` to `ZippyConfig` |
| [src/zippy.cpp](src/zippy.cpp) | Impl #2: thread agg_type; Impl #3: union bound for pass 1 prune; Impl #4: locality helper + physical partition helper + pass-loop decision |
| [src/main.cpp](src/main.cpp) | Impl #2: parse `--agg [sum\|count\|max\|min]`; Impl #4: parse `--locality-threshold`, `--locality-segment-size` |

---

## Execution Order

1. **Improvement 3** (pass 1 prune fix) — 10-line change, zero risk, immediate gain. Do first.
2. **Improvement 2** (multi-aggregate) — data structure change that downstream work depends on. Do before #1.
3. **Improvement 1** (Hoeffding CI) — builds on sampler, depends on nothing else.
4. **Improvement 4** (locality + physical partitioning) — most complex, do last.

## Verification

After each improvement, rebuild with CMake and run:
```
build\zippy.exe --input data\S0.bin --n-rows 10000 --k 10 --mode brute-force --output results\bf.json
build\zippy.exe --input data\S0.bin --n-rows 10000 --k 10 --mode baseline --output results\bl.json --verbose
python python\compare_phase4c_results.py results\bf.json results\bl.json
```
Baseline results must still match brute-force top-k groups exactly.

For multi-aggregate: re-run with `--agg count`, `--agg max`, `--agg min` and verify against brute-force outputs for each aggregate type.
