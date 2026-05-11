#pragma once
// zippy.h — Public interface of the Zippy top-k engine.
//
// Phase 4C: brute-force + baseline (sampling + pass1 pruning + multi-pass) implemented.
// Phase 5:  Extension A (stratified sampling via GroupOccurrenceIndex) implemented.
// Phase 6:  Extension B (measure column index) implemented.
// Phase 7:  Combined extension (ext-ab) implemented.

#include "data_structures.h"
#include "utils.h"
#include <vector>
#include <string>
#include <unordered_set>

struct ZippyConfig {
    size_t fa_capacity      = 50000;   // groups FA can hold
    size_t n_partitions     = 10000;   // CA logical partition count (= Q in patent)
    double sample_frac      = 0.01;    // uniform sample fraction
    double delta            = 0.05;    // sampling tolerance Δ
    double alpha_ci         = 0.05;    // CI confidence for sample size
    double beta_ci          = 0.95;    // Hoeffding CI confidence
    // Aggregate function (paper §2: monotonic SUM / COUNT / MAX / MIN, Y ≥ 0)
    AggFunc agg_func          = AggFunc::SUM;
    // Adaptive partitioning constants (patent description col. 11; defaults
    // s = 100k, α₀ = 0.20 from the patent's benchmarking).
    size_t  segment_size      = 100000;
    double  alpha_locality    = 0.20;
    // Extension A
    double underrep_threshold = 0.5;
    size_t boost_rows         = 10;
    // Extension B
    size_t measure_m          = 500;
    // Output
    bool   output_fa_groups   = false;
    bool   verbose            = false;
};

// Brute-force reference (always correct, used for verification).
// Single pass with multi-aggregate accumulation; selects top-k by `agg_func`.
std::vector<std::pair<uint64_t,double>> run_brute_force(
    const std::vector<Row>& dataset, int k, AggFunc agg_func = AggFunc::SUM);

// Baseline Zippy: Algorithm 2 sampling + Pass 1 routing/pruning + Phase 4C multi-pass loop.
RunMetrics run_zippy_baseline(
    const std::vector<Row>& dataset,
    int k,
    const ZippyConfig& cfg,
    std::vector<std::pair<uint64_t,double>>& out_results,
    std::vector<uint64_t>& out_fa_groups);

// Extension A (Phase 5): stratified sampling via GroupOccurrenceIndex.
// Builds the index in one pre-pass, then uses two-phase sampling before
// running the same baseline Zippy pipeline (Pass 1 → MergeAndPrune → loop).
RunMetrics run_zippy_ext_a(
    const std::vector<Row>& dataset,
    int k,
    const ZippyConfig& cfg,
    std::vector<std::pair<uint64_t,double>>& out_results,
    std::vector<uint64_t>& out_fa_groups);

// Extension B (Phase 6): measure column index — finds extreme-value rows via
// a min-heap pre-pass and force-injects their group IDs into FA before sampling.
RunMetrics run_zippy_ext_b(
    const std::vector<Row>& dataset,
    int k,
    const ZippyConfig& cfg,
    std::vector<std::pair<uint64_t,double>>& out_results,
    std::vector<uint64_t>& out_fa_groups);

// Combined Extensions A + B (Phase 7): builds both indices in one shared
// pre-pass, then force-injects extreme-value groups (Ext B) first and fills
// remaining FA slots via stratified sampling (Ext A).
RunMetrics run_zippy_ext_ab(
    const std::vector<Row>& dataset,
    int k,
    const ZippyConfig& cfg,
    std::vector<std::pair<uint64_t,double>>& out_results,
    std::vector<uint64_t>& out_fa_groups);
