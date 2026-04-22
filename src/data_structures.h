#pragma once
#include <cstdint>
#include <cstddef>

// ── Sentinel value for empty FA slots — never a valid group_id ─────────────
static constexpr uint64_t FA_EMPTY_KEY = UINT64_MAX;

// ── Row struct — matches binary dataset format exactly ─────────────────────
// Each row is 16 bytes: (group_id: uint64_t, value: double), little-endian,
// no padding (both fields are 8-byte aligned).
struct Row {
    uint64_t group_id;  // 8 bytes
    double   value;     // 8 bytes
};

// ============================================================================
// Hash Functions
// ============================================================================
// Four independent hash functions to avoid correlation between subsystems.
// See AGENTS.md Section 6 "Hash Functions" for rationale.

// Hash 1: Partition assignment — maps group_id to a CA partition index.
// Fibonacci hashing with modulo — correct for any n_partitions value.
inline size_t partition_hash(uint64_t group_id, size_t n_partitions) {
    return static_cast<size_t>((group_id * 11400714819323198485ULL) % n_partitions);
}

// Hash 2: FA lookup — maps group_id to a slot in the FA table.
// Wang hash — different constant from partition_hash.
inline size_t fa_hash(uint64_t group_id, size_t table_size) {
    uint64_t x = group_id;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x = x ^ (x >> 31);
    return static_cast<size_t>(x % table_size);
}

