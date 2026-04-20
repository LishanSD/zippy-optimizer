# AGENTS.md — Zippy Top-K Extension Project

This file is the single source of truth for any coding agent working on this project.
Read it fully before writing any code. It explains the original algorithm, the two
extensions being implemented, the complete project architecture, data structures,
invariants, and testing strategy.

---

## 1. Background: What Is Zippy?

**Paper:** "Cache-Efficient Top-k Aggregation over High Cardinality Large Datasets"
Siddiqui et al., PVLDB 17(4): 644–656, 2023. DOI: 10.14778/3636218.3636222

**No public codebase exists.** This project implements Zippy from scratch in C++,
then extends it. Do not search for an existing Zippy repository — there is none.

### The Problem Zippy Solves

A top-k aggregation query has the form:

```sql
SELECT salesperson, SUM(revenue)
FROM transactions
GROUP BY salesperson
ORDER BY SUM(revenue) DESC
LIMIT k
```

On large datasets (200M rows, 30M unique groups), the naive approach — compute exact
aggregates for every group, then pick the top k — causes massive CPU cache thrashing
because 30M groups cannot fit in L1/L2 cache simultaneously. Each cache miss is
~100× slower than a cache hit.

**Zippy's core insight:** Real-world data is heavily skewed (Zipf / power-law
distribution). A small fraction of groups holds the vast majority of aggregate value.
If we can identify those groups early, we can track them exactly in the fast cache
and handle everything else cheaply.

### Key Terminology

| Term | Definition |
|------|-----------|
| N | Total number of rows in the dataset |
| M | Number of unique groups |
| C | Cache capacity in number of groups (L1+L2 fit ~C groups) |
| k | Number of top results requested by the query |
| FA | Fine-grained Aggregates — exact tracker for candidate groups |
| CA | Coarse-grained Aggregates — rough tracker for non-candidate groups |
| topKBound | The k-th largest aggregate seen so far in FA; used as a pruning threshold |
| Candidate | A group identified by sampling as likely to appear in the final top-k |
| Partition | A bucket of non-candidate groups, hashed together in CA |
| Pruning | Eliminating an entire CA partition because its max possible aggregate < topKBound |

---

## 2. Zippy Algorithm — Full Detail

> **See Section 3 for the verbatim paper pseudocode (Algorithms 1–4).** The prose
> below explains the algorithm intuitively. When implementing, always refer to
> Section 3 as the authoritative specification.

### Phase 0: Uniform Random Sampling

Before any data pass, Zippy reads a small fraction (configurable, typically 1%) of
rows at random. It runs a mini-aggregation over this sample to estimate which groups
will likely rank in the top-k.

The output is a **candidate set**: the top `C_FA` groups by estimated aggregate in
the sample, where `C_FA` is the number of FA slots available (half the cache budget).

```
sample_fraction = 0.01   (1% of rows, configurable)
candidates = top_C_FA_groups( mini_aggregate(sample) )
```

**Critical constraint:** |candidates| ≤ C_FA. The FA structure must fit in cache.

### Phase 1: Full Data Scan — Populating FA and CA

Zippy makes one full sequential pass over all N rows. For each row `(group_id, value)`:

```
if group_id ∈ FA:
    FA[group_id].exact_sum += value
else:
    p = hash(group_id) % n_partitions
    CA[p].total_sum   += value
    CA[p].max_value    = max(CA[p].max_value, value)   # per-row max (not group max)
    CA[p].count       += 1
```

**FA structure** (hash table, cache-resident):
- Key: group_id
- Value: exact running aggregate (sum, count, max, or min depending on query)
- Capacity: C_FA entries (fixed at initialization)
- Lookup: O(1) open-addressing hash table

**CA structure** (array of partition structs, cache-resident):
- Indexed by `hash(group_id) % n_partitions`
- Each entry stores: `{total_sum, max_value, count}`
- `total_sum`: sum of all values routed to this partition
- `max_value`: maximum single value seen in this partition
- `count`: number of rows routed to this partition
- This is NOT a per-group tracker — multiple groups hash to the same partition

**Key invariant for SUM queries:**
For any group g in partition p, g's true total sum ≤ CA[p].total_sum.
This is because all values are non-negative and the partition sum is the sum of all
groups within it. This invariant is what makes pruning mathematically safe.

### Phase 1 End: Pruning Step

After the full scan:

```
topKBound = k-th largest value in FA (i.e., the smallest of the top-k FA entries)

for each partition p in CA:
    if CA[p].total_sum < topKBound:
        PRUNE partition p   # no group inside p can possibly be in the top-k
    else:
        KEEP partition p for further processing
```

**Why this is always safe:** If partition p's total_sum < topKBound, then every
group inside p has a true aggregate < topKBound, so none of them can be in the top-k.
No group is ever incorrectly eliminated.

**Why this is an efficiency improvement:** If the initial FA candidates are poor
(missing true top-k groups), topKBound is artificially low, fewer partitions get
pruned, and more multi-pass work is needed. Our extensions raise topKBound earlier.

