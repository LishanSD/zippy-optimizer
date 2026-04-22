// zippy.cpp — Core Zippy engine implementation.
//
// Phase 4C: brute-force + baseline multi-pass convergence are implemented.
// Phases 5–7 will add extension modes.

#include "zippy.h"
#include "sampler.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using ExactAggregates = std::unordered_map<uint64_t, double>;
using PartialAggregates = std::unordered_map<uint64_t, double>;
using ChildPartitions = std::unordered_map<size_t, CAPartition>;

struct MergeAndPruneResult {
    double top_k_bound = 0.0;
    size_t top_k_confirmed = 0;
    bool done = false;
    ChildPartitions surviving_partitions;
};

inline size_t partition_id_for_level(uint64_t group_id, size_t n_partitions, int level) {
    if (level <= 1) return partition_hash(group_id, n_partitions);
    return child_partition_hash(group_id, n_partitions, level);
}

inline void update_partition_from_row(CAPartition& part, uint64_t group_id, double val) {
    part.total_sum += val;
    if (val > part.max_value) part.max_value = val;
    if (val < part.min_value) part.min_value = val;
    part.count++;
    part.fm.update(group_id);
}

double kth_highest_or_zero(std::vector<double>& values, size_t k) {
    if (k == 0 || values.size() < k) return 0.0;
    std::nth_element(values.begin(), values.begin() + (k - 1), values.end(), std::greater<double>());
    return values[k - 1];
}

bool row_in_active_path(
    uint64_t group_id,
    size_t n_partitions,
    int up_to_level,
    const std::vector<std::unordered_set<size_t>>& active_history)
{
    for (int lvl = 1; lvl <= up_to_level; ++lvl) {
        if (lvl >= static_cast<int>(active_history.size())) return false;
        if (active_history[lvl].empty()) return false;
        const size_t pid = partition_id_for_level(group_id, n_partitions, lvl);
        if (active_history[lvl].count(pid) == 0) return false;
    }
    return true;
}

MergeAndPruneResult merge_and_prune(
    ExactAggregates& exact_aggregates,
    const PartialAggregates& partial_aggregates,
    const ChildPartitions& child_partitions,
    size_t k)
{
    for (const auto& [gid, val] : partial_aggregates) {
        exact_aggregates[gid] += val;
    }

    std::vector<double> union_values;
    union_values.reserve(exact_aggregates.size() + child_partitions.size());
    for (const auto& [gid, exact] : exact_aggregates) {
        (void)gid;
        union_values.push_back(exact);
    }
    for (const auto& [pid, part] : child_partitions) {
        (void)pid;
        union_values.push_back(part.total_sum);  // SUM UB
    }

    const double top_k_bound = kth_highest_or_zero(union_values, k);

    size_t top_k_confirmed = 0;
    for (const auto& [gid, exact] : exact_aggregates) {
        (void)gid;
        if (exact > top_k_bound) ++top_k_confirmed;
    }

    ChildPartitions surviving;
    surviving.reserve(child_partitions.size());
    for (const auto& [pid, part] : child_partitions) {
        if (part.total_sum >= top_k_bound) {
            surviving.emplace(pid, part);
        }
    }

    MergeAndPruneResult result;
    result.top_k_bound = top_k_bound;
    result.top_k_confirmed = top_k_confirmed;
    result.done = (top_k_confirmed >= k) || surviving.empty();
    result.surviving_partitions = std::move(surviving);
    return result;
}

