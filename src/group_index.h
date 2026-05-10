#pragma once
// group_index.h — GroupOccurrenceIndex for Extension A (Stratified Sampling).
//
// Builds a single-pass index mapping each group_id to a list of its row
// positions in the dataset. Used by the stratified sampler to fetch additional
// rows for underrepresented groups (groups whose sampling proportion falls
// below UNDERREP_THRESHOLD × expected proportion).
//
// AGENTS.md §4 "Extension A" and §6 "GroupOccurrenceIndex" specify this API.

#include "data_structures.h"
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

class GroupOccurrenceIndex {
    // Maps group_id → sorted list of 0-based row positions in the dataset.
    // Built in one sequential scan — O(N) time and O(N) memory.
    std::unordered_map<uint64_t, std::vector<uint64_t>> index_;
    size_t total_rows_ = 0;

public:
    // Build the index from the dataset's group_id column.
    // Stores every row position for every group — one sequential scan.
    void build(const std::vector<Row>& dataset);

    // Returns true if the group was significantly underrepresented in the
    // uniform sample, i.e.:
    //   observed_count < threshold × expected_count
    // where expected_count = (group_size / total_rows) × sample_size.
    //
    // Parameters:
    //   group_id       — the group to test
    //   observed_count — rows for this group actually selected in uniform sample
    //   sample_size    — total rows selected in uniform sample
    //   threshold      — fraction of expected count below which we boost (e.g. 0.5)
    bool is_underrepresented(uint64_t group_id,
                             size_t   observed_count,
                             size_t   sample_size,
                             double   threshold) const;

    // Return up to n_boost row positions for a given group.
    // Returns the first min(n_boost, group_size) positions recorded.
    std::vector<uint64_t> get_boost_rows(uint64_t group_id, size_t n_boost) const;

    // Number of distinct groups in the index.
    size_t group_count() const { return index_.size(); }

    // Total rows in the indexed dataset.
    size_t total_rows() const { return total_rows_; }

    // Number of rows recorded for a specific group (0 if not present).
    size_t row_count_for(uint64_t group_id) const;

    // Iteration support — provides read-only access to the full index map.
    const std::unordered_map<uint64_t, std::vector<uint64_t>>& index() const {
        return index_;
    }
};
