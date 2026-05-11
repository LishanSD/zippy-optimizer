# Session Context — Zippy Top-K Optimizer

> Drop this file into a fresh Claude session along with the repo. It captures
> the prior conversation's outcomes so a new agent doesn't have to re-derive
> them. Companion files:
> - `AGENTS.md` — full project spec / plan (long; current status in §1.5)
> - `PAPER_AUDIT.md` — paper + patent vs implementation audit
> - `p644-siddiqui-paper.pdf` — the paper
> - `patent-application.pdf` — US 12,380,098 B2 (more detailed than the paper)

---

## 1. The project in one paragraph

This is a C++17 implementation of **Zippy**, a cache-conscious top-k aggregation
algorithm by Siddiqui et al. (PVLDB 17(4), 2023, also patented as US 12,380,098 B2).
The repo is being built as a final-year project that will eventually add two
*extensions* the paper proposes as future work: stratified sampling via a group
occurrence index (Ext A) and a measure-column index for extreme values (Ext B).
Before working on extensions, the user wanted the baseline to be a faithful
implementation of the paper. The session below brought it to that state.

---

## 2. What happened in the prior session

1. **User asked "is this implemented properly?"** with the paper and patent in
   the repo. I had not previously read the paper.
2. **I audited paper + patent vs. `src/`** and produced `PAPER_AUDIT.md`.
   Findings: the skeleton was right but ~7 substantive deviations from the paper
   — a stubbed `// TODO` in the sampler that bypassed Hoeffding entirely, SUM-only
   (no MAX/MIN/COUNT), no locality test, no logical/physical decision, FA-only
   pass-1 `topKBound`, no skew validation, heavy-hitter top-up keyed on the wrong
   metric. Plus parallelism (deferred) and Extensions A/B (deferred).
3. **User said "implement everything except parallelism and the extensions".**
   I did. See §3.
4. **User asked "did you actually validate it?"** I had only smoke-tested.
   Then I ran the full test gates and edge-case sweep — see §4.
5. **User asked about extensive validation, dataset sizes, and GPUs.** I gave a
   sizing analysis (see §5) and proposed a 3-tier test plan; they then asked
   for documentation rather than running it.

---

## 3. What's in `src/` now

| File | What changed |
|---|---|
| `src/data_structures.h` | Added `AggFunc {SUM,COUNT,MAX,MIN}` enum, `parse_agg_func`, `agg_func_name`. `FAEntry` widened from 16→32 bytes; tracks all four aggregates simultaneously. `FATable::update` updates all four; `get(gid, AggFunc)` and `top_k(k, AggFunc)` overloads. `CAPartition::upper_bound(AggFunc)` returns `total_sum` for SUM/AVG, `count` for COUNT, `max_value` for MAX/MIN. `CATable::prune(topKBound, AggFunc)`. `FATable::lowest_count()` for T_c. |
| `src/sampler.{h,cpp}` | Real Hoeffding LB via `hoeffding_eps_per_tuple` and `aggregate_lower_bound`. Computes L_k = K-th highest LB. `temp_groups = {g : LB(g) ≥ L_k}`. **`is_optimizable = false` if `\|temp_groups\| > C_f`** (patent claim 7 / 17). Heavy-hitter top-up sorted by sample count. Secondary skew gate: top-K share of total < 1% → fallback. `SampleResult` exposes `l_k_lower_bound` and `cs_above_lk`. **Signature changed**: now takes `int k, AggFunc agg_func` as positional args 3 and 4. |
| `src/zippy.{h,cpp}` | `ZippyConfig` got `agg_func`, `segment_size` (default 100k), `alpha_locality` (default 0.20). New `ExactAccum` tracks all four aggregates. `merge_and_prune` uses `partition.upper_bound(agg)` for UBs. **Pass 1 `topKBound` is K-th highest of `{FA exact values} ∪ {partition UBs}`** — was FA-only. New `classify_partition()` returns `EXACT` (FM<C_f or `d/count<α₀`), `LOGICAL` (`C_p/Q < T_c`, patent claim 4 / 14), or `PHYSICAL`. Counts surfaced in `RunMetrics`. `run_brute_force(dataset, k, AggFunc)` aggregates all four metrics, returns top-K by chosen agg. |
| `src/utils.h` | `RunMetrics` extended: `l_k_lower_bound`, `cs_above_lk`, `partitions_exact_agg`, `partitions_logical`, `partitions_physical`. JSON writer outputs all of them. |
| `src/main.cpp` | `--agg sum\|count\|max\|min`, `--alpha-locality`, `--segment-size`. `ext-a/b/ab` modes still return errors. |
| `src/test_phase4b.cpp` | Updated to use the new union-UB pass-1 `topKBound` (the old test was anchored to the FA-only formula and would otherwise fail). |
| `src/test_sampler.cpp` | Updated to pass `k` and `AggFunc::SUM` to the new `uniform_sample_and_select` signature. |

**Files left untouched**: `group_index.{h,cpp}`, `measure_index.{h,cpp}`,
`stratified_sampler.{h,cpp}` are unchanged stubs for the deferred extensions.
The Python utilities (`python/generate_data.py`, `python/compare_phase4c_results.py`)
are unchanged.

---

## 4. Validation actually performed

```bash
# Build (clean, 0 warnings)
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -o build/zippy \
    src/main.cpp src/zippy.cpp src/sampler.cpp \
    src/group_index.cpp src/stratified_sampler.cpp src/measure_index.cpp -Isrc/

# All four test gates: PASS
./build/test_ds         # 1028/1028 passed
./build/test_sampler    # PASS
./build/test_phase4b    # PASS (after updating to new union-UB rule)
./build/test_phase4c --input data/S0.bin --n-rows 10091 --k 10  # PASS

# All four aggregates: top_k_results identical to brute-force on S0
for AGG in sum count max min; do …; done   # all 4 ok

# Edge cases: k=1, k=500, varying --delta, --sample-frac, --n-partitions: all match
# Forced-fallback (--fa-capacity 5): is_optimizable=false, fall back to brute-force, top-K still matches
```

