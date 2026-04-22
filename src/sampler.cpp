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

SampleResult uniform_sample_and_select(
    const std::vector<Row> &dataset,
    size_t fa_capacity,
    double sample_frac,
    double delta,
    double alpha_ci,
    double beta_ci,
    uint64_t seed)
{
  (void)beta_ci; // TODO: use beta_ci when full Hoeffding CI bounds are implemented.

  SampleResult result;
  if (dataset.empty() || fa_capacity == 0)
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

  const double p = static_cast<double>(sample_size_target) / static_cast<double>(n_rows);

  std::mt19937_64 rng(seed);
  std::bernoulli_distribution select(p);

  result.sample_stats.reserve(std::min(sample_size_target * 2, n_rows));

  for (const auto &row : dataset)
  {
    if (!select(rng))
      continue;

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

  struct Candidate
  {
    uint64_t group_id;
    double sum;
    double count;
  };

  std::vector<Candidate> candidates;
  candidates.reserve(result.sample_stats.size());
  for (const auto &[gid, stats] : result.sample_stats)
  {
    candidates.push_back(Candidate{gid, stats.sum, stats.count});
  }

  // TODO: replace point estimates with Hoeffding lower bounds for candidate selection.
  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate &a, const Candidate &b)
            {
              if (a.sum != b.sum)
                return a.sum > b.sum;
              if (a.count != b.count)
                return a.count > b.count;
              return a.group_id < b.group_id;
            });

  const size_t top_by_sum = std::min(fa_capacity, candidates.size());
  for (size_t i = 0; i < top_by_sum; ++i)
  {
    result.fa_groups.insert(candidates[i].group_id);
  }

  const size_t cf_bytes = fa_capacity * sizeof(FAEntry);
  const size_t cs_bytes = result.fa_groups.size() * sizeof(FAEntry);
  if (cs_bytes > cf_bytes)
  {
    result.is_optimizable = false;
    result.fa_groups.clear();
    return result;
  }

  if (result.fa_groups.size() < fa_capacity)
  {
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &a, const Candidate &b)
              {
                if (a.count != b.count)
                  return a.count > b.count;
                if (a.sum != b.sum)
                  return a.sum > b.sum;
                return a.group_id < b.group_id;
              });

    for (const auto &c : candidates)
    {
      if (result.fa_groups.size() >= fa_capacity)
        break;
      result.fa_groups.insert(c.group_id);
    }
  }

  return result;
}
