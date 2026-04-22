// zippy.cpp — Core Zippy engine implementation.
//
// Phase 4B: brute-force + single-pass baseline (Pass 1) are implemented.
// Phases 5–7 will add extension modes and Phase 4C multi-pass convergence.

#include "zippy.h"
#include "sampler.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <unordered_map>

// ── Brute-force baseline ───────────────────────────────────────────────────
// Computes exact aggregates for every group in one pass, then selects the
// top-k by aggregate value using partial_sort. This is the ground-truth
// comparator — it does NOT use FA/CA and is always correct regardless of
// data distribution.
//
// See AGENTS.md Section 7 "Brute-Force Mode Implementation".

std::vector<std::pair<uint64_t,double>> run_brute_force(
    const std::vector<Row>& dataset, int k)
{
    // One-pass exact aggregation over all groups
    std::unordered_map<uint64_t, double> agg;
    agg.reserve(dataset.size() / 10);  // rough capacity hint
    for (const auto& row : dataset)
        agg[row.group_id] += row.value;

    // Partial sort: find top-k without fully sorting all M groups
    std::vector<std::pair<uint64_t,double>> all(agg.begin(), agg.end());
    size_t n = static_cast<size_t>(k);
    if (n > all.size()) n = all.size();

    if (n > 0) {
        std::partial_sort(all.begin(), all.begin() + n, all.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
    }
    all.resize(n);
    return all;
}

RunMetrics run_zippy_baseline(
    const std::vector<Row>& dataset,
    int k,
    const ZippyConfig& cfg,
    std::vector<std::pair<uint64_t,double>>& out_results,
    std::vector<uint64_t>& out_fa_groups)
{
    RunMetrics metrics;
    out_results.clear();
    out_fa_groups.clear();

    Timer total_timer;
    total_timer.reset();

    // Algorithm 2 (simplified): uniform sampling + FA candidate selection.
    Timer sample_timer;
    sample_timer.reset();
    const SampleResult sample = uniform_sample_and_select(
        dataset,
        cfg.fa_capacity,
        cfg.sample_frac,
        cfg.delta,
        cfg.alpha_ci,
        cfg.beta_ci,
        42);
    metrics.sample_duration_ms = sample_timer.elapsed_ms();
    metrics.sample_size_actual = sample.sample_size_actual;
    metrics.fa_candidates_count = sample.fa_groups.size();
    metrics.is_optimizable = sample.is_optimizable;

    if (!sample.is_optimizable) {
        out_results = run_brute_force(dataset, k);
        metrics.total_passes = 0;
        metrics.total_duration_ms = total_timer.elapsed_ms();
        return metrics;
    }

    // Phase 4B: Pass 1 only (Algorithm 3 lines 21–28 + Algorithm 4 pruning step).
    FATable fa(cfg.fa_capacity);
    for (uint64_t gid : sample.fa_groups) {
        fa.insert(gid);
    }

    out_fa_groups = fa.all_group_ids();
    std::sort(out_fa_groups.begin(), out_fa_groups.end());

    CATable ca(cfg.n_partitions);

    Timer pass1_timer;
    pass1_timer.reset();
    for (const auto& row : dataset) {
        assert(row.group_id != FA_EMPTY_KEY && "group_id must not equal FA sentinel key");
        if (fa.contains(row.group_id)) {
            fa.update(row.group_id, row.value);
        } else {
            ca.update(row.group_id, row.value);
        }
    }

    const size_t k_size = (k > 0) ? static_cast<size_t>(k) : 0;
    out_results = fa.top_k(k_size);

    // topKBound: k-th largest FA value; if fewer than k values, keep bound at 0.
    double topKBound = 0.0;
    if (k_size > 0 && out_results.size() >= k_size) {
        topKBound = out_results[k_size - 1].second;
    }

    ca.prune(topKBound);

    metrics.pass1_duration_ms = pass1_timer.elapsed_ms();
    metrics.topKBound_after_pass1 = topKBound;
    metrics.partitions_pruned_pct = ca.pruning_fraction();
    metrics.total_passes = 1;
    metrics.total_duration_ms = total_timer.elapsed_ms();

    if (cfg.verbose) {
        const auto survivors = ca.surviving_partitions();
        std::fprintf(stderr,
                     "[baseline-pass1] topKBound=%.6f pruned=%.2f%% survivors=%zu\n",
                     metrics.topKBound_after_pass1,
                     metrics.partitions_pruned_pct * 100.0,
                     survivors.size());
        std::fprintf(stderr, "[baseline-pass1] FA candidates (%zu): ", out_fa_groups.size());
        for (size_t i = 0; i < out_fa_groups.size(); ++i) {
            std::fprintf(stderr,
                         "%llu%s",
                         static_cast<unsigned long long>(out_fa_groups[i]),
                         (i + 1 < out_fa_groups.size()) ? "," : "\n");
        }
    }

    return metrics;
}
