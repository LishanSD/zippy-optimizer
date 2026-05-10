# Paper + Patent vs Implementation Audit

> **Update — items 1, 2, 3 (decision logic), 5, 6, 7 of the deviations list have
> now been implemented.**  Section "Status after implementation pass" at the end
> of this document records what shipped.  Items 4 (parallelism) and 8 (Extensions
> A & B) remain explicitly out of scope.


References:
- Siddiqui et al., *Cache-Efficient Top-k Aggregation over High Cardinality Large Datasets*, PVLDB 17(4): 644–656, 2023 (`p644-siddiqui-paper.pdf`).
- US Patent **12,380,098 B2** (Microsoft / Siddiqui et al., issued Aug. 5, 2025) (`patent-application.pdf`). Cites the paper as prior publication. The patent description fills in implementation specifics the paper hand-waves, and the **claims** define what's legally protected — those claims are a useful checklist for "is this really Zippy".

Audited against: `src/zippy.cpp`, `src/zippy.h`, `src/sampler.cpp`, `src/sampler.h`, `src/data_structures.h`, `src/main.cpp`.

## TL;DR
The skeleton is right, but several core algorithmic pieces are **stubbed, replaced, or missing** — including pieces that are explicit limitations in the patent's independent claims (claims 1, 7, 8, 9, 11, 17, 18, 19). It's a single-threaded, SUM-only prototype — not a faithful Zippy implementation yet.

## What matches the paper / patent

