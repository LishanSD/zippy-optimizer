// group_index.cpp — GroupOccurrenceIndex implementation (Extension A).
//
// One sequential scan records every row position per group_id.
// The resulting index is used by StratifiedSampler to fetch boost rows for
// underrepresented groups.

#include "group_index.h"

#include <algorithm>

void GroupOccurrenceIndex::reset(size_t total_rows, size_t estimated_groups) {
    entries_.clear();
    if (estimated_groups > 0) {
        entries_.reserve(estimated_groups);
    }
    total_rows_ = total_rows;
}

void GroupOccurrenceIndex::add_occurrence(
    uint64_t group_id,
    uint64_t row_position,
    size_t   max_positions_per_group)
{
    auto [it, inserted] = entries_.try_emplace(group_id);
    (void)inserted;
    GroupOccurrenceEntry& entry = it->second;
    ++entry.row_count;
    if (entry.positions.size() < max_positions_per_group) {
        entry.positions.push_back(row_position);
    }
}

void GroupOccurrenceIndex::build(
    const std::vector<Row>& dataset,
    size_t max_positions_per_group)
{
    total_rows_ = dataset.size();

    // Extension A is most useful on high-cardinality inputs, so under-reserving
    // here turns the index build into repeated full-table rehashes. We do not
    // know the exact distinct count without another pass; N/4 is a pragmatic
    // estimate for the high-cardinality benchmark shape and avoids the memory
    // cost of reserving one bucket per row.
    const size_t estimated_groups = std::max<size_t>(1, total_rows_ / 4);
    reset(total_rows_, estimated_groups);

    for (size_t i = 0; i < dataset.size(); ++i) {
        add_occurrence(dataset[i].group_id, static_cast<uint64_t>(i), max_positions_per_group);
    }
}

bool GroupOccurrenceIndex::is_underrepresented(
    uint64_t group_id,
    size_t   observed_count,
    size_t   sample_size,
    double   threshold) const
{
    if (total_rows_ == 0 || sample_size == 0 || threshold <= 0.0) return false;

    const size_t group_size = row_count_for(group_id);
    if (group_size == 0) return false;

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
    const auto it = entries_.find(group_id);
    if (it == entries_.end()) return {};

    const auto& positions = it->second.positions;
    const size_t take = std::min(n_boost, positions.size());
    return std::vector<uint64_t>(positions.begin(), positions.begin() + take);
}

size_t GroupOccurrenceIndex::row_count_for(uint64_t group_id) const {
    const auto it = entries_.find(group_id);
    return (it == entries_.end()) ? 0 : it->second.row_count;
}
