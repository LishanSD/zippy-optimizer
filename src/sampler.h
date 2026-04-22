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
};

// Result of Algorithm 2 (simplified): sampling + FA candidate selection.
struct SampleResult {
    bool is_optimizable = true;
    std::unordered_set<uint64_t> fa_groups;
    std::unordered_map<uint64_t, SampleGroupStats> sample_stats;
    size_t sample_size_actual = 0;
};

// Uniform random sampling and FA candidate selection.
// Simplification for the prototype: use point estimates (epsilon = 0) instead of
// full Hoeffding confidence intervals, then select top fa_capacity by sample sum.
SampleResult uniform_sample_and_select(
    const std::vector<Row>& dataset,
    size_t fa_capacity,
    double sample_frac,
    double delta,
    double alpha_ci,
    double beta_ci,
    uint64_t seed = 42);