**One known artefact** found during validation: with `--n-partitions 500` on S0
(which has 6 groups tied at exactly 30000), baseline returns a *valid* top-10
(same aggregate-value multiset) but picks a different subset of the 6-way tie
than brute-force. This is the new, paper-correct pass-1 union UB pruning being
strictly more aggressive than the old FA-only rule; it's safe-by-construction
(Algorithm 4's pruning is sound) but tie-breaking diverges from brute-force's
"smallest gid" convention. Documented in `PAPER_AUDIT.md`'s "Caveats".

**Not yet run**: Tier 2 (50M rows on this laptop) and Tier 3 (200M rows on a
VM). Plan was sketched; user opted to document instead of run.

---

## 5. The "should we use a GPU?" answer (for the record)

User asked. **No, GPUs are not the right tool here**:

1. The hot path is hash-table probes and irregular memory access. GPUs win on
   regular dense compute (matmul, reductions over arrays). Hash tables on GPU
   exist but rarely beat well-tuned CPU code.
2. The paper's whole premise is **L1/L2 CPU cache efficiency**. GPU SM caches
   are tens of KB per SM — wrong scale. Section 3 of the paper explicitly
   targets multi-core CPUs.
3. PCIe transfer eats the gain at this dataset size.
4. For *validation* specifically, GPU adds zero — we're checking correctness
   against brute-force, not chasing throughput.

**The real speedup path**, if needed: implement multi-threading (audit deviation
#4, currently deferred). The paper benchmarks ~7× at 64 cores. This also
satisfies patent claim 1's "each core" wording.

**Sizing on this laptop**: 7.6 GB RAM (1.3 GB available), 8 logical cores.
Comfortably handles up to ~50M rows. For paper-scale (200–400M rows), need a VM
with ≥16–32 GB RAM. Single-threaded estimates: 50M rows / full test matrix ≈
30–60 min; 200M rows / matrix ≈ 2–4 hours.

---

## 6. Where things stand and what's next

**Done**:
- Audit items 1, 2, 3 (decision logic), 5, 6, 7 from `PAPER_AUDIT.md` are closed.
- All four aggregates work end-to-end.
- All test gates pass.
- Documentation: `PAPER_AUDIT.md`, `AGENTS.md` §1.5, this file.

**Deliberately deferred** (user decision):
- **Item 4: parallelism**. Single-threaded prototype is correct; speedup
  requires this. Without it, LOGICAL vs PHYSICAL classification has no
  execution effect (decisions tracked in metrics only).
- **Item 8: Extensions A & B**. The reason the project exists. Stubs are in
  place under `src/{group_index,stratified_sampler,measure_index}.{h,cpp}`.

**Likely next steps** (priority order):
1. Run the proposed Tier 1/2 validation matrix to produce thesis throughput
   numbers — see Q1 below.
2. Implement Extension A (stratified sampling). The interface is sketched in
   `AGENTS.md` §4 and the stub files exist.
3. Implement Extension B (measure-column min-heap). Similarly stubbed.
4. Combined mode `ext-ab`.
5. (Only if needed for thesis) multi-threading.

**Open questions for the user**:
- Q1: Run validation on this laptop (Tier 1+2, ~45 min) or prep a script for
  their VM (Tier 3, paper-scale)?
- Q2: Do they want multi-threading before running Tier 3 (~1–2 h impl, then
  Tier 3 finishes in ~20–30 min instead of 2–4 h)?
- Q3: Start Extension A or finish validation first?

---

## 7. How to brief a fresh Claude

Paste this whole file, then say: *"Read SESSION_CONTEXT.md and PAPER_AUDIT.md.
Then read AGENTS.md §1.5 for the most recent state. The paper is in
`p644-siddiqui-paper.pdf`, the patent is in `patent-application.pdf` — these
have been audited and the implementation is paper-faithful for the items listed
under §3 above. Confirm what you see in `src/` matches the description before
making any changes."*

A few gotchas a fresh agent should know:

- The `python/generate_data.py` defaults inject rare high-value groups (rare
  groups get a 100× value multiplier across 3 rows = exact 30000 sum). On S0
  with 10K rows / 500 groups / `--rare-group-fraction 0.1`, the top-10 has 6
  groups tied at exactly 30000 — this is intentional (S0 is meant to stress
  Extensions A/B) but causes the tie-breaking artefact under aggressive
  partitioning. Use `--rare-group-fraction 0` for clean validation.
- The default sample size formula `s ≥ Z²/(4Δ²)` with α=0.05, Δ=0.05 gives ~384
  samples regardless of N. For very large N you may want `--sample-frac 0.001`
  or smaller `--delta` to scale up.
- `FAEntry` is now 32 bytes (was 16). 2 entries per cache line instead of 4.
  Acceptable trade-off for prototype to support all four aggregates without
  templating; could be revisited.
- `LOGICAL` and `PHYSICAL` classifications run identical execution paths in
  this build. The metric counters are real but the speedup the paper reports
  for physical partitioning requires parallelism + per-core caches, which we
  don't have.
- The paper's §4.2.1 specifies β/2-percentile CIs for MAX/MIN; we use sample
  MIN as a heuristic (technically an upper bound on group MIN, not lower).
  Top-K-by-MIN ranking is approximate.
