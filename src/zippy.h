#pragma once
// zippy.h — Public interface of the Zippy top-k engine.
//
// Phase 2: only run_brute_force() is implemented.
// Subsequent phases will add baseline, ext-a, ext-b, ext-ab.

#include "data_structures.h"
#include "utils.h"
#include <vector>
#include <string>
#include <unordered_set>

struct ZippyConfig {
    size_t fa_capacity      = 50000;   // groups FA can hold
    size_t n_partitions     = 10000;   // CA logical partition count
    double sample_frac      = 0.01;    // uniform sample fraction
    double delta            = 0.05;    // sampling tolerance Δ
    double alpha_ci         = 0.05;    // CI confidence for sample size
    double beta_ci          = 0.95;    // Hoeffding CI confidence
    // Extension A
    double underrep_threshold = 0.5;
    size_t boost_rows         = 10;
    // Extension B
    size_t measure_m          = 500;
    // Output
    bool   output_fa_groups   = false;
    bool   verbose            = false;
};

// Brute-force reference (always correct, used for verification)
// Single pass: unordered_map aggregate, then partial_sort to find top-k.
std::vector<std::pair<uint64_t,double>> run_brute_force(
    const std::vector<Row>& dataset, int k);

// ── Future phases will add these declarations ──────────────────────────────
// Phase 4: RunMetrics run_zippy_baseline(...)
// Phase 5: RunMetrics run_zippy_ext_a(...)
// Phase 6: RunMetrics run_zippy_ext_b(...)
// Phase 7: RunMetrics run_zippy_ext_ab(...)