std::vector<std::pair<uint64_t, double>> top_k_from_exact(
    const ExactAggregates& exact_aggregates,
    size_t k)
{
    std::vector<std::pair<uint64_t, double>> all(exact_aggregates.begin(), exact_aggregates.end());
    const size_t n = std::min(k, all.size());
    if (n > 0) {
        std::partial_sort(
            all.begin(), all.begin() + n, all.end(),
            [](const auto& a, const auto& b) {
                if (a.second != b.second) return a.second > b.second;
                return a.first < b.first;
            });
    }
    all.resize(n);
    return all;
}

}  // namespace

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
            [](const auto& a, const auto& b) {
                if (a.second != b.second) return a.second > b.second;
                return a.first < b.first;
            });
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
    metrics.total_passes = 0;

    Timer total_timer;
    total_timer.reset();
    const size_t k_size = (k > 0) ? static_cast<size_t>(k) : 0;

    if (dataset.empty() || k_size == 0) {
        metrics.total_duration_ms = total_timer.elapsed_ms();
        return metrics;
    }

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
        metrics.total_duration_ms = total_timer.elapsed_ms();
        return metrics;
    }

    // Pass 1 (Phase 4B): FA/CA routing + pruning with k-th FA value.
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

    out_results = fa.top_k(k_size);  // temporary, replaced after Phase 4C loop

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

    ExactAggregates exact_aggregates;
    exact_aggregates.reserve(std::max<size_t>(fa.size() * 2, 1));
    for (uint64_t gid : out_fa_groups) {
        exact_aggregates[gid] = fa.get(gid);
    }

    std::vector<std::unordered_set<size_t>> active_history(2);  // index 0 unused
    ChildPartitions active_partitions;
    for (size_t pid : ca.surviving_partitions()) {
        active_history[1].insert(pid);
        active_partitions.emplace(pid, ca.partition(pid));
    }

    if (cfg.verbose) {
        std::fprintf(stderr,
                     "[baseline-pass1] topKBound=%.6f pruned=%.2f%% survivors=%zu\n",
                     metrics.topKBound_after_pass1,
                     metrics.partitions_pruned_pct * 100.0,
                     active_partitions.size());
        std::fprintf(stderr, "[baseline-pass1] FA candidates (%zu): ", out_fa_groups.size());
        for (size_t i = 0; i < out_fa_groups.size(); ++i) {
            std::fprintf(stderr,
                         "%llu%s",
                         static_cast<unsigned long long>(out_fa_groups[i]),
                         (i + 1 < out_fa_groups.size()) ? "," : "\n");
        }
    }

    // Phase 4C: MergeAndPrune + multi-pass loop.
    int level = 2;  // Pass 2 children are hashed with child_partition_hash(..., pass=2).
    bool done = active_partitions.empty();

    while (!done && !active_partitions.empty()) {
        std::unordered_set<size_t> exact_parent_ids;
        exact_parent_ids.reserve(active_partitions.size());
        for (const auto& [pid, part] : active_partitions) {
            (void)pid;
            if (static_cast<size_t>(part.fm.estimate()) < cfg.fa_capacity) {
                exact_parent_ids.insert(pid);
            }
        }

        PartialAggregates partial;
        ChildPartitions children;
        partial.reserve(4096);
        children.reserve(active_partitions.size());

        Timer pass_timer;
        pass_timer.reset();
        for (const auto& row : dataset) {
            if (fa.contains(row.group_id)) {
                continue;  // FA groups are already exact after Pass 1.
            }

            if (!row_in_active_path(row.group_id, cfg.n_partitions, level - 1, active_history)) {
                continue;
            }

            const size_t parent_id = partition_id_for_level(row.group_id, cfg.n_partitions, level - 1);
            if (exact_parent_ids.count(parent_id) > 0) {
                partial[row.group_id] += row.value;
            } else {
                const size_t child_id = partition_id_for_level(row.group_id, cfg.n_partitions, level);
                update_partition_from_row(children[child_id], row.group_id, row.value);
            }
        }
        metrics.pass2plus_duration_ms += pass_timer.elapsed_ms();
        metrics.total_passes += 1;

        const MergeAndPruneResult merged =
            merge_and_prune(exact_aggregates, partial, children, k_size);
        done = merged.done;
        if (done) break;

        active_partitions = merged.surviving_partitions;
        if (static_cast<int>(active_history.size()) <= level) {
            active_history.resize(level + 1);
        }
        active_history[level].clear();
        for (const auto& [pid, part] : active_partitions) {
            (void)part;
            active_history[level].insert(pid);
        }

        ++level;
    }

    out_results = top_k_from_exact(exact_aggregates, k_size);
    metrics.total_duration_ms = total_timer.elapsed_ms();

    return metrics;
}
