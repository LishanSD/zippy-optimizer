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
// Phase 3 will add: FAEntry, FATable, FMSketch, CAPartition, CATable
// ============================================================================
