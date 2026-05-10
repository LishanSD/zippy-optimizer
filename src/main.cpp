// main.cpp — CLI entry point for the Zippy top-k optimizer
//
// Argument parsing, dataset loading, and mode dispatch.
// Algorithm implementations are in zippy.cpp.

#include "zippy.h"
#include "utils.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>

// ZippyConfig is defined in zippy.h

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
        else if (!strcmp(argv[i], "--agg"))          cfg.agg_func = parse_agg_func(argv[++i]);
        else if (!strcmp(argv[i], "--fa-capacity"))  cfg.fa_capacity   = std::stoull(argv[++i]);
        else if (!strcmp(argv[i], "--n-partitions")) cfg.n_partitions  = std::stoull(argv[++i]);
        else if (!strcmp(argv[i], "--sample-frac"))  cfg.sample_frac   = std::stod(argv[++i]);
        else if (!strcmp(argv[i], "--delta"))        cfg.delta         = std::stod(argv[++i]);
        else if (!strcmp(argv[i], "--alpha-ci"))     cfg.alpha_ci      = std::stod(argv[++i]);
        else if (!strcmp(argv[i], "--beta-ci"))      cfg.beta_ci       = std::stod(argv[++i]);
        else if (!strcmp(argv[i], "--alpha-locality")) cfg.alpha_locality = std::stod(argv[++i]);
        else if (!strcmp(argv[i], "--segment-size"))  cfg.segment_size  = std::stoull(argv[++i]);
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
    std::vector<std::pair<uint64_t,double>> results;
    std::vector<uint64_t> fa_groups;
    RunMetrics metrics;
    cfg.output_fa_groups = (cfg.output_fa_groups);  // already parsed

    Timer total_timer;
    total_timer.reset();

    if (mode == "brute-force") {
        results = run_brute_force(dataset, k, cfg.agg_func);
        metrics.total_duration_ms = total_timer.elapsed_ms();
        metrics.is_optimizable = true;   // brute-force always "works"
    } else if (mode == "baseline") {
        metrics = run_zippy_baseline(dataset, k, cfg, results, fa_groups);
    } else if (mode == "ext-a") {
        metrics = run_zippy_ext_a(dataset, k, cfg, results, fa_groups);
    } else if (mode == "ext-b") {
        metrics = run_zippy_ext_b(dataset, k, cfg, results, fa_groups);
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
