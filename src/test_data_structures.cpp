// test_data_structures.cpp — Phase 3 gate: unit tests for FATable, FMSketch, CATable
//
// Compile: g++ -std=c++17 -O2 -o build/test_ds src/test_data_structures.cpp -Isrc/
// Run:     ./build/test_ds

#include "data_structures.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cassert>
#include <unordered_set>
#include <algorithm>

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); \
        ++tests_failed; \
    } else { \
        ++tests_passed; \
    } \
} while(0)

// ═══════════════════════════════════════════════════════════════════════════
// Test 1: FATable — insert, contains, update, get, top_k
// ═══════════════════════════════════════════════════════════════════════════
void test_fa_table() {
    fprintf(stderr, "\n[FATable Tests]\n");
    const size_t CAP = 1000;
    FATable fa(CAP);

    // Insert 1000 known group_ids (0..999)
    for (uint64_t i = 0; i < CAP; ++i) {
        fa.insert(i);
    }
    CHECK(fa.size() == CAP, "FA size should be 1000 after inserting 1000");
    CHECK(fa.full(), "FA should be full at capacity");

    // Verify contains() returns true for all inserted keys
    for (uint64_t i = 0; i < CAP; ++i) {
        CHECK(fa.contains(i), "FA should contain inserted key");
    }

    // Verify contains() returns false for 1000 non-inserted keys (1000..1999)
    bool any_false_positive = false;
    for (uint64_t i = CAP; i < 2 * CAP; ++i) {
        if (fa.contains(i)) { any_false_positive = true; break; }
    }
    CHECK(!any_false_positive, "FA should not contain non-inserted keys");

    // Update with known values: group i gets value (i+1) × 10.0, twice
    for (uint64_t i = 0; i < CAP; ++i) {
        fa.update(i, static_cast<double>(i + 1) * 10.0);
        fa.update(i, static_cast<double>(i + 1) * 10.0);
    }

    // Verify get() returns correct sums
    bool sums_correct = true;
    for (uint64_t i = 0; i < CAP; ++i) {
        double expected = static_cast<double>(i + 1) * 20.0;
        if (std::abs(fa.get(i) - expected) > 1e-9) {
            fprintf(stderr, "  FAIL: get(%llu) = %.2f, expected %.2f\n",
                    (unsigned long long)i, fa.get(i), expected);
            sums_correct = false;
            break;
        }
    }
    CHECK(sums_correct, "FA get() should return correct sums (2x value)");

    // Verify top_k(10) returns the correct 10 groups in descending order
    // Highest sums: group 999 → 20000, group 998 → 19980, ..., group 990 → 19820
    auto top10 = fa.top_k(10);
    CHECK(top10.size() == 10, "top_k(10) should return 10 entries");

    bool top10_correct = true;
    for (size_t j = 0; j < 10; ++j) {
        uint64_t expected_gid = CAP - 1 - j;  // 999, 998, ..., 990
        double   expected_sum = static_cast<double>(expected_gid + 1) * 20.0;
        if (top10[j].first != expected_gid || std::abs(top10[j].second - expected_sum) > 1e-9) {
            fprintf(stderr, "  FAIL: top_k[%zu] = (%llu, %.2f), expected (%llu, %.2f)\n",
                    j, (unsigned long long)top10[j].first, top10[j].second,
                    (unsigned long long)expected_gid, expected_sum);
            top10_correct = false;
            break;
        }
    }
    CHECK(top10_correct, "top_k(10) should return groups 999..990 in descending order");

    // Idempotent insert: re-inserting should not change size
    size_t before = fa.size();
    fa.insert(0);  // already exists
    CHECK(fa.size() == before, "Re-inserting existing key should not change size");

    // all_group_ids
    auto ids = fa.all_group_ids();
    CHECK(ids.size() == CAP, "all_group_ids should return 1000 IDs");

    fprintf(stderr, "  FATable: %d passed, %d failed\n", tests_passed, tests_failed);
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 2: FMSketch — approximate distinct count
// ═══════════════════════════════════════════════════════════════════════════
void test_fm_sketch() {
    fprintf(stderr, "\n[FMSketch Tests]\n");
    int fm_pass = 0, fm_fail = 0;

    // Test at various cardinalities: 10, 100, 1000, 10000
    size_t test_sizes[] = {10, 100, 1000, 10000};
    for (size_t n : test_sizes) {
        FMSketch fm;
        for (uint64_t i = 0; i < n; ++i) {
            fm.update(i);
        }
        uint32_t est = fm.estimate();
        double ratio = static_cast<double>(est) / static_cast<double>(n);

        // FM is very rough (single-sketch, high variance) —
        // accept within 10× (AGENTS.md: "order-of-magnitude is fine")
        bool ok = (ratio >= 0.1 && ratio <= 10.0);
        if (ok) {
            ++fm_pass;
            ++tests_passed;
        } else {
            ++fm_fail;
            ++tests_failed;
        }
        fprintf(stderr, "  n=%5zu  estimate=%6u  ratio=%.2f  %s\n",
                n, est, ratio, ok ? "OK" : "FAIL (outside 3x)");
    }

    // Edge case: empty sketch should return 1
    FMSketch empty;
    CHECK(empty.estimate() >= 1, "Empty FMSketch should estimate >= 1");

    fprintf(stderr, "  FMSketch: %d passed, %d failed (of FM-specific)\n", fm_pass, fm_fail);
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 3: CATable — update, prune, surviving/ranked partitions
// ═══════════════════════════════════════════════════════════════════════════
void test_ca_table() {
    fprintf(stderr, "\n[CATable Tests]\n");
    const size_t N_PART = 100;
    CATable ca(N_PART);

    CHECK(ca.n_partitions() == N_PART, "n_partitions should match constructor arg");

    // Route 10K rows with known group_ids and values.
    // Groups 0..99 with values = group_id * 1.0 (so group 99 has highest per-row value)
    for (uint64_t i = 0; i < 10000; ++i) {
        uint64_t gid = i % 100;         // 100 distinct groups
        double val = static_cast<double>(gid) * 1.0 + 1.0;  // value in [1, 100]
        ca.update(gid, val);
    }

    // Verify that surviving_partitions returns non-empty partitions
    auto survivors = ca.surviving_partitions();
    CHECK(survivors.size() > 0, "Should have surviving partitions after updates");
    CHECK(survivors.size() <= N_PART, "Survivors should not exceed total partitions");

    // Prune with a moderate threshold — some partitions should be pruned
    // First, find median total_sum to use as threshold
    std::vector<double> sums;
    for (size_t i = 0; i < N_PART; ++i) {
        if (ca.partition(i).count > 0)
            sums.push_back(ca.partition(i).total_sum);
    }
    std::sort(sums.begin(), sums.end());
    double median_sum = sums[sums.size() / 2];

    ca.prune(median_sum);
    auto after_prune = ca.surviving_partitions();
    CHECK(after_prune.size() < survivors.size(),
          "Pruning with median should reduce survivor count");
    CHECK(ca.pruning_fraction() > 0.0, "Pruning fraction should be > 0 after pruning");
    CHECK(ca.pruning_fraction() < 1.0, "Pruning fraction should be < 1 (not all pruned)");

    fprintf(stderr, "  Before prune: %zu survivors, after: %zu (pruned %.1f%%)\n",
            survivors.size(), after_prune.size(), ca.pruning_fraction() * 100.0);

    // Verify ranked_surviving_partitions returns descending estimated order
    auto ranked = ca.ranked_surviving_partitions();
    CHECK(ranked.size() == after_prune.size(), "Ranked should have same size as survivors");

    bool descending = true;
    for (size_t i = 1; i < ranked.size(); ++i) {
        if (ca.partition(ranked[i]).estimated_per_group_sum()
            > ca.partition(ranked[i-1]).estimated_per_group_sum()) {
            descending = false;
            break;
        }
    }
    CHECK(descending, "Ranked partitions should be in descending estimated order");

    // Verify pruned partitions are not in survivors
    for (size_t idx : after_prune) {
        CHECK(!ca.partition(idx).pruned, "Surviving partition should not be pruned");
    }

    fprintf(stderr, "  CATable: tests complete\n");
}

// ═══════════════════════════════════════════════════════════════════════════
int main() {
    fprintf(stderr, "=== Phase 3 Gate: Data Structure Unit Tests ===\n");

    test_fa_table();
    test_fm_sketch();
    test_ca_table();

    fprintf(stderr, "\n=== SUMMARY: %d passed, %d failed ===\n",
            tests_passed, tests_failed);

    if (tests_failed > 0) {
        fprintf(stderr, "GATE FAILED\n");
        return 1;
    }
    fprintf(stderr, "GATE PASSED\n");
    return 0;
}
