// main.cpp — CLI entry point for the Zippy top-k optimizer
//
// Phase 1: argument parsing, dataset loading, mode dispatch skeleton.
// Algorithm implementations will be wired in during Phases 2–7.

#include "data_structures.h"
#include "utils.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>

// ---------------------------------------------------------------------------
// ZippyConfig — mirrors the full config from zippy.h (Section 6).
// Defined locally in Phase 1 to avoid depending on unimplemented headers.
// Will be moved to zippy.h in Phase 2.
// ---------------------------------------------------------------------------
struct ZippyConfig {
    size_t fa_capacity      = 50000;   // groups FA can hold
    size_t n_partitions     = 10000;   // CA logical partition count
    double sample_frac      = 0.01;    // uniform sample fraction
    double delta            = 0.05;    // sampling tolerance Δ
    double alpha_ci         = 0.05;    // CI confidence for sample size
    double beta_ci          = 0.95;    // Hoeffding CI confidence
    // Extension A
    double underrep_threshold = 0.5;
    size_t boost_rows         = 10;
    // Extension B
    size_t measure_m          = 500;
    // Output
    bool   output_fa_groups   = false;
    bool   verbose            = false;
};

int main(int argc, char* argv[]) {
    // 1. Parse arguments (hand-rolled — no getopt dependency)
    std::string input_path, output_path, mode = "baseline";
    int k = 50;
    size_t n_rows = 0;
    ZippyConfig cfg;

    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "--input"))        input_path  = argv[++i];
        else if (!strcmp(argv[i], "--output"))       output_path = argv[++i];
        else if (!strcmp(argv[i], "--mode"))         mode        = argv[++i];
        else if (!strcmp(argv[i], "--n-rows"))       n_rows      = std::stoull(argv[++i]);
        else if (!strcmp(argv[i], "--k"))            k           = std::stoi(argv[++i]);
        else if (!strcmp(argv[i], "--fa-capacity"))  cfg.fa_capacity   = std::stoull(argv[++i]);
        else if (!strcmp(argv[i], "--n-partitions")) cfg.n_partitions  = std::stoull(argv[++i]);
        else if (!strcmp(argv[i], "--sample-frac"))  cfg.sample_frac   = std::stod(argv[++i]);
        else if (!strcmp(argv[i], "--delta"))        cfg.delta         = std::stod(argv[++i]);
        else if (!strcmp(argv[i], "--alpha-ci"))     cfg.alpha_ci      = std::stod(argv[++i]);
        else if (!strcmp(argv[i], "--beta-ci"))      cfg.beta_ci       = std::stod(argv[++i]);
        else if (!strcmp(argv[i], "--underrep-threshold")) cfg.underrep_threshold = std::stod(argv[++i]);
        else if (!strcmp(argv[i], "--boost-rows"))   cfg.boost_rows    = std::stoull(argv[++i]);
        else if (!strcmp(argv[i], "--measure-m"))    cfg.measure_m     = std::stoull(argv[++i]);
        else if (!strcmp(argv[i], "--output-fa-groups")) cfg.output_fa_groups = true;
        else if (!strcmp(argv[i], "--verbose"))      cfg.verbose = true;
        else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return 1;
        }
    }

    if (input_path.empty() || output_path.empty() || n_rows == 0) {
        fprintf(stderr,
            "Usage: zippy --input <path> --n-rows <N> --k <k> "
            "--mode <mode> --output <path> [options]\n\n"
            "Modes: brute-force | baseline | ext-a | ext-b | ext-ab\n");
        return 1;
    }

    // 2. Load dataset (timing excluded from benchmarks)
    fprintf(stderr, "Loading %zu rows from %s ...\n", n_rows, input_path.c_str());
    std::vector<Row> dataset(n_rows);
    {
        FILE* f = fopen(input_path.c_str(), "rb");
        if (!f) {
            perror("fopen");
            fprintf(stderr, "Failed to open input file: %s\n", input_path.c_str());
            return 1;
        }
        size_t read = fread(dataset.data(), sizeof(Row), n_rows, f);
        fclose(f);
        if (read != n_rows) {
            fprintf(stderr, "Short read: expected %zu rows, got %zu\n", n_rows, read);
            return 1;
        }
    }
    fprintf(stderr, "Loaded %zu rows (%.1f MB). First row: group_id=%llu, value=%.4f\n",
            n_rows,
            static_cast<double>(n_rows * sizeof(Row)) / (1024.0 * 1024.0),
            (unsigned long long)dataset[0].group_id,
            dataset[0].value);

    // 3. Dispatch to the appropriate mode
    //    Phase 1: no algorithm is implemented yet — all modes print an error.
    //    Phase 2 will add brute-force; Phase 4 will add baseline; etc.
    std::vector<std::pair<uint64_t,double>> results;
    std::vector<uint64_t> fa_groups;
    RunMetrics metrics;

    if (mode == "brute-force") {
        fprintf(stderr, "ERROR: brute-force mode not yet implemented (Phase 2)\n");
        return 1;
    } else if (mode == "baseline") {
        fprintf(stderr, "ERROR: baseline mode not yet implemented (Phase 4)\n");
        return 1;
    } else if (mode == "ext-a") {
        fprintf(stderr, "ERROR: ext-a mode not yet implemented (Phase 5)\n");
        return 1;
    } else if (mode == "ext-b") {
        fprintf(stderr, "ERROR: ext-b mode not yet implemented (Phase 6)\n");
        return 1;
    } else if (mode == "ext-ab") {
        fprintf(stderr, "ERROR: ext-ab mode not yet implemented (Phase 7)\n");
        return 1;
    } else {
        fprintf(stderr, "Unknown mode: %s\n", mode.c_str());
        return 1;
    }

    // 4. Write output JSON (reached only when a mode is implemented)
    write_output_json(output_path, mode, k, n_rows,
                      /* n_groups estimate */ 0,
                      results, metrics, fa_groups, cfg.output_fa_groups);

    if (cfg.verbose) {
        fprintf(stderr, "Results written to %s\n", output_path.c_str());
    }
    return 0;
}
