// stratified_sampler.cpp — Two-phase stratified sampler (Extension A).
//
// Replaces Algorithm 2 lines 4–11 (uniform sample) with a two-phase procedure:
//
//   Phase 1: Bernoulli-sample at fraction s1_fraction → per-group stats.
//   Phase 2: For each group in GroupOccurrenceIndex, check underrepresentation.
//            If underrepresented, fetch up to BOOST_ROWS additional rows from
//            the index and add them to the sample aggregate.
//
// Lines 12–29 of Algorithm 2 (Hoeffding LB / L_k / heavy-hitter top-up) are
// applied identically to the merged Phase 1 + Phase 2 aggregates.
//
// Underrepresentation condition (AGENTS.md §4):
//   observed_count < UNDERREP_THRESHOLD × expected_count
//   where expected_count = (group_rows / N) × sample_size_phase1
//
// This ensures that groups below the Δ threshold — which are not guaranteed
// representation in the uniform sample — still get enough sample rows for
// meaningful Hoeffding CI computation.

#include "stratified_sampler.h"
#include "math_utils.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <unordered_set>
#include <vector>

using zippy_math::z_alpha_over_two;
using zippy_math::hoeffding_eps_per_tuple;

namespace {

// Local alias so existing call sites below don't need renaming.
inline double z_alpha_over_two_strat(double alpha_ci) {
    return z_alpha_over_two(alpha_ci);
}
inline double hoeffding_eps_strat(double a, double b, double beta_ci, double n) {
    return hoeffding_eps_per_tuple(a, b, beta_ci, n);
}

// Aggregate lower bound for one group in the sample.
double aggregate_lb_strat(const SampleGroupStats& s, AggFunc f, double beta_ci) {
    switch (f) {
        case AggFunc::SUM: {
            const double eps = hoeffding_eps_strat(s.min_val, s.max_val, beta_ci, s.count);
            return s.sum - eps * s.count;
        }
        case AggFunc::COUNT:
            return s.count;
        case AggFunc::MAX:
            return s.max_val;
        case AggFunc::MIN:
            return s.min_val;
    }
    return 0.0;
}

// Accumulate a single row's value into per-group stats.
inline void accumulate(SampleGroupStats& stats, double val) {
    stats.sum   += val;
    stats.count += 1.0;
    if (val < stats.min_val) stats.min_val = val;
    if (val > stats.max_val) stats.max_val = val;
}

}  // namespace

