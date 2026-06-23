// zippy.cpp — Core Zippy engine implementation.
//
// Implements: Algorithm 1 framework, Algorithm 2 sampling+candidate selection
// (Hoeffding LB + L_k via sampler.cpp), Algorithm 3 routing with locality test
// + logical/physical decision (patent claims 3, 4, 13, 14), Algorithm 4
// MergeAndPrune with multi-aggregate UBs (patent description col. 13).

#include "zippy.h"
#include "sampler.h"
#include "measure_index.h" // Add this at the top of zippy.cpp
#include "group_index.h"
#include "stratified_sampler.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct ExactAccum {
    double   sum   = 0.0;
    uint64_t count = 0;
    double   max_v = std::numeric_limits<double>::lowest();
    double   min_v = std::numeric_limits<double>::max();

    void update(double v) {
        sum   += v;
        count += 1;
        if (v > max_v) max_v = v;
        if (v < min_v) min_v = v;
    }
    double value(AggFunc f) const {
        switch (f) {
            case AggFunc::SUM:   return sum;
            case AggFunc::COUNT: return static_cast<double>(count);
            case AggFunc::MAX:   return max_v;
            case AggFunc::MIN:   return min_v;
        }
        return sum;
    }
    void merge(const ExactAccum& o) {
        sum   += o.sum;
        count += o.count;
        if (o.max_v > max_v) max_v = o.max_v;
        if (o.min_v < min_v) min_v = o.min_v;
    }
};

using ExactAggregates  = std::unordered_map<uint64_t, ExactAccum>;
using PartialAggregates = std::unordered_map<uint64_t, ExactAccum>;
using ChildPartitions   = std::unordered_map<size_t, CAPartition>;