### Phase 2+: Multi-Pass on Surviving Partitions

For each surviving partition:

**Option A — Logical partition:** The original data was not physically moved. Zippy
must re-scan the entire dataset but only process rows belonging to surviving
partitions. This adds another full pass.

**Option B — Physical partition:** During Pass 1, rows for surviving-probability
partitions were copied into dedicated memory regions. Pass 2 only reads those regions.

Zippy decides between logical and physical partitioning adaptively:
- If a partition is likely to be pruned → logical (don't waste memory copy effort)
- If a partition is likely to survive → physical (pay copy cost now, save re-scan later)

In Pass 2+, each individual group within surviving partitions is fully aggregated
and the true top-k is determined.

### When Zippy Falls Back to Brute Force

If the data is not skewed (uniform distribution), sampling doesn't identify useful
candidates, and topKBound stays low. Zippy detects this and falls back to standard
full-aggregation at the cost of ~10% overhead from the validation step.

---

## 3. Paper Algorithms — Verbatim from Source (Authoritative Reference)

These are the four algorithms exactly as given in the paper. They are the ground
truth for the C++ implementation. When any description in this document conflicts
with these algorithms, **these algorithms take precedence.**

Variable name mapping from paper → used in this document and codebase:

| Paper variable | Meaning |
|---------------|---------|
| K | k (number of top results) |
| N | n_rows (total rows) |
| M | n_groups (unique groups) |
| CF | fa_capacity (max FA entries, cache-sized) |
| C | total cache size in groups |
| Q | n_partitions (logical partitions CA can hold in cache) |
| X | grouping column(s) |
| Y | aggregate (measure) column |
| A | aggregation function (SUM, COUNT, MAX, MIN) |
| s | segment size (for locality calculation) |
| α₀ | locality constant threshold |
| α, β | confidence levels for CI bounds in sampling |
| Lk | K-th highest lower-bound in sample CI bounds |
| Cp | size of a specific partition being processed |
| Tc | lowest aggregated count among FA groups |
| E | estimated elements per logical partition = Cp / Q |

---

### Algorithm 1 — TopKAggregation (Overall Framework)

```
Input:  Input data (split into blocks of fixed size tuples), K, N,
        M (cardinality of grouping keys), CF (FA cache size),
        grouping column(s) X, Aggregation func A,
        Aggregate column Y, Total cache size C,
        Q (maximum number of logical partitions cache can hold)
        System constants: segment size s, locality constant α₀,
        confidence levels α, β
Output: Top K groups (key-aggregate pairs): top-k groups

Procedure TopKAggregation:
 1  top-k groups = {}           // maintains top K groups
 2  partitions = data           // partitions point to input data blocks in first pass
 3  isOptimizable, FAgroups, topkBound =
 4      validateAndIdentifyFAgroups(partitions)
 5      // step 1 in Sec 4.1.2, Algorithm 2
 6  if not isOptimizable:       // if true, run baseline
 7      exact-aggregates-all-groups = Perform Multi-pass Aggregation
 8          // (Section 3.2)
 9      top-k groups = Priority-queue-based top-k selection over
10          exact-aggregates-all-groups
11      return top-k groups
12  exactAggregates = {}        // exact aggregates in FA groups
13  While Size(top-k groups) < K:
14      for each unprocessed partition P-i in partitions:  // runs in parallel
15          partialAggregates-i, childPartitions-i
16              = AggregateAndPartition(P-i, FAgroups)
17              // step 2 (Sec 4.1.2) Algorithm 3
18      top-k groups, topkBound, exactAggregates, partitions
19          = MergeAndPrune(exactAggregates (from previous pass),
20              partialAggregates-i, childPartitions-i for all i)
21              // step 3 in Sec 4.1.2, Algorithm 4
22  Return top-k groups
```

**Implementation notes for Algorithm 1:**
- `partitions = data` in line 2 means the first "partition" is the entire dataset
  (no physical partitioning has happened yet). In subsequent passes, `partitions`
  refers only to surviving child partitions from the previous pass.
- The `while` loop (line 13) continues until K exact results are confirmed. In
  practice this usually terminates after 1–2 passes on skewed data.
- The `for each partition` loop (line 14) is marked parallel in the paper. In this
  project's prototype, **implement it sequentially** (see Section 16 — out of scope).
- `topkBound` returned by `validateAndIdentifyFAgroups` is the initial bound from
  sampling (the K-th highest lower confidence bound). It is updated each pass by
  `MergeAndPrune`.

---

### Algorithm 2 — ValidateAndIdentifyFAgroups (Sampling & Skew Validation)

```
Input:  partitions, and other relevant inputs listed in Algorithm 1
Output: IsOptimizable (is input amenable to Top-K Optimization),
        FAgroups (grouping keys of FA groups)

Procedure ValidateAndIdentifyFAgroups:
 1  // computing aggregates over a sample of data
 2  sampleSize = Compute sample size (Section 4.2)
 3  sampling probability, p = sampleSize / N
 4  for each partition-i in partitions (in parallel):
 5      partialSampleAggregates-i = {}
 6          // stores standard aggregates for each group in the sample
 7      For each tuple in the partition-i:
 8          selected = select the tuple with probability p
 9          if selected:
10              update partialSampleAggregates by aggregating this tuple
11  sampleAggregates = merge (partialSampleAggregates-i for all i)
12  // checking skew in sample aggregates using CI
13  sampleAggregatesBounds = compute CI bounds for each group
14      in sampleAggregates  // Section 4.2
15  Lk = Kth highest lower bound in sampleAggregatesBounds
16  tempGroups = {}
17  For each group in sampleAggregatesBounds:
18      If the lower bound of the group >= Lk, add the group to tempGroups
19  If size of tempGroups > CF:
20      isOptimizable = False  // number of candidate groups much larger
21          // than we can efficiently aggregate in FA
22      Return isOptimizable, {}
23  // otherwise we can optimize. If candidate groups smaller than FA,
24  // we can fill leftover space in FA with heavy hitters
25  FAgroups = tempGroups
26  While the size of FAgroups < CF:
27      g = select the group (not already in FAgroups) with the
28          highest sample count aggregate in sampleAggregates
29      Add g to FAgroups
30  Return isOptimizable, FAgroups
```

**Implementation notes for Algorithm 2:**
- **CI bounds (lines 13–14):** The paper uses confidence intervals computed from the
  sample to determine which groups are statistically likely to be in the top-k. The
  lower bound of a group's CI is a conservative estimate of its true aggregate. Only
  groups whose lower bound ≥ Lk (the K-th highest lower bound) become FA candidates.
  For the prototype, a simplified version is acceptable: use the raw sample aggregate
  as a point estimate (no CI), and select the top-CF groups by sample aggregate as
  FAgroups. Mark this simplification clearly in code comments.
- **isOptimizable = False (line 19–22):** Triggered when too many groups pass the CI
  threshold — meaning data is not skewed enough to benefit from FA/CA separation.
  The system falls back to full multi-pass aggregation. In the prototype, trigger this
  when the top-CF groups' sample aggregates are not meaningfully separated from the
  rest (e.g., when the CF-th and (CF+1)-th sample aggregates are within 5% of each
  other).
- **Heavy hitter fill (lines 26–29):** After CI-qualifying groups fill tempGroups,
  remaining FA slots are filled with the highest-frequency groups from the sample
  (by count, not by aggregate value). This is the paper's robustness mechanism.
- **Extension A modifies this algorithm.** The stratified sampler replaces lines 4–11
  with a two-phase sampling procedure that uses the GroupOccurrenceIndex to boost
  underrepresented groups. Lines 12–29 remain unchanged.
- **Extension B modifies this algorithm.** Before line 25, the MeasureIndex
  FORCED_SET is inserted into FAgroups first, then remaining slots are filled by
  the CI/heavy-hitter logic. If |FORCED_SET| > CF, truncate to CF (log a warning).

---

### Algorithm 3 — AggregateAndPartition (Per-Partition Worker)

```
Input:  a specific partition to be aggregated or repartitioned at a
        given worker thread: partition-i, FAgroups, size of partition: Cp
Output: partialAggregates-i, childPartitions-i

Procedure AggregateAndPartition:
 1  // first decide between exact aggregation, logical/physical partitioning
 2  if the size of distinct groups in partition-i < CF:  // few distinct groups
 3      exactAggregation = true
 4  else:  // check for locality of groups
 5      For each segment of size s in the partition-i:
 6          calculate ds (number of distinct groups) and cs (cardinality)
 7              for segment s
 8      Locality of groups, l = Σs ds/cs/t, where t is the number of segments
 9      if l < α₀:
10          exactAggregation = true  // there is sufficient locality
11  if not exactAggregation:
12      If logical partitioning was performed on partition-i in previous pass:
13          partitioning = physical  // logical did not prune previously
14      else:
15          Set Tc to the lowest aggregated count among FAgroups
16          Estimated number of elements per logical partition, E = Cp/Q
17          if E < Tc:
18              partitioning = logical
19          else:
20              partitioning = physical
21  for each tuple in the partition-i:
22      if exactAggregation or tuple.group in FAgroups:
23          update the partialAggregates-i by aggregating this tuple
24      else:
25          compute the partition hash for this tuple
26          If partition hash is not present in childPartitions:
27              Create a new partition in childPartitions-i
28          Update partitionAggregates corresponding to the partition hash
29          if partitioning == physical:
30              move the tuple to the corresponding partition in childPartitions-i
31              if cache-line size of the corresponding partition is full:
32                  store the partition tuples to memory using non-temporal
33                  instruction  // (Section 4.1.1)
34  Return partialAggregates-i, childPartitions-i
```

**Implementation notes for Algorithm 3:**
- **Locality check (lines 4–10):** Locality l measures how concentrated groups are
  within segments of the data. Low l (< α₀) means groups are already locally
  clustered, so exact aggregation is efficient (few cache misses). In the prototype,
  **skip the locality check** and always proceed to the partitioning decision
  (lines 11–20). This is a safe simplification — it means we may do unnecessary
  partitioning in some cases but never skip necessary partitioning.
- **exactAggregation path (lines 22–23):** When partition is small enough to fit
  in cache (distinct groups < CF), or locality is sufficient, aggregate all groups
  in this partition exactly into partialAggregates-i. FA groups are also aggregated
  here (their rows are not sent to childPartitions).
- **FA group rows (line 22):** Rows belonging to FAgroups are ALWAYS aggregated
  exactly into partialAggregates-i, regardless of exactAggregation flag.
- **Logical vs physical partitioning decision (lines 12–20):**
  - If the previous pass used logical partitioning for this partition and it was NOT
    pruned, upgrade to physical (line 13). This avoids repeated full re-scans.
  - Otherwise, compare E (estimated elements per child partition = Cp/Q) to Tc
    (lowest FA group count). If E < Tc, use logical; else physical.
  - In this project's prototype: **implement logical partitioning only** (see
    Section 16). Physical partitioning (lines 29–33) is out of scope.
