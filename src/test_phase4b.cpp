#include "sampler.h"
#include "zippy.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>
#include <vector>

namespace {

bool load_dataset(const char* path, size_t n_rows, std::vector<Row>& out_dataset) {
    out_dataset.resize(n_rows);
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::perror("fopen");
        return false;
    }
    const size_t read = std::fread(out_dataset.data(), sizeof(Row), n_rows, f);
    std::fclose(f);
    return read == n_rows;
}

}  // namespace

int main() {
    constexpr const char* S0_PATH = "data/S0.bin";
    constexpr size_t S0_ROWS = 10089;
    constexpr int K = 5;

    std::vector<Row> dataset;
    if (!load_dataset(S0_PATH, S0_ROWS, dataset)) {
        std::fprintf(stderr, "FAIL: unable to load %s with %zu rows\n", S0_PATH, S0_ROWS);
        return 1;
    }

    ZippyConfig cfg;
    cfg.fa_capacity = 100;
    cfg.n_partitions = 100;
    cfg.sample_frac = 0.01;
    cfg.delta = 0.05;
    cfg.alpha_ci = 0.05;
    cfg.beta_ci = 0.95;
    cfg.verbose = false;

    std::vector<std::pair<uint64_t, double>> baseline_topk;
    std::vector<uint64_t> fa_groups;
    const RunMetrics metrics =
        run_zippy_baseline(dataset, K, cfg, baseline_topk, fa_groups);

    if (!metrics.is_optimizable) {
        std::fprintf(stderr, "FAIL: baseline reported is_optimizable=false on S0\n");
        return 1;
    }

    const auto brute_topk = run_brute_force(dataset, K);
    std::unordered_set<uint64_t> brute_ids;
    std::unordered_set<uint64_t> baseline_ids;
    for (const auto& [gid, _sum] : brute_topk) {
        (void)_sum;
        brute_ids.insert(gid);
    }
    for (const auto& [gid, _sum] : baseline_topk) {
        (void)_sum;
        baseline_ids.insert(gid);
    }

    size_t overlap_hits = 0;
    for (uint64_t gid : brute_ids) {
        if (baseline_ids.count(gid) > 0) ++overlap_hits;
    }
    const double overlap_ratio =
        static_cast<double>(overlap_hits) / static_cast<double>(K);
    if (overlap_ratio < 0.80) {
        std::fprintf(stderr,
                     "FAIL: FA top-k overlap %.2f%% < 80%% (%zu/%d)\n",
                     overlap_ratio * 100.0,
                     overlap_hits,
                     K);
        return 1;
    }

    // Reconstruct single-pass state for the pruning-safety gate checks.
    const SampleResult sample = uniform_sample_and_select(
        dataset,
        cfg.fa_capacity,
        K,
        cfg.agg_func,
        cfg.sample_frac,
        cfg.delta,
        cfg.alpha_ci,
        cfg.beta_ci,
        42);
    if (!sample.is_optimizable) {
        std::fprintf(stderr, "FAIL: sampler is_optimizable=false on S0\n");
        return 1;
    }

    FATable fa(cfg.fa_capacity);
    for (uint64_t gid : sample.fa_groups) {
        fa.insert(gid);
    }

    CATable ca(cfg.n_partitions);
    for (const auto& row : dataset) {
        if (fa.contains(row.group_id)) {
            fa.update(row.group_id, row.value);
        } else {
            ca.update(row.group_id, row.value);
        }
    }

    // Algorithm 4 line 12: K-th highest among {FA exact values} ∪ {partition UBs}.
    std::vector<double> union_values;
    {
        const auto fa_all = fa.top_k(fa.size(), cfg.agg_func);
        for (const auto& [gid, val] : fa_all) { (void)gid; union_values.push_back(val); }
        for (size_t pid = 0; pid < ca.n_partitions(); ++pid) {
            const CAPartition& part = ca.partition(pid);
            if (part.count > 0) union_values.push_back(part.upper_bound(cfg.agg_func));
        }
    }
    double topKBound = 0.0;
    if (union_values.size() >= static_cast<size_t>(K)) {
        std::nth_element(union_values.begin(), union_values.begin() + (K - 1),
                         union_values.end(), std::greater<double>());
        topKBound = union_values[K - 1];
    }
    ca.prune(topKBound, cfg.agg_func);

    const auto survivors = ca.surviving_partitions();
    const double pruned_pct = ca.pruning_fraction();

    if (std::abs(topKBound - metrics.topKBound_after_pass1) > 1e-9) {
        std::fprintf(stderr, "FAIL: topKBound mismatch between baseline and gate check\n");
        return 1;
    }
    if (std::abs(pruned_pct - metrics.partitions_pruned_pct) > 1e-9) {
        std::fprintf(stderr, "FAIL: pruned %% mismatch between baseline and gate check\n");
        return 1;
    }
    if (pruned_pct < 0.50) {
        std::fprintf(stderr, "FAIL: pruned %.2f%% < expected 50%% on S0\n", pruned_pct * 100.0);
        return 1;
    }

    // Critical safety check: no brute-force top-k non-FA group may be in a pruned partition.
    for (const auto& [gid, _sum] : brute_topk) {
        (void)_sum;
        if (sample.fa_groups.count(gid) > 0) continue;
        const size_t p = ca.partition_of(gid);
        if (ca.partition(p).pruned) {
            std::fprintf(stderr,
                         "FAIL: brute-force top-k group %llu is in pruned partition %zu\n",
                         static_cast<unsigned long long>(gid),
                         p);
            return 1;
        }
    }

    std::sort(fa_groups.begin(), fa_groups.end());
    std::fprintf(stderr, "PASS: Phase 4B gate passed\n");
    std::fprintf(stderr, "  topKBound              = %.6f\n", topKBound);
    std::fprintf(stderr, "  partitions_pruned_pct  = %.2f%%\n", pruned_pct * 100.0);
    std::fprintf(stderr, "  surviving_partitions   = %zu\n", survivors.size());
    std::fprintf(stderr, "  FA top-k overlap       = %.2f%% (%zu/%d)\n",
                 overlap_ratio * 100.0,
                 overlap_hits,
                 K);
    std::fprintf(stderr, "  FA candidates (%zu)    = ", fa_groups.size());
    for (size_t i = 0; i < fa_groups.size(); ++i) {
        std::fprintf(stderr,
                     "%llu%s",
                     static_cast<unsigned long long>(fa_groups[i]),
                     (i + 1 < fa_groups.size()) ? "," : "\n");
    }

    return 0;
}
