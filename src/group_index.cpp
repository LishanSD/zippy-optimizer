// group_index.cpp — GroupOccurrenceIndex implementation (Extension A).
//
// One sequential scan records every row position per group_id.
// The resulting index is used by StratifiedSampler to fetch boost rows for
// underrepresented groups.

#include "group_index.h"

#include <algorithm>
#include <cmath>

void GroupOccurrenceIndex::build(const std::vector<Row>& dataset) {
    index_.clear();
    total_rows_ = dataset.size();

    // Pre-reserve a reasonable bucket count to reduce rehashing.
    // Use sqrt(N) as a rough estimate of distinct groups (conservative).
    index_.reserve(static_cast<size_t>(std::sqrt(static_cast<double>(total_rows_))) + 1);

    for (size_t i = 0; i < dataset.size(); ++i) {
        index_[dataset[i].group_id].push_back(static_cast<uint64_t>(i));
    }
}

bool GroupOccurrenceIndex::is_underrepresented(
    uint64_t group_id,
    size_t   observed_count,
    size_t   sample_size,
    double   threshold) const
{
    if (total_rows_ == 0 || sample_size == 0 || threshold <= 0.0) return false;

    const auto it = index_.find(group_id);
    if (it == index_.end()) return false;

    const size_t group_size = it->second.size();

    // expected_count = (group_size / total_rows) × sample_size
    const double expected =
        (static_cast<double>(group_size) / static_cast<double>(total_rows_))
        * static_cast<double>(sample_size);

    // Group is underrepresented if observed < threshold × expected.
    // Avoid boosting groups where expected ≈ 0 (they are genuinely too rare
    // to matter regardless of threshold).
    if (expected < 0.5) return false;

    return static_cast<double>(observed_count) < threshold * expected;
}

std::vector<uint64_t> GroupOccurrenceIndex::get_boost_rows(
    uint64_t group_id, size_t n_boost) const
{
    const auto it = index_.find(group_id);
    if (it == index_.end()) return {};

    const auto& positions = it->second;
    const size_t take = std::min(n_boost, positions.size());
    return std::vector<uint64_t>(positions.begin(), positions.begin() + take);
}

size_t GroupOccurrenceIndex::row_count_for(uint64_t group_id) const {
    const auto it = index_.find(group_id);
    return (it == index_.end()) ? 0 : it->second.size();
}
