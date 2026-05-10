#pragma once
// measure_index.h — MeasureIndex stub for Extension B (Phase 6).
//
// A min-heap of size m tracking the m rows with the largest individual values.
// After a single scan, get_forced_candidates() returns the group IDs of those rows.
// These are force-injected into FA before normal sampling.
//
// Extension B is NOT yet implemented — this header is a compile-time scaffold
// so the build command includes measure_index.cpp without errors.

#include "data_structures.h"
#include <cstddef>
#include <cstdint>
#include <queue>
#include <unordered_set>
#include <utility>
#include <vector>

class MeasureIndex {
    using Entry = std::pair<double, uint64_t>;  // (value, group_id)
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> heap_;
    size_t m_;

public:
    explicit MeasureIndex(size_t m) : m_(m) {}

    // Call for each row during the index-build pass.
    void process(double value, uint64_t group_id) {
        if (heap_.size() < m_) {
            heap_.emplace(value, group_id);
        } else if (value > heap_.top().first) {
            heap_.pop();
            heap_.emplace(value, group_id);
        }
    }

    // After full scan: return group IDs of extreme-value rows.
    std::unordered_set<uint64_t> get_forced_candidates() const {
        std::unordered_set<uint64_t> result;
        // Copy heap to extract entries without destroying it.
        auto tmp = heap_;
        while (!tmp.empty()) {
            result.insert(tmp.top().second);
            tmp.pop();
        }
        return result;
    }

    size_t heap_size() const { return heap_.size(); }
};
