#include "measure_index.h"

void MeasureIndexBuilder::observe(const Row& row) {
    if (m_ == 0) return;

    if (min_heap_.size() < m_) {
        min_heap_.push({row.value, row.group_id});
    } else if (row.value > min_heap_.top().first) {
        min_heap_.pop();
        min_heap_.push({row.value, row.group_id});
    }
}

std::unordered_set<uint64_t> MeasureIndexBuilder::finish() {
    std::unordered_set<uint64_t> extreme_groups;
    extreme_groups.reserve(min_heap_.size());
    while (!min_heap_.empty()) {
        extreme_groups.insert(min_heap_.top().second);
        min_heap_.pop();
    }
    return extreme_groups;
}

std::unordered_set<uint64_t> build_measure_index(
    const std::vector<Row>& dataset, 
    size_t m) 
{
    if (dataset.empty() || m == 0) return {};

    MeasureIndexBuilder builder(m);
    for (const auto& row : dataset) {
        builder.observe(row);
    }
    return builder.finish();
}