- **childPartitions-i:** These become the `partitions` list for the next pass in
  Algorithm 1. Each child partition is identified by its hash value and contains
  the CA statistics (total_sum, max_value, count) for that hash bucket.

---

### Algorithm 4 — MergeAndPrune (Pruning & topKBound Update)

```
Input:  exactAggregates (from previous pass), and
        <partialAggregates, childPartitions> computed for all partitions
        in current pass
Output: FAaggregates, CAPartitions, topKBound

Procedure MergeAndPrune:
 1  exactAggregates = Merge partialAggregates across all partitions with
 2      matching grouping keys and append them to old exactAggregates
 3  childPartitionAggregates = Merge childPartitionAggregates across
 4      all partitions with matching childPartition hash
 5  For each childPartition:
 6      compute upper bound (UB) for groups using childPartitionAggregates
 7          (Section 4.1.2 step 3)
 8  topKBound = Find kth highest value among exactAggregates
 9      and upper bounds of childPartitions
10  top-k groups = Add all groups with exact aggregates value > topkBound
11  if Size(top-k groups) >= K:
12      Return top-k groups, +infinity, {}, {}  // we are done
13  partitions = Remove any partition in childPartitions with
14      UB less than topKBounds.  // unpruned partitions for next pass
15  If the number of partitions > worker threads:
16      partitions = Rank partitions using partition aggregates
17          // (Section 4.4)
18  Return top-k groups, topKBound, exactAggregates, partitions
```