struct MergeAndPruneResult {
    double top_k_bound = 0.0;
    size_t top_k_confirmed = 0;
    bool   done = false;
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

// active_history[lvl] is a vector<bool> of size n_partitions.
// A true bit at index pid means that partition survived to that level.
// Membership check is a single array index instead of a hash lookup,
// and the bitset (10 000 partitions = 1 250 bytes) fits in L1 cache.
bool row_in_active_path(
    uint64_t group_id,
    size_t n_partitions,
    int up_to_level,
    const std::vector<std::vector<bool>>& active_history)
{
    for (int lvl = 1; lvl <= up_to_level; ++lvl) {
        if (lvl >= static_cast<int>(active_history.size())) return false;
        if (active_history[lvl].empty()) return false;
        const size_t pid = partition_id_for_level(group_id, n_partitions, lvl);
        if (!active_history[lvl][pid]) return false;
    }
    return true;
}

// Algorithm 3 lines 5–13 + patent claim 4: classify a parent partition for the
// next pass as one of:
//   EXACT   — distinct < FA capacity, OR locality d/c < α₀
//   LOGICAL — C_p / Q < T_c   (statistics-only routing; no row movement)
//   PHYSICAL — otherwise      (would move rows; metric-only in this prototype)
enum class PartitionDecision { EXACT, LOGICAL, PHYSICAL };

PartitionDecision classify_partition(
    const CAPartition& part,
    const ZippyConfig& cfg,
    uint64_t t_c)
{
    const uint32_t d = part.fm.estimate();
    if (d < cfg.fa_capacity) return PartitionDecision::EXACT;

    if (part.count > 0) {
        double locality;
        if (!part.segment_sketches.empty()) {
            // Segmented locality (paper §4.3, patent col. 11):
            // l = (1/t) Σ_s (distinct_s / count_s)
            double sum = 0.0;
            int active = 0;
            for (size_t s = 0; s < part.segment_sketches.size(); ++s) {
                if (part.segment_counts[s] == 0) continue;
                sum += static_cast<double>(part.segment_sketches[s].estimate())
                     / static_cast<double>(part.segment_counts[s]);
                ++active;
            }
            locality = active > 0 ? sum / static_cast<double>(active) : 1.0;
        } else {
            locality = static_cast<double>(d) / static_cast<double>(part.count);
        }
        if (locality < cfg.alpha_locality) return PartitionDecision::EXACT;
    }

    // Patent claim 4 / 14: C_p / Q < T_c ⇒ logical, else physical.
    if (cfg.n_partitions == 0 || t_c == 0) return PartitionDecision::LOGICAL;
    const double e = static_cast<double>(part.count) / static_cast<double>(cfg.n_partitions);
    if (e < static_cast<double>(t_c)) return PartitionDecision::LOGICAL;
    return PartitionDecision::PHYSICAL;
}

MergeAndPruneResult merge_and_prune(
    ExactAggregates& exact_aggregates,
    const PartialAggregates& partial_aggregates,
    const ChildPartitions& child_partitions,
    size_t k,
    AggFunc agg)
{
    for (const auto& [gid, acc] : partial_aggregates) {
        exact_aggregates[gid].merge(acc);
    }

    std::vector<double> union_values;
    union_values.reserve(exact_aggregates.size() + child_partitions.size());
    for (const auto& [gid, acc] : exact_aggregates) {
        (void)gid;
        union_values.push_back(acc.value(agg));
    }
    for (const auto& [pid, part] : child_partitions) {
        (void)pid;
        union_values.push_back(part.upper_bound(agg));
    }

    const double top_k_bound = kth_highest_or_zero(union_values, k);

    size_t top_k_confirmed = 0;
    for (const auto& [gid, acc] : exact_aggregates) {
        (void)gid;
        if (acc.value(agg) > top_k_bound) ++top_k_confirmed;
    }

    ChildPartitions surviving;
    surviving.reserve(child_partitions.size());
    for (const auto& [pid, part] : child_partitions) {
        if (part.upper_bound(agg) >= top_k_bound) {
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
    size_t k,
    AggFunc agg)
{
    std::vector<std::pair<uint64_t, double>> all;
    all.reserve(exact_aggregates.size());
    for (const auto& [gid, acc] : exact_aggregates) {
        all.emplace_back(gid, acc.value(agg));
    }
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
// One-pass exact aggregation for every group, then partial_sort to pick top-k
// by `agg_func`. Always correct — used as the ground-truth comparator.
std::vector<std::pair<uint64_t,double>> run_brute_force(
    const std::vector<Row>& dataset, int k, AggFunc agg_func)
{
    std::unordered_map<uint64_t, ExactAccum> agg;
    agg.reserve(dataset.size() / 10);
    for (const auto& row : dataset)
        agg[row.group_id].update(row.value);

    std::vector<std::pair<uint64_t,double>> all;
    all.reserve(agg.size());
    for (const auto& [gid, acc] : agg)
        all.emplace_back(gid, acc.value(agg_func));

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

// Shared multi-pass MergeAndPrune loop used by all run functions.
// Extracted to avoid ~80-line triplication (M11 / Fix 9).
// All state is passed by reference and mutated in place.
static void run_multipass_loop(
    const std::vector<Row>&                        dataset,
    const FATable&                                 fa,
    ExactAggregates&                               exact_aggregates,
    ChildPartitions&                               active_partitions,
    std::vector<std::vector<bool>>&                active_history,
    const ZippyConfig&                             cfg,
    size_t                                         k_size,
    RunMetrics&                                    metrics,
    const char*                                    mode_label)
{
    int  level = 2;
    bool done  = active_partitions.empty();
    const uint64_t t_c = fa.lowest_count();  // FA is frozen after Pass 1; compute once

    while (!done && !active_partitions.empty()) {
        std::unordered_map<size_t, PartitionDecision> decisions;
        decisions.reserve(active_partitions.size());
        size_t pass_exact = 0, pass_logical = 0, pass_physical = 0;
        for (const auto& [pid, part] : active_partitions) {
            const PartitionDecision d = classify_partition(part, cfg, t_c);
            decisions[pid] = d;
            switch (d) {
                case PartitionDecision::EXACT:    ++pass_exact;    break;
                case PartitionDecision::LOGICAL:  ++pass_logical;  break;
                case PartitionDecision::PHYSICAL: ++pass_physical; break;
            }
        }
        metrics.partitions_exact_agg += pass_exact;
        metrics.partitions_logical   += pass_logical;
        metrics.partitions_physical  += pass_physical;

        PartialAggregates partial;
        ChildPartitions   children;
        partial.reserve(4096);
        children.reserve(active_partitions.size());

        Timer pass_timer;
        pass_timer.reset();
        for (const auto& row : dataset) {
            if (fa.contains(row.group_id)) continue;
            if (!row_in_active_path(row.group_id, cfg.n_partitions, level - 1, active_history))
                continue;

            const size_t parent_id = partition_id_for_level(row.group_id, cfg.n_partitions, level - 1);
            const auto it = decisions.find(parent_id);
            if (it == decisions.end()) continue;

            if (it->second == PartitionDecision::EXACT) {
                partial[row.group_id].update(row.value);
            } else {
                const size_t child_id = partition_id_for_level(row.group_id, cfg.n_partitions, level);
                update_partition_from_row(children[child_id], row.group_id, row.value);
            }
        }
        metrics.pass2plus_duration_ms += pass_timer.elapsed_ms();
        metrics.total_passes += 1;

        const MergeAndPruneResult merged =
            merge_and_prune(exact_aggregates, partial, children, k_size, cfg.agg_func);
        done = merged.done;
        if (cfg.verbose) {
            std::fprintf(stderr,
                         "[%s-pass%d] exact=%zu logical=%zu physical=%zu "
                         "topKBound=%.6f survivors=%zu confirmed=%zu\n",
                         mode_label, metrics.total_passes,
                         pass_exact, pass_logical, pass_physical,
                         merged.top_k_bound,
                         merged.surviving_partitions.size(),
                         merged.top_k_confirmed);
        }
        if (done) break;

        active_partitions = merged.surviving_partitions;
        while (static_cast<int>(active_history.size()) <= level) {
            active_history.emplace_back(cfg.n_partitions, false);
        }
        std::fill(active_history[level].begin(), active_history[level].end(), false);
        for (const auto& [pid, part] : active_partitions) {
            (void)part;
            active_history[level][pid] = true;
        }
        ++level;
    }
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

    // ── Algorithm 2: sampling + Hoeffding LB + L_k candidate selection ───
    Timer sample_timer;
    sample_timer.reset();
    const SampleResult sample = uniform_sample_and_select(
        dataset,
        cfg.fa_capacity,
        k,
        cfg.agg_func,
        cfg.sample_frac,
        cfg.delta,
        cfg.alpha_ci,
        cfg.beta_ci,
        42);
    metrics.sample_duration_ms = sample_timer.elapsed_ms();
    metrics.sample_size_actual = sample.sample_size_actual;
    metrics.fa_candidates_count = sample.fa_groups.size();
    metrics.is_optimizable = sample.is_optimizable;
    metrics.l_k_lower_bound = sample.l_k_lower_bound;
    metrics.cs_above_lk = sample.cs_above_lk;

    if (!sample.is_optimizable) {
        // Algorithm 1 line 14: fall back to baseline aggregation.
        out_results = run_brute_force(dataset, k, cfg.agg_func);
        metrics.total_duration_ms = total_timer.elapsed_ms();
        return metrics;
    }

    // ── Pass 1: FA / CA routing ──────────────────────────────────────────
    FATable fa(cfg.fa_capacity);
    for (uint64_t gid : sample.fa_groups) fa.insert(gid);

    out_fa_groups = fa.all_group_ids();
    std::sort(out_fa_groups.begin(), out_fa_groups.end());

    CATable ca(cfg.n_partitions);

    Timer pass1_timer;
    pass1_timer.reset();
    {
        constexpr size_t PREFETCH_DIST = 16;
        const size_t n = dataset.size();
        for (size_t i = 0; i < n; ++i) {
            if (i + PREFETCH_DIST < n)
                fa.prefetch(dataset[i + PREFETCH_DIST].group_id);
            const auto& row = dataset[i];
            assert(row.group_id != FA_EMPTY_KEY && "group_id must not equal FA sentinel key");
            if (fa.contains(row.group_id)) {
                fa.update(row.group_id, row.value);
            } else {
                ca.update_with_segment(row.group_id, row.value, i, cfg.segment_size);
            }
        }
    }

    // Pass 1 top-K bound: K-th highest among {FA exact values} ∪ {partition UBs}
    // (Algorithm 4 line 12 — paper / patent require the union, not FA-only).
    std::vector<double> pass1_union;
    pass1_union.reserve(fa.size() + cfg.n_partitions);
    {
        const auto fa_all = fa.all_aggregates(cfg.agg_func);
        for (const auto& [gid, val] : fa_all) {
            (void)gid;
            pass1_union.push_back(val);
        }
        for (size_t pid = 0; pid < cfg.n_partitions; ++pid) {
            const CAPartition& part = ca.partition(pid);
            if (part.count > 0) pass1_union.push_back(part.upper_bound(cfg.agg_func));
        }
    }
    const double topKBound = kth_highest_or_zero(pass1_union, k_size);

    ca.prune(topKBound, cfg.agg_func);

    metrics.pass1_duration_ms = pass1_timer.elapsed_ms();
    metrics.topKBound_after_pass1 = topKBound;
    metrics.partitions_pruned_pct = ca.pruning_fraction();
    metrics.total_passes = 1;

    ExactAggregates exact_aggregates;
    exact_aggregates.reserve(std::max<size_t>(fa.size() * 2, 1));
    for (uint64_t gid : out_fa_groups) {
        ExactAccum acc;
        acc.sum   = fa.get(gid, AggFunc::SUM);
        acc.count = static_cast<uint64_t>(fa.get(gid, AggFunc::COUNT));
        acc.max_v = fa.get(gid, AggFunc::MAX);
        acc.min_v = fa.get(gid, AggFunc::MIN);
        exact_aggregates[gid] = acc;
    }

    std::vector<std::vector<bool>> active_history(2, std::vector<bool>(cfg.n_partitions, false));
    ChildPartitions active_partitions;
    for (size_t pid : ca.ranked_surviving_partitions(cfg.agg_func)) {
        active_history[1][pid] = true;
        active_partitions.emplace(pid, ca.partition(pid));
    }



    // ── Pass 2+: MergeAndPrune + adaptive partitioning ───────────────────
    run_multipass_loop(dataset, fa, exact_aggregates, active_partitions,
                       active_history, cfg, k_size, metrics, "baseline");

    out_results = top_k_from_exact(exact_aggregates, k_size, cfg.agg_func);
    if (!out_fa_groups.empty() && !out_results.empty()) {
        std::unordered_set<uint64_t> fa_set(out_fa_groups.begin(), out_fa_groups.end());
        size_t hits = 0;
        for (const auto& [gid, val] : out_results) {
            if (fa_set.count(gid)) ++hits;
        }
        metrics.fa_hit_rate = static_cast<double>(hits)
                            / static_cast<double>(out_results.size());
    }
    metrics.total_duration_ms = total_timer.elapsed_ms();

    return metrics;
}

// ── Extension A: stratified sampling via GroupOccurrenceIndex ───────────────
// Phase 5 implementation.
//
// Differences from run_zippy_baseline (only the sampling step changes):
//   1. Build GroupOccurrenceIndex in one pre-pass (index_build_duration_ms).
//   2. Call stratified_sample_and_select() instead of uniform_sample_and_select().
// From Pass 1 onward, the pipeline is IDENTICAL to run_zippy_baseline.
//
// Correctness guarantee: unchanged — pruning is always safe via partition UBs.
RunMetrics run_zippy_ext_a(
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

    // ── Step 1: Build GroupOccurrenceIndex (one pre-pass) ───────────────────
    Timer index_timer;
    index_timer.reset();

    GroupOccurrenceIndex group_index;
    group_index.build(dataset);

    metrics.index_build_duration_ms = index_timer.elapsed_ms();

    // ── Step 2: Stratified sampling (Algorithm 2, Extension A variant) ──────
    Timer sample_timer;
    sample_timer.reset();

    const SampleResult sample = stratified_sample_and_select(
        dataset,
        group_index,
        cfg.fa_capacity,
        k,
        cfg.agg_func,
        cfg.sample_frac,
        cfg.delta,
        cfg.alpha_ci,
        cfg.beta_ci,
        cfg.underrep_threshold,
        cfg.boost_rows,
        42);

    metrics.sample_duration_ms  = sample_timer.elapsed_ms();
    metrics.sample_size_actual  = sample.sample_size_actual;
    metrics.fa_candidates_count = sample.fa_groups.size();
    metrics.is_optimizable      = sample.is_optimizable;
    metrics.l_k_lower_bound     = sample.l_k_lower_bound;
    metrics.cs_above_lk         = sample.cs_above_lk;

    if (!sample.is_optimizable) {
        out_results = run_brute_force(dataset, k, cfg.agg_func);
        metrics.total_duration_ms = total_timer.elapsed_ms();
        return metrics;
    }

    // ── Steps 3–end: Identical to run_zippy_baseline from Pass 1 onward ─────

    FATable fa(cfg.fa_capacity);
    for (uint64_t gid : sample.fa_groups) fa.insert(gid);

    out_fa_groups = fa.all_group_ids();
    std::sort(out_fa_groups.begin(), out_fa_groups.end());

    CATable ca(cfg.n_partitions);

    Timer pass1_timer;
    pass1_timer.reset();
    {
        constexpr size_t PREFETCH_DIST = 16;
        const size_t n = dataset.size();
        for (size_t i = 0; i < n; ++i) {
            if (i + PREFETCH_DIST < n)
                fa.prefetch(dataset[i + PREFETCH_DIST].group_id);
            const auto& row = dataset[i];
            assert(row.group_id != FA_EMPTY_KEY && "group_id must not equal FA sentinel key");
            if (fa.contains(row.group_id)) {
                fa.update(row.group_id, row.value);
            } else {
                ca.update_with_segment(row.group_id, row.value, i, cfg.segment_size);
            }
        }
    }

    // Pass 1 top-K bound: K-th highest among {FA exact values} ∪ {partition UBs}
    std::vector<double> pass1_union;
    pass1_union.reserve(fa.size() + cfg.n_partitions);
    {
        const auto fa_all = fa.all_aggregates(cfg.agg_func);
        for (const auto& [gid, val] : fa_all) {
            (void)gid;
            pass1_union.push_back(val);
        }
        for (size_t pid = 0; pid < cfg.n_partitions; ++pid) {
            const CAPartition& part = ca.partition(pid);
            if (part.count > 0) pass1_union.push_back(part.upper_bound(cfg.agg_func));
        }
    }
    const double topKBound_ea = kth_highest_or_zero(pass1_union, k_size);

    ca.prune(topKBound_ea, cfg.agg_func);

    metrics.pass1_duration_ms     = pass1_timer.elapsed_ms();
    metrics.topKBound_after_pass1 = topKBound_ea;
    metrics.partitions_pruned_pct = ca.pruning_fraction();
    metrics.total_passes = 1;

    ExactAggregates exact_aggregates_ea;
    exact_aggregates_ea.reserve(std::max<size_t>(fa.size() * 2, 1));
    for (uint64_t gid : out_fa_groups) {
        ExactAccum acc;
        acc.sum   = fa.get(gid, AggFunc::SUM);
        acc.count = static_cast<uint64_t>(fa.get(gid, AggFunc::COUNT));
        acc.max_v = fa.get(gid, AggFunc::MAX);
        acc.min_v = fa.get(gid, AggFunc::MIN);
        exact_aggregates_ea[gid] = acc;
    }

    std::vector<std::vector<bool>> active_history_ea(2, std::vector<bool>(cfg.n_partitions, false));
    ChildPartitions active_partitions_ea;
    for (size_t pid : ca.ranked_surviving_partitions(cfg.agg_func)) {
        active_history_ea[1][pid] = true;
        active_partitions_ea.emplace(pid, ca.partition(pid));
    }

    // Pass 2+: use shared multipass helper.
    run_multipass_loop(dataset, fa, exact_aggregates_ea, active_partitions_ea,
                       active_history_ea, cfg, k_size, metrics, "ext-a");

    out_results = top_k_from_exact(exact_aggregates_ea, k_size, cfg.agg_func);
    if (!out_fa_groups.empty() && !out_results.empty()) {
        std::unordered_set<uint64_t> fa_set(out_fa_groups.begin(), out_fa_groups.end());
        size_t hits = 0;
        for (const auto& [gid, val] : out_results) {
            if (fa_set.count(gid)) ++hits;
        }
        metrics.fa_hit_rate = static_cast<double>(hits)
                            / static_cast<double>(out_results.size());
    }
    metrics.total_duration_ms = total_timer.elapsed_ms();

    return metrics;
}

RunMetrics run_zippy_ext_b(
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

    // ── Extension B: Build Measure Index (Pre-Pass) ───────────
    Timer index_timer;
    index_timer.reset();
    std::unordered_set<uint64_t> extreme_groups = build_measure_index(dataset, cfg.measure_m);
    metrics.index_build_duration_ms = index_timer.elapsed_ms();

    // ── Algorithm 2: Sampling + Injection ─────────────────────
    Timer sample_timer;
    sample_timer.reset();
    const SampleResult sample = uniform_sample_and_select(
        dataset,
        cfg.fa_capacity,
        k,
        cfg.agg_func,
        cfg.sample_frac,
        cfg.delta,
        cfg.alpha_ci,
        cfg.beta_ci,
        42,
        extreme_groups); // <-- Pass the extreme groups here!
    
    metrics.sample_duration_ms = sample_timer.elapsed_ms();
    
    metrics.sample_size_actual = sample.sample_size_actual;
    metrics.fa_candidates_count = sample.fa_groups.size();
    metrics.is_optimizable = sample.is_optimizable;
    metrics.l_k_lower_bound = sample.l_k_lower_bound;
    metrics.cs_above_lk = sample.cs_above_lk;

    if (!sample.is_optimizable) {
        // Algorithm 1 line 14: fall back to baseline aggregation.
        out_results = run_brute_force(dataset, k, cfg.agg_func);
        metrics.total_duration_ms = total_timer.elapsed_ms();
        return metrics;
    }

    // ── Pass 1: FA / CA routing ──────────────────────────────────────────
    FATable fa(cfg.fa_capacity);
    for (uint64_t gid : sample.fa_groups) fa.insert(gid);

    out_fa_groups = fa.all_group_ids();
    std::sort(out_fa_groups.begin(), out_fa_groups.end());

    CATable ca(cfg.n_partitions);

    Timer pass1_timer;
    pass1_timer.reset();
    {
        size_t row_idx = 0;
        for (const auto& row : dataset) {
            assert(row.group_id != FA_EMPTY_KEY && "group_id must not equal FA sentinel key");
            if (fa.contains(row.group_id)) {
                fa.update(row.group_id, row.value);
            } else {
                ca.update_with_segment(row.group_id, row.value, row_idx, cfg.segment_size);
            }
            ++row_idx;
        }
    }

    // Pass 1 top-K bound: K-th highest among {FA exact values} ∪ {partition UBs}
    // (Algorithm 4 line 12 — paper / patent require the union, not FA-only).
    std::vector<double> pass1_union;
    pass1_union.reserve(fa.size() + cfg.n_partitions);
    {
        const auto fa_all = fa.all_aggregates(cfg.agg_func);
        for (const auto& [gid, val] : fa_all) {
            (void)gid;
            pass1_union.push_back(val);
        }
        for (size_t pid = 0; pid < cfg.n_partitions; ++pid) {
            const CAPartition& part = ca.partition(pid);
            if (part.count > 0) pass1_union.push_back(part.upper_bound(cfg.agg_func));
        }
    }
    const double topKBound = kth_highest_or_zero(pass1_union, k_size);

    ca.prune(topKBound, cfg.agg_func);

    metrics.pass1_duration_ms = pass1_timer.elapsed_ms();
    metrics.topKBound_after_pass1 = topKBound;
    metrics.partitions_pruned_pct = ca.pruning_fraction();
    metrics.total_passes = 1;

    ExactAggregates exact_aggregates;
    exact_aggregates.reserve(std::max<size_t>(fa.size() * 2, 1));
    for (uint64_t gid : out_fa_groups) {
        ExactAccum acc;
        acc.sum   = fa.get(gid, AggFunc::SUM);
        acc.count = static_cast<uint64_t>(fa.get(gid, AggFunc::COUNT));
        acc.max_v = fa.get(gid, AggFunc::MAX);
        acc.min_v = fa.get(gid, AggFunc::MIN);
        exact_aggregates[gid] = acc;
    }

    std::vector<std::vector<bool>> active_history(2, std::vector<bool>(cfg.n_partitions, false));
    ChildPartitions active_partitions;
    for (size_t pid : ca.ranked_surviving_partitions(cfg.agg_func)) {
        active_history[1][pid] = true;
        active_partitions.emplace(pid, ca.partition(pid));
    }

    if (cfg.verbose) {
        std::fprintf(stderr,
                     "[ext-b-pass1] agg=%s topKBound=%.6f pruned=%.2f%% "
                     "survivors=%zu L_k=%.6f Cs=%zu\n",
                     agg_func_name(cfg.agg_func),
                     metrics.topKBound_after_pass1,
                     metrics.partitions_pruned_pct * 100.0,
                     active_partitions.size(),
                     metrics.l_k_lower_bound,
                     metrics.cs_above_lk);
    }

    // ── Pass 2+: MergeAndPrune + adaptive partitioning ───────────────────
    run_multipass_loop(dataset, fa, exact_aggregates, active_partitions,
                       active_history, cfg, k_size, metrics, "ext-b");

    out_results = top_k_from_exact(exact_aggregates, k_size, cfg.agg_func);
    if (!out_fa_groups.empty() && !out_results.empty()) {
        std::unordered_set<uint64_t> fa_set(out_fa_groups.begin(), out_fa_groups.end());
        size_t hits = 0;
        for (const auto& [gid, val] : out_results) {
            if (fa_set.count(gid)) ++hits;
        }
        metrics.fa_hit_rate = static_cast<double>(hits)
                            / static_cast<double>(out_results.size());
    }
    metrics.total_duration_ms = total_timer.elapsed_ms();

    return metrics;
}

// ── Extension A+B: stratified sampling + measure-index injection ─────────────
// Combines Extension A (GroupOccurrenceIndex for stratified sampling) with
// Extension B (MeasureIndex extreme-group injection into FA candidates).
RunMetrics run_zippy_ext_ab(
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

    Timer index_timer;
    index_timer.reset();

    // Phase 1 (Extension A): build GroupOccurrenceIndex for stratified sampling.
    GroupOccurrenceIndex group_index;
    group_index.build(dataset);

    // Phase 2 (Extension B): build MeasureIndex to identify extreme-value groups.
    std::unordered_set<uint64_t> extreme_groups = build_measure_index(dataset, cfg.measure_m);

    metrics.index_build_duration_ms = index_timer.elapsed_ms();

    // Phase 3: stratified sample with extreme groups pre-injected into FA.
    Timer sample_timer;
    sample_timer.reset();
    const SampleResult sample = stratified_sample_and_select(
        dataset,
        group_index,
        cfg.fa_capacity,
        k,
        cfg.agg_func,
        cfg.sample_frac,
        cfg.delta,
        cfg.alpha_ci,
        cfg.beta_ci,
        cfg.underrep_threshold,
        cfg.boost_rows,
        42,
        extreme_groups);
    metrics.sample_duration_ms   = sample_timer.elapsed_ms();
    metrics.sample_size_actual   = sample.sample_size_actual;
    metrics.fa_candidates_count  = sample.fa_groups.size();
    metrics.is_optimizable       = sample.is_optimizable;
    metrics.l_k_lower_bound      = sample.l_k_lower_bound;
    metrics.cs_above_lk          = sample.cs_above_lk;

    if (!sample.is_optimizable) {
        out_results = run_brute_force(dataset, k, cfg.agg_func);
        metrics.total_duration_ms = total_timer.elapsed_ms();
        return metrics;
    }

    // Phases 4+: identical to baseline pipeline.
    FATable fa(cfg.fa_capacity);
    for (uint64_t gid : sample.fa_groups) fa.insert(gid);

    out_fa_groups = fa.all_group_ids();
    std::sort(out_fa_groups.begin(), out_fa_groups.end());

    CATable ca(cfg.n_partitions);

    Timer pass1_timer;
    pass1_timer.reset();
    {
        size_t row_idx = 0;
        for (const auto& row : dataset) {
            assert(row.group_id != FA_EMPTY_KEY && "group_id must not equal FA sentinel key");
            if (fa.contains(row.group_id)) {
                fa.update(row.group_id, row.value);
            } else {
                ca.update_with_segment(row.group_id, row.value, row_idx, cfg.segment_size);
            }
            ++row_idx;
        }
    }

    std::vector<double> pass1_union_ab;
    {
        const auto fa_all = fa.all_aggregates(cfg.agg_func);
        pass1_union_ab.reserve(fa_all.size() + cfg.n_partitions);
        for (const auto& [gid, val] : fa_all) {
            (void)gid;
            pass1_union_ab.push_back(val);
        }
        for (size_t pid = 0; pid < cfg.n_partitions; ++pid) {
            const CAPartition& part = ca.partition(pid);
            if (part.count > 0) pass1_union_ab.push_back(part.upper_bound(cfg.agg_func));
        }
    }
    const double topKBound_ab = kth_highest_or_zero(pass1_union_ab, k_size);

    ca.prune(topKBound_ab, cfg.agg_func);

    metrics.pass1_duration_ms       = pass1_timer.elapsed_ms();
    metrics.topKBound_after_pass1   = topKBound_ab;
    metrics.partitions_pruned_pct   = ca.pruning_fraction();
    metrics.total_passes            = 1;

    ExactAggregates exact_aggregates_ab;
    exact_aggregates_ab.reserve(std::max<size_t>(fa.size() * 2, 1));
    for (uint64_t gid : out_fa_groups) {
        ExactAccum acc;
        acc.sum   = fa.get(gid, AggFunc::SUM);
        acc.count = static_cast<uint64_t>(fa.get(gid, AggFunc::COUNT));
        acc.max_v = fa.get(gid, AggFunc::MAX);
        acc.min_v = fa.get(gid, AggFunc::MIN);
        exact_aggregates_ab[gid] = acc;
    }

    std::vector<std::vector<bool>> active_history_ab(2, std::vector<bool>(cfg.n_partitions, false));
    ChildPartitions active_partitions_ab;
    for (size_t pid : ca.ranked_surviving_partitions(cfg.agg_func)) {
        active_history_ab[1][pid] = true;
        active_partitions_ab.emplace(pid, ca.partition(pid));
    }

    run_multipass_loop(dataset, fa, exact_aggregates_ab, active_partitions_ab,
                       active_history_ab, cfg, k_size, metrics, "ext-ab");

    out_results = top_k_from_exact(exact_aggregates_ab, k_size, cfg.agg_func);
    if (!out_fa_groups.empty() && !out_results.empty()) {
        std::unordered_set<uint64_t> fa_set(out_fa_groups.begin(), out_fa_groups.end());
        size_t hits = 0;
        for (const auto& [gid, val] : out_results) {
            if (fa_set.count(gid)) ++hits;
        }
        metrics.fa_hit_rate = static_cast<double>(hits)
                            / static_cast<double>(out_results.size());
    }
    metrics.total_duration_ms = total_timer.elapsed_ms();

    return metrics;
}
