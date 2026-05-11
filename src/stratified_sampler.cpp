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

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <unordered_set>
#include <vector>

namespace {

// Compute z_{α/2} using the Acklam approximation (copied from sampler.cpp
// to keep stratified_sampler self-contained; extract to a shared header if
// a third consumer appears).
double inverse_standard_normal_cdf_strat(double p) {
    static constexpr double a1 = -3.969683028665376e+01;
    static constexpr double a2 =  2.209460984245205e+02;
    static constexpr double a3 = -2.759285104469687e+02;
    static constexpr double a4 =  1.383577518672690e+02;
    static constexpr double a5 = -3.066479806614716e+01;
    static constexpr double a6 =  2.506628277459239e+00;

    static constexpr double b1 = -5.447609879822406e+01;
    static constexpr double b2 =  1.615858368580409e+02;
    static constexpr double b3 = -1.556989798598866e+02;
    static constexpr double b4 =  6.680131188771972e+01;
    static constexpr double b5 = -1.328068155288572e+01;

    static constexpr double c1 = -7.784894002430293e-03;
    static constexpr double c2 = -3.223964580411365e-01;
    static constexpr double c3 = -2.400758277161838e+00;
    static constexpr double c4 = -2.549732539343734e+00;
    static constexpr double c5 =  4.374664141464968e+00;
    static constexpr double c6 =  2.938163982698783e+00;

    static constexpr double d1 =  7.784695709041462e-03;
    static constexpr double d2 =  3.224671290700398e-01;
    static constexpr double d3 =  2.445134137142996e+00;
    static constexpr double d4 =  3.754408661907416e+00;

    static constexpr double p_low  = 0.02425;
    static constexpr double p_high = 1.0 - p_low;

    if (p <= 0.0) return -1.0 / 0.0;  // -inf
    if (p >= 1.0) return  1.0 / 0.0;  // +inf

    if (p < p_low) {
        const double q = std::sqrt(-2.0 * std::log(p));
        return (((((c1*q+c2)*q+c3)*q+c4)*q+c5)*q+c6) /
               ((((d1*q+d2)*q+d3)*q+d4)*q+1.0);
    }
    if (p <= p_high) {
        const double q = p - 0.5;
        const double r = q * q;
        return (((((a1*r+a2)*r+a3)*r+a4)*r+a5)*r+a6)*q /
               (((((b1*r+b2)*r+b3)*r+b4)*r+b5)*r+1.0);
    }
    const double q = std::sqrt(-2.0 * std::log(1.0 - p));
    return -(((((c1*q+c2)*q+c3)*q+c4)*q+c5)*q+c6) /
            ((((d1*q+d2)*q+d3)*q+d4)*q+1.0);
}

double z_alpha_over_two_strat(double alpha_ci) {
    const double alpha = (alpha_ci > 0.0 && alpha_ci < 1.0) ? alpha_ci : 0.05;
    return inverse_standard_normal_cdf_strat(1.0 - alpha / 2.0);
}

// Hoeffding half-width on the per-tuple mean for one group.
// Mirrors the implementation in sampler.cpp.
double hoeffding_eps_strat(double a, double b, double beta_ci, double n_i_prime) {
    if (n_i_prime <= 0.0) return 0.0;
    const double safe_beta = (beta_ci > 0.0 && beta_ci < 1.0) ? beta_ci : 0.95;
    const double range     = (b > a) ? (b - a) : 0.0;
    if (range == 0.0) return 0.0;
    const double ln_term = std::log(2.0 / (1.0 - safe_beta));
    return range * std::sqrt(ln_term / (2.0 * n_i_prime));
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
    uint64_t                                  seed,
    const std::unordered_set<uint64_t>&       pre_injected_groups)
{
    SampleResult result;

    if (dataset.empty() || fa_capacity == 0 || k <= 0) {
        result.is_optimizable = false;
        return result;
    }

    // ── Extension AB: pre-inject forced groups (Extension B) first ───────────
    // These occupy FA slots before stratified sampling can select candidates.
    // We track them so they are excluded from the heavy-hitter top-up below.
    for (uint64_t gid : pre_injected_groups) {
        if (result.fa_groups.size() >= fa_capacity) break;
        result.fa_groups.insert(gid);
    }
    // Remaining FA capacity after forced injection.
    const size_t remaining_capacity = (fa_capacity > result.fa_groups.size())
        ? fa_capacity - result.fa_groups.size() : 0;

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

    // tempGroups = {g : LB(g) >= L_k}, excluding already-injected groups
    std::vector<Candidate> temp_groups;
    temp_groups.reserve(candidates.size());
    for (const auto& c : candidates) {
        if (result.fa_groups.count(c.group_id)) continue;  // already injected
        if (c.lb >= l_k) temp_groups.push_back(c);
    }
    result.cs_above_lk = temp_groups.size();

    // Algorithm 2 line 19: if C_s > remaining FA capacity, the skew is insufficient
    // (checked against remaining_capacity so pre-injected groups are not counted).
    if (remaining_capacity == 0 || temp_groups.size() > remaining_capacity) {
        // If FA is completely consumed by forced groups, still mark as optimizable
        // (those forced groups will drive pruning). Only fall back if the
        // remaining sample-derived candidates also overflow the remaining space.
        if (remaining_capacity == 0) {
            // Nothing left for stratified candidates — forced groups fill FA.
            // Still optimizable if we had at least some groups pre-injected.
            if (result.fa_groups.empty()) {
                result.is_optimizable = false;
                return result;
            }
            // else: fa_groups already set; skip to return.
            return result;
        }
        result.is_optimizable = false;
        return result;
    }

    // Skew validation gate (same as in uniform sampler — claim 2 / 12):
    // if top-K aggregate share of total < 1% → fall back.
    if (agg_func == AggFunc::SUM || agg_func == AggFunc::COUNT) {
        double total_agg = 0.0;
        std::vector<double> per_group_agg;
        per_group_agg.reserve(result.sample_stats.size());
        for (const auto& [gid, stats] : result.sample_stats) {
            (void)gid;
            const double v = (agg_func == AggFunc::SUM) ? stats.sum : stats.count;
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

    // FAgroups += tempGroups (pre-injected groups were already inserted above)
    for (const auto& c : temp_groups) result.fa_groups.insert(c.group_id);

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
            result.fa_groups.insert(c.group_id);  // insert is a no-op if already present
        }
    }

    return result;
}
