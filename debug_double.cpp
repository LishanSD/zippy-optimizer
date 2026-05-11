#include "zippy.h"
#include <iostream>
#include <vector>

std::vector<Row> load_dataset(const std::string& path, size_t n_rows) {
    std::vector<Row> data(n_rows);
    FILE* f = fopen(path.c_str(), "rb");
    fread(data.data(), sizeof(Row), n_rows, f);
    fclose(f);
    return data;
}

int main() {
    auto dataset = load_dataset("data/S0.bin", 10089);
    ZippyConfig cfg;
    cfg.fa_capacity = 50;
    cfg.measure_m = 500;
    cfg.n_partitions = 50;
    cfg.verbose = true;
    std::vector<std::pair<uint64_t, double>> results;
    std::vector<uint64_t> fa_groups;
    run_zippy_ext_ab(dataset, 10, cfg, results, fa_groups);
    for (int i=0; i<10; ++i) std::cout << results[i].first << ": " << results[i].second << "\n";
}
