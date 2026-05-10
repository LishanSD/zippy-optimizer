#pragma once

#include "data_structures.h"

#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Per-group statistics accumulated during sampling.
struct SampleGroupStats {
    double sum = 0.0;
    double count = 0.0;
    double min_val = std::numeric_limits<double>::max();
    double max_val = std::numeric_limits<double>::lowest();
    double hoeffding_lb = 0.0;  // Hoeffding lower bound on full-dataset sum (computed after sampling)
};

// Result of Algorithm 2: sampling + FA candidate selection via Hoeffding CI bounds.
struct SampleResult {
    bool is_optimizable = true;
    std::unordered_set<uint64_t> fa_groups;
    std::unordered_map<uint64_t, SampleGroupStats> sample_stats;
    size_t sample_size_actual = 0;
};

// Uniform random sampling and FA candidate selection (Algorithm 2).
// Implements Hoeffding CI lower bounds for candidate selection and L_k gate.
// k: number of top-k groups requested (used for L_k threshold computation).
SampleResult uniform_sample_and_select(
    const std::vector<Row>& dataset,
    size_t fa_capacity,
    double sample_frac,
    double delta,
    double alpha_ci,
    double beta_ci,
    uint64_t seed = 42,
    size_t k = 0);