**Implementation notes for Algorithm 4:**
- **Upper bound (UB) computation (lines 5–7):** For SUM queries, the UB of any
  group in a child partition = childPartition's `total_sum`. This is the key pruning
  invariant: since all values are non-negative, no single group within a partition
  can have a higher sum than the partition total.
- **topKBound computation (lines 8–9):** topKBound is the K-th highest value when
  considering both exact FA aggregates and the UBs of all surviving child partitions.
  Concretely: sort the union of {FA exact values} ∪ {partition UBs} descending;
  topKBound = the K-th element.
- **Early termination (lines 11–12):** If K groups have exact aggregates all
  exceeding topKBound, we are done. Return +infinity as the new topKBound to signal
  completion to Algorithm 1's while loop. In practice this means: once K FA groups
  are confirmed and all surviving partitions have UB < the K-th FA aggregate,
  terminate.
- **Partition ranking (lines 15–17):** If many partitions survive, rank them by
  their UB descending so the most promising partitions are processed first in the
  next pass. In the prototype with sequential processing, implement this ranking but
  it mainly affects pass ordering, not correctness.
- **Merging exactAggregates (lines 1–2):** partialAggregates-i contains the
  per-partition contributions to FA groups from the current pass. These must be
  merged (summed, for SUM queries) with the running exactAggregates carried over
  from previous passes. After merging, exactAggregates[g] holds the exact total for
  FA group g across all passes processed so far.
- **The returned `partitions`** feeds back into Algorithm 1 line 2 as the partition
  list for the next while-loop iteration.

---

## 4. Our Extensions — What We Are Adding

The original paper identifies two enhancements in Section 7 (Limitations and
Extensions) but does not implement or evaluate either. This project implements both.

### Why These Extensions Matter

**The inefficiency we are fixing:** Rare groups — groups with very few rows but
high aggregate values — are systematically underrepresented in uniform random
samples. For example:

```
Group A: 1,000,000 rows × $1    → total $1,000,000  (easy to sample)
Group B:         1 row  × $999,999 → total $999,999  (almost never sampled)
```

