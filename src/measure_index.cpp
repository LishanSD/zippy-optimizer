#include "measure_index.h"
#include <queue>

std::unordered_set<uint64_t> build_measure_index(
    const std::vector<Row>& dataset, 
    size_t m) 
{
    std::unordered_set<uint64_t> extreme_groups;
    if (dataset.empty() || m == 0) return extreme_groups;

    // Min-heap: stores pairs of {row_value, group_id}
    // top() will always be the SMALLEST of the top-m values we've seen so far.
    using HeapItem = std::pair<double, uint64_t>;
    std::priority_queue<HeapItem, std::vector<HeapItem>, std::greater<HeapItem>> min_heap;

    for (const auto& row : dataset) {
        if (min_heap.size() < m) {
            min_heap.push({row.value, row.group_id});
        } else if (row.value > min_heap.top().first) {
            min_heap.pop();
            min_heap.push({row.value, row.group_id});
        }
    }

    // Extract the unique group IDs from the top-m rows
    extreme_groups.reserve(min_heap.size());
    while (!min_heap.empty()) {
        extreme_groups.insert(min_heap.top().second);
        min_heap.pop();
    }

    return extreme_groups;
}