// Hash 3: FM sketch — must differ from both above.
// MurmurHash3 finalizer — produces geometrically distributed trailing zeros.
inline uint64_t fm_hash(uint64_t group_id) {
    uint64_t x = group_id;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

// Hash 4: Child partition assignment in Pass 2+ — must differ from Hash 1
// so that child partitions subdivide parent partitions differently each pass.
// Pass number is mixed in to produce a different hash per pass.
inline size_t child_partition_hash(uint64_t group_id, size_t n_child_partitions,
                                    int pass_number) {
    uint64_t mixed = group_id ^ (static_cast<uint64_t>(pass_number) * 2654435761ULL);
    return static_cast<size_t>((mixed * 6364136223846793005ULL) % n_child_partitions);
}

// ============================================================================
// Portable trailing-zero count (needed by FMSketch in Phase 3)
// ============================================================================
#ifdef _MSC_VER
#include <intrin.h>
inline uint32_t portable_ctzll(uint64_t x) {
    unsigned long index;
    if (_BitScanForward64(&index, x)) return static_cast<uint32_t>(index);
    return 64;  // all bits zero
}
#else
inline uint32_t portable_ctzll(uint64_t x) {
    if (x == 0) return 64;
    return static_cast<uint32_t>(__builtin_ctzll(x));
}
#endif

// ============================================================================
// FAEntry + FATable — Fine-grained Aggregates (exact tracking)
// ============================================================================
// Single-level open-addressing hash table with linear probing.
// Patent: "single-level hash table with linear probing" to eliminate branching
// and chaining overhead. 50% load factor (array size = 2 × capacity).
// See AGENTS.md Section 6 "FATable" for full specification.

#include <vector>
#include <algorithm>
#include <limits>
#include <cassert>
#include <cmath>

struct FAEntry {
    uint64_t group_id = FA_EMPTY_KEY;  // FA_EMPTY_KEY means slot is vacant
    double   exact_sum = 0.0;          // running exact aggregate (SUM queries)
    // 16 bytes total — 4 entries fit per 64-byte cache line
};

class FATable {
    std::vector<FAEntry> table_;   // size = 2 * capacity_
    size_t capacity_;              // Cf = C/2: max groups FA can hold
    size_t occupied_ = 0;          // current number of non-empty entries

    // Linear probe starting position
    size_t probe(uint64_t group_id) const {
        return fa_hash(group_id, table_.size());
    }

public:
    explicit FATable(size_t capacity)
        : capacity_(capacity)
    {
        // 50% load factor: allocate 2× capacity, filled with sentinel
        table_.resize(2 * capacity_);
        // FAEntry default-constructs with group_id = FA_EMPTY_KEY
    }

    bool contains(uint64_t group_id) const {
        size_t idx = probe(group_id);
        for (size_t i = 0; i < table_.size(); ++i) {
            size_t pos = (idx + i) % table_.size();
            if (table_[pos].group_id == group_id) return true;
            if (table_[pos].group_id == FA_EMPTY_KEY) return false;
        }
        return false;  // table full, not found (should not happen at 50% load)
    }

    // Insert a group_id as an FA candidate (called during setup, before Pass 1).
    // Initialises exact_sum to 0. Idempotent: re-inserting an existing key is a no-op.
    void insert(uint64_t group_id) {
        assert(group_id != FA_EMPTY_KEY && "Cannot insert sentinel key");
        size_t idx = probe(group_id);
        for (size_t i = 0; i < table_.size(); ++i) {
            size_t pos = (idx + i) % table_.size();
            if (table_[pos].group_id == group_id) {
                return;  // already inserted (idempotent)
            }
            if (table_[pos].group_id == FA_EMPTY_KEY) {
                assert(occupied_ < capacity_ && "FA table is full");
                table_[pos].group_id = group_id;
                table_[pos].exact_sum = 0.0;
                ++occupied_;
                return;
            }
        }
    }

    // Update aggregate for a known FA group (called in Pass 1 hot loop).
    // Precondition: group_id was previously inserted via insert().
    void update(uint64_t group_id, double val) {
        size_t idx = probe(group_id);
        for (size_t i = 0; i < table_.size(); ++i) {
            size_t pos = (idx + i) % table_.size();
            if (table_[pos].group_id == group_id) {
                table_[pos].exact_sum += val;
                return;
            }
            if (table_[pos].group_id == FA_EMPTY_KEY) {
                return;  // not found — should not happen if insert() was called
            }
        }
    }

    double get(uint64_t group_id) const {
        size_t idx = probe(group_id);
        for (size_t i = 0; i < table_.size(); ++i) {
            size_t pos = (idx + i) % table_.size();
            if (table_[pos].group_id == group_id) return table_[pos].exact_sum;
            if (table_[pos].group_id == FA_EMPTY_KEY) return 0.0;
        }
        return 0.0;
    }

    // Return the top-k groups by exact_sum, sorted descending.
    std::vector<std::pair<uint64_t,double>> top_k(size_t k) const {
        // Collect all occupied entries
        std::vector<std::pair<uint64_t,double>> entries;
        entries.reserve(occupied_);
        for (const auto& e : table_) {
            if (e.group_id != FA_EMPTY_KEY) {
                entries.emplace_back(e.group_id, e.exact_sum);
            }
        }
        size_t n = std::min(k, entries.size());
        if (n > 0) {
            std::partial_sort(entries.begin(), entries.begin() + n, entries.end(),
                [](const auto& a, const auto& b) { return a.second > b.second; });
        }
        entries.resize(n);
        return entries;
    }

    // Return all occupied group_ids (for --output-fa-groups)
    std::vector<uint64_t> all_group_ids() const {
        std::vector<uint64_t> ids;
        ids.reserve(occupied_);
        for (const auto& e : table_) {
            if (e.group_id != FA_EMPTY_KEY)
                ids.push_back(e.group_id);
        }
        return ids;
    }

    size_t size()     const { return occupied_; }
    size_t capacity() const { return capacity_; }
    bool   full()     const { return occupied_ >= capacity_; }
};

// ============================================================================
// FMSketch — Flajolet-Martin approximate distinct count
// ============================================================================
// Patent: "The approximate distinct count is determined using the Flajolet
// Martin (FM) algorithm." Maintains a 64-bit bitmask; estimate ≈ 2^(lowest 0-bit).
// Uses fm_hash() (Hash 3) — independent of partition_hash (Hash 1).

struct FMSketch {
    // Stochastic averaging FM: split group_ids into NUM_BUCKETS buckets using
    // the top bits of the hash, track max trailing zeros in each bucket.
    // Estimate = NUM_BUCKETS × 2^(average max_trailing_zeros across buckets).
    // This dramatically reduces variance compared to a single sketch.
    static constexpr int NUM_BUCKETS = 32;
    static constexpr int BUCKET_BITS = 5;  // log2(32)

    uint8_t max_tz[NUM_BUCKETS] = {};  // max trailing zeros per bucket

    void update(uint64_t group_id) {
        uint64_t h = fm_hash(group_id);
        if (h == 0) return;
        // Use top bits for bucket assignment, remaining bits for trailing-zero count
        uint32_t bucket = static_cast<uint32_t>(h >> (64 - BUCKET_BITS)) & (NUM_BUCKETS - 1);
        uint64_t remaining = h | (1ULL << (64 - BUCKET_BITS));  // mask out bucket bits
        uint32_t tz = portable_ctzll(remaining);
        if (tz > max_tz[bucket])
            max_tz[bucket] = static_cast<uint8_t>(tz);
    }

    uint32_t estimate() const {
        // Average the max_tz values across non-empty buckets, then estimate = NUM_BUCKETS × 2^avg
        int active = 0;
        double sum_tz = 0.0;
        for (int i = 0; i < NUM_BUCKETS; ++i) {
            if (max_tz[i] > 0) {
                sum_tz += max_tz[i];
                ++active;
            }
        }
        if (active == 0) return 1;
        double avg_tz = sum_tz / active;
        double raw = static_cast<double>(active) * std::pow(2.0, avg_tz);
        if (raw > 2e9) return static_cast<uint32_t>(2e9);  // cap
        return std::max(1u, static_cast<uint32_t>(raw));
    }
};

// ============================================================================
// CAPartition + CATable — Coarse-grained Aggregates (partition-level tracking)
// ============================================================================

struct CAPartition {
    double   total_sum = 0.0;                                   // UB for SUM pruning
    double   max_value = std::numeric_limits<double>::lowest();  // UB for MAX pruning
    double   min_value = std::numeric_limits<double>::max();     // UB for MIN pruning
    uint64_t count     = 0;                                     // row count
    FMSketch fm;                                                 // approx distinct groups
    bool     pruned    = false;                                  // pruned flag

    // Estimated per-group aggregate (Algorithm 4 partition ranking).
    // Patent formula: total_sum / approx_distinct_count.
    double estimated_per_group_sum() const {
        uint32_t d = fm.estimate();
        return d == 0 ? total_sum : total_sum / static_cast<double>(d);
    }
};

class CATable {
    std::vector<CAPartition> partitions_;
    size_t n_partitions_;

public:
    explicit CATable(size_t n_partitions)
        : n_partitions_(n_partitions)
    {
        partitions_.resize(n_partitions_);
        // CAPartition default-constructs with safe zero/sentinel values
    }

    size_t n_partitions() const { return n_partitions_; }

    size_t partition_of(uint64_t group_id) const {
        return partition_hash(group_id, n_partitions_);
    }

    // Hot path: called for every non-FA row in Pass 1.
    // Updates all five fields of the target partition.
    void update(uint64_t group_id, double val) {
        size_t p = partition_of(group_id);
        CAPartition& part = partitions_[p];
        part.total_sum += val;
        if (val > part.max_value) part.max_value = val;
        if (val < part.min_value) part.min_value = val;
        part.count++;
        part.fm.update(group_id);
    }

    // Prune all partitions whose total_sum < topKBound.
    // Safe for SUM: total_sum is an upper bound on any group's aggregate.
    void prune(double topKBound) {
        for (auto& part : partitions_) {
            if (!part.pruned && part.total_sum < topKBound) {
                part.pruned = true;
            }
        }
    }

    // Return indices of surviving (non-pruned) partitions.
    std::vector<size_t> surviving_partitions() const {
        std::vector<size_t> survivors;
        for (size_t i = 0; i < n_partitions_; ++i) {
            if (!partitions_[i].pruned && partitions_[i].count > 0)
                survivors.push_back(i);
        }
        return survivors;
    }

    // Return surviving partitions sorted by estimated_per_group_sum() descending.
    // Algorithm 4 partition ranking: process highest-potential partitions first.
    std::vector<size_t> ranked_surviving_partitions() const {
        auto survivors = surviving_partitions();
        std::sort(survivors.begin(), survivors.end(),
            [this](size_t a, size_t b) {
                return partitions_[a].estimated_per_group_sum()
                     > partitions_[b].estimated_per_group_sum();
            });
        return survivors;
    }

    // Fraction of partitions that were pruned (for metrics).
    double pruning_fraction() const {
        if (n_partitions_ == 0) return 0.0;
        size_t pruned_count = 0;
        size_t active_count = 0;
        for (const auto& part : partitions_) {
            if (part.count > 0) {  // only count partitions that received data
                ++active_count;
                if (part.pruned) ++pruned_count;
            }
        }
        return active_count == 0 ? 0.0
             : static_cast<double>(pruned_count) / static_cast<double>(active_count);
    }

    // Direct access to partition data (for MergeAndPrune, Pass 2+ re-scan)
    const CAPartition& partition(size_t idx) const { return partitions_[idx]; }
    CAPartition& partition(size_t idx) { return partitions_[idx]; }
};
