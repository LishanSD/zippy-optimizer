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

  // Hoeffding CI half-width for a group's estimated full-dataset sum.
  // Patent col. 9: ε_g = (b-a) * sqrt(ln(2/(1-β)) / (2 * n_g))
  // n_g = sample count for this group; b-a = global value range in sample; β = CI confidence.
  double hoeffding_epsilon(double range, double n_g, double beta)
  {
    if (n_g <= 0.0 || range <= 0.0 || beta <= 0.0 || beta >= 1.0)
      return std::numeric_limits<double>::max();
    return range * std::sqrt(std::log(2.0 / (1.0 - beta)) / (2.0 * n_g));
  }

  // k-th highest value in v; returns lowest() when fewer than k values present.
  double kth_highest(std::vector<double>& v, size_t k)
  {
    if (k == 0 || v.size() < k) return std::numeric_limits<double>::lowest();
    std::nth_element(v.begin(), v.begin() + (k - 1), v.end(), std::greater<double>());
    return v[k - 1];
  }

} // namespace

SampleResult uniform_sample_and_select(
    const std::vector<Row> &dataset,
    size_t fa_capacity,
    double sample_frac,
    double delta,
    double alpha_ci,
    double beta_ci,
    uint64_t seed,
    size_t k)
{
  SampleResult result;
  if (dataset.empty() || fa_capacity == 0)
  {
    result.is_optimizable = false;
    return result;
  }

  const size_t n_rows = dataset.size();
  const double safe_delta = (delta > 0.0) ? delta : 0.05;
  const double safe_frac = (sample_frac > 0.0) ? sample_frac : 0.0;
  const double safe_beta = (beta_ci > 0.0 && beta_ci < 1.0) ? beta_ci : 0.95;

  // Sample size: max of formula s ≥ Z²/(4Δ²) and fraction-based floor.
  const double z = z_alpha_over_two(alpha_ci);
  const double formula_sample =
      std::ceil((z * z) / (4.0 * safe_delta * safe_delta));
  const double frac_sample = std::ceil(safe_frac * static_cast<double>(n_rows));

  size_t sample_size_target =
      static_cast<size_t>(std::max(1.0, std::max(formula_sample, frac_sample)));
  if (sample_size_target > n_rows)
    sample_size_target = n_rows;

  const double p = static_cast<double>(sample_size_target) / static_cast<double>(n_rows);

  std::mt19937_64 rng(seed);
  std::bernoulli_distribution sel(p);

  result.sample_stats.reserve(std::min(sample_size_target * 2, n_rows));

  // Track global value range for Hoeffding epsilon computation.
  double global_min_val = std::numeric_limits<double>::max();
  double global_max_val = std::numeric_limits<double>::lowest();

  for (const auto &row : dataset)
  {
    if (!sel(rng))
      continue;

    auto &stats = result.sample_stats[row.group_id];
    stats.sum += row.value;
    stats.count += 1.0;
    if (row.value < stats.min_val) stats.min_val = row.value;
    if (row.value > stats.max_val) stats.max_val = row.value;
    if (row.value < global_min_val) global_min_val = row.value;
    if (row.value > global_max_val) global_max_val = row.value;
    ++result.sample_size_actual;
  }

  if (result.sample_stats.empty())
  {
    result.is_optimizable = false;
    return result;
  }

  const double n_sample = static_cast<double>(result.sample_size_actual);
  const double N = static_cast<double>(n_rows);
  const double value_range = global_max_val - global_min_val;

  // Algorithm 2: compute Hoeffding lower bound for each group's full-dataset sum.
  // Patent col. 9: LB_g = (s_g / n_s) * N  -  ε_g
  //                ε_g = (b-a) * sqrt(ln(2/(1-β)) / (2 * n_g))
  struct Candidate
  {
    uint64_t group_id;
    double lb;    // Hoeffding lower bound
    double count; // sample count (for heavy-hitter top-up tiebreak)
  };

  std::vector<Candidate> candidates;
  candidates.reserve(result.sample_stats.size());
  for (auto &[gid, stats] : result.sample_stats)
  {
    const double eps = hoeffding_epsilon(value_range, stats.count, safe_beta);
    const double lb = (stats.sum / n_sample) * N - eps;
    stats.hoeffding_lb = lb;
    candidates.push_back(Candidate{gid, lb, stats.count});
  }

  // Sort by Hoeffding LB descending (primary), count descending (tiebreak).
  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate &a, const Candidate &b)
            {
              if (a.lb != b.lb) return a.lb > b.lb;
              if (a.count != b.count) return a.count > b.count;
              return a.group_id < b.group_id;
            });

  // Compute L_k = k-th highest Hoeffding LB (skew-validation gate, patent claim 7).
  // L_k is a threshold: only groups with LB > L_k are primary FA candidates.
  const size_t effective_k = (k == 0) ? fa_capacity : k;
  std::vector<double> lb_vals;
  lb_vals.reserve(candidates.size());
  for (const auto &c : candidates) lb_vals.push_back(c.lb);
  const double L_k = kth_highest(lb_vals, effective_k);

  // Select primary FA candidates: groups whose LB exceeds L_k.
  for (const auto &c : candidates)
  {
    if (result.fa_groups.size() >= fa_capacity) break;
    if (c.lb > L_k) result.fa_groups.insert(c.group_id);
  }

  // Skew-validation gate: C_s > C_f → distribution not skewed enough, fall back.
  if (result.fa_groups.size() > fa_capacity)
  {
    result.is_optimizable = false;
    result.fa_groups.clear();
    return result;
  }

  // Heavy-hitter top-up (patent col. 10): fill remaining FA slots with groups
  // sorted by Hoeffding LB (already in order), skipping already-selected groups.
  for (const auto &c : candidates)
  {
    if (result.fa_groups.size() >= fa_capacity) break;
    result.fa_groups.insert(c.group_id);
  }

  return result;
}