Group B belongs in the top-2 but uniform sampling will almost never select its
single row. As a result, Group B is absent from FA, its partition's total_sum still
exceeds topKBound (the partition is correctly kept), but topKBound is lower than it
would be if Group B were in FA. This causes fewer partitions to be pruned, requiring
more multi-pass work.

**This is an efficiency problem, not a correctness problem.** Pruning is always
mathematically safe via the total_sum invariant. The result is always correct.
The problem is unnecessary extra passes.

### Extension A: Stratified Sampling via Grouping Column Index

**Paper quote (Section 7):**
> "Adding indexes on the groups can help perform stratified sampling to have more
> coverage of rare groups."

**What to build:** A lightweight, single-pass group occurrence directory built as a
preprocessing step before the main sampling phase.

**Group Occurrence Index structure:**
```
GroupIndex: unordered_map<group_id, vector<row_position>>
```

Built in one sequential scan of the dataset. Records which row positions belong to
each group. This is the "index on grouping columns."

**Stratified Sampling Algorithm:**
```
Phase 1 (uniform): Sample s1_fraction of rows randomly → mini_aggregate → sample_counts
Phase 2 (stratified correction):
    For each group g in GroupIndex:
        expected_count = GroupIndex[g].size() / N * sample_size
        actual_count   = sample_counts.get(g, 0)
        if actual_count < UNDERREP_THRESHOLD * expected_count:
            # Group is underrepresented — fetch a few of its rows from the index
            fetch min(BOOST_ROWS, GroupIndex[g].size()) rows from GroupIndex[g]
            add them to the sample aggregate for group g
Merge Phase 1 and Phase 2 aggregates → pick top C_FA as FA candidates
```

**Key design parameters (tune experimentally):**
- `UNDERREP_THRESHOLD`: fraction below which a group is considered underrepresented
  (e.g., 0.5 means "if we got less than 50% of expected representation")
- `BOOST_ROWS`: how many rows to fetch from the index for an underrepresented group
  (e.g., 5–20 rows)
- `s1_fraction`: uniform sample fraction (default 0.01)

**Overhead:** One extra sequential scan to build the GroupIndex. Memory: O(N) for
storing all row positions (can be reduced with sampling of positions).

### Extension B: Measure Column Index for Extreme Value Detection

**Paper quote (Section 7):**
> "With indexes on measure columns, we may be able to identify tuples with extreme
> values and add the corresponding groups in the first aggregation pass to process
> them earlier."

**What to build:** A single-pass min-heap over the value column that identifies the
`m` rows with the largest individual values. Their group IDs are force-injected into
FA before normal sampling.

**Measure Column Index structure:**
```
MeasureIndex: min-heap of size m, storing (value, group_id) pairs
```

Built in one sequential scan. After the scan, the heap contains the m rows with
the highest individual values.

**Integration with Zippy:**
```
Step 1: Build MeasureIndex in one pass → extract top-m group IDs (call this FORCED_SET)
Step 2: Reserve |FORCED_SET| FA slots for FORCED_SET groups
Step 3: Fill remaining FA slots from normal uniform sampling candidates
Step 4: Run normal Zippy from Phase 1 onward
```

**Key design parameter:**
- `m`: how many extreme-value rows to look at (tune experimentally)
  - Too small: miss some rare high-value groups
  - Too large: waste FA slots on groups whose one extreme value doesn't represent
    their true total rank
  - Expected sweet spot: 3k to 10k (where k is the query parameter)

**Research question to answer in evaluation:** Plot m vs. (number of passes needed)
and m vs. (index build overhead). Find the knee of the curve.

---

## 5. Project Architecture

```
zippy-topk/
├── AGENTS.md                    ← this file
│
├── src/                         ← C++ core engine
│   ├── main.cpp                 ← CLI entry point, argument parsing
│   ├── zippy.h / zippy.cpp      ← baseline Zippy: FA, CA, Pass1, Pruning, Pass2+
│   ├── data_structures.h        ← FA hash table, CA partition struct definitions
│   ├── sampler.h / sampler.cpp  ← uniform random sampler (baseline)
│   ├── group_index.h / group_index.cpp    ← Extension A: GroupOccurrenceIndex
│   ├── stratified_sampler.h / .cpp        ← Extension A: stratified sampling logic
│   ├── measure_index.h / measure_index.cpp ← Extension B: min-heap MeasureIndex
│   └── utils.h                  ← timer, stats collection helpers
│
├── python/                      ← Python orchestration layer
│   ├── generate_data.py         ← synthetic dataset generator (Zipf + rare groups)
│   ├── run_experiments.py       ← sweeps parameters, calls C++ binary, collects results
│   ├── plot_results.py          ← matplotlib plots for the paper/report
│   └── verify_correctness.py   ← checks C++ output matches brute-force pandas groupby
│
├── data/                        ← generated datasets (gitignored if large)
│   └── .gitkeep
│
├── results/                     ← JSON output from experiments
│   └── .gitkeep
│
├── plots/                       ← PNG/PDF output from plot_results.py
│   └── .gitkeep
│
├── CMakeLists.txt               ← build configuration
└── README.md                    ← how to build and run
```

