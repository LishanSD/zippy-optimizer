#pragma once
// stratified_sampler.h — Two-phase stratified sampler for Extension A.
//
// Algorithm (AGENTS.md §4 "Extension A"):
//
// Also used by Extension AB (combined): when pre_injected_groups is non-empty,
// those groups are inserted into FA first (Extension B forced candidates), and
// the remaining FA slots are filled via stratified sampling (Extension A).
//
//   Phase 1 (uniform):
//     Bernoulli-sample dataset at fraction s1_fraction.
//     Accumulate per-group {sum, count, min, max} → sample_stats.
//
//   Phase 2 (stratified correction):
//     For each group in GroupOccurrenceIndex:
//       If group is underrepresented in Phase 1 sample:
//         Fetch up to BOOST_ROWS additional rows from the index.
//         Add their values to sample_stats for that group.
//
//   Candidate selection:
//     Merge Phase 1 + Phase 2 aggregates.
//     Apply same Hoeffding LB / L_k logic as uniform_sample_and_select().
//     Return SampleResult with fa_groups, sample_stats, is_optimizable.
//
// The rest of the Zippy pipeline (Pass 1 routing, MergeAndPrune, multi-pass
// loop) is **identical** to the baseline — only the sampling phase changes.

#include "sampler.h"
#include "group_index.h"

// Perform two-phase stratified sampling and FA candidate selection.
//
// Parameters (all shared with uniform_sample_and_select unless noted):
//   dataset        — full in-memory dataset
//   group_index    — pre-built GroupOccurrenceIndex (one scan)
//   fa_capacity    — max FA slots (Cf)
//   k              — top-K parameter (for L_k computation)
//   agg_func       — aggregate function
//   sample_frac    — Phase 1 uniform fraction (default 0.01)
//   delta          — sampling tolerance Δ (for sample-size formula)
//   alpha_ci       — CI confidence level (for sample-size formula)
//   beta_ci        — Hoeffding CI confidence (for lower-bound computation)
//   underrep_threshold — fraction below expected that triggers a boost (e.g. 0.5)
//   boost_rows     — max additional rows to fetch per underrepresented group
//   seed           — RNG seed for reproducibility
//
// Returns:
//   SampleResult with the same fields as uniform_sample_and_select(), plus
//   sample_stats updated to include Phase 2 boost rows.
SampleResult stratified_sample_and_select(
    const std::vector<Row>&                   dataset,
    const GroupOccurrenceIndex&               group_index,
    size_t                                    fa_capacity,
    int                                       k,
    AggFunc                                   agg_func,
    double                                    sample_frac,
    double                                    delta,
    double                                    alpha_ci,
    double                                    beta_ci,
    double                                    underrep_threshold,
    size_t                                    boost_rows,
    uint64_t                                  seed = 42,
    const std::unordered_set<uint64_t>&       pre_injected_groups = {});