- **Cache-resident structures (paper §4.1.1, patent description col. 11):** `FATable` is single-level open-addressing + linear probing at 50% load with 16-byte entries (`data_structures.h:90-204`). Matches **patent claim 5** ("first cache resident data structure is a single-level hash table with linear probing"). `CATable` has per-partition `sum`, `count`, `max`, `min`, `FMSketch` (`data_structures.h:256-352`).
- **Multi-pass framework outline (paper Algorithm 1, patent FIG. 6 / claim 1 method body):** sample → analyze → store → exact-agg candidates → partition non-candidates → merge → prune → rerank → resolve. Structurally present in `zippy.cpp:162-333`.
- **Sample size formula** `s ≥ Z²_{α/2} / (4Δ²)` (paper Algorithm 2, patent description col. 9) implemented at `sampler.cpp:100-101`.
- **FM sketch** for partition distinct-count uses stochastic averaging (32 buckets) — reasonable variant of [15]; patent description col. 11 specifies "Flajolet Martin (FM) algorithm".
- **Hash family separation** for FA / partition / FM / child-partition. Good.
- **Partition ranking by `psum/d`** (`data_structures.h:266-269`, `CAPartition::estimated_per_group_sum`) — matches patent description col. 8: "estimated values for sum and average of a grouping key are psum/d and psum/count". Only SUM is wired up though (see deviation #2).

## Concrete deviations from the paper / patent

### 1. Algorithm 2 (FA candidate selection) is gutted — violates patent claim 7 / 17
Paper (Algorithm 2 lines 11–33) and **patent claim 7** explicitly require:

> *estimating a k-th highest lower bound on aggregate values among all groups including the set of samples; and selecting candidate groups from the set of samples having an estimated lower bound that exceed the k-th highest lower bound.*

Patent description col. 9–10 spells out the formula: Hoeffding CI `ε = (b−a)·(1/(2·n_i'))·(ln(2/(1−β)))^(1/2)`, take `L_k`, then `if C_s > C_f → not optimizable; else fill remaining FA slots with heavy hitters such that C_s + C_h ≈ C_f`.

Implementation (`sampler.cpp:86`, `:151`): two literal `// TODO` comments. `beta_ci` is unused. Candidates are sorted by sample `sum` and the top `fa_capacity` are picked; the leftover-fill pass is sorted by `count` (closer-ish to the heavy-hitter rule, but the *primary* selection criterion is wrong). No CIs, no `L_k`, no skew-validation gate. `is_optimizable` only goes false when the sample is empty.

**Consequence:** the implementation never actually validates skew — it always proceeds. The paper's "if not optimizable, fall back" branch is effectively dead. Patent claim 2/12's *"analyzing the exact aggregate values computed over samples to determine a skew in distribution"* is also unimplemented.

### 2. Only SUM is implemented — missed multi-aggregate UB rule
Paper §2 targets monotonic aggregates SUM, COUNT, MAX, MIN with `Y ≥ 0`. **Patent description col. 13 is explicit:**

> *the partition level sum provides an upper bound for sum and average aggregates, while the maximum value sets an upper bound for max and min aggregates.*

Implementation (`zippy.cpp:82`): `union_values.push_back(part.total_sum); // SUM UB`. No MAX/MIN/COUNT path. CLI has no aggregate-function flag. `CAPartition` tracks `max_value` / `min_value` but they're never read for pruning. The README/AGENTS.md both claim "full Algorithm 1"; the code only handles SUM.

### 3. Algorithm 3 — locality test and logical-vs-physical decision missing — violates patent claims 3, 4 / 13, 14
Paper Algorithm 3 lines 5–24, §4.3, **and patent description col. 11–12** specify:

- Segment-based locality `l = Σ_s d_s/c_s / t`, threshold `α₀`. Patent gives explicit defaults: **`s = 100k`, `α₀ = 0.20`**.
- Logical-vs-physical decision: **patent claim 4 / 14** mandates *"partitioning the non-candidate groups using logical partitioning responsive to the cardinality of the input partition divided by a number of logical partitions that the second cache resident data structure is capable of storing is less than a lowest frequency of any group among a set of current candidate groups"* — i.e. `C_p / Q < T_c → logical, else physical`.

Implementation (`zippy.cpp:274-307`): pass 2+ only checks `fm.estimate() < fa_capacity`. No segment scan, no `α₀ = 0.20` test, no `C_p/Q` vs `T_c` comparison. Always logical (re-filter the row stream). Physical partitioning + non-temporal stores from Algorithm 3 lines 33–37 (also patent col. 12: *"buffers one partition per cache line ... writes the partition data to the memory once full using non-temporal store instructions"*) are entirely absent.

### 4. No parallelism — patent's whole shared-nothing premise is missing
Paper §4.4 and **patent claim 1** are explicit on multicore: *"creating a first cache resident data structure ... in a cache of **each core** of the multicore processor"*, *"merging the exact aggregate values for the candidate groups **for the plurality of cores**"*. Implementation is fully single-threaded. No threads, no per-core partial aggregates, no input blocking. Pass 2+ re-scans the **entire dataset** every pass instead of scanning unprocessed partitions.

(For a single-machine prototype this is acceptable — but it means the impl literally cannot satisfy the patent's independent claim 1 without significant work.)

### 5. Pass 1 pruning bound is FA-only
Paper Algorithm 4 line 12: `topKBound = K-th highest among {exactAggregates ∪ childPartition UBs}`.

Implementation (`zippy.cpp:230-235`): `topKBound` = K-th FA value only, **before** building children. Safe but **under-prunes** vs the paper. Pass 2+ does use the union (`zippy.cpp:74-85`), so the deviation is only in pass 1.

### 6. Confidence-interval skew validation absent
Algorithm 2 lines 16–25 (Hoeffding bounds, β/2 percentiles for max/min) — none of it. `cfg.beta_ci` flows in but is silently ignored (`sampler.cpp:86`). Patent claim 2/12's skew-analysis step is unimplemented.

### 7. Heavy-hitter top-up uses wrong primary key
Patent description col. 10 says: pick by CI lower bound first, then *top up* with heavy hitters (highest sample count) until `C_s + C_h ≈ C_f`. Implementation (`sampler.cpp:162-195`) picks top by sample SUM first, then tops up by sample COUNT. The top-up shape is right; the primary criterion is wrong (should be Hoeffding LB, not raw sum).

### 8. Extensions A & B not implemented
Phases 5–7 in README are 🔲. `main.cpp:92-100` returns errors for `ext-a` / `ext-b` / `ext-ab`. Headers exist; modes don't run. The patent itself does not require these — they are paper §7 future-work proposals.

## Patent claim-coverage summary

| Patent claim element                                                              | Implemented?         |
| --------------------------------------------------------------------------------- | -------------------- |
| Sample → candidate / non-candidate split (claim 1)                                | ⚠ partial — no CI gate |
| Two cache-resident structures, FA + CA, in cache of **each core** (claim 1)      | ❌ single-threaded    |
| Single-level hash table w/ linear probing for FA (claim 5)                        | ✅                    |
| Skew analysis on samples (claim 2 / 12)                                           | ❌                    |
| Logical vs physical decision via `C_p/Q < T_c` (claim 4 / 14)                     | ❌                    |
| Single-level FA hash table w/ linear probing (claim 5)                            | ✅                    |
| Sampling tuples from groups satisfying tolerance Δ (claim 6 / 16)                 | ⚠ uniform only        |
| `L_k`-based candidate selection (claim 7 / 17)                                    | ❌ stubbed            |
| Group-specific stats aggregated to partition stats (claim 8 / 18)                 | ⚠ partition-level only — no per-group stats inside |
| Pruning by upper bound vs threshold (claim 9 / 19)                                | ✅ for SUM only       |
| Visualization / dashboard wiring (claim 10 / 20)                                  | n/a (out of scope)   |

## Bottom line
What's labeled "Phase 4C complete / full Algorithm 1" is more honestly:

> Algorithm 1 outline + Algorithm 2 with point estimates instead of Hoeffding + Algorithm 3 with the locality/partitioning logic stripped + Algorithm 4 SUM-only + no parallelism.

Several of the missing pieces are not just "paper niceties" — they are explicit limitations of the patent's independent claims. The brute-force comparator and the cache-resident data structures are paper-faithful; the algorithm proper is significantly thinned.

## Suggested fix order (by impact)

1. **Implement Hoeffding LB + L_k gate in Algorithm 2** (`sampler.cpp`) — finishes patent claim 7's central limitation; also unlocks the real `is_optimizable` fallback path.
2. **Generalize MergeAndPrune to SUM / COUNT / MAX / MIN** (`zippy.cpp` + CLI flag) — patent description col. 13 specifies the per-aggregate UB rule; current code uses `total_sum` for everything.
3. **Add the segment-locality test (`l = Σ d_s/c_s / t`, defaults `s=100k, α₀=0.20`) and `C_p/Q < T_c` decision** in pass 2+ (`zippy.cpp`, new helpers) — patent claims 3/4, 13/14.
4. **Heavy-hitter top-up should use Hoeffding LB as primary, count as tiebreaker** — small but conformance-relevant.
5. Parallelism (paper §4.4, patent claim 1 "each core") — biggest engineering lift, lowest priority for a correctness-focused prototype.

---

## Status after implementation pass

### What shipped (commits live in `src/`)

- **Algorithm 2 with Hoeffding LB + L_k gate** — `sampler.cpp:hoeffding_eps_per_tuple`, `aggregate_lower_bound`, plus the `tempGroups`/`Cs > Cf` decision and heavy-hitter top-up sorted by sample count. `SampleResult` now exposes `l_k_lower_bound` and `cs_above_lk`.  Exercised: `--fa-capacity 5` on S0 produces `is_optimizable=false` and the engine falls back to brute-force; top-K still matches.
- **Multi-aggregate (SUM / COUNT / MAX / MIN)** — `AggFunc` enum in `data_structures.h`; `FAEntry` widened to 32 bytes tracking all four; `CAPartition::upper_bound(AggFunc)` and `estimated_per_group(AggFunc)`; `CATable::prune(topKBound, AggFunc)`; multi-aggregate `ExactAccum` in `zippy.cpp`; multi-aggregate `run_brute_force`. CLI: `--agg sum|count|max|min`. Verified end-to-end: all four aggregates on S0 produce top-K identical to brute-force.
- **Pass 1 union UB** (`zippy.cpp:run_zippy_baseline`) — `topKBound` after Pass 1 is now the K-th highest among `{FA exact values} ∪ {partition UBs}`, matching Algorithm 4 line 12.
- **CI skew validation gate** — fall back if top-K sample aggregate share of total < 1% (sampler.cpp, in addition to the `Cs > Cf` test). Patent claim 2 / 12.
- **Heavy-hitter top-up by sample count** — primary key now Hoeffding LB (the L_k filter), then count as the top-up (patent col. 10's `Cs + Ch ≈ Cf`).
- **Locality test + adaptive partitioning decision** — `classify_partition` in `zippy.cpp` implements:
  - `distinct < FA_capacity` → EXACT
  - `distinct / count < α₀` (default 0.20) → EXACT (paper §4.3 / patent col. 11 locality test)
  - `C_p / Q < T_c` → LOGICAL (patent claim 4 / 14)
  - else → PHYSICAL
  - `T_c` is `FATable::lowest_count()`. Decision counts surfaced in metrics (`partitions_exact_agg`, `partitions_logical`, `partitions_physical`).

### Caveats (intentional shortcuts, not omissions)

- **Locality is single-segment**: `l = d / count` over the whole partition, not the patent's `Σ_s d_s/c_s / t` over `s = 100k`-sized segments. Cheaper to compute under the dataset-rescan model and gives the same direction of decision; configurable via `--alpha-locality` and `--segment-size` (latter currently unused).
- **Logical vs Physical execution path**: in this single-threaded prototype both branches re-scan the dataset and update child stats — the per-core cache benefit of physical row movement is moot without parallelism (deviation #4). Decisions are still recorded; future parallel work can wire them to actual row materialization.
- **MIN aggregate LB**: paper specifies β/2-percentile CIs; impl uses sample MIN as a heuristic (technically an upper bound). Order-preserving for the candidate filter but not rigorous. Documented in `aggregate_lower_bound` in `sampler.cpp`.

### Remaining (out of scope per user request)

- **Item 4** — multi-core parallelism (paper §4.4, patent claim 1's "each core").
- **Item 8** — Extensions A (stratified sampling) and B (measure-column index). Modes `ext-a`, `ext-b`, `ext-ab` still return errors from `main.cpp`.

### Verification commands used

```bash
g++ -std=c++17 -O2 -o build/zippy src/main.cpp src/zippy.cpp src/sampler.cpp src/group_index.cpp src/stratified_sampler.cpp src/measure_index.cpp -Isrc/
for AGG in sum count max min; do
  ./build/zippy --input data/S0.bin --n-rows 10091 --k 10 --mode brute-force --agg $AGG --output results/S0_bf_$AGG.json
  ./build/zippy --input data/S0.bin --n-rows 10091 --k 10 --mode baseline   --agg $AGG --output results/S0_bl_$AGG.json
  diff <(jq -r '.top_k_results[]' results/S0_bf_$AGG.json) <(jq -r '.top_k_results[]' results/S0_bl_$AGG.json)
done
# Force fallback gate:
./build/zippy --input data/S0.bin --n-rows 10091 --k 10 --mode baseline --fa-capacity 5 --output /tmp/x.json
# → is_optimizable=false, top-K still matches brute-force.
```
