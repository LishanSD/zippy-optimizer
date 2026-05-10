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

// kth_bound: k-th value to use as pruning threshold.
// For MIN, we want the k-th *lowest* exact value (largest allowed min).
double kth_bound_for_exact(std::vector<double>& values, size_t k, AggregateType agg_type) {
    if (k == 0 || values.empty()) return 0.0;
    if (agg_type == AggregateType::MIN) {
        if (values.size() < k) return std::numeric_limits<double>::max();
        std::nth_element(values.begin(), values.begin() + (k - 1), values.end());
        return values[k - 1];
    }
    return kth_highest_or_zero(values, k);
}

MergeAndPruneResult merge_and_prune(
    ExactAggregates& exact_aggregates,
    const PartialAggregates& partial_aggregates,
    const ChildPartitions& child_partitions,
    size_t k,
    AggregateType agg_type)
{
    // Merge partial aggregates into exact (operator depends on type)
    for (const auto& [gid, val] : partial_aggregates) {
        switch (agg_type) {
            case AggregateType::SUM:   exact_aggregates[gid] += val; break;
            case AggregateType::COUNT: exact_aggregates[gid] += val; break;
            case AggregateType::MAX: {
                auto it = exact_aggregates.find(gid);
                if (it == exact_aggregates.end()) exact_aggregates[gid] = val;
                else if (val > it->second) it->second = val;
                break;
            }
            case AggregateType::MIN: {
                auto it = exact_aggregates.find(gid);
                if (it == exact_aggregates.end()) exact_aggregates[gid] = val;
                else if (val < it->second) it->second = val;
                break;
            }
        }
    }

    std::vector<double> union_values;
    union_values.reserve(exact_aggregates.size() + child_partitions.size());
    for (const auto& [gid, exact] : exact_aggregates) {
        (void)gid;
        union_values.push_back(exact);
    }
    for (const auto& [pid, part] : child_partitions) {
        (void)pid;
        union_values.push_back(part.upper_bound(agg_type));
    }

    const double top_k_bound = kth_bound_for_exact(union_values, k, agg_type);

    size_t top_k_confirmed = 0;
    for (const auto& [gid, exact] : exact_aggregates) {
        (void)gid;
        if (agg_type == AggregateType::MIN) {
            if (exact < top_k_bound) ++top_k_confirmed;
        } else {
            if (exact > top_k_bound) ++top_k_confirmed;
        }
    }

    ChildPartitions surviving;
    surviving.reserve(child_partitions.size());
    for (const auto& [pid, part] : child_partitions) {
        const double ub = part.upper_bound(agg_type);
        bool keep = (agg_type == AggregateType::MIN)
                    ? (ub <= top_k_bound)
                    : (ub >= top_k_bound);
        if (keep) surviving.emplace(pid, part);
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
    size_t k,
    AggregateType agg_type = AggregateType::SUM)
{
    std::vector<std::pair<uint64_t, double>> all(exact_aggregates.begin(), exact_aggregates.end());
    const size_t n = std::min(k, all.size());
    if (n > 0) {
        if (agg_type == AggregateType::MIN) {
            std::partial_sort(all.begin(), all.begin() + n, all.end(),
                [](const auto& a, const auto& b) {
                    if (a.second != b.second) return a.second < b.second;
                    return a.first < b.first;
                });
        } else {
            std::partial_sort(all.begin(), all.begin() + n, all.end(),
                [](const auto& a, const auto& b) {
                    if (a.second != b.second) return a.second > b.second;
                    return a.first < b.first;
                });
        }
    }
    all.resize(n);
    return all;
}

// ── Algorithm 3 helpers ────────────────────────────────────────────────────

// Segment-based locality score: l = (1/t) * Σ d_s/c_s
// Paper §4.3, patent defaults: segment_size=100k, α₀=0.20.
// d_s = distinct group_ids among active-partition rows in segment,
// c_s = total active-partition rows in segment.
// Low l → rows clustered by group → physical partitioning is beneficial.
double compute_locality(
    const std::vector<Row>& dataset,
    const std::unordered_set<size_t>& active_pid_set,
    size_t n_partitions,
    int level,
    size_t segment_size)
{
    if (dataset.empty() || active_pid_set.empty()) return 1.0;

    double sum_ratio = 0.0;
    size_t n_segments = 0;

    std::unordered_set<uint64_t> seg_groups;
    seg_groups.reserve(segment_size / 4);
    size_t seg_active_rows = 0;
    size_t row_in_seg = 0;

    auto flush_segment = [&]() {
        if (seg_active_rows > 0) {
            sum_ratio += static_cast<double>(seg_groups.size())
                       / static_cast<double>(seg_active_rows);
            ++n_segments;
        }
        seg_groups.clear();
        seg_active_rows = 0;
    };

    for (const auto& row : dataset) {
        const size_t pid = partition_id_for_level(row.group_id, n_partitions, level - 1);
        if (active_pid_set.count(pid) > 0) {
            seg_groups.insert(row.group_id);
            ++seg_active_rows;
        }
        if (++row_in_seg == segment_size) {
            flush_segment();
            row_in_seg = 0;
        }
    }
    flush_segment();  // tail segment

    return (n_segments == 0) ? 1.0 : sum_ratio / static_cast<double>(n_segments);
}

// Physical partitioning: one pass over the dataset, routing each non-FA row
// whose parent partition is in active_pid_set into a per-partition buffer.
// Returns map<partition_id, vector<Row>> for the surviving partitions.
std::unordered_map<size_t, std::vector<Row>> physical_partition_rows(
    const std::vector<Row>& dataset,
    const FATable& fa,
    const std::unordered_set<size_t>& active_pid_set,
    size_t n_partitions,
    int level)
{
    std::unordered_map<size_t, std::vector<Row>> buffers;
    buffers.reserve(active_pid_set.size());

    for (const auto& row : dataset) {
        if (fa.contains(row.group_id)) continue;
        const size_t pid = partition_id_for_level(row.group_id, n_partitions, level - 1);
        if (active_pid_set.count(pid) == 0) continue;
        buffers[pid].push_back(row);
    }
    return buffers;
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
        42,
        k_size);
    metrics.sample_duration_ms = sample_timer.elapsed_ms();
    metrics.sample_size_actual = sample.sample_size_actual;
    metrics.fa_candidates_count = sample.fa_groups.size();
    metrics.is_optimizable = sample.is_optimizable;

    if (!sample.is_optimizable) {
        out_results = run_brute_force(dataset, k);
        metrics.total_duration_ms = total_timer.elapsed_ms();
        return metrics;
    }

    // Pass 1 (Phase 4B): FA/CA routing + pruning.
    // FA identity: 0 for SUM/COUNT, lowest() for MAX, max() for MIN.
    const double fa_identity = (cfg.agg_type == AggregateType::MAX)
        ? std::numeric_limits<double>::lowest()
        : (cfg.agg_type == AggregateType::MIN)
            ? std::numeric_limits<double>::max()
            : 0.0;

    FATable fa(cfg.fa_capacity);
    for (uint64_t gid : sample.fa_groups) {
        fa.insert(gid, fa_identity);
    }

    out_fa_groups = fa.all_group_ids();
    std::sort(out_fa_groups.begin(), out_fa_groups.end());

    CATable ca(cfg.n_partitions);

    Timer pass1_timer;
    pass1_timer.reset();
    for (const auto& row : dataset) {
        assert(row.group_id != FA_EMPTY_KEY && "group_id must not equal FA sentinel key");
        if (fa.contains(row.group_id)) {
            fa.update(row.group_id, row.value, cfg.agg_type);
        } else {
            ca.update(row.group_id, row.value);
        }
    }

    out_results = fa.top_k(k_size, cfg.agg_type);  // temporary, replaced after Phase 4C loop

    // topKBound: k-th highest of {FA exact values ∪ CA partition UBs}.
    // Using only FA values under-prunes vs Algorithm 4 line 12 of the paper.
    double topKBound = 0.0;
    {
        std::vector<double> union_vals;
        union_vals.reserve(out_fa_groups.size() + ca.n_partitions());
        for (uint64_t gid : out_fa_groups)
            union_vals.push_back(fa.get(gid));
        for (size_t pid = 0; pid < ca.n_partitions(); ++pid)
            if (ca.partition(pid).count > 0)
                union_vals.push_back(ca.partition(pid).upper_bound(cfg.agg_type));
        topKBound = kth_bound_for_exact(union_vals, k_size, cfg.agg_type);
    }

    ca.prune(topKBound, cfg.agg_type);

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
        std::unordered_set<size_t> active_pid_set;
        exact_parent_ids.reserve(active_partitions.size());
        active_pid_set.reserve(active_partitions.size());
        for (const auto& [pid, part] : active_partitions) {
            active_pid_set.insert(pid);
            if (static_cast<size_t>(part.fm.estimate()) < cfg.fa_capacity) {
                exact_parent_ids.insert(pid);
            }
        }

        // Algorithm 3: decide logical vs physical partitioning.
        // Locality score l = (1/t)*Σ d_s/c_s; if l < α₀ → physical.
        // Also apply C_p/Q < T_c check (patent claims 3/4, 13/14).
        const double locality = compute_locality(
            dataset, active_pid_set, cfg.n_partitions, level, cfg.locality_segment_size);

        // C_p/Q < T_c: C_p = total estimated distinct groups, Q = #active partitions,
        // T_c = min FA exact aggregate (lowest confirmed top-k threshold).
        double T_c = std::numeric_limits<double>::max();
        for (uint64_t gid : out_fa_groups) {
            const double v = fa.get(gid);
            if (v < T_c) T_c = v;
        }
        double C_p = 0.0;
        for (const auto& [pid, part] : active_partitions) {
            (void)pid;
            C_p += static_cast<double>(part.fm.estimate());
        }
        const double Q = static_cast<double>(active_partitions.size());
        const bool cp_q_logical = (Q > 0.0) && ((C_p / Q) < T_c);
        const bool use_physical = (locality < cfg.locality_threshold) && !cp_q_logical;

        if (cfg.verbose) {
            std::fprintf(stderr,
                "[baseline-pass%d] locality=%.3f use_physical=%s C_p/Q=%.1f T_c=%.3f\n",
                level, locality, use_physical ? "yes" : "no", Q > 0 ? C_p/Q : 0.0, T_c);
        }

        PartialAggregates partial;
        ChildPartitions children;
        partial.reserve(4096);
        children.reserve(active_partitions.size());

        Timer pass_timer;
        pass_timer.reset();

        auto accumulate_row = [&](const Row& row) {
            const size_t parent_id = partition_id_for_level(row.group_id, cfg.n_partitions, level - 1);
            if (exact_parent_ids.count(parent_id) > 0) {
                auto [it, inserted] = partial.emplace(row.group_id, fa_identity);
                switch (cfg.agg_type) {
                    case AggregateType::SUM:   it->second += row.value; break;
                    case AggregateType::COUNT: it->second += 1.0; break;
                    case AggregateType::MAX:   if (row.value > it->second) it->second = row.value; break;
                    case AggregateType::MIN:   if (row.value < it->second) it->second = row.value; break;
                }
            } else {
                const size_t child_id = partition_id_for_level(row.group_id, cfg.n_partitions, level);
                update_partition_from_row(children[child_id], row.group_id, row.value);
            }
        };

        if (use_physical) {
            // Physical partitioning: pre-partition rows into per-partition buffers,
            // then process each buffer sequentially for better cache locality.
            auto buffers = physical_partition_rows(
                dataset, fa, active_pid_set, cfg.n_partitions, level);
            for (auto& [pid, buf] : buffers) {
                (void)pid;
                for (const auto& row : buf) {
                    accumulate_row(row);
                }
            }
        } else {
            // Logical partitioning: single full-dataset scan with row filtering.
            for (const auto& row : dataset) {
                if (fa.contains(row.group_id)) continue;
                if (!row_in_active_path(row.group_id, cfg.n_partitions, level - 1, active_history)) continue;
                accumulate_row(row);
            }
        }

        metrics.pass2plus_duration_ms += pass_timer.elapsed_ms();
        metrics.total_passes += 1;

        const MergeAndPruneResult merged =
            merge_and_prune(exact_aggregates, partial, children, k_size, cfg.agg_type);
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

    out_results = top_k_from_exact(exact_aggregates, k_size, cfg.agg_type);
    metrics.total_duration_ms = total_timer.elapsed_ms();

    return metrics;
}
