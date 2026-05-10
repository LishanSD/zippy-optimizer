#pragma once

#include "data_structures.h"
#include <vector>
#include <unordered_set>

// Builds Extension B's Measure Column Index.
// Scans the dataset in a single pass to find the top-m largest individual
// row values and returns the unique group_ids associated with them.
std::unordered_set<uint64_t> build_measure_index(
    const std::vector<Row>& dataset, 
    size_t m);