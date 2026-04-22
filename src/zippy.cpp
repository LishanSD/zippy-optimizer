// zippy.cpp — Core Zippy engine implementation.
//
// Phase 2: brute-force baseline only.
// Phase 4 will add run_zippy_baseline().
// Phases 5–7 will add extension modes.

#include "zippy.h"
#include <unordered_map>
#include <algorithm>
#include <cstdio>

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
