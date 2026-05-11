// test_phase7.cpp — Phase 7 correctness gate: Extension AB (combined mode).
//
// Verifies that run_zippy_ext_ab() produces a top-k result that matches
// run_brute_force() exactly on the S0 dataset, and checks several structural
// properties of the combined mode.
//
// Usage:
//   g++ -std=c++17 -O2 -Wall -Wextra \
//       -o build/test_phase7 \
//       src/test_phase7.cpp src/zippy.cpp src/sampler.cpp \
//       src/group_index.cpp src/stratified_sampler.cpp src/measure_index.cpp \
//       -Isrc/
//   ./build/test_phase7 --input data/S0.bin --n-rows 10089 --k 10
//
// All tests use SUM aggregation by default (the principal aggregate).
// Expected output: "N / N tests passed."

#include "zippy.h"
#include "utils.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

// ── Helpers ─────────────────────────────────────────────────────────────────

static std::vector<Row> load_dataset(const std::string& path, size_t n_rows) {
    std::vector<Row> data(n_rows);
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { perror("fopen"); return {}; }
    size_t rd = fread(data.data(), sizeof(Row), n_rows, f);
    fclose(f);
    if (rd != n_rows) {
        fprintf(stderr, "Short read: expected %zu, got %zu\n", n_rows, rd);
        return {};
    }
    return data;
}

// Extract group IDs from top-k results into a sorted set for comparison.
static std::set<uint64_t> group_id_set(
    const std::vector<std::pair<uint64_t, double>>& results)
{
    std::set<uint64_t> s;
    for (const auto& [gid, _] : results) s.insert(gid);
    return s;
}

// ── Test runner ──────────────────────────────────────────────────────────────

static int tests_passed = 0;
static int tests_total  = 0;

static void check(bool condition, const char* description) {
    ++tests_total;
    if (condition) {
        ++tests_passed;
        fprintf(stdout, "  [PASS] %s\n", description);
    } else {
        fprintf(stdout, "  [FAIL] %s\n", description);
    }
}