SampleResult stratified_sample_and_select(
    const std::vector<Row>&              dataset,
    const GroupOccurrenceIndex&          group_index,
    size_t                               fa_capacity,
    int                                  k,
    AggFunc                              agg_func,
    double                               sample_frac,
    double                               delta,
    double                               alpha_ci,
    double                               beta_ci,
    double                               underrep_threshold,
    size_t                               boost_rows,
    uint64_t                             seed,
    const std::unordered_set<uint64_t>&  pre_injected_groups)
{
    SampleResult result;

    if (dataset.empty() || fa_capacity == 0 || k <= 0) {
        result.is_optimizable = false;
        return result;
    }

    const size_t n_rows      = dataset.size();
    const double safe_delta  = (delta > 0.0) ? delta : 0.05;
    const double safe_frac   = (sample_frac > 0.0) ? sample_frac : 0.0;

    // ── Sample size (same formula as uniform_sample_and_select) ─────────────
    const double z = z_alpha_over_two_strat(alpha_ci);
    const double formula_sample =
        std::ceil((z * z) / (4.0 * safe_delta * safe_delta));
    const double frac_sample =
        std::ceil(safe_frac * static_cast<double>(n_rows));

    size_t sample_size_target =
        static_cast<size_t>(std::max(1.0, std::max(formula_sample, frac_sample)));
    if (sample_size_target > n_rows) sample_size_target = n_rows;

    const double p =
        static_cast<double>(sample_size_target) / static_cast<double>(n_rows);

    // ── Phase 1: Uniform Bernoulli sample ───────────────────────────────────
    std::mt19937_64 rng(seed);
    std::bernoulli_distribution select(p);

    result.sample_stats.reserve(std::min(sample_size_target * 2, n_rows));

    for (const auto& row : dataset) {
        if (!select(rng)) continue;
        accumulate(result.sample_stats[row.group_id], row.value);
        ++result.sample_size_actual;
    }

    if (result.sample_stats.empty()) {
        result.is_optimizable = false;
        return result;
    }

    // ── Phase 2: Stratified correction for underrepresented groups ──────────
    // For each group in the occurrence index, check if it is underrepresented
    // in the Phase 1 sample. If yes, fetch boost_rows additional rows from the
    // index and add them to sample_stats.
    //
    // This addresses the Δ-threshold limitation: groups with true proportion
    // below Δ are not guaranteed representation by the sample size formula
    // (AGENTS.md §3.5 / patent col. 12). The index gives us direct access to
    // their rows without requiring another full dataset scan.

    size_t n_groups_boosted = 0;

    for (const auto& [gid, positions] : group_index.index()) {
        // Observed count in Phase 1 sample.
        size_t observed = 0;
        {
            const auto it = result.sample_stats.find(gid);
            if (it != result.sample_stats.end()) {
                observed = static_cast<size_t>(it->second.count);
            }
        }

        if (!group_index.is_underrepresented(
                gid, observed, result.sample_size_actual, underrep_threshold)) {
            continue;
        }

        // Fetch up to boost_rows row positions from the index and incorporate
        // their values into the sample aggregate for this group.
        const std::vector<uint64_t> boost_positions =
            group_index.get_boost_rows(gid, boost_rows);

        if (boost_positions.empty()) continue;

        SampleGroupStats& stats = result.sample_stats[gid];
        for (uint64_t pos : boost_positions) {
            if (pos >= static_cast<uint64_t>(dataset.size())) continue;
            accumulate(stats, dataset[static_cast<size_t>(pos)].value);
        }
        ++n_groups_boosted;
    }

    (void)n_groups_boosted;  // available for debugging; suppress unused-variable warning

    // ── Algorithm 2 lines 15–22: Hoeffding LB + L_k + candidate selection ──
    // (Identical to uniform_sample_and_select from this point onward)

    struct Candidate {
        uint64_t group_id;
        double   lb;     // aggregate lower bound
        double   count;  // sample count (heavy-hitter tiebreaker)
    };

    std::vector<Candidate> candidates;
    candidates.reserve(result.sample_stats.size());
    for (const auto& [gid, stats] : result.sample_stats) {
        candidates.push_back(Candidate{
            gid,
            aggregate_lb_strat(stats, agg_func, beta_ci),
            stats.count});
    }

    // L_k = K-th highest lower bound.
    const size_t k_size = static_cast<size_t>(k);
    double l_k = 0.0;
    if (candidates.size() >= k_size && k_size > 0) {
        std::vector<double> lbs;
        lbs.reserve(candidates.size());
        for (const auto& c : candidates) lbs.push_back(c.lb);
        std::nth_element(lbs.begin(), lbs.begin() + (k_size - 1), lbs.end(),
                         std::greater<double>());
        l_k = lbs[k_size - 1];
    }
    result.l_k_lower_bound = l_k;

    // tempGroups = {g : LB(g) ≥ L_k}
    std::vector<Candidate> temp_groups;
    temp_groups.reserve(candidates.size());
    for (const auto& c : candidates) {
        if (c.lb >= l_k) temp_groups.push_back(c);
    }
    result.cs_above_lk = temp_groups.size();

    // Algorithm 2 line 19: if C_s > C_f, the skew is insufficient — fall back.
    if (temp_groups.size() > fa_capacity) {
        result.is_optimizable = false;
        return result;
    }

    // Skew validation gate (same as in uniform sampler — claim 2 / 12):
    // if top-K aggregate share of total < 1% → fall back. Applies to all
    // aggregate functions using absolute lower-bound values.
    {
        double total_agg = 0.0;
        std::vector<double> per_group_agg;
        per_group_agg.reserve(result.sample_stats.size());
        for (const auto& [gid, stats] : result.sample_stats) {
            (void)gid;
            const double v = std::abs(aggregate_lb_strat(stats, agg_func, beta_ci));
            per_group_agg.push_back(v);
            total_agg += v;
        }
        if (total_agg > 0.0 && !per_group_agg.empty()) {
            const size_t take = std::min(k_size, per_group_agg.size());
            std::nth_element(per_group_agg.begin(),
                             per_group_agg.begin() + take,
                             per_group_agg.end(),
                             std::greater<double>());
            double top_k_share = 0.0;
            for (size_t i = 0; i < take; ++i) top_k_share += per_group_agg[i];
            if (top_k_share / total_agg < 0.01) {
                result.is_optimizable = false;
                return result;
            }
        }
    }

    // Extension B injection: force-insert extreme groups before tempGroups.
    for (uint64_t gid : pre_injected_groups) {
        if (result.fa_groups.size() >= fa_capacity) break;
        result.fa_groups.insert(gid);
    }

    // FAgroups = tempGroups
    for (const auto& c : temp_groups) {
        if (result.fa_groups.size() >= fa_capacity) break;
        result.fa_groups.insert(c.group_id);
    }

    // Heavy-hitter top-up: fill remaining FA slots with highest-count groups
    // not already in fa_groups (Algorithm 2 lines 26–29 / patent col. 10).
    if (result.fa_groups.size() < fa_capacity) {
        std::vector<Candidate> sorted_by_count = candidates;
        std::sort(sorted_by_count.begin(), sorted_by_count.end(),
                  [](const Candidate& a, const Candidate& b) {
                      if (a.count != b.count) return a.count > b.count;
                      if (a.lb    != b.lb)    return a.lb    > b.lb;
                      return a.group_id < b.group_id;
                  });
        for (const auto& c : sorted_by_count) {
            if (result.fa_groups.size() >= fa_capacity) break;
            result.fa_groups.insert(c.group_id);
        }
    }

    return result;
}