---

## 6. C++ Data Structures — Detailed Spec

### FATable (Fine-grained Aggregates)

```cpp
struct FAEntry {
    uint64_t group_id;
    double   exact_sum;    // for SUM queries (extend for COUNT/MAX/MIN)
    bool     occupied;
};

class FATable {
    // Open-addressing hash table, fixed capacity C_FA
    // Must fit in L2 cache: C_FA × sizeof(FAEntry) ≤ cache_budget / 2
    std::vector<FAEntry> table;
    size_t capacity;        // C_FA
    size_t size;            // current number of occupied entries

public:
    bool   contains(uint64_t group_id) const;
    void   insert(uint64_t group_id);            // called during candidate setup
    void   update(uint64_t group_id, double val); // called during Pass 1
    double get(uint64_t group_id) const;
    std::vector<std::pair<uint64_t,double>> top_k(size_t k) const;
};
```

### CATable (Coarse-grained Aggregates)

```cpp
struct CAPartition {
    double   total_sum;    // sum of ALL values routed to this partition
    double   max_value;    // maximum single value seen (not group max — row max)
    uint64_t count;        // number of rows routed here
    bool     pruned;       // set to true after pruning step
};

class CATable {
    std::vector<CAPartition> partitions;
    size_t n_partitions;

public:
    size_t partition_of(uint64_t group_id) const;  // hash(group_id) % n_partitions
    void   update(uint64_t group_id, double val);
    void   prune(double topKBound);                 // marks partitions as pruned
    std::vector<size_t> surviving_partitions() const;
    double pruning_fraction() const;               // for metrics collection
};
```

### GroupOccurrenceIndex (Extension A)

```cpp
class GroupOccurrenceIndex {
    // Maps group_id → list of row positions in the dataset
    std::unordered_map<uint64_t, std::vector<uint64_t>> index;

public:
    // Build in one sequential scan of the dataset
    void build(const std::vector<uint64_t>& group_ids);

    // Returns true if group is underrepresented in the current sample
    bool is_underrepresented(uint64_t group_id,
                             size_t   observed_count,
                             size_t   total_rows,
                             size_t   sample_size,
                             double   threshold) const;

    // Returns row positions to fetch for a given group
    std::vector<uint64_t> get_boost_rows(uint64_t group_id, size_t n_boost) const;

    size_t group_count() const;
    size_t row_count_for(uint64_t group_id) const;
};
```

### MeasureIndex (Extension B)

```cpp
class MeasureIndex {
    // Min-heap of (value, group_id), fixed size m
    // After build(), heap contains the m highest-value rows seen
    using Entry = std::pair<double, uint64_t>;  // (value, group_id)
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> heap;
    size_t m;  // max heap size

public:
    explicit MeasureIndex(size_t m);

    // Call for each row during the index-build pass
    void process(double value, uint64_t group_id);

    // After full scan: returns group IDs of extreme-value rows
    std::unordered_set<uint64_t> get_forced_candidates() const;
};
```

---

## 7. C++ CLI Interface

The C++ binary is called by the Python driver. It must support the following flags:

```
./zippy [OPTIONS]

Required:
  --input <path>          Path to binary dataset file (format described below)
  --n-rows <int>          Number of rows in dataset
  --k <int>               Number of top results to return

Algorithm selection:
  --mode <string>         One of: baseline | ext-a | ext-b | ext-ab | brute-force
                          baseline  = Zippy with uniform sampling only
                          ext-a     = Zippy + stratified sampling (Extension A)
                          ext-b     = Zippy + measure index (Extension B)
                          ext-ab    = Zippy + both extensions
                          brute-force = naive full groupby (correctness reference)

Zippy parameters:
  --sample-frac <float>   Uniform sample fraction (default: 0.01)
  --fa-capacity <int>     Max entries in FA hash table (default: 50000)
  --n-partitions <int>    Number of CA partitions (default: 10000)

Extension A parameters:
  --underrep-threshold <float>  Fraction below expected to trigger boost (default: 0.5)
  --boost-rows <int>            Rows to fetch per underrepresented group (default: 10)

Extension B parameters:
  --measure-m <int>       Number of extreme-value rows to track (default: 500)

Output:
  --output <path>         Path to write JSON results file
  --verbose               Print per-pass statistics to stderr
```

### Output JSON Format

```json
{
  "mode": "ext-ab",
  "k": 50,
  "n_rows": 10000000,
  "n_groups": 1000000,
  "top_k_results": [
    {"group_id": 42, "aggregate": 9821.5},
    ...
  ],
  "metrics": {
    "total_passes": 2,
    "pass1_duration_ms": 143.2,
    "pass2_duration_ms": 12.1,
    "total_duration_ms": 155.3,
    "fa_hit_rate": 0.94,
    "partitions_pruned_pct": 0.97,
    "index_build_duration_ms": 88.4,
    "sample_duration_ms": 22.1
  }
}
```

