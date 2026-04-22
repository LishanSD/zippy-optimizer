#include "zippy.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

bool load_dataset(const std::string& path, size_t n_rows, std::vector<Row>& out_dataset) {
    out_dataset.resize(n_rows);
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        std::perror("fopen");
        return false;
    }
    const size_t read = std::fread(out_dataset.data(), sizeof(Row), n_rows, f);
    std::fclose(f);
    return read == n_rows;
}

std::unordered_set<uint64_t> topk_set(const std::vector<std::pair<uint64_t, double>>& rows) {
    std::unordered_set<uint64_t> s;
    s.reserve(rows.size() * 2);
    for (const auto& [gid, _sum] : rows) {
        (void)_sum;
        s.insert(gid);
    }
    return s;
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string input_path = "data/S0.bin";
    size_t n_rows = 10089;
    int k = 10;

    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--input") && i + 1 < argc) input_path = argv[++i];
        else if (!std::strcmp(argv[i], "--n-rows") && i + 1 < argc) n_rows = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--k") && i + 1 < argc) k = std::atoi(argv[++i]);
    }

    std::vector<Row> dataset;
    if (!load_dataset(input_path, n_rows, dataset)) {
        std::fprintf(stderr, "FAIL: unable to load dataset %s (%zu rows)\n", input_path.c_str(), n_rows);
        return 1;
    }

    ZippyConfig cfg;
    cfg.verbose = false;

    std::vector<std::pair<uint64_t, double>> baseline_topk;
    std::vector<uint64_t> fa_groups;
    const RunMetrics metrics = run_zippy_baseline(dataset, k, cfg, baseline_topk, fa_groups);
    const auto brute_topk = run_brute_force(dataset, k);

    const auto baseline_set = topk_set(baseline_topk);
    const auto brute_set = topk_set(brute_topk);
    if (baseline_set != brute_set) {
        std::fprintf(stderr, "FAIL: baseline top-k set does not match brute-force\n");
        std::fprintf(stderr, "  Missing (%zu): ", brute_set.size() - baseline_set.size());
        for (uint64_t gid : brute_set) {
            if (baseline_set.count(gid) == 0) {
                std::fprintf(stderr, "%llu,", static_cast<unsigned long long>(gid));
            }
        }
        std::fprintf(stderr, "\n  Extra: ");
        for (uint64_t gid : baseline_set) {
            if (brute_set.count(gid) == 0) {
                std::fprintf(stderr, "%llu,", static_cast<unsigned long long>(gid));
            }
        }
        std::fprintf(stderr, "\n");
        return 1;
    }

    std::fprintf(stderr, "PASS: Phase 4C correctness gate passed\n");
    std::fprintf(stderr, "  input                  = %s\n", input_path.c_str());
    std::fprintf(stderr, "  n_rows                 = %zu\n", n_rows);
    std::fprintf(stderr, "  k                      = %d\n", k);
    std::fprintf(stderr, "  total_passes           = %d\n", metrics.total_passes);
    std::fprintf(stderr, "  partitions_pruned_pct  = %.2f%%\n", metrics.partitions_pruned_pct * 100.0);
    std::fprintf(stderr, "  topKBound_after_pass1  = %.6f\n", metrics.topKBound_after_pass1);
    return 0;
}
