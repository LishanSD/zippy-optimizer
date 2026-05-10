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

// Result of Algorithm 2: sampling + Hoeffding-based FA candidate selection.
struct SampleResult {
    bool is_optimizable = true;
    std::unordered_set<uint64_t> fa_groups;
    std::unordered_map<uint64_t, SampleGroupStats> sample_stats;
    size_t sample_size_actual = 0;
    double l_k_lower_bound = 0.0;     // K-th highest Hoeffding lower bound (Algorithm 2 line 19)
    size_t cs_above_lk     = 0;       // |{g : LB(g) ≥ L_k}|  — patent description col. 9-10
};

// Uniform random sampling + Algorithm 2 FA candidate selection.
//
// Steps (paper Algorithm 2 / patent claim 7):
//   1. Compute sample size s ≥ Z²_{α/2} / (4·Δ²).
//   2. Bernoulli-sample at p = s/N, accumulating per-group {sum, count, min, max}.
//   3. For each group with n_i' >= 1 in sample, compute Hoeffding lower bound on
//      aggregate value using ε = (b-a)·sqrt(ln(2/(1-β))/(2n_i')).
//   4. L_k = K-th highest LB across all sampled groups.
//   5. C_s = |{g : LB(g) ≥ L_k}|.  If C_s > C_f → not optimizable (skew gate).
//   6. Otherwise FA = those groups; top up remaining slots with heavy hitters
//      (highest sample count) so C_s + C_h ≈ C_f (patent col. 10).
//
// `k` is the user's TOP-K request (needed to compute L_k).  `agg_func` selects
// which aggregate the bounds are computed against.
SampleResult uniform_sample_and_select(
    const std::vector<Row>& dataset,
    size_t fa_capacity,
    int    k,
    AggFunc agg_func,
    double sample_frac,
    double delta,
    double alpha_ci,
    double beta_ci,
    uint64_t seed = 42);