---

## 8. Dataset File Format

Binary format for maximum I/O speed. Each row is 16 bytes:

```
[ group_id : uint64_t (8 bytes) | value : double (8 bytes) ]
```

Rows are stored contiguously. No header. Total file size = n_rows × 16 bytes.

The Python generator writes this format:

```python
import numpy as np
import struct

def write_dataset(path, group_ids, values):
    """
    group_ids: np.ndarray of uint64
    values:    np.ndarray of float64
    """
    data = np.empty(len(group_ids) * 2, dtype=np.float64)
    data[0::2] = group_ids.view(np.float64)  # reinterpret cast
    data[1::2] = values
    data.tofile(path)
```

The C++ reader reads it as:

```cpp
struct Row { uint64_t group_id; double value; };
// mmap or fread into vector<Row>
```

---

## 9. Python Data Generator Spec

File: `python/generate_data.py`

Must support generating datasets with configurable properties:

```python
def generate_dataset(
    output_path: str,
    n_rows: int,
    n_groups: int,
    zipf_alpha: float,      # Zipf skew. Higher = more skew. 1.0 = moderate, 2.0 = heavy
    value_distribution: str,  # "exponential" | "uniform" | "constant"
    value_scale: float,     # mean/scale of value distribution
    rare_group_fraction: float,  # fraction of groups to make "rare high-value"
    rare_group_rows: int,   # max rows per rare group (e.g., 1–5)
    rare_group_value_multiplier: float,  # rare group values = multiplier × normal values
    seed: int = 42
) -> dict:                  # returns metadata dict (n_groups_actual, etc.)
```

### Generating Rare High-Value Groups

The key adversarial pattern to generate:

```python
# After generating the main Zipf-distributed dataset:
n_rare = int(n_groups * rare_group_fraction)
rare_group_ids = range(n_groups, n_groups + n_rare)  # new group IDs

for gid in rare_group_ids:
    n_rows_for_group = random.randint(1, rare_group_rows)
    rare_values = value_scale * rare_group_value_multiplier * np.ones(n_rows_for_group)
    # append (gid, rare_value) rows to dataset
    # these groups have very few rows but high values — the adversarial case
```

---

## 10. Metrics to Collect

Every experiment run must collect these metrics for comparison across modes:

| Metric | Description | Why It Matters |
|--------|-------------|----------------|
| `fa_hit_rate` | Fraction of true top-k groups that appear in FA after sampling | Core measure of sampling quality — our extensions should improve this |
| `partitions_pruned_pct` | Fraction of CA partitions pruned after Pass 1 | Higher = less work in Pass 2+ |
| `total_passes` | Number of data passes until convergence | Main efficiency metric — our extensions should reduce this |
| `total_duration_ms` | Wall-clock time for full query | Overall performance |
| `index_build_duration_ms` | Time to build GroupIndex / MeasureIndex | Extension overhead cost |
| `topKBound_after_pass1` | Value of topKBound after the first pass | Higher = better pruning |

---

## 11. Experiment Matrix

Run all experiments across this matrix. The Python driver (`run_experiments.py`)
should sweep these parameters and produce one JSON result per configuration.

### Datasets

| ID | n_rows | n_groups | zipf_alpha | rare_group_fraction | rare_group_rows | rare_value_mult |
|----|--------|----------|------------|---------------------|-----------------|-----------------|
| S1 | 10M | 1M | 1.2 | 0.0 | — | — |
| S2 | 10M | 1M | 1.2 | 0.0001 | 3 | 100× |
| S3 | 10M | 1M | 1.2 | 0.001 | 3 | 100× |
| S4 | 50M | 5M | 1.2 | 0.0001 | 3 | 100× |
| S5 | 10M | 1M | 0.8 | 0.0001 | 3 | 100× |

S1 = favorable (no rare groups, baseline should do well)
S2/S3 = adversarial (rare groups, extensions should help)
S4 = scale-up
S5 = lower skew (harder for Zippy in general)

### Modes

For each dataset, run all five modes: `brute-force`, `baseline`, `ext-a`, `ext-b`, `ext-ab`

### Parameter Sweeps

After the main matrix, run these targeted sweeps on dataset S2:

**Sweep 1 — Effect of m (Extension B):**
m ∈ {50, 100, 250, 500, 1000, 2500, 5000} with mode=ext-b, k=50

**Sweep 2 — Effect of underrep_threshold (Extension A):**
threshold ∈ {0.1, 0.25, 0.5, 0.75, 1.0} with mode=ext-a, k=50

**Sweep 3 — Effect of k:**
k ∈ {1, 5, 10, 25, 50, 100} with mode=baseline and ext-ab, dataset=S2

---

## 12. Correctness Verification

