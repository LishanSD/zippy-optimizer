#include "sampler.h"
#include "math_utils.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

using zippy_math::z_alpha_over_two;
using zippy_math::hoeffding_eps_per_tuple;

// LB on the aggregate VALUE of a group within the sample, per `f`.
// Used as the comparator for L_k (paper Algorithm 2 line 19, patent claim 7).
static double aggregate_lower_bound(const SampleGroupStats& s, AggFunc f, double beta_ci)
{
  switch (f) {
    case AggFunc::SUM: {
      const double eps = hoeffding_eps_per_tuple(s.min_val, s.max_val, beta_ci, s.count);
      return s.sum - eps * s.count;
    }
    case AggFunc::COUNT:
      // Sample count is exact for the sample population — point estimate is its
      // own (trivial) lower bound; rigorous extrapolation requires N_i which we
      // do not know.  Order-preserving for top-K selection.
      return s.count;
    case AggFunc::MAX:
      // Sample max ≤ true max ⇒ sample max IS a valid lower bound on group MAX.
      return s.max_val;
    case AggFunc::MIN:
      // Sample min ≥ true min — strictly an upper bound. Used as a heuristic
      // ranking signal for top-K-by-MIN; full β/2-percentile CI (paper §4.2.1)
      // is left as future work.
      return s.min_val;
  }
  return 0.0;
}

