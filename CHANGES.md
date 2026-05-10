# Code Improvements — Paper Audit Implementation

Based on `PAPER_AUDIT.md` deviations from Siddiqui et al. (PVLDB 2023) and US Patent 12,380,098 B2.

---

## Improvement 1 — Hoeffding CI Bounds + L_k Gate (Algorithm 2)

**Audit ref:** Deviations #1, #6, #7 | **Patent:** Claims 7/17

**Files changed:** `src/sampler.h`, `src/sampler.cpp`, `src/zippy.cpp`

### Problem
`beta_ci` was silently ignored (`(void)beta_ci`). Candidates were selected by sorting on raw sample SUM. `is_optimizable` only went false when the sample was empty — the skew-validation gate was dead.

### What changed

**`src/sampler.h`**
- Added `hoeffding_lb` field to `SampleGroupStats` — stores the computed Hoeffding lower bound for each group after sampling.
- Added `k` parameter (default `0`) to `uniform_sample_and_select()` — used for computing the L_k threshold.
- Updated doc comment to reflect the Hoeffding CI implementation.

**`src/sampler.cpp`**
- Added `hoeffding_epsilon(range, n_g, beta)` helper in the anonymous namespace:
  ```
  ε_g = (b-a) * sqrt(ln(2/(1-β)) / (2 * n_g))
  ```
- Added `kth_highest(v, k)` helper — returns the k-th highest value in a vector (used to compute L_k).
- Sampling loop now tracks `global_min_val` / `global_max_val` to compute the global value range `(b-a)` needed for the Hoeffding formula.
- After sampling, computes per-group Hoeffding lower bound:
  ```
  LB_g = (s_g / n_sample) * N  -  ε_g
  ```
  where `N` = total dataset size, `n_sample` = actual sample size.
- Stores `lb` in `SampleGroupStats::hoeffding_lb`.
- Candidates are now **sorted by Hoeffding LB descending** (not raw SUM) — fixes Deviation #7.
- Computes **L_k** = k-th highest LB among all sampled groups. Groups with `LB > L_k` become primary FA candidates — implements patent claim 7's central limitation.
- **Heavy-hitter top-up** now fills remaining FA slots sorted by Hoeffding LB (not COUNT), conforming to patent col. 10.
- `is_optimizable` gate: remains false if `|candidates| > fa_capacity`.
- Removed the `// TODO` comment and `(void)beta_ci` suppression.

**`src/zippy.cpp`**
- Updated `uniform_sample_and_select()` call to pass `k_size` as the `k` argument.

---

## Improvement 2 — Multi-Aggregate Support: SUM / COUNT / MAX / MIN

**Audit ref:** Deviation #2 | **Patent:** col. 13

**Files changed:** `src/data_structures.h`, `src/zippy.h`, `src/zippy.cpp`, `src/main.cpp`

### Problem
`FAEntry` only stored `exact_sum`. `CATable::prune()` always compared `total_sum`. `merge_and_prune()` always pushed `part.total_sum` as the upper bound. `max_value`, `min_value`, and `count` in `CAPartition` were tracked but never used for pruning.

### What changed

**`src/data_structures.h`**

- Added `AggregateType` enum:
  ```cpp
  enum class AggregateType { SUM, COUNT, MAX, MIN };
  ```
- Renamed `FAEntry::exact_sum` → `FAEntry::exact_agg` (type-neutral name).
- Modified `FATable::insert()` to accept `initial_agg` — identity value per type:
  - SUM/COUNT: `0.0`
  - MAX: `std::numeric_limits<double>::lowest()`
  - MIN: `std::numeric_limits<double>::max()`
- Modified `FATable::update()` to accept `AggregateType` and branch:
  - SUM: `exact_agg += val`
  - COUNT: `exact_agg += 1.0`
  - MAX: `exact_agg = max(exact_agg, val)`
  - MIN: `exact_agg = min(exact_agg, val)`
- Modified `FATable::top_k()` to accept `AggregateType`:
  - SUM/COUNT/MAX: sorted descending (highest first)
  - MIN: sorted ascending (lowest first, since top-k MIN = smallest values)
- Added `CAPartition::upper_bound(AggregateType)` method — returns the correct partition-level upper bound per patent col. 13:
  - SUM → `total_sum`
  - COUNT → `count` (cast to double)
  - MAX → `max_value`
  - MIN → `min_value`
- Modified `CATable::prune(bound, AggregateType)` — prunes with correct comparison direction:
  - SUM/COUNT/MAX: prune when `upper_bound < bound`
  - MIN: prune when `upper_bound > bound` (top-k MIN = smallest values, so flip direction)

**`src/zippy.h`**
- Added `AggregateType agg_type = AggregateType::SUM` to `ZippyConfig`.
- Added `double locality_threshold = 0.20` and `size_t locality_segment_size = 100'000` to `ZippyConfig` (used in Improvement 4).

**`src/zippy.cpp`**
- Added `kth_bound_for_exact(values, k, agg_type)` — returns k-th highest for SUM/COUNT/MAX, k-th lowest for MIN.
- `merge_and_prune()` now accepts `AggregateType` and:
  - Merges partial aggregates with the correct operation per type.
  - Pushes `part.upper_bound(agg_type)` instead of hardcoded `part.total_sum`.
  - Counts `top_k_confirmed` with the correct comparison direction (< for MIN, > for others).
  - Prunes child partitions using `upper_bound(agg_type)` with the correct direction.
