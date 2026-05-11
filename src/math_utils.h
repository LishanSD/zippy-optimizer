#pragma once
// math_utils.h — Shared statistical math utilities.
//
// Provides the Acklam inverse-normal-CDF approximation and derived helpers
// used by both the uniform sampler (sampler.cpp) and the stratified sampler
// (stratified_sampler.cpp).

#include <cmath>

namespace zippy_math {

// Acklam rational approximation for the inverse CDF of the standard normal.
// Maximum absolute error < 1.15e-9 over the full (0, 1) range.
inline double inverse_standard_normal_cdf(double p) {
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

    if (p <= 0.0) return -INFINITY;
    if (p >= 1.0) return  INFINITY;

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

// z_{α/2}: upper tail quantile used for CI half-widths.
inline double z_alpha_over_two(double alpha_ci) {
    const double alpha = (alpha_ci > 0.0 && alpha_ci < 1.0) ? alpha_ci : 0.05;
    return inverse_standard_normal_cdf(1.0 - alpha / 2.0);
}

// Hoeffding half-width on the per-tuple mean for one group:
//   ε = (b − a) · sqrt( ln(2/(1−β)) / (2·n_i') )
// (paper §4.2.1, patent col. 9).
inline double hoeffding_eps_per_tuple(double a, double b, double beta_ci, double n_i_prime) {
    if (n_i_prime <= 0.0) return 0.0;
    const double safe_beta = (beta_ci > 0.0 && beta_ci < 1.0) ? beta_ci : 0.95;
    const double range     = (b > a) ? (b - a) : 0.0;
    if (range == 0.0) return 0.0;
    return range * std::sqrt(std::log(2.0 / (1.0 - safe_beta)) / (2.0 * n_i_prime));
}

} // namespace zippy_math
