#include "sampler.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace
{

  // Acklam approximation for inverse CDF of the standard normal distribution.
  double inverse_standard_normal_cdf(double p)
  {
    static constexpr double a1 = -3.969683028665376e+01;
    static constexpr double a2 = 2.209460984245205e+02;
    static constexpr double a3 = -2.759285104469687e+02;
    static constexpr double a4 = 1.383577518672690e+02;
    static constexpr double a5 = -3.066479806614716e+01;
    static constexpr double a6 = 2.506628277459239e+00;

    static constexpr double b1 = -5.447609879822406e+01;
    static constexpr double b2 = 1.615858368580409e+02;
    static constexpr double b3 = -1.556989798598866e+02;
    static constexpr double b4 = 6.680131188771972e+01;
    static constexpr double b5 = -1.328068155288572e+01;

    static constexpr double c1 = -7.784894002430293e-03;
    static constexpr double c2 = -3.223964580411365e-01;
    static constexpr double c3 = -2.400758277161838e+00;
    static constexpr double c4 = -2.549732539343734e+00;
    static constexpr double c5 = 4.374664141464968e+00;
    static constexpr double c6 = 2.938163982698783e+00;

    static constexpr double d1 = 7.784695709041462e-03;
    static constexpr double d2 = 3.224671290700398e-01;
    static constexpr double d3 = 2.445134137142996e+00;
    static constexpr double d4 = 3.754408661907416e+00;

    static constexpr double p_low = 0.02425;
    static constexpr double p_high = 1.0 - p_low;

    if (p <= 0.0)
      return -INFINITY;
    if (p >= 1.0)
      return INFINITY;

    if (p < p_low)
    {
      const double q = std::sqrt(-2.0 * std::log(p));
      return (((((c1 * q + c2) * q + c3) * q + c4) * q + c5) * q + c6) /
             ((((d1 * q + d2) * q + d3) * q + d4) * q + 1.0);
    }
    if (p <= p_high)
    {
      const double q = p - 0.5;
      const double r = q * q;
      return (((((a1 * r + a2) * r + a3) * r + a4) * r + a5) * r + a6) * q /
             (((((b1 * r + b2) * r + b3) * r + b4) * r + b5) * r + 1.0);
    }

    const double q = std::sqrt(-2.0 * std::log(1.0 - p));
    return -(((((c1 * q + c2) * q + c3) * q + c4) * q + c5) * q + c6) /
           ((((d1 * q + d2) * q + d3) * q + d4) * q + 1.0);
  }

  double z_alpha_over_two(double alpha_ci)
  {
    const double alpha =
        (alpha_ci > 0.0 && alpha_ci < 1.0) ? alpha_ci : 0.05; // default fallback
    const double p = 1.0 - (alpha / 2.0);
    return inverse_standard_normal_cdf(p);
  }

} // namespace

// Hoeffding half-width on the per-tuple mean of values for one group:
//   ε = (b - a) · sqrt( ln(2/(1-β)) / (2 n_i') )
// (paper §4.2.1, patent description col. 9). For SUM aggregate, the LB on
// the group's sum-in-sample is sample_sum - ε · n_i'.
static double hoeffding_eps_per_tuple(double a, double b, double beta_ci, double n_i_prime)
{
  if (n_i_prime <= 0.0) return 0.0;
  const double safe_beta = (beta_ci > 0.0 && beta_ci < 1.0) ? beta_ci : 0.95;
  const double range     = (b > a) ? (b - a) : 0.0;
  if (range == 0.0) return 0.0;
  const double ln_term = std::log(2.0 / (1.0 - safe_beta));
  return range * std::sqrt(ln_term / (2.0 * n_i_prime));
}

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
    uint64_t seed)
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
  
  // 1. DEFINE the uniform distribution to pick random array indices
  std::uniform_int_distribution<size_t> dist(0, n_rows - 1);

  result.sample_stats.reserve(std::min(sample_size_target * 2, n_rows));

  // 2. Loop EXACTLY sample_size_target times
  for (size_t i = 0; i < sample_size_target; ++i) {
    // 3. O(1) random access
    const auto& row = dataset[dist(rng)];

    // 4. NO COIN FLIP! Update stats directly.
    auto &stats = result.sample_stats[row.group_id];
    stats.sum += row.value;
    stats.count += 1.0;
    if (row.value < stats.min_val)
      stats.min_val = row.value;
    if (row.value > stats.max_val)
      stats.max_val = row.value;
      
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
  if (agg_func == AggFunc::SUM || agg_func == AggFunc::COUNT)
  {
    double total_agg = 0.0;
    std::vector<double> per_group_agg;
    per_group_agg.reserve(result.sample_stats.size());
    for (const auto &[gid, stats] : result.sample_stats)
    {
      (void)gid;
      const double v = (agg_func == AggFunc::SUM) ? stats.sum : stats.count;
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

  // FAgroups = tempGroups (Algorithm 2 line 33).
  for (const auto& c : temp_groups) result.fa_groups.insert(c.group_id);

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