- `top_k_from_exact()` now accepts `AggregateType` and sorts ascending for MIN, descending otherwise.
- Pass 1 computes `fa_identity` based on `cfg.agg_type` and passes it to `fa.insert()`.
- `fa.update()` and `fa.top_k()` calls in pass 1 now pass `cfg.agg_type`.
- Partial accumulation in pass 2+ uses type-aware `switch` instead of always `+= val`.
- `merge_and_prune()` and `top_k_from_exact()` call sites pass `cfg.agg_type`.

**`src/main.cpp`**
- Added `--agg [sum|count|max|min]` CLI flag mapping to `cfg.agg_type`.
- Added `--locality-threshold <float>` and `--locality-segment-size <int>` CLI flags.

---

## Improvement 3 — Pass 1 Pruning Under-Tightness Fix

**Audit ref:** Deviation #5 | **Paper:** Algorithm 4 line 12

**Files changed:** `src/zippy.cpp`

### Problem
After pass 1, `topKBound` was set to the k-th FA value only. CA partitions were pruned with this weaker bound before the union with CA upper bounds was computed.

### What changed

**`src/zippy.cpp`** — replaced the pass 1 `topKBound` computation:

- **Before:** `topKBound = out_results[k_size - 1].second` (k-th FA value only)
- **After:** builds a union vector of all FA exact values + all non-empty CA partition upper bounds, then takes the k-th element via `kth_bound_for_exact()`.

This tightens the pruning bound at pass 1 to match Algorithm 4 line 12: `topKBound = K-th highest among {exactAggregates ∪ childPartition UBs}`. The extra vector build is O(|FA| + |CA|) — negligible overhead compared to the dataset scan.

---

## Improvement 4 — Segment-Locality Test + C_p/Q < T_c Decision (Algorithm 3)

**Audit ref:** Deviation #3 | **Patent:** Claims 3/4, 13/14

**Files changed:** `src/zippy.cpp`, `src/zippy.h`, `src/main.cpp`

### Problem
Pass 2+ always re-filtered the full row stream (logical partitioning). No locality score was computed, no C_p/Q < T_c threshold was checked, and physical partitioning (pre-sort rows into per-partition buffers) was absent.

### What changed

**`src/zippy.cpp`** — two new helpers in the anonymous namespace:

**`compute_locality(dataset, active_pid_set, n_partitions, level, segment_size)`**
- Scans the dataset in chunks of `segment_size` rows (patent default: 100,000).
- For each chunk, counts `d_s` = distinct group_ids and `c_s` = total rows among rows belonging to active partitions.
- Returns `l = (1/t) * Σ d_s/c_s` averaged over all chunks.
- Low `l` → rows are temporally clustered by group → physical partitioning is beneficial.

**`physical_partition_rows(dataset, fa, active_pid_set, n_partitions, level)`**
- One full-dataset scan, routing each non-FA row whose parent partition is in `active_pid_set` into a per-partition `vector<Row>` buffer.
- Returns `unordered_map<partition_id, vector<Row>>` for downstream processing.

**Pass 2+ loop** — locality decision added at the start of each iteration:
- Computes `locality` via `compute_locality()`.
- Computes C_p/Q < T_c check:
  - `C_p` = sum of `fm.estimate()` across surviving partitions (total estimated distinct groups)
  - `Q` = number of surviving partitions
  - `T_c` = minimum FA exact aggregate (lowest confirmed top-k value)
  - If `C_p / Q < T_c` → logical partitioning preferred
- `use_physical = (locality < locality_threshold) && !cp_q_logical`
- When `use_physical`: calls `physical_partition_rows()` and processes each partition buffer sequentially (better cache locality per partition).
- When logical (default): uses the existing full-dataset row-filtering scan.
- Row accumulation extracted to a `accumulate_row` lambda to avoid code duplication between the two paths.
- Verbose mode logs locality score, decision, C_p/Q, and T_c per pass.

**`src/zippy.h`**
- `ZippyConfig` gains `locality_threshold = 0.20` and `locality_segment_size = 100'000` (patent default values).

**`src/main.cpp`**
- `--locality-threshold <float>` overrides `cfg.locality_threshold`.
- `--locality-segment-size <int>` overrides `cfg.locality_segment_size`.

---

## Summary Table

| Deviation | Audit # | Patent Claim | File(s) | Status |
|-----------|---------|--------------|---------|--------|
| Algorithm 2 gutted — no CI, no L_k | #1, #6 | 7/17 | `sampler.cpp`, `sampler.h` | Fixed |
| SUM-only implementation | #2 | col. 13 | `data_structures.h`, `zippy.cpp`, `zippy.h`, `main.cpp` | Fixed |
| Algorithm 3 locality/partitioning missing | #3 | 3/4, 13/14 | `zippy.cpp`, `zippy.h`, `main.cpp` | Fixed |
| Pass 1 under-prune (FA-only bound) | #5 | Algorithm 4 line 12 | `zippy.cpp` | Fixed |
| Skew validation absent | #6 | 2/12 | `sampler.cpp` | Fixed |
| Heavy-hitter top-up wrong primary key | #7 | col. 10 | `sampler.cpp` | Fixed |
| No parallelism | #4 | claim 1 | — | Out of scope |
| Extensions A & B not implemented | #8 | n/a | — | Out of scope |