int main(int argc, char* argv[]) {
    // ── Parse arguments ──────────────────────────────────────────────────────
    std::string input_path;
    size_t n_rows = 0;
    int k = 10;

    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "--input"))  input_path = argv[++i];
        else if (!strcmp(argv[i], "--n-rows")) n_rows     = std::stoull(argv[++i]);
        else if (!strcmp(argv[i], "--k"))      k          = std::stoi(argv[++i]);
    }

    if (input_path.empty() || n_rows == 0) {
        fprintf(stderr,
            "Usage: test_phase7 --input <path> --n-rows <N> [--k <k>]\n");
        return 1;
    }

    fprintf(stdout, "Phase 7 correctness gate — loading %zu rows from %s ...\n",
            n_rows, input_path.c_str());

    const std::vector<Row> dataset = load_dataset(input_path, n_rows);
    if (dataset.empty()) return 1;

    fprintf(stdout, "Loaded %zu rows. Running tests with k=%d\n\n", n_rows, k);

    // ── Default config (generous FA capacity for small dataset) ─────────────
    ZippyConfig cfg;
    cfg.fa_capacity       = 200;
    cfg.n_partitions      = 50;
    cfg.sample_frac       = 0.05;
    cfg.delta             = 0.05;
    cfg.alpha_ci          = 0.05;
    cfg.beta_ci           = 0.95;
    cfg.underrep_threshold = 0.5;
    cfg.boost_rows        = 10;
    cfg.measure_m         = static_cast<size_t>(k) * 5;  // 5× k extreme rows
    cfg.agg_func          = AggFunc::SUM;
    cfg.verbose           = false;

    // ── Test 1: ext-ab top-k matches brute-force (SUM) ──────────────────────
    fprintf(stdout, "Test 1: ext-ab (SUM) matches brute-force\n");
    {
        std::vector<std::pair<uint64_t, double>> bf_results, ab_results;
        std::vector<uint64_t> fa_groups;

        bf_results = run_brute_force(dataset, k, AggFunc::SUM);
        RunMetrics m = run_zippy_ext_ab(dataset, k, cfg, ab_results, fa_groups);

        const auto bf_ids = group_id_set(bf_results);
        const auto ab_ids = group_id_set(ab_results);

        check(ab_results.size() == static_cast<size_t>(k),
              "ext-ab returns exactly k results");
        check(ab_ids == bf_ids,
              "ext-ab group IDs match brute-force exactly");
        check(m.is_optimizable,
              "ext-ab is_optimizable = true on skewed data");
        check(m.total_passes >= 1,
              "ext-ab completes in >= 1 pass");
        check(m.fa_candidates_count > 0,
              "ext-ab selects at least one FA candidate");
    }

    // ── Test 2: ext-ab top-k values match brute-force (SUM) ────────────────────
    // Note: ext-ab may pick different tied groups than baseline (both are correct
    // top-k subsets). We verify aggregate values match brute-force, not group IDs.
    fprintf(stdout, "\nTest 2: ext-ab (SUM) aggregate values match brute-force\n");
    {
        std::vector<std::pair<uint64_t, double>> bf_results, ab_results;
        std::vector<uint64_t> fa_groups;

        bf_results = run_brute_force(dataset, k, AggFunc::SUM);
        RunMetrics ab_m = run_zippy_ext_ab(dataset, k, cfg, ab_results, fa_groups);

        // Extract sorted aggregate value multisets for comparison.
        std::vector<double> bf_vals, ab_vals;
        for (const auto& [_, v] : bf_results) bf_vals.push_back(v);
        for (const auto& [_, v] : ab_results) ab_vals.push_back(v);
        std::sort(bf_vals.begin(), bf_vals.end(), std::greater<double>());
        std::sort(ab_vals.begin(), ab_vals.end(), std::greater<double>());

        bool vals_match = (bf_vals == ab_vals);
        check(vals_match,
              "ext-ab aggregate value multiset matches brute-force");
        check(ab_m.partitions_pruned_pct >= 0.0 &&
              ab_m.partitions_pruned_pct <= 1.0,
              "ext-ab partitions_pruned_pct in [0,1]");
    }

    // ── Test 3: ext-ab top-k matches brute-force (COUNT) ────────────────────
    fprintf(stdout, "\nTest 3: ext-ab (COUNT) matches brute-force\n");
    {
        ZippyConfig cfg_cnt = cfg;
        cfg_cnt.agg_func = AggFunc::COUNT;

        std::vector<std::pair<uint64_t, double>> bf_results, ab_results;
        std::vector<uint64_t> fa_groups;

        bf_results = run_brute_force(dataset, k, AggFunc::COUNT);
        run_zippy_ext_ab(dataset, k, cfg_cnt, ab_results, fa_groups);

        const auto bf_ids = group_id_set(bf_results);
        const auto ab_ids = group_id_set(ab_results);
        check(ab_ids == bf_ids,
              "ext-ab COUNT matches brute-force");
    }

    // ── Test 4: ext-ab top-k matches brute-force (MAX) ──────────────────────
    fprintf(stdout, "\nTest 4: ext-ab (MAX) matches brute-force\n");
    {
        ZippyConfig cfg_max = cfg;
        cfg_max.agg_func = AggFunc::MAX;

        std::vector<std::pair<uint64_t, double>> bf_results, ab_results;
        std::vector<uint64_t> fa_groups;

        bf_results = run_brute_force(dataset, k, AggFunc::MAX);
        run_zippy_ext_ab(dataset, k, cfg_max, ab_results, fa_groups);

        const auto bf_ids = group_id_set(bf_results);
        const auto ab_ids = group_id_set(ab_results);
        check(ab_ids == bf_ids,
              "ext-ab MAX matches brute-force");
    }

    // ── Test 5: forced-group injection visible in FA candidates ─────────────
    // With measure_m = k*5 and a small dataset, some forced groups should
    // appear in the FA candidate list returned via out_fa_groups.
    fprintf(stdout, "\nTest 5: forced groups appear in FA candidates\n");
    {
        ZippyConfig cfg_fg = cfg;
        cfg_fg.output_fa_groups = true;
        cfg_fg.measure_m = static_cast<size_t>(k) * 3;

        std::vector<std::pair<uint64_t, double>> ab_results;
        std::vector<uint64_t> out_fa;

        run_zippy_ext_ab(dataset, k, cfg_fg, ab_results, out_fa);

        check(!out_fa.empty(),
              "FA groups list is non-empty");
        check(out_fa.size() <= cfg_fg.fa_capacity,
              "FA groups list does not exceed fa_capacity");
    }

    // ── Test 6: Forced truncation — |FORCED_SET| saturates FA capacity ──────
    // measure_m = fa_capacity * 5 so MeasureIndex returns more distinct groups
    // than fa_capacity. The forced injection loop truncates at fa_capacity.
    // Since all FA slots are consumed by forced groups, remaining_capacity = 0
    // and the stratified phase is skipped. The pipeline must still converge.
    fprintf(stdout, "\nTest 6: ext-ab handles measure_m >> fa_capacity gracefully\n");
    {
        ZippyConfig cfg_trunc = cfg;
        // fa_capacity = 50 gives enough room for k=10 correct answers
        // but measure_m = 500 gives many more extreme groups than FA slots.
        cfg_trunc.fa_capacity = 50;
        cfg_trunc.measure_m   = 500;

        std::vector<std::pair<uint64_t, double>> bf_results, ab_results;
        std::vector<uint64_t> fa_groups;

        bf_results = run_brute_force(dataset, k, AggFunc::SUM);
        RunMetrics m = run_zippy_ext_ab(dataset, k, cfg_trunc, ab_results, fa_groups);

        // Extract sorted aggregate value multisets for comparison.
        std::vector<double> bf_vals, ab_vals;
        for (const auto& [_, v] : bf_results) bf_vals.push_back(v);
        for (const auto& [_, v] : ab_results) ab_vals.push_back(v);
        std::sort(bf_vals.begin(), bf_vals.end(), std::greater<double>());
        std::sort(ab_vals.begin(), ab_vals.end(), std::greater<double>());

        check(bf_vals == ab_vals,
              "ext-ab with measure_m >> fa_capacity still produces correct aggregate values");
        check(m.total_passes >= 1,
              "ext-ab with forced-fill FA converges");
    }

    // ── Summary ──────────────────────────────────────────────────────────────
    fprintf(stdout, "\n%d / %d tests passed.\n", tests_passed, tests_total);
    return (tests_passed == tests_total) ? 0 : 1;
}
