#pragma once

#include "data_structures.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <queue>
#include <vector>
#include <unordered_set>

class MeasureIndexBuilder {
    using HeapItem = std::pair<double, uint64_t>;
    using MinHeap =
        std::priority_queue<HeapItem, std::vector<HeapItem>, std::greater<HeapItem>>;

    size_t  m_ = 0;
    MinHeap min_heap_;

public:
    explicit MeasureIndexBuilder(size_t m) : m_(m) {}

    void observe(const Row& row);
    std::unordered_set<uint64_t> finish();
};

// Builds Extension B's Measure Column Index.
// Scans the dataset in a single pass to find the top-m largest individual
// row values and returns the unique group_ids associated with them.
std::unordered_set<uint64_t> build_measure_index(
    const std::vector<Row>& dataset, 
    size_t m);
