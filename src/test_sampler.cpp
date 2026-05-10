#include "sampler.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <unordered_map>
#include <vector>

namespace
{

    std::vector<Row> generate_s0_like_dataset(
        size_t n_rows,
        size_t n_groups,
        double zipf_alpha,
        uint64_t seed)
    {
        std::vector<double> weights;
        weights.reserve(n_groups);
        for (size_t i = 1; i <= n_groups; ++i)
        {
            weights.push_back(1.0 / std::pow(static_cast<double>(i), zipf_alpha));
        }

        std::mt19937_64 rng(seed);
        std::discrete_distribution<size_t> group_dist(weights.begin(), weights.end());
        std::exponential_distribution<double> value_dist(1.0 / 100.0); // scale=100

        std::vector<Row> dataset;
        dataset.reserve(n_rows);
        for (size_t i = 0; i < n_rows; ++i)
        {
            dataset.push_back(Row{
                static_cast<uint64_t>(group_dist(rng)),
                value_dist(rng),
            });
        }
        return dataset;
    }

    std::vector<std::pair<uint64_t, double>> brute_force_top_k(
        const std::vector<Row> &dataset,
        size_t k)
    {
        std::unordered_map<uint64_t, double> agg;
        agg.reserve(dataset.size() / 4);
        for (const auto &row : dataset)
        {
            agg[row.group_id] += row.value;
        }

        std::vector<std::pair<uint64_t, double>> all(agg.begin(), agg.end());
        const size_t n = std::min(k, all.size());
        if (n > 0)
        {
            std::partial_sort(
                all.begin(), all.begin() + n, all.end(),
                [](const auto &a, const auto &b)
                { return a.second > b.second; });
        }
        all.resize(n);
        return all;
    }

} // namespace

int main()
{
    constexpr size_t N_ROWS = 10000;
    constexpr size_t N_GROUPS = 500;
    constexpr size_t FA_CAPACITY = 100;

    const auto dataset = generate_s0_like_dataset(N_ROWS, N_GROUPS, 1.2, 42);

    const auto start = std::chrono::high_resolution_clock::now();
    const SampleResult sample = uniform_sample_and_select(
        dataset,
        FA_CAPACITY,
        /*k=*/10,
        AggFunc::SUM,
        0.01,
        0.05,
        0.05,
        0.95,
        42);
    const auto end = std::chrono::high_resolution_clock::now();
    const double sample_duration_ms =
        std::chrono::duration<double, std::milli>(end - start).count();

    if (!sample.is_optimizable)
    {
        std::fprintf(stderr, "FAIL: is_optimizable == false\n");
        return 1;
    }

    if (sample.fa_groups.size() != FA_CAPACITY)
    {
        std::fprintf(stderr,
                     "FAIL: fa_groups.size() = %zu, expected %zu\n",
                     sample.fa_groups.size(),
                     FA_CAPACITY);
        return 1;
    }

    const auto top10 = brute_force_top_k(dataset, 10);
    size_t hits = 0;
    for (const auto &[gid, _sum] : top10)
    {
        (void)_sum;
        if (sample.fa_groups.count(gid) > 0)
            ++hits;
    }

    if (hits < 8)
    {
        std::fprintf(stderr,
                     "FAIL: only %zu/10 true top groups found in FA (need >= 8)\n",
                     hits);
        return 1;
    }

    std::vector<uint64_t> candidates(sample.fa_groups.begin(), sample.fa_groups.end());
    std::sort(candidates.begin(), candidates.end());

    std::fprintf(stderr, "PASS: sampler gate passed\n");
    std::fprintf(stderr, "  sample_duration_ms = %.3f\n", sample_duration_ms);
    std::fprintf(stderr, "  sample_size_actual = %zu\n", sample.sample_size_actual);
    std::fprintf(stderr, "  top10_hits_in_fa   = %zu/10\n", hits);
    std::fprintf(stderr, "  candidate_count    = %zu\n", candidates.size());
    std::fprintf(stderr, "  FA candidates      = ");
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        std::fprintf(stderr, "%llu%s",
                     static_cast<unsigned long long>(candidates[i]),
                     (i + 1 < candidates.size()) ? "," : "\n");
    }

    return 0;
}
