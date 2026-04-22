#pragma once
#include <chrono>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdint>

// ── Timer ──────────────────────────────────────────────────────────────────
struct Timer {
    using Clock = std::chrono::high_resolution_clock;
    std::chrono::time_point<Clock> start_;

    void   reset() { start_ = Clock::now(); }
    double elapsed_ms() const {
        auto end = Clock::now();
        return std::chrono::duration<double, std::milli>(end - start_).count();
    }
};

// ── Metrics collected per run ──────────────────────────────────────────────
struct RunMetrics {
    bool   is_optimizable       = true;
    int    total_passes         = 0;
    size_t sample_size_actual   = 0;
    size_t fa_candidates_count  = 0;
    double sample_duration_ms   = 0;
    double pass1_duration_ms    = 0;
    double pass2plus_duration_ms= 0;
    double total_duration_ms    = 0;
    double index_build_duration_ms = 0;
    double fa_hit_rate          = -1;   // -1 = not computed (needs --output-fa-groups)
    double topKBound_after_pass1= 0;
    double partitions_pruned_pct= 0;
};

// ── JSON writer (no external library) ─────────────────────────────────────
// Write the output JSON to `path`. All strings are plain ASCII — no escaping needed.
inline void write_output_json(
    const std::string& path,
    const std::string& mode,
    int k, size_t n_rows, size_t n_groups,
    const std::vector<std::pair<uint64_t,double>>& top_k_results,
    const RunMetrics& m,
    const std::vector<uint64_t>& fa_group_ids,   // empty if --output-fa-groups not set
    bool output_fa_groups)
{
    FILE* f = fopen(path.c_str(), "w");
    if (!f) { perror("fopen output"); return; }

    fprintf(f, "{\n");
    fprintf(f, "  \"mode\": \"%s\",\n", mode.c_str());
    fprintf(f, "  \"k\": %d,\n", k);
    fprintf(f, "  \"n_rows\": %zu,\n", n_rows);
    fprintf(f, "  \"n_groups\": %zu,\n", n_groups);

    fprintf(f, "  \"top_k_results\": [\n");
    for (size_t i = 0; i < top_k_results.size(); ++i) {
        fprintf(f, "    {\"group_id\": %llu, \"aggregate\": %.6f}%s\n",
                (unsigned long long)top_k_results[i].first,
                top_k_results[i].second,
                i + 1 < top_k_results.size() ? "," : "");
    }
    fprintf(f, "  ],\n");

    if (output_fa_groups && !fa_group_ids.empty()) {
        fprintf(f, "  \"fa_group_ids\": [");
        for (size_t i = 0; i < fa_group_ids.size(); ++i)
            fprintf(f, "%llu%s", (unsigned long long)fa_group_ids[i],
                    i + 1 < fa_group_ids.size() ? "," : "");
        fprintf(f, "],\n");
    }

    fprintf(f, "  \"metrics\": {\n");
    fprintf(f, "    \"is_optimizable\": %s,\n", m.is_optimizable ? "true" : "false");
    fprintf(f, "    \"total_passes\": %d,\n", m.total_passes);
    fprintf(f, "    \"sample_size_actual\": %zu,\n", m.sample_size_actual);
    fprintf(f, "    \"fa_candidates_count\": %zu,\n", m.fa_candidates_count);
    fprintf(f, "    \"sample_duration_ms\": %.3f,\n", m.sample_duration_ms);
    fprintf(f, "    \"pass1_duration_ms\": %.3f,\n", m.pass1_duration_ms);
    fprintf(f, "    \"pass2plus_duration_ms\": %.3f,\n", m.pass2plus_duration_ms);
    fprintf(f, "    \"total_duration_ms\": %.3f,\n", m.total_duration_ms);
    fprintf(f, "    \"index_build_duration_ms\": %.3f,\n", m.index_build_duration_ms);
    fprintf(f, "    \"fa_hit_rate\": %.6f,\n", m.fa_hit_rate);
    fprintf(f, "    \"topKBound_after_pass1\": %.6f,\n", m.topKBound_after_pass1);
    fprintf(f, "    \"partitions_pruned_pct\": %.6f\n", m.partitions_pruned_pct);
    fprintf(f, "  }\n}\n");
    fclose(f);
}