SampleResult uniform_sample_and_select(
    const std::vector<Row> &dataset,
    size_t fa_capacity,
    int    k,
    AggFunc agg_func,
    double sample_frac,
    double delta,
    double alpha_ci,
    double beta_ci,
    uint64_t seed,
    const std::unordered_set<uint64_t>& pre_injected_groups
  )
{
  SampleResult result;
  if (dataset.empty() || fa_capacity == 0 || k <= 0)
  {
    result.is_optimizable = false;
    return result;
  }

  const size_t n_rows = dataset.size();
  const double safe_delta = (delta > 0.0) ? delta : 0.05;
  const double safe_frac = (sample_frac > 0.0) ? sample_frac : 0.0;

  const double z = z_alpha_over_two(alpha_ci);
  const double formula_sample =
      std::ceil((z * z) / (4.0 * safe_delta * safe_delta));
  const double frac_sample = std::ceil(safe_frac * static_cast<double>(n_rows));

  size_t sample_size_target =
      static_cast<size_t>(std::max(1.0, std::max(formula_sample, frac_sample)));
  if (sample_size_target > n_rows)
    sample_size_target = n_rows;

  std::mt19937_64 rng(seed);

  // Bernoulli sampling: each row is independently included with probability p,
  // matching Algorithm 2 and patent col. 9. This ensures n_i' counts distinct
  // rows sampled per group, which the Hoeffding CI formula requires.
  const double p_row = static_cast<double>(sample_size_target)
                     / static_cast<double>(n_rows);
  std::bernoulli_distribution coin(p_row);

  result.sample_stats.reserve(std::min(sample_size_target * 2, n_rows));

  for (const auto& row : dataset) {
    if (!coin(rng)) continue;
    auto& stats = result.sample_stats[row.group_id];
    stats.sum   += row.value;
    stats.count += 1.0;
    if (row.value < stats.min_val) stats.min_val = row.value;
    if (row.value > stats.max_val) stats.max_val = row.value;
    ++result.sample_size_actual;
  }

  if (result.sample_stats.empty())
  {
    result.is_optimizable = false;
    return result;
  }

  // ── Algorithm 2 lines 17–20: compute lower bounds and L_k ────────────────
  struct Candidate
  {
    uint64_t group_id;
    double   lb;       // aggregate-value lower bound
    double   count;    // sample count (heavy-hitter tiebreaker)
  };

  std::vector<Candidate> candidates;
  candidates.reserve(result.sample_stats.size());
  for (const auto &[gid, stats] : result.sample_stats)
  {
    candidates.push_back(Candidate{
        gid,
        aggregate_lower_bound(stats, agg_func, beta_ci),
        stats.count});
  }

  // L_k = K-th highest lower bound.
  const size_t k_size = static_cast<size_t>(k);
  double l_k = 0.0;
  if (candidates.size() >= k_size && k_size > 0)
  {
    std::vector<double> lbs;
    lbs.reserve(candidates.size());
    for (const auto& c : candidates) lbs.push_back(c.lb);
    std::nth_element(lbs.begin(), lbs.begin() + (k_size - 1), lbs.end(),
                     std::greater<double>());
    l_k = lbs[k_size - 1];
  }
  result.l_k_lower_bound = l_k;

  // tempGroups = {g : LB(g) ≥ L_k}.  Algorithm 2 line 22.
  std::vector<Candidate> temp_groups;
  temp_groups.reserve(candidates.size());
  for (const auto& c : candidates)
  {
    if (c.lb >= l_k) temp_groups.push_back(c);
  }
  result.cs_above_lk = temp_groups.size();

  // Algorithm 2 lines 23–26 / patent col. 10: if C_s > C_f, the sample skew is
  // insufficient — skip the optimization entirely.
  if (temp_groups.size() > fa_capacity)
  {
    result.is_optimizable = false;
    return result;
  }

  // ── Skew validation gate (claim 2 / 12) ───────────────────────────────────
  // Independent signal: if the top-K sample aggregate accounts for a vanishing
  // share of total sample aggregate, the distribution is not skewed enough for
  // top-K optimization to be worthwhile.
  {
    double total_agg = 0.0;
    std::vector<double> per_group_agg;
    per_group_agg.reserve(result.sample_stats.size());
    for (const auto &[gid, stats] : result.sample_stats)
    {
      (void)gid;
      const double v = std::abs(aggregate_lower_bound(stats, agg_func, beta_ci));
      per_group_agg.push_back(v);
      total_agg += v;
    }
    if (total_agg > 0.0 && !per_group_agg.empty())
    {
      const size_t take = std::min(k_size, per_group_agg.size());
      std::nth_element(per_group_agg.begin(),
                       per_group_agg.begin() + take,
                       per_group_agg.end(),
                       std::greater<double>());
      double top_k_share = 0.0;
      for (size_t i = 0; i < take; ++i) top_k_share += per_group_agg[i];
      // If top-K aggregates contribute less than 1% of total, distribution is
      // ~uniform — fall back.
      if (top_k_share / total_agg < 0.01)
      {
        result.is_optimizable = false;
        return result;
      }
    }
  }

  // --- Extension B Injection ---
  // VIP access: Force-inject the extreme groups first.
  for (uint64_t gid : pre_injected_groups) {
      if (result.fa_groups.size() >= fa_capacity) break;
      result.fa_groups.insert(gid);
  }

  // FAgroups = tempGroups (Algorithm 2 line 33), up to capacity.
  for (const auto& c : temp_groups) {
      if (result.fa_groups.size() >= fa_capacity) break;
      result.fa_groups.insert(c.group_id);
  }

  // Algorithm 2 lines 30–33 / patent col. 10: top up remaining FA slots with
  // heavy hitters (highest sample count) so C_s + C_h ≈ C_f.
  if (result.fa_groups.size() < fa_capacity)
  {
    std::vector<Candidate> sorted_by_count = candidates;
    std::sort(sorted_by_count.begin(), sorted_by_count.end(),
              [](const Candidate &a, const Candidate &b)
              {
                if (a.count != b.count) return a.count > b.count;
                if (a.lb    != b.lb)    return a.lb    > b.lb;
                return a.group_id < b.group_id;
              });

    for (const auto &c : sorted_by_count)
    {
      if (result.fa_groups.size() >= fa_capacity) break;
      result.fa_groups.insert(c.group_id);
    }
  }

  return result;
}