**Rule:** The top-k result from any mode must exactly match the brute-force result
for every experiment. Verify this in `python/verify_correctness.py`.

```python
def verify(brute_force_result, zippy_result, k):
    bf_groups = [r["group_id"] for r in brute_force_result[:k]]
    zy_groups = [r["group_id"] for r in zippy_result[:k]]
    assert set(bf_groups) == set(zy_groups), (
        f"Correctness failure!\n"
        f"Brute-force top-k: {bf_groups}\n"
        f"Zippy top-k:       {zy_groups}\n"
        f"Missing: {set(bf_groups) - set(zy_groups)}\n"
        f"Extra:   {set(zy_groups) - set(bf_groups)}"
    )
```

Always run correctness verification on S1–S3 before running the full experiment matrix.

---

## 13. Build System

Use CMake with C++17. Optimize for performance (`-O3`).

```cmake
cmake_minimum_required(VERSION 3.16)
project(zippy_topk CXX)
set(CMAKE_CXX_STANDARD 17)

# Release build with full optimization
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")

add_executable(zippy
    src/main.cpp
    src/zippy.cpp
    src/sampler.cpp
    src/group_index.cpp
    src/stratified_sampler.cpp
    src/measure_index.cpp
)

target_include_directories(zippy PRIVATE src/)
```

Build and run:
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
cd ..
./build/zippy --input data/S2.bin --n-rows 10000000 --k 50 --mode ext-ab --output results/s2_ext-ab.json
```

---

## 14. Python Driver Interface

`python/run_experiments.py` calls the C++ binary via subprocess:

```python
import subprocess, json, pathlib

def run_zippy(mode, dataset_path, n_rows, k, output_path, extra_args=None):
    cmd = [
        "./build/zippy",
        "--input", dataset_path,
        "--n-rows", str(n_rows),
        "--k", str(k),
        "--mode", mode,
        "--output", output_path,
    ]
    if extra_args:
        cmd.extend(extra_args)
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    if result.returncode != 0:
        raise RuntimeError(f"Zippy failed:\n{result.stderr}")
    with open(output_path) as f:
        return json.load(f)
```

---

## 15. Key Invariants and Edge Cases

Agents must preserve these invariants in all code:

1. **FA never exceeds capacity.** If the sample produces more candidates than
   `fa_capacity`, take only the top `fa_capacity` by estimated aggregate.

2. **FORCED_SET from MeasureIndex takes priority in FA.** In ext-b and ext-ab,
   the measure index groups always occupy FA slots, even if the sampler would
   have chosen different groups. Remaining slots go to sampler candidates.

3. **Pruning is based on total_sum, not max_value.** `CA[p].max_value` is the
   largest single row value seen in partition p, not the largest group total. Do not
   use it as the pruning bound (it would be incorrect). Use `CA[p].total_sum`.

4. **All values must be non-negative** for the total_sum pruning invariant to hold.
   The generator enforces this. If supporting negative values in future, the pruning
   logic must change.

5. **Brute-force baseline must be single-threaded.** For fair comparison, no
   parallelism anywhere in this prototype.

6. **Timing must exclude dataset loading from disk.** Start the clock after the data
   is in memory. Index build time is measured separately and added to overhead.

---

## 16. What NOT to Implement (Scope Boundaries)

The following are described in the paper but are out of scope for this project:

- **Parallelism / multi-threading** — the paper uses a 48-core server; we do not
- **SIMD optimization** — not needed to demonstrate algorithmic improvement
- **Physical partitioning** — implement only logical partitioning; physical adds
  complexity without changing the algorithmic claims we are testing
- **Rolling top-k (paginated queries)** — an orthogonal optimization in the paper
- **Non-monotonic aggregates (AVG)** — implement SUM only; AVG requires different
  bounding logic
- **pybind11 bindings** — subprocess interface is sufficient; use pybind11 only if
  the subprocess overhead becomes a bottleneck in tight benchmark loops

---

## 17. Summary of What We Are Contributing

The paper's Section 7 lists both extensions as future work with no implementation
or evaluation. This project:

1. Implements a faithful C++ prototype of baseline Zippy (SUM queries, logical
   partitioning, uniform sampling)
2. Implements Extension A: GroupOccurrenceIndex + stratified sampling, which raises
   FA hit rate for rare groups on adversarial datasets
3. Implements Extension B: MeasureIndex (min-heap), which force-injects extreme-value
   groups into FA regardless of sampling outcome
4. Evaluates both extensions on synthetic datasets with controlled rare-group
   injection, measuring FA hit rate, pruning fraction, pass count, and wall-clock time
5. Identifies optimal parameter settings (m for Extension B, threshold for Extension A)
   through systematic sweeps

The **core claim** being evaluated: on adversarial datasets (rare high-value groups),
our extensions raise topKBound after Pass 1, increase the fraction of CA partitions
pruned, and reduce total multi-pass overhead — while adding only a bounded
preprocessing cost and zero impact on correctness.
