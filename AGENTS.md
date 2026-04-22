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
| C | Total L1+L2 cache capacity for a single core, measured in number of (key, aggregate) pairs |
| Cf | Cache space allocated to FA = C/2 |
| Cc | Cache space allocated to CA = C/2 |
| k | Number of top results requested by the query |
| FA | Fine-grained Aggregates — exact tracker for candidate groups |
| CA | Coarse-grained Aggregates — rough tracker for non-candidate groups |
| topKBound | The k-th largest aggregate seen so far in FA; used as a pruning threshold |
| Candidate | A group identified by sampling as likely to appear in the final top-k |
| Partition | A bucket of non-candidate groups, hashed together in CA |
| Pruning | Eliminating an entire CA partition because its max possible aggregate < topKBound |
| Δ (delta) | Sampling tolerance level: groups with population proportion below Δ are excluded from sampling analysis as unlikely candidates |
| α | Confidence level for sample representativeness (used in sample size formula) |
| β | Confidence level for Hoeffding CI bounds used in skew validation |
| Cs | Cache space occupied by CI-qualifying candidate groups |
| Ch | Cache space occupied by heavy-hitter fill groups |
| d | Estimated distinct group count within a partition (used in partition ranking and physical/logical decision) |

---

## 2. Zippy Algorithm — Full Detail

> **See Section 3 for the verbatim paper pseudocode (Algorithms 1–4).** The prose
> below explains the algorithm intuitively. When implementing, always refer to
> Section 3 as the authoritative specification.

### Phase 0: Uniform Random Sampling

Before any data pass, Zippy reads a small fraction (configurable, typically 1%) of
rows at random. It runs a mini-aggregation over this sample to estimate which groups
will likely rank in the top-k.

The output is a **candidate set**: the top `fa_capacity` groups by estimated aggregate
in the sample, where `fa_capacity` (= Cf = C/2) is the number of FA slots available.

```
sample_fraction = max(ceil(z²/(4Δ²)), 0.01 × N)   rows sampled
candidates = top_fa_capacity_groups( mini_aggregate(sample) )
```

**Critical constraint:** |candidates| ≤ fa_capacity. The FA structure must fit in cache.

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
- Capacity: Cf entries (= C/2, half the total L1+L2 cache budget for this core)
- Implementation: **single-level open-addressing hash table with linear probing**
  (explicitly confirmed by patent). Linear probing is chosen over chaining because
  it eliminates pointer-following branches and keeps all data in contiguous memory,
  maximising cache-line utilisation.
- Allocated array size: `2 × Cf` slots (50% load factor to minimise collision chains)
- Empty-slot sentinel: use `UINT64_MAX` as the empty key value — never store it as
  a real group_id. Handle it by masking group_ids to `[0, UINT64_MAX - 1]`.
- Lookup: O(1) average, branch-free inner loop

**CA structure** (array of partition structs, cache-resident):
- Indexed by `hash(group_id) % n_partitions`
- Each entry stores: `{total_sum, max_value, min_value, count, approx_distinct_count}`
- `total_sum`: sum of all values routed to this partition (UB for SUM pruning)
- `max_value`: maximum single row value seen in this partition (UB for MAX pruning)
- `min_value`: minimum single row value seen in this partition
- `count`: number of rows routed to this partition
- `approx_distinct_count`: estimated number of distinct groups in this partition,
  computed using the **Flajolet-Martin (FM) algorithm** (a single-pass probabilistic
  distinct count sketch). This is used in the partition ranking step and the
  logical/physical partitioning decision. See FM implementation note below.
- This is NOT a per-group tracker — multiple groups hash to the same partition
- Capacity: Cc = C/2 partitions (equal split with FA)

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

| Paper variable | Codebase name | Meaning |
|---------------|--------------|---------|
| K | k | Number of top results |
| N | n_rows | Total rows in dataset |
| M | n_groups | Unique groups |
| CF | fa_capacity | Max FA entries (= Cf in groups) |
| C | cache_size | Total L1+L2 per core in (key,agg) pairs |
| Cf | fa_capacity × sizeof(FAEntry) | FA cache budget = C/2 |
| Cc | n_partitions × sizeof(CAPartition) | CA cache budget = C/2 |
| Q | n_partitions | Logical partitions CA can hold in cache |
| X | grouping column(s) | group_id in binary dataset |
| Y | aggregate (measure) column | value in binary dataset |
| A | aggregation function | SUM only in this prototype |
| s | segment_size | Rows per locality segment (default 100,000) |
| α₀ | locality_threshold | Locality cutoff (default 0.20) |
| α | alpha_ci | CI confidence for sample size formula |
| β | beta_ci | CI confidence for Hoeffding skew validation |
| Δ | delta | Sampling tolerance level (default 0.05) |
| Lk | topKBound_sample | K-th highest lower-bound in sample CI |
| Cp | partition_size | Row count of a specific input partition |
| Tc | min_fa_count | Lowest row-count among FA candidate groups |
| E | estimated_elements | Estimated elements per child partition = Cp/Q |
| Cs | cs_bytes | Cache space of CI-qualifying groups = count × sizeof(FAEntry) |
| Ch | ch_bytes | Cache space of heavy-hitter fill groups |
| d | approx_distinct | FM-estimated distinct groups in a partition |

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
  project's prototype, **implement it sequentially** (see Section 18 — out of scope).
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
- **Sample size formula (line 2):** The patent gives the exact formula. For a
  tolerance level Δ ∈ [0,1] and confidence level (1−α), the minimum sample size is:

  ```
  s ≥ (z_{α/2})² / (4 × Δ²)
  ```

  where z_{α/2} is the standard normal quantile (e.g., 1.96 for α=0.05, giving
  95% confidence). This sample size is **independent of N** — it does not grow with
  the dataset. Δ controls the tolerance for ratio deviation between sample and true
  group proportions. Groups with true proportion below Δ are not guaranteed to be
  represented and are treated as too rare to capture via sampling alone (they may
  still be captured in Pass 2 via partitions).

  Recommended defaults: Δ=0.05, α=0.05 → s ≈ 384. In practice the paper uses
  larger samples (~1% of N) to get better aggregate estimates; use whichever is
  larger: the formula bound or `0.01 × N`.

- **Hoeffding CI bounds (lines 13–14):** The patent names this explicitly.
  For each group i seen in the sample with n'_i sample rows, min value a, max
  value b, the confidence interval half-width at confidence level β is:

  ```
  ε_i = (b - a) × sqrt( ln(2 / (1 - β)) / (2 × n'_i) )
  ```

  The lower bound of group i's aggregate CI is: `sample_aggregate_i - ε_i`.
  The upper bound is: `sample_aggregate_i + ε_i`.
  Use β=0.95 as the default confidence level.

  For the prototype simplification: use raw sample aggregates as point estimates
  (ε_i = 0) and select top-Cf groups by sample aggregate as FA candidates. Mark
  this simplification clearly in code comments with a TODO for full CI.

- **isOptimizable check (lines 19–22):** The patent clarifies the check is on
  **cache space** `Cs`, not raw group count. `Cs` is the total memory occupied by
  grouping keys and aggregate values for all groups whose CI lower bound ≥ Lk.
  If `Cs > Cf` (where Cf = C/2 is the FA cache budget), validation fails.
  In the prototype: approximate `Cs` as `|tempGroups| × sizeof(FAEntry)` and
  compare against the FA capacity budget.

- **Heavy hitter fill (lines 26–29):** After CI-qualifying groups fill tempGroups,
  remaining FA space is filled with heavy hitters (highest sample-count groups not
  already in tempGroups) until `Cs + Ch ≈ Cf`. The patent notes this reduces
  tuples sent to CA partitions, improving pruning bounds in subsequent passes.

- **Extension A modifies this algorithm.** The stratified sampler replaces lines 4–11
  with a two-phase sampling procedure that uses the GroupOccurrenceIndex to boost
  underrepresented groups. Lines 12–29 remain unchanged.
- **Extension B modifies this algorithm.** Before line 25, the MeasureIndex
  FORCED_SET is inserted into FAgroups first, then remaining slots are filled by
  the CI/heavy-hitter logic. If |FORCED_SET| > Cf, truncate to Cf (log a warning).

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
- **Locality check constants (lines 4–10):** The patent gives the benchmarked values
  explicitly: **segment size s = 100,000 rows** and **locality threshold α₀ = 0.20**.
  These were determined through benchmarking on a specific machine. Use these as the
  defaults. The locality formula is:

  ```
  l = (Σ_s  ds/cs) / t
  ```

  where for each segment s of size cs rows, ds = distinct groups in that segment,
  and t = total number of segments in the partition. If `l < 0.20`, use exact
  aggregation. In the prototype, **skip the locality check** and always proceed to
  the partitioning decision (lines 11–20). This is a safe simplification.

- **exactAggregation path (lines 22–23):** When partition is small enough to fit
  in cache (distinct groups < Cf), or locality is sufficient, aggregate all groups
  in this partition exactly into partialAggregates-i. FA groups are also aggregated
  here (their rows are not sent to childPartitions).
- **FA group rows (line 22):** Rows belonging to FAgroups are ALWAYS aggregated
  exactly into partialAggregates-i, regardless of exactAggregation flag.
- **Logical vs physical partitioning decision (lines 12–20):**
  - If the previous pass used logical partitioning for this partition and it was NOT
    pruned, upgrade to physical (line 13). This avoids repeated full re-scans.
  - Otherwise: let Z = max logical partitions CA can hold (= n_partitions), Tc =
    lowest row-count among FA groups (= the least-frequent FA candidate), C_p =
    cardinality (row count) of the input partition. Compute `E = C_p / Z`.
    If `E < Tc`: use logical. Else: use physical.
  - Intuition: if each logical child partition would have fewer rows than the
    least-frequent FA candidate, logical is worthwhile (partitions will likely be
    pruned). Otherwise, physical is better because at least one child may contain
    a result-bound group.
  - In this project's prototype: **implement logical partitioning only** (see
    Section 18). Physical partitioning (lines 29–33) is out of scope.
- **childPartitions-i:** These become the `partitions` list for the next pass in
  Algorithm 1. Each child partition is identified by its hash value and contains
  the CA statistics (total_sum, max_value, min_value, count, approx_distinct_count)
  for that hash bucket.

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
- **Upper bound (UB) computation per aggregate type (lines 5–7):**
  The patent gives the explicit UB formula for each aggregate function:

  | Aggregate | UB for any group in partition p |
  |-----------|--------------------------------|
  | SUM       | `CA[p].total_sum` |
  | AVG       | `CA[p].total_sum / CA[p].count` (loose; uses partition total) |
  | MAX       | `CA[p].max_value` |
  | MIN       | `CA[p].min_value` |

  Since this prototype implements SUM only, use `CA[p].total_sum` as UB. The SUM
  UB is the tightest and most effective for pruning.

- **topKBound computation (lines 8–9):** topKBound is the K-th highest value when
  considering both exact FA aggregates and the UBs of all surviving child partitions.
  Concretely: sort the union of {FA exact values} ∪ {partition UBs} descending;
  topKBound = the K-th element.
- **Early termination (lines 11–12):** If K groups have exact aggregates all
  exceeding topKBound, we are done. Return +infinity as the new topKBound to signal
  completion to Algorithm 1's while loop.
- **Partition ranking (lines 15–17):** The patent gives the explicit formula.
  When surviving partitions exceed the number of worker threads (or simply to
  prioritise promising partitions), rank each surviving partition by its **estimated
  per-group aggregate**:

  ```
  estimated_value_per_group[p] = CA[p].total_sum / d[p]
  ```

  where `d[p]` is the approximate distinct group count from the FM sketch stored in
  `CA[p].approx_distinct_count`. Process partitions in descending order of this
  estimate. In the single-threaded prototype this ordering affects pass efficiency
  but not correctness — implement it as a sort before Pass 2 begins.

- **Merging exactAggregates (lines 1–2):** partialAggregates-i contains the
  per-partition contributions to FA groups from the current pass. These must be
  merged (summed, for SUM queries) with the running exactAggregates carried over
  from previous passes. After merging, exactAggregates[g] holds the exact total for
  FA group g across all passes processed so far.
- **The returned `partitions`** feeds back into Algorithm 1 line 2 as the partition
  list for the next while-loop iteration.

---

## 3.5 Patent-Clarified Implementation Details (US 12380098 / Application 20250103591)

The following details come from the patent filing by the same authors. They are
**additive** to the paper — they do not contradict the algorithms in Section 3, but
fill in implementation specifics the paper leaves implicit. **The paper algorithms
take precedence** in any case of apparent conflict.

### Cache Budget Split

The patent is explicit: FA and CA each receive **exactly half the total L1+L2 cache
budget** for a given core.

```
C  = size of L1_data + L2 for one core (in units of (group_id, aggregate) pairs)
Cf = C / 2    ← FA budget
Cc = C / 2    ← CA budget
```

The FA table array is allocated at `2 × Cf` slots (50% load factor).
The CA partition array holds up to `Cc` partitions.

### FA Data Structure: Single-Level Hash Table with Linear Probing

The patent names the FA implementation explicitly:

> "The first cache resident data structure utilises a single-level hash table with
> linear probing with a sufficiently large size to minimise collisions. A technical
> benefit of using single-level hash tables is that this approach improves performance
> by eliminating branching and chaining."

Key implementation consequences:
- **No chaining, no two-level structure.** A flat array of `FAEntry` with linear
  probing only.
- **50% load factor target** (2× array size vs. capacity) to keep probe chains short.
- **Sentinel key** `UINT64_MAX` marks empty slots. Keep `FAEntry` at exactly 16 bytes
  (`uint64_t` + `double`) so 4 entries fit per 64-byte cache line.
- Hash function: use a fast integer hash (e.g., FNV-1a or multiply-shift) rather
  than `std::hash<uint64_t>` which may not distribute well for sequential IDs.

### CA: Approximate Distinct Count via Flajolet-Martin (FM)

The patent names the algorithm explicitly:

> "The approximate distinct count is determined using the Flajolet Martin (FM)
> algorithm, which is used to approximate the number of unique elements in a
> partition in one pass."

The FM sketch is used in two places:
1. **Logical/physical partitioning decision** (Algorithm 3): `d = fm.estimate()` is
   used as the estimated distinct group count within a partition to compute `E = Cp/Q`
   more accurately than assuming uniform distribution.
2. **Partition ranking** (Algorithm 4): `estimated_sum_per_group = total_sum / d`
   gives the estimated per-group contribution, used to rank surviving partitions.

A minimal FM implementation for the prototype:
```cpp
// Use a hash function different from the partition hash to avoid correlation.
// OR the hashed group_id into a running bitmap per partition.
// Estimate = 2^(position of lowest 0-bit in accumulated bitmap).
uint64_t bitmap = 0;
// Per row routed to this partition:
bitmap |= fm_hash(group_id);   // fm_hash returns a uint64_t with geometric
                                // distribution of trailing zeros
// Estimate:
uint32_t d = 1u << __builtin_ctzll(~bitmap);  // lowest 0-bit position → estimate
```

### Sampling Tolerance Level Δ and Sample Size Formula

The patent gives the closed-form sample size formula:

```
s ≥ (z_{α/2})² / (4 × Δ²)
```

Groups with true population proportion below Δ are **not guaranteed** to appear in
the sample and are intentionally excluded from sampling-based candidate selection.
They will still be captured in Pass 2+ via their CA partition.

This is the justification for our Extension A (stratified sampling): by augmenting
the uniform sample with targeted rows from the group index, we capture groups below
the Δ threshold that would otherwise miss the FA candidate list.

Recommended defaults: Δ=0.05, α=0.05 → s ≈ 384 (minimum). Use `max(384, 0.01×N)`
in practice for better aggregate estimation.

### Hoeffding CI for Skew Validation

The patent gives the explicit CI formula used in Algorithm 2 step 3 (skew
validation):

```
ε_i = (b_i - a_i) × sqrt( ln(2 / (1 - β)) / (2 × n'_i) )
```

where for group i in the sample:
- `a_i` = minimum value seen for group i in the sample
- `b_i` = maximum value seen for group i in the sample
- `n'_i` = number of sample rows belonging to group i
- `β` = confidence level (recommended: 0.95)

Lower bound of group i's aggregate CI = `sample_aggregate_i - ε_i`
Upper bound = `sample_aggregate_i + ε_i`

In the prototype, use point estimates (ε_i = 0) with a TODO comment. Implement
the full Hoeffding CI if time permits.

### isOptimizable: Cache-Space Check, Not Group-Count Check

The patent clarifies that the isOptimizable decision (Algorithm 2 line 19) operates
on **cache space** `Cs`, not raw group count:

> "If Cs > Cf, the validation fails because the cache space occupied by the grouping
> keys exceeds the size of the cache space allocated to the FA."

`Cs = |tempGroups| × sizeof(FAEntry)`. In the prototype, `sizeof(FAEntry) = 16` bytes.
If `Cs > Cf` (i.e., too many CI-qualifying groups to fit in the FA cache), fall back
to brute-force full aggregation.

### Segment and Locality Constants (Benchmarked Values)

The patent gives the concrete benchmarked values for the locality check in Algorithm 3:

```
segment_size s = 100,000 rows
locality_threshold α₀ = 0.20
```

These were tuned on the authors' specific hardware. Use as defaults. The locality
check itself is out of scope for this prototype (always skip to the partitioning
decision), but record these values in code comments for completeness.

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
Merge Phase 1 and Phase 2 aggregates → pick top fa_capacity as FA candidates
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
zippy-optimizer/
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

## 5.1 Recommended Implementation Order

An agent building this from scratch should follow this sequence. Each step is
independently testable before moving to the next. Steps within a phase are
sequential; do not skip ahead until the verification gate at the end of
each phase passes.

> **Guiding principle:** Build the thinnest possible vertical slice first —
> generate data → load it → compute brute-force top-k → verify. Only then add
> Zippy's sampling, FA/CA routing, pruning, and multi-pass loop one piece at a
> time. Each extension is a self-contained layer on top of baseline Zippy.

---

**Phase 1 — Build system + data layer (no algorithm yet)**

1. **`CMakeLists.txt`** — write the CMake build configuration (Section 15). Ensure
   `cmake .. -DCMAKE_BUILD_TYPE=Release && make` compiles an empty `main.cpp` that
   returns 0. This catches toolchain issues early.

2. **`python/generate_data.py`** — implement the full data generator (Section 9):
   Zipf group-ID generation, value distributions, rare-group injection, binary
   writer, and UINT64_MAX sentinel masking. Test by generating a **tiny** dataset:
   ```bash
   python python/generate_data.py --output data/tiny.bin \
       --n-rows 1000 --n-groups 50 --zipf-alpha 1.2 \
       --rare-group-fraction 0.1 --rare-group-rows 3 \
       --rare-group-value-multiplier 100
   ```
   Verify the output file size equals `n_rows_actual × 16` bytes.
   Also generate the small test dataset **S0** (10K rows, 500 groups, same params)
   for use throughout Phases 2–4. S0 is small enough for sub-second C++ runs
   during rapid debugging.

3. **`src/utils.h`** — implement `Timer`, `RunMetrics`, and `write_output_json()`
   exactly as specified in Section 6. Do NOT put the `Row` struct here — it belongs
   in `data_structures.h` (next step).

4. **`src/data_structures.h`** — define the `Row` struct and all four hash functions
   (`partition_hash`, `fa_hash`, `fm_hash`, `child_partition_hash`) as specified in
   Section 6. These are standalone and have no dependencies beyond `<cstdint>`.
   Leave the `FATable`, `CATable`, `FMSketch` class bodies as forward declarations
   or stubs — they will be filled in Phase 3.

5. **`src/main.cpp`** — implement CLI argument parsing (hand-rolled, per Section 6's
   `main.cpp` template), binary file loading into `std::vector<Row>`, and mode
   dispatch skeleton (switch on `--mode`, print "mode not implemented" for
   everything except `brute-force`). Wire `write_output_json()` for output.

6. **Gate:** Build and run:
   ```bash
   ./build/zippy --input data/tiny.bin --n-rows 1000 --k 5 \
       --mode brute-force --output results/tiny_bf.json
   ```
   This will fail (brute-force not implemented yet), but it must **compile, load
   the dataset, and print a sensible error** (e.g., "mode not implemented"). This
   confirms the data pipeline works end-to-end.

---

**Phase 2 — Brute-force baseline (correctness anchor)**

7. **`src/zippy.cpp` + `src/zippy.h`** — implement `run_brute_force()` only:
   one pass through all rows, `std::unordered_map<uint64_t,double>` for exact
   aggregates, then `std::partial_sort` to find top-k (Section 7, "Brute-Force
   Mode Implementation"). Wire this into the mode dispatch in `main.cpp`.

8. **Gate:** Run brute-force on the tiny dataset and S0:
   ```bash
   ./build/zippy --input data/tiny.bin --n-rows 1000 --k 5 \
       --mode brute-force --output results/tiny_bf.json
   ```
   Cross-check the JSON output against a Python pandas `groupby().sum()` on the
   same binary file. The top-k group IDs and aggregates must match exactly
   (within floating-point tolerance ≤ 1e-6). This is your ground truth for
   all future correctness checks.

---

**Phase 3 — Core data structures**

9. **`FATable` in `data_structures.h`** — implement the full linear-probing hash
   table: constructor (allocates `2 × capacity` slots, fills with `FA_EMPTY_KEY`),
   `contains()`, `insert()`, `update()`, `get()`, `top_k()`. Follow the exact
   spec in Section 6 (sentinel key, 50% load factor, `fa_hash()`).

10. **`FMSketch` in `data_structures.h`** — implement the minimal FM sketch:
    `bitmap`, `update(group_id)` using `fm_hash()`, `estimate()` using trailing
    zeros. Tiny struct, no dependencies.

11. **`CAPartition` + `CATable` in `data_structures.h`** — implement the partition
    struct (`total_sum`, `max_value`, `min_value`, `count`, `FMSketch`, `pruned`,
    `estimated_per_group_sum()`) and the `CATable` class (`partition_of()`,
    `update()`, `prune()`, `surviving_partitions()`,
    `ranked_surviving_partitions()`). Follow the `CATable::update()` body in
    Section 6 exactly.

12. **Gate — unit-test data structures:** Write a small test (can be in `main.cpp`
    behind a `--test` flag, or a separate test file):
    - **FATable:** insert 1000 known group_ids with known values, verify
      `contains()` returns true for all inserted keys and false for 1000
      non-inserted keys, verify `get()` returns correct sums, verify `top_k(10)`
      returns the correct 10 groups in descending order.
    - **FMSketch:** feed 10, 100, 1000, 10000 distinct IDs; verify `estimate()`
      is within 3× of the true count (FM is rough — order-of-magnitude is fine).
    - **CATable:** route 10K rows, verify `prune()` marks the right partitions,
      verify `ranked_surviving_partitions()` returns descending estimated order.

---

**Phase 4A — Sampler (Algorithm 2, standalone)**

The sampler can be built and tested in complete isolation — it has no dependency
on FATable/CATable routing. Get it right before wiring it into the pipeline.

13. **`src/sampler.h` + `src/sampler.cpp`** — implement `uniform_sample_and_select()`
    (Algorithm 2, simplified: point estimates, no full Hoeffding CI). This function:
    - Computes sample size = `max(ceil(z² / (4Δ²)), sample_frac × N)`.
    - Selects rows with probability `p = sample_size / N`.
    - Aggregates sampled rows into `SampleGroupStats` per group.
    - Selects top `fa_capacity` groups by sample aggregate as FA candidates.
    - Fills remaining FA slots with heavy hitters (highest sample count).
    - Returns `SampleResult` with `is_optimizable` flag and `fa_groups` set.

14. **Gate — sampler unit test:** Write a small test (inline in a scratch file or
    behind a `--test-sampler` flag):
    - Generate or load the S0 dataset (10K rows, 500 groups, Zipf α=1.2).
    - Call `uniform_sample_and_select()` with `fa_capacity = 100`.
    - Verify: `is_optimizable == true` (data is skewed).
    - Verify: `fa_groups.size() == fa_capacity` (all slots filled).
    - Verify: the true top-10 groups (from brute-force) appear in `fa_groups`
      with high probability (≥8 out of 10 for Zipf α=1.2).
    - Print the sample duration and candidate list for manual inspection.

---

**Phase 4B — Single-pass FA/CA routing + pruning (Algorithms 3 + 4, pass 1 only)**

Wire the sampler output into FATable/CATable for a single data pass and verify
that pruning works correctly. Do NOT implement multi-pass yet — that's Phase 4C.

15. **Pass 1 in `src/zippy.cpp`** — implement the first-pass FA/CA routing loop
    (Algorithm 3, lines 21–28, logical partitioning only):
    - Initialise `FATable` with candidates from sampler.
    - Initialise `CATable` with `n_partitions` empty partitions.
    - Iterate all rows: if `fa.contains(gid)` → `fa.update(gid, val)`;
      else → `ca.update(gid, val)`.
    - After the loop: compute `topKBound` = k-th largest FA value via `fa.top_k(k)`.
    - Call `ca.prune(topKBound)`.
    - Record `partitions_pruned_pct` and `topKBound_after_pass1` in metrics.

16. **Gate — single-pass verification on S0:**
    - Expose a temporary `run_zippy_single_pass()` (or reuse `run_zippy_baseline()`
      but skip the multi-pass loop — just return FA top-k after pass 1).
    - Run on S0: the FA top-k should overlap ≥80% with brute-force top-k.
    - Print: `topKBound`, `partitions_pruned_pct`, FA candidate list, surviving
      partition count. With Zipf α=1.2, expect >50% pruning.
    - Cross-check: no brute-force top-k group should be in a **pruned** partition
      (this would mean pruning is unsafe — a critical bug).

---

**Phase 4C — MergeAndPrune + multi-pass loop (full Algorithm 1)**

Add the convergence loop: after Pass 1 pruning, re-scan surviving partitions
until all top-k groups are confirmed with exact aggregates.

17. **MergeAndPrune in `src/zippy.cpp`** — implement Algorithm 4:
    - Merge `partialAggregates` into cumulative `exactAggregates`.
    - Compute UBs for child partitions (`total_sum` for SUM).
    - Compute `topKBound` as the k-th highest among exact aggregates ∪ UBs.
    - Prune child partitions with UB < topKBound.
    - Check early termination: if k groups have exact aggs > topKBound, done.

18. **Multi-pass loop in `src/zippy.cpp`** — implement Algorithm 1's while-loop:
    - Pass 2+: re-scan the dataset, filter rows to surviving partitions, re-hash
      with `child_partition_hash(gid, n_partitions, pass)`, route FA rows to
      `partialAggregates` and non-FA rows to `childPartitions`.
    - Call MergeAndPrune after each pass.
    - Loop until `top_k_confirmed >= k` or no surviving partitions remain.
    - Wire this as `run_zippy_baseline()` and expose under `--mode baseline`.

19. **Gate — correctness on S0 and S1:**
    - Generate **S1** dataset (10M rows, 1M groups, no rare groups).
    - Run `--mode baseline` and `--mode brute-force` on S0 (small, instant).
    - Compare: top-k group ID **sets** must be identical.
    - Run on S1: sets must still match. Record `total_passes` and
      `partitions_pruned_pct` — with Zipf α=1.2 and no rare groups, expect
      high pruning (>90%) and convergence in 1–2 passes.

---

**Phase 5 — Extension A (stratified sampling)**

20. **`src/group_index.h` + `src/group_index.cpp`** — implement
    `GroupOccurrenceIndex`: `build()` (one scan, populate
    `unordered_map<gid, vector<row_pos>>`), `is_underrepresented()`,
    `get_boost_rows()`, `group_count()`, `row_count_for()`.

21. **`src/stratified_sampler.h` + `src/stratified_sampler.cpp`** — implement the
    two-phase sampler:
    - Phase 1: uniform sample (reuse `uniform_sample_and_select` logic).
    - Phase 2: iterate all groups in GroupIndex, check underrepresentation,
      fetch boost rows from the index, add to sample aggregates.
    - Merge and select top `fa_capacity` groups as FA candidates.

22. **Wire `--mode ext-a`** in `zippy.cpp` — call `run_zippy_ext_a()` which uses
    the stratified sampler instead of the uniform sampler. The rest of the Zippy
    pipeline (Pass 1, MergeAndPrune, multi-pass) is identical to baseline.

23. **Gate — correctness on S2:**
    - Generate **S2** dataset (10M rows, 1M groups, 0.01% rare groups).
    - Run `--mode ext-a`, `--mode baseline`, `--mode brute-force` on S2.
    - All top-k sets must match brute-force. Compare `fa_hit_rate` and
      `partitions_pruned_pct` between baseline and ext-a — ext-a should show
      improvement on the adversarial dataset.

---

**Phase 6 — Extension B (measure column index)**

24. **`src/measure_index.h` + `src/measure_index.cpp`** — implement the min-heap
    `MeasureIndex`: `process(value, group_id)` called per row during a single
    build pass, `get_forced_candidates()` returns `unordered_set<uint64_t>` of
    group IDs from the top-m rows.

25. **Wire `--mode ext-b`** in `zippy.cpp` — call `run_zippy_ext_b()` which:
    - Builds MeasureIndex in one pass → extracts FORCED_SET.
    - Reserves FORCED_SET slots in FA before sampling.
    - Fills remaining FA slots from uniform sampling candidates.
    - Runs normal Zippy pipeline.

26. **Gate — correctness on S2:**
    - Run `--mode ext-b` and `--mode brute-force` on S2.
    - Top-k sets must match. Compare `topKBound_after_pass1` between baseline
      and ext-b — ext-b should produce a higher bound.

---

**Phase 7 — Combined mode**

27. **Wire `--mode ext-ab`** in `zippy.cpp` — combines both extensions:
    - Build GroupOccurrenceIndex + MeasureIndex (can share the same scan pass
      or run sequentially).
    - FORCED_SET occupies FA slots first, then stratified sampling fills the rest.
    - Run normal Zippy pipeline.

28. **Gate — full correctness sweep:**
    - Run `python/verify_correctness.py` (Section 13) across S1, S2, S3 for all
      four modes. **All must pass before proceeding to experiments.**

---

**Phase 8 — Experiments, plotting, and documentation**

29. **`python/run_experiments.py`** — implement the full experiment driver
    (Section 11): main matrix (S1–S5 × 5 modes) plus parameter sweeps on S2
    (measure_m, underrep_threshold, k). Generate datasets S3–S5 at this point.
    Run the full matrix. Collect all JSON results.

30. **`python/plot_results.py`** — implement all 9 plots specified in Section 11.
    Save to `plots/`. Update `README.md` with build instructions, usage examples,
    and a summary of results.

---

## 6. C++ Data Structures — Detailed Spec

### FATable (Fine-grained Aggregates)

```cpp
// Sentinel value for empty slots — never a valid group_id
static constexpr uint64_t FA_EMPTY_KEY = UINT64_MAX;

struct FAEntry {
    uint64_t group_id;   // FA_EMPTY_KEY means slot is vacant
    double   exact_sum;  // running exact aggregate (SUM queries)
    // Note: no separate 'occupied' bool — use group_id == FA_EMPTY_KEY as sentinel
    // This keeps the struct at 16 bytes, fitting 4 entries per 64-byte cache line
};

class FATable {
    // Single-level open-addressing hash table with linear probing.
    // Patent explicitly specifies this design: "single-level hash table with
    // linear probing" to eliminate branching and chaining overhead.
    //
    // Allocated size = 2 × capacity (50% load factor).
    // group_ids must be masked to [0, UINT64_MAX - 1] before use.
    std::vector<FAEntry> table;   // size = 2 * capacity
    size_t capacity;              // Cf = C/2: number of groups FA can hold
    size_t occupied;              // current number of non-empty entries

    size_t probe(uint64_t group_id) const;  // linear probe: hash(id) % table.size()

public:
    explicit FATable(size_t capacity);
    bool   contains(uint64_t group_id) const;
    void   insert(uint64_t group_id);             // called during candidate setup
    void   update(uint64_t group_id, double val); // called during Pass 1 inner loop
    double get(uint64_t group_id) const;
    std::vector<std::pair<uint64_t,double>> top_k(size_t k) const;
    size_t size() const { return occupied; }
    bool   full()  const { return occupied >= capacity; }
};
```

### CATable (Coarse-grained Aggregates)

```cpp
// Flajolet-Martin sketch for approximate distinct count within a partition.
// A minimal single-pass FM sketch: maintain a bitmask of leading zeros seen
// in hash(group_id). Estimated distinct count ≈ 2^(max_leading_zeros + 0.77).
// For the prototype, a 64-bit trailing-zero FM sketch suffices.
struct FMSketch {
    uint64_t bitmap = 0;  // OR of hash(group_id) values seen
    uint32_t estimate() const {
        // count trailing zeros of bitmap as proxy for log2(distinct_count)
        return bitmap == 0 ? 1 : (1u << __builtin_ctzll(~bitmap));
    }
    void update(uint64_t group_id) {
        bitmap |= hash_for_fm(group_id);  // use a separate hash from partition hash
    }
};

struct CAPartition {
    double   total_sum;    // sum of ALL values routed here — UB for SUM pruning
    double   max_value;    // max single row value — UB for MAX pruning
    double   min_value;    // min single row value — UB for MIN pruning
    uint64_t count;        // total row count routed here
    FMSketch fm;           // approximate distinct group count (for partition ranking)
    bool     pruned;       // set true after pruning step, skip in subsequent passes

    // Estimated per-group aggregate (used for partition ranking in Algorithm 4)
    // Patent formula: estimated_sum_per_group = total_sum / approx_distinct_count
    double estimated_per_group_sum() const {
        uint32_t d = fm.estimate();
        return d == 0 ? total_sum : total_sum / d;
    }
};

class CATable {
    std::vector<CAPartition> partitions;
    size_t n_partitions;  // Q in paper notation = Cc (CA cache budget)

public:
    size_t partition_of(uint64_t group_id) const;  // hash(group_id) % n_partitions
    void   update(uint64_t group_id, double val);
    void   prune(double topKBound);                  // marks partitions as pruned
    std::vector<size_t> surviving_partitions() const;
    // Returns surviving partitions sorted by estimated_per_group_sum() descending
    // (Algorithm 4 partition ranking — process highest-potential partitions first)
    std::vector<size_t> ranked_surviving_partitions() const;
    double pruning_fraction() const;                 // for metrics collection
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

### Hash Functions

Two hash functions are needed. They must be different to avoid correlation between
the partition assignment hash and the FM sketch hash.

```cpp
// Hash 1: Partition assignment — maps group_id to a CA partition index.
// Always uses modulo so it works for any n_partitions value (not just powers of 2).
inline size_t partition_hash(uint64_t group_id, size_t n_partitions) {
    // Fibonacci hashing with modulo fallback — correct for any n_partitions
    return (group_id * 11400714819323198485ULL) % n_partitions;
}

// Hash 2: FA lookup — maps group_id to a slot in the FA table
inline size_t fa_hash(uint64_t group_id, size_t table_size) {
    // Different constant — Wang hash
    uint64_t x = group_id;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x = x ^ (x >> 31);
    return x % table_size;
}

// Hash 3: FM sketch — must differ from both above
inline uint64_t fm_hash(uint64_t group_id) {
    // MurmurHash3 finalizer — produces geometrically distributed trailing zeros
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
    return (mixed * 6364136223846793005ULL) % n_child_partitions;
}
```

**Important:** `partition_hash` and `fa_hash` must use different constants.
`fm_hash` must differ from both. Using the same hash for FM and partitioning
corrupts the distinct-count estimate.

### Data Loading

The C++ binary loads the entire dataset into memory before starting the clock.

```cpp
// In main.cpp, after argument parsing:
std::vector<Row> dataset(n_rows);
{
    FILE* f = fopen(input_path.c_str(), "rb");
    if (!f) { perror("fopen"); exit(1); }
    size_t read = fread(dataset.data(), sizeof(Row), n_rows, f);
    fclose(f);
    if (read != n_rows) { fprintf(stderr, "Short read\n"); exit(1); }
}
// Start timing AFTER this block — file I/O excluded from benchmarks

// Row struct (matches binary format exactly):
struct Row {
    uint64_t group_id;  // 8 bytes
    double   value;     // 8 bytes
};  // total: 16 bytes, no padding needed (both 8-byte aligned)
```

### PartialAggregates Structure

Algorithm 3 returns `partialAggregates-i`, which is the per-pass exact aggregate
contribution for FA groups from a specific partition. In the single-threaded
prototype this simplifies to a single map:

```cpp
// Maps FA group_id → sum of values seen for that group in THIS pass
// (not cumulative — merging across passes happens in Algorithm 4)
using PartialAggregates = std::unordered_map<uint64_t, double>;

// ChildPartitions: maps partition_hash → CAPartition stats accumulated this pass
using ChildPartitions = std::unordered_map<uint64_t, CAPartition>;
```

In the multi-pass loop, `exactAggregates` is the running cumulative sum across all
passes, while `partialAggregates` is only the current pass contribution:

```cpp
// In the main Zippy loop (Algorithm 1 / Algorithm 4):
std::unordered_map<uint64_t, double> exactAggregates;  // cumulative, all passes

// Each pass:
PartialAggregates partial;
ChildPartitions   children;
aggregate_and_partition(partition, fa_groups, partial, children);

// Merge (Algorithm 4 lines 1-2):
for (auto& [gid, val] : partial)
    exactAggregates[gid] += val;
```

### `utils.h` — Timer and JSON Writer

```cpp
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
```

### `zippy.h` — Public Interface of the Core Engine

```cpp
#pragma once
#include "data_structures.h"
#include "utils.h"
#include <vector>
#include <string>
#include <unordered_set>

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

// Baseline Zippy (uniform sampling, logical partitioning, SUM only)
RunMetrics run_zippy_baseline(
    const std::vector<Row>& dataset,
    int k,
    const ZippyConfig& cfg,
    std::vector<std::pair<uint64_t,double>>& out_results,
    std::vector<uint64_t>& out_fa_groups);

// Extension A: stratified sampling via GroupOccurrenceIndex
RunMetrics run_zippy_ext_a(
    const std::vector<Row>& dataset,
    int k,
    const ZippyConfig& cfg,
    std::vector<std::pair<uint64_t,double>>& out_results,
    std::vector<uint64_t>& out_fa_groups);

// Extension B: measure column index for extreme value detection
RunMetrics run_zippy_ext_b(
    const std::vector<Row>& dataset,
    int k,
    const ZippyConfig& cfg,
    std::vector<std::pair<uint64_t,double>>& out_results,
    std::vector<uint64_t>& out_fa_groups);

// Extensions A + B combined
RunMetrics run_zippy_ext_ab(
    const std::vector<Row>& dataset,
    int k,
    const ZippyConfig& cfg,
    std::vector<std::pair<uint64_t,double>>& out_results,
    std::vector<uint64_t>& out_fa_groups);

// Brute-force reference (always correct, used for verification)
std::vector<std::pair<uint64_t,double>> run_brute_force(
    const std::vector<Row>& dataset, int k);
```

### `sampler.h` — Uniform Random Sampler Interface

```cpp
#pragma once
#include "data_structures.h"
#include <vector>
#include <unordered_map>
#include <unordered_set>

// Per-group statistics accumulated during sampling
struct SampleGroupStats {
    double   sum   = 0;
    double   count = 0;
    double   min_val = std::numeric_limits<double>::max();
    double   max_val = std::numeric_limits<double>::lowest();
};

// Result of the sampling + skew validation phase (Algorithm 2)
struct SampleResult {
    bool is_optimizable = true;
    std::unordered_set<uint64_t> fa_groups;   // group_ids selected for FA
    std::unordered_map<uint64_t, SampleGroupStats> sample_stats;  // for CI/debug
    size_t sample_size_actual = 0;
};

// Perform uniform random sampling and FA group selection (Algorithm 2, simplified)
// Uses point estimates (no full Hoeffding CI). Selects top fa_capacity groups by
// sample aggregate, then fills remaining slots with heavy hitters.
SampleResult uniform_sample_and_select(
    const std::vector<Row>& dataset,
    size_t fa_capacity,
    double sample_frac,
    double delta,
    double alpha_ci,
    double beta_ci,
    uint64_t seed = 42);
```

### `main.cpp` Structure — How Everything Wires Together

```cpp
#include "zippy.h"
#include "utils.h"
#include <cstdlib>
#include <cstring>
#include <stdexcept>

int main(int argc, char* argv[]) {
    // 1. Parse arguments (hand-rolled — no getopt dependency)
    std::string input_path, output_path, mode = "baseline";
    int k = 50;
    size_t n_rows = 0;
    ZippyConfig cfg;
    bool output_fa_groups = false;

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
        else if (!strcmp(argv[i], "--output-fa-groups")) output_fa_groups = true;
        else if (!strcmp(argv[i], "--verbose"))      cfg.verbose = true;
    }
    if (input_path.empty() || output_path.empty() || n_rows == 0) {
        fprintf(stderr, "Usage: zippy --input <path> --n-rows <N> --k <k> "
                        "--mode <mode> --output <path> [options]\n");
        return 1;
    }

    // 2. Load dataset (timing excluded)
    std::vector<Row> dataset(n_rows);
    {
        FILE* f = fopen(input_path.c_str(), "rb");
        if (!f) { perror("fopen"); return 1; }
        if (fread(dataset.data(), sizeof(Row), n_rows, f) != n_rows) {
            fprintf(stderr, "Short read from %s\n", input_path.c_str());
            return 1;
        }
        fclose(f);
    }

    // 3. Dispatch to the appropriate mode
    std::vector<std::pair<uint64_t,double>> results;
    std::vector<uint64_t> fa_groups;
    RunMetrics metrics;
    cfg.output_fa_groups = output_fa_groups;

    Timer total_timer; total_timer.reset();

    if (mode == "brute-force") {
        results = run_brute_force(dataset, k);
        metrics.total_duration_ms = total_timer.elapsed_ms();
        metrics.is_optimizable    = true;   // brute-force always "works"
    } else if (mode == "baseline") {
        metrics = run_zippy_baseline(dataset, k, cfg, results, fa_groups);
    } else if (mode == "ext-a") {
        metrics = run_zippy_ext_a(dataset, k, cfg, results, fa_groups);
    } else if (mode == "ext-b") {
        metrics = run_zippy_ext_b(dataset, k, cfg, results, fa_groups);
    } else if (mode == "ext-ab") {
        metrics = run_zippy_ext_ab(dataset, k, cfg, results, fa_groups);
    } else {
        fprintf(stderr, "Unknown mode: %s\n", mode.c_str());
        return 1;
    }

    // 4. Write output JSON
    write_output_json(output_path, mode, k, n_rows,
                      /* n_groups estimate */ 0,  // fill in from sampler if available
                      results, metrics, fa_groups, output_fa_groups);
    return 0;
}
```

### `CATable::update()` — Full Implementation Body

The `update()` method is the hot path called for every non-FA row in Pass 1. It must update all five fields of `CAPartition`:

```cpp
void CATable::update(uint64_t group_id, double val) {
    size_t p = partition_of(group_id);          // partition_hash(group_id, n_partitions)
    CAPartition& part = partitions[p];
    part.total_sum += val;
    if (val > part.max_value) part.max_value = val;
    if (val < part.min_value) part.min_value = val;
    part.count++;
    part.fm.update(group_id);                   // FM sketch update (uses fm_hash)
}

// Initialise CAPartition to safe defaults (call in CATable constructor):
CAPartition zero_partition() {
    return {
        .total_sum = 0.0,
        .max_value = std::numeric_limits<double>::lowest(),
        .min_value = std::numeric_limits<double>::max(),
        .count     = 0,
        .fm        = FMSketch{},
        .pruned    = false
    };
}
```

### Multi-Pass Child Partitioning — How Pass 3+ Works

On Pass 2+, each surviving partition is re-partitioned into child partitions using `child_partition_hash()` with the current pass number mixed in. The child partition count should be roughly `n_partitions` again — surviving partitions are divided, not the whole dataset:

```
Pass 1: route all N rows → n_partitions CA buckets using partition_hash(gid, n_partitions)
        → prune → survivors: {p3, p7, p12, ...}

Pass 2: for each surviving partition pX:
          re-scan full dataset, filter rows where partition_hash(gid, n_partitions) == pX
          re-route those rows → child_partition_hash(gid, n_child, pass=2) child buckets
          → prune child buckets → survivors carried to Pass 3

Pass 3: repeat on surviving child buckets, now pass=3 in child_partition_hash
        → each generation of partitions is finer-grained
```

In the single-threaded prototype, "filter rows for partition pX" means a full re-scan of the original dataset each pass. The child partition count is controlled by `n_partitions` (same constant). Each pass, fewer rows remain unresolved, so passes get progressively cheaper:

```cpp
// Simplified multi-pass loop skeleton in run_zippy_baseline():
int pass = 1;
std::unordered_set<size_t> active_partitions;  // initially: all n_partitions

while (top_k_confirmed < k && !active_partitions.empty()) {
    PartialAggregates partial;
    ChildPartitions   children;

    for (const auto& row : dataset) {
        if (fa.contains(row.group_id)) {
            partial[row.group_id] += row.value;
            continue;
        }
        size_t p;
        if (pass == 1)
            p = partition_hash(row.group_id, n_partitions);
        else
            p = child_partition_hash(row.group_id, n_partitions, pass);

        if (active_partitions.count(p))
            children[p].update_from_row(row);  // update stats + FM sketch
    }

    // Merge + Prune (Algorithm 4)
    for (auto& [gid, val] : partial)
        exactAggregates[gid] += val;

    double topKBound = compute_topkbound(exactAggregates, children, k);
    active_partitions = prune_partitions(children, topKBound);
    pass++;
}
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
                          baseline    = Zippy with uniform sampling only
                          ext-a       = Zippy + stratified sampling (Extension A)
                          ext-b       = Zippy + measure index (Extension B)
                          ext-ab      = Zippy + both extensions
                          brute-force = naive full groupby (correctness reference)

Zippy core parameters:
  --fa-capacity <int>     Number of groups FA hash table can hold (default: 50000)
                          Allocated array size = 2 × fa-capacity (50% load factor)
                          Rule of thumb: set so fa-capacity × 16 bytes ≈ L2_size / 2
  --n-partitions <int>    Number of CA logical partitions (default: 10000)
                          Rule of thumb: set so n-partitions × ~48 bytes ≈ L2_size / 2
  --sample-frac <float>   Uniform sample fraction (default: 0.01)
                          Actual sample size = max(ceil(z²/(4Δ²)), sample-frac × N)

Sampling validation parameters:
  --delta <float>         Sampling tolerance Δ ∈ (0,1] (default: 0.05)
                          Groups with proportion < Δ are excluded from CI analysis
                          Sample size formula: s ≥ (z_{α/2})² / (4 × Δ²)
  --alpha-ci <float>      CI confidence level α for sample representativeness (default: 0.05)
                          Controls sample size via z_{α/2} = 1.96 when α=0.05
  --beta-ci <float>       CI confidence level β for Hoeffding skew validation (default: 0.95)
                          Controls CI half-width: ε = (b-a)×sqrt(ln(2/(1-β))/(2n'_i))

Extension A parameters:
  --underrep-threshold <float>  Fraction below expected to trigger boost (default: 0.5)
  --boost-rows <int>            Rows to fetch per underrepresented group (default: 10)

Extension B parameters:
  --measure-m <int>       Number of extreme-value rows to track (default: 500)

Output:
  --output <path>         Path to write JSON results file
  --verbose               Print per-pass statistics to stderr
  --output-fa-groups      Include FA group ID list in JSON output (for fa_hit_rate
                          computation). Adds overhead — disable for timing runs.
```

### Brute-Force Mode Implementation

The `brute-force` mode is the correctness reference. It computes exact aggregates
for every group in one pass, then selects the top-k. It must NOT use FA/CA.

```cpp
std::vector<std::pair<uint64_t,double>> brute_force(
    const std::vector<Row>& dataset, size_t k)
{
    std::unordered_map<uint64_t, double> agg;
    agg.reserve(dataset.size() / 10);  // rough capacity hint
    for (const auto& row : dataset)
        agg[row.group_id] += row.value;

    // Partial sort: find top-k without fully sorting all M groups
    std::vector<std::pair<uint64_t,double>> all(agg.begin(), agg.end());
    std::partial_sort(all.begin(), all.begin() + k, all.end(),
        [](const auto& a, const auto& b){ return a.second > b.second; });
    all.resize(k);
    return all;
}
```

### Pass 2+ Re-scan Implementation (Logical Partitioning)

With logical partitioning, surviving partitions are identified by their hash value
but the rows are NOT physically co-located. Pass 2 therefore re-scans the **entire
original dataset** and only processes rows whose partition hash is in the survivor
set. This is the key cost of logical vs physical partitioning.

```cpp
// After MergeAndPrune, surviving partition hashes are known:
std::unordered_set<uint64_t> surviving_hashes = ca.surviving_partition_hashes();

// Pass 2: re-scan full dataset, aggregate only rows in surviving partitions
std::unordered_map<uint64_t, double> pass2_agg;
for (const auto& row : dataset) {
    if (fa.contains(row.group_id)) {
        // FA groups: continue accumulating (already done in pass 1, but
        // for logical partitioning we must re-confirm their exact totals)
        // In practice: FA exact aggregates are already complete after pass 1.
        // Do NOT re-add them here — exactAggregates already has their totals.
        continue;
    }
    uint64_t ph = partition_hash(row.group_id, n_partitions);
    if (surviving_hashes.count(ph)) {
        pass2_agg[row.group_id] += row.value;
    }
}
// Now pass2_agg contains exact aggregates for all groups in surviving partitions.
// Merge with exactAggregates and re-run MergeAndPrune.
```

**Note:** In subsequent passes, `partitions` refers to child partitions of the
survivors (sub-partitions with a finer hash), not the original n_partitions
partition scheme. Each pass re-hashes with a different modulus or a deeper hash
to create child partitions from the survivors.

```json
{
  "mode": "ext-ab",
  "k": 50,
  "n_rows": 10000000,
  "n_groups": 1000000,
  "top_k_results": [
    {"group_id": 42, "aggregate": 9821.5},
    "..."
  ],
  "metrics": {
    "is_optimizable": true,
    "total_passes": 2,
    "sample_size_actual": 100000,
    "fa_candidates_count": 48731,
    "sample_duration_ms": 22.1,
    "pass1_duration_ms": 143.2,
    "pass2plus_duration_ms": 12.1,
    "total_duration_ms": 177.4,
    "index_build_duration_ms": 88.4,
    "fa_hit_rate": 0.94,
    "topKBound_after_pass1": 18432.7,
    "partitions_pruned_pct": 0.97
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

The Python generator writes this format (little-endian, no padding):

```python
import struct

def write_dataset(path, group_ids, values):
    """
    group_ids: np.ndarray of uint64
    values:    np.ndarray of float64
    Writes interleaved (group_id, value) pairs, 16 bytes per row, little-endian.
    """
    with open(path, 'wb') as f:
        for gid, val in zip(group_ids, values):
            f.write(struct.pack('<Qd', int(gid), float(val)))
    # For large datasets (>1M rows), use the faster vectorised approach:
    # import numpy as np
    # buf = np.empty(len(group_ids) * 2, dtype=np.float64)
    # buf.view(np.uint64)[0::2] = group_ids   # safe: same byte width
    # buf[1::2] = values
    # buf.tofile(path)
```

The C++ reader reads it as:

```cpp
struct Row { uint64_t group_id; double value; };
// mmap or fread into vector<Row>
```

---

## 9. Python Data Generator Spec

File: `python/generate_data.py`

**Required packages:** `numpy`, `struct`

Must support generating datasets with configurable properties:

```python
def generate_dataset(
    output_path: str,
    n_rows: int,
    n_groups: int,
    zipf_alpha: float,      # Zipf skew parameter (must be > 1.0). Higher = more skew.
                            # 1.2 = moderate (matches paper's real datasets)
                            # 2.0 = heavy skew
    value_distribution: str,  # "exponential" | "uniform" | "constant"
    value_scale: float,     # mean/scale of value distribution (all values > 0)
    rare_group_fraction: float,  # fraction of groups to make "rare high-value"
    rare_group_rows: int,   # max rows per rare group (e.g., 1–5)
    rare_group_value_multiplier: float,  # rare group values = multiplier × normal values
    seed: int = 42
) -> dict:                  # returns metadata dict
```

### Correct Zipf Generation Pattern

`numpy.random.Generator.zipf(alpha)` returns ranks in [1, ∞), NOT bounded group IDs.
Map ranks to bounded group IDs using modulo:

```python
import numpy as np

def generate_dataset(output_path, n_rows, n_groups, zipf_alpha,
                     value_distribution="exponential", value_scale=100.0,
                     rare_group_fraction=0.0, rare_group_rows=3,
                     rare_group_value_multiplier=100.0, seed=42):
    rng = np.random.default_rng(seed)

    # Generate Zipf-distributed group IDs bounded to [0, n_groups - 1]
    # zipf() returns ranks in [1, ∞) — subtract 1 and take modulo
    ranks = rng.zipf(zipf_alpha, size=n_rows)
    group_ids = ((ranks - 1) % n_groups).astype(np.uint64)

    # Generate positive values
    if value_distribution == "exponential":
        values = rng.exponential(scale=value_scale, size=n_rows)
    elif value_distribution == "uniform":
        values = rng.uniform(0.0, value_scale * 2, size=n_rows)
    elif value_distribution == "constant":
        values = np.full(n_rows, value_scale)
    else:
        raise ValueError(f"Unknown value_distribution: {value_distribution}")

    # Inject rare high-value groups (adversarial pattern for extensions)
    n_rare = int(n_groups * rare_group_fraction)
    if n_rare > 0:
        rare_ids = np.arange(n_groups, n_groups + n_rare, dtype=np.uint64)
        extra_group_ids = []
        extra_values = []
        for gid in rare_ids:
            n_rare_rows = rng.integers(1, rare_group_rows + 1)
            extra_group_ids.extend([gid] * n_rare_rows)
            extra_values.extend([value_scale * rare_group_value_multiplier] * n_rare_rows)
        group_ids = np.concatenate([group_ids,
                                    np.array(extra_group_ids, dtype=np.uint64)])
        values    = np.concatenate([values,
                                    np.array(extra_values, dtype=np.float64)])
        # Shuffle so rare groups are not all at the end
        perm = rng.permutation(len(group_ids))
        group_ids = group_ids[perm]
        values    = values[perm]

    # Mask group_ids to exclude UINT64_MAX (FA sentinel)
    group_ids = np.where(group_ids == np.iinfo(np.uint64).max,
                         np.uint64(0), group_ids)

    # Write binary file: interleaved (group_id: uint64, value: float64) rows
    # Each row = 16 bytes. Use struct.pack for correctness (no reinterpret tricks).
    actual_n_rows = len(group_ids)
    with open(output_path, 'wb') as f:
        import struct
        for gid, val in zip(group_ids, values):
            f.write(struct.pack('<Qd', int(gid), float(val)))

    # For large datasets, use the faster numpy approach:
    # data = np.empty(actual_n_rows * 2, dtype=np.float64)
    # data.view(np.uint64)[0::2] = group_ids   # write uint64 into float64 buffer
    # data[1::2] = values
    # data.tofile(output_path)
    # Note: the view() approach works because both types are 8 bytes, but
    # verify with: assert data.view(np.uint64)[0::2][0] == group_ids[0]

    return {
        "n_rows": actual_n_rows,
        "n_groups_base": n_groups,
        "n_groups_total": n_groups + n_rare,
        "n_rare_groups": n_rare,
        "zipf_alpha": zipf_alpha,
        "output_path": output_path,
    }
```

### Generating Rare High-Value Groups

The key adversarial pattern is already embedded above. The rare groups:
- Have IDs in `[n_groups, n_groups + n_rare)` — distinct from normal groups
- Have only 1–`rare_group_rows` rows each — almost never sampled uniformly
- Have values = `value_scale × rare_group_value_multiplier` — high enough to
  appear in true top-k despite low frequency
- Are shuffled into the dataset randomly so they cannot be found by position

### How to Compute `fa_hit_rate`

`fa_hit_rate` requires knowing (a) which groups were in FA after sampling, and
(b) which groups are in the true top-k. The C++ binary must emit the FA group IDs
in the output JSON. The Python driver then computes:

```python
def compute_fa_hit_rate(zippy_result, brute_force_result, k):
    true_topk = set(r["group_id"] for r in brute_force_result[:k])
    fa_groups  = set(zippy_result.get("fa_group_ids", []))
    hits = true_topk & fa_groups
    return len(hits) / k

# The C++ binary must include in JSON output:
# "fa_group_ids": [42, 1337, 9999, ...]  ← list of group_ids placed in FA
```

Add `--output-fa-groups` flag to the C++ CLI to enable this output (it adds
overhead and should be off by default in timing runs).

---

## 10. Metrics to Collect

Every experiment run must collect these metrics for comparison across modes:

| Metric | Description | Why It Matters |
|--------|-------------|----------------|
| `fa_hit_rate` | Fraction of true top-k groups that appear in FA after sampling | Core measure of sampling quality — our extensions should improve this |
| `partitions_pruned_pct` | Fraction of CA partitions pruned after Pass 1 | Higher = less work in Pass 2+ |
| `total_passes` | Number of data passes until convergence | Main efficiency metric — our extensions should reduce this |
| `total_duration_ms` | Wall-clock time for full query | Overall performance |
| `sample_duration_ms` | Time for the sampling + CI validation phase | Baseline overhead of Algorithm 2 |
| `index_build_duration_ms` | Time to build GroupIndex / MeasureIndex | Extension overhead cost |
| `pass1_duration_ms` | Wall-clock time for Pass 1 (full data scan) | Dominant cost in most runs |
| `pass2plus_duration_ms` | Wall-clock time for all passes after Pass 1 | Should shrink with better extensions |
| `topKBound_after_pass1` | Value of topKBound immediately after Pass 1 pruning step | Higher = better pruning; directly shows extension impact |
| `fa_candidates_count` | Number of groups placed in FA before Pass 1 | Sanity check: should equal min(fa_capacity, CI-qualifying groups) |
| `is_optimizable` | Whether Zippy's skew validation passed | If false, fell back to brute-force |
| `sample_size_actual` | Actual number of rows sampled | Should equal max(formula bound, sample_frac × N) |

---

## 11. Python Experiment Driver and Plotting Spec

### `python/run_experiments.py`

**Required packages:** `subprocess`, `json`, `pathlib`, `itertools`, `pandas`

The driver's outer structure:

```python
import subprocess, json, itertools
from pathlib import Path

DATASETS = [
    {"id": "S1", "n_rows": 10_000_000, "n_groups": 1_000_000,
     "zipf_alpha": 1.2, "rare_group_fraction": 0.0},
    {"id": "S2", "n_rows": 10_000_000, "n_groups": 1_000_000,
     "zipf_alpha": 1.2, "rare_group_fraction": 0.0001,
     "rare_group_rows": 3, "rare_group_value_multiplier": 100},
    {"id": "S3", "n_rows": 10_000_000, "n_groups": 1_000_000,
     "zipf_alpha": 1.2, "rare_group_fraction": 0.001,
     "rare_group_rows": 3, "rare_group_value_multiplier": 100},
    {"id": "S4", "n_rows": 50_000_000, "n_groups": 5_000_000,
     "zipf_alpha": 1.2, "rare_group_fraction": 0.0001,
     "rare_group_rows": 3, "rare_group_value_multiplier": 100},
    {"id": "S5", "n_rows": 10_000_000, "n_groups": 1_000_000,
     "zipf_alpha": 0.8, "rare_group_fraction": 0.0001,
     "rare_group_rows": 3, "rare_group_value_multiplier": 100},
]
MODES = ["brute-force", "baseline", "ext-a", "ext-b", "ext-ab"]
K = 50
RESULTS_DIR = Path("results")
RESULTS_DIR.mkdir(exist_ok=True)

def run_zippy(mode, dataset_meta, k, extra_args=None):
    ds = dataset_meta
    output_path = RESULTS_DIR / f"{ds['id']}_{mode}_k{k}.json"
    cmd = ["./build/zippy",
           "--input",   f"data/{ds['id']}.bin",
           "--n-rows",  str(ds["n_rows"]),
           "--k",       str(k),
           "--mode",    mode,
           "--output",  str(output_path)]
    if extra_args:
        cmd.extend(extra_args)
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    if r.returncode != 0:
        raise RuntimeError(f"[{mode}/{ds['id']}] Failed:\n{r.stderr}")
    return json.loads(output_path.read_text())

# Main experiment loop
all_results = []
for ds in DATASETS:
    for mode in MODES:
        result = run_zippy(mode, ds, K)
        result["dataset"] = ds["id"]
        all_results.append(result)

# Parameter sweeps (on S2 only)
s2 = next(d for d in DATASETS if d["id"] == "S2")
for m in [50, 100, 250, 500, 1000, 2500, 5000]:
    result = run_zippy("ext-b", s2, K, ["--measure-m", str(m)])
    result["sweep"] = "measure_m"; result["sweep_val"] = m
    all_results.append(result)

for thresh in [0.1, 0.25, 0.5, 0.75, 1.0]:
    result = run_zippy("ext-a", s2, K, ["--underrep-threshold", str(thresh)])
    result["sweep"] = "underrep_threshold"; result["sweep_val"] = thresh
    all_results.append(result)

for k_val in [1, 5, 10, 25, 50, 100]:
    for mode in ["baseline", "ext-ab"]:
        result = run_zippy(mode, s2, k_val)
        result["sweep"] = "k"; result["sweep_val"] = k_val
        all_results.append(result)

import json
Path("results/all_results.json").write_text(json.dumps(all_results, indent=2))
```

### `python/plot_results.py`

**Required packages:** `matplotlib`, `pandas`, `json`

Produce these plots (save as PNG to `plots/` directory):

| Plot filename | X-axis | Y-axis | Series | Dataset |
|---|---|---|---|---|
| `fa_hit_rate_by_mode.png` | Dataset (S1–S5) | fa_hit_rate | mode | all |
| `passes_by_mode.png` | Dataset (S1–S5) | total_passes | mode | all |
| `duration_by_mode.png` | Dataset (S1–S5) | total_duration_ms | mode | all |
| `pruned_pct_by_mode.png` | Dataset (S1–S5) | partitions_pruned_pct | mode | all |
| `sweep_m_passes.png` | measure_m | total_passes | — | S2 |
| `sweep_m_duration.png` | measure_m | total_duration_ms + index_build_duration_ms | — | S2 |
| `sweep_threshold_hits.png` | underrep_threshold | fa_hit_rate | — | S2 |
| `sweep_k_passes.png` | k | total_passes | baseline vs ext-ab | S2 |
| `topkbound_comparison.png` | Dataset (S2, S3) | topKBound_after_pass1 | mode | S2,S3 |

All bar charts use the mode as colour legend. All line charts use markers.
Title each plot clearly. Save at 150 DPI.

---

## 12. Experiment Matrix

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

## 13. Correctness Verification

**Rule:** The top-k result from any mode must exactly match the brute-force result
for every experiment. This is checked in `python/verify_correctness.py`.

The set of top-k group IDs must be identical — order does not matter since ranks
within top-k can vary for equal aggregates, but the *set* must be the same.

```python
#!/usr/bin/env python3
"""
python/verify_correctness.py

Runs baseline, ext-a, ext-b, ext-ab against brute-force on datasets S1, S2, S3
and verifies correctness of all modes.

Usage: python verify_correctness.py
Prerequisite: build/zippy is compiled, data/S1.bin, data/S2.bin, data/S3.bin exist.
"""
import subprocess, json, sys
from pathlib import Path

BINARY  = "./build/zippy"
DATA_DIR = Path("data")
RESULTS  = Path("results/correctness")
RESULTS.mkdir(parents=True, exist_ok=True)

DATASETS = [
    {"id": "S1", "n_rows": 10_000_000},
    {"id": "S2", "n_rows": 10_000_000},
    {"id": "S3", "n_rows": 10_000_000},
]
MODES = ["baseline", "ext-a", "ext-b", "ext-ab"]
K = 50

def run(mode, ds_id, n_rows, extra=None):
    out = RESULTS / f"{ds_id}_{mode}.json"
    cmd = [BINARY, "--input", str(DATA_DIR / f"{ds_id}.bin"),
           "--n-rows", str(n_rows), "--k", str(K),
           "--mode", mode, "--output", str(out)]
    if extra: cmd.extend(extra)
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    if r.returncode != 0:
        raise RuntimeError(f"[{mode}/{ds_id}] FAILED:\n{r.stderr}")
    return json.loads(out.read_text())

def top_k_set(result):
    return set(r["group_id"] for r in result["top_k_results"])

failures = 0
for ds in DATASETS:
    print(f"\n=== Dataset {ds['id']} ===")
    bf = run("brute-force", ds["id"], ds["n_rows"])
    bf_set = top_k_set(bf)
    print(f"  brute-force top-{K}: {sorted(bf_set)[:5]}...")

    for mode in MODES:
        result = run(mode, ds["id"], ds["n_rows"])
        got_set = top_k_set(result)
        passes  = result["metrics"]["total_passes"]
        pruned  = result["metrics"]["partitions_pruned_pct"]

        if got_set == bf_set:
            print(f"  ✓ {mode:12s}  passes={passes}  pruned={pruned:.1%}")
        else:
            print(f"  ✗ {mode:12s}  CORRECTNESS FAILURE")
            print(f"    Missing from result : {bf_set - got_set}")
            print(f"    Extra in result     : {got_set - bf_set}")
            failures += 1

print(f"\n{'ALL PASSED' if failures == 0 else f'{failures} FAILURES'}")
sys.exit(0 if failures == 0 else 1)
```

Always run this before the full experiment matrix. If any failure occurs, do not proceed to benchmarking.

---

## 14. Dependencies

### C++ (no external libraries required beyond the standard library)

All C++ code uses only the C++17 standard library. No third-party dependencies.

Required headers:
```cpp
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>      // std::partial_sort, std::sort, std::nth_element
#include <queue>          // std::priority_queue (MeasureIndex)
#include <cstdint>        // uint64_t, UINT64_MAX
#include <cmath>          // std::sqrt, std::log
#include <cstdio>         // fread, fopen, fclose
#include <cstring>        // memset
#include <chrono>         // high_resolution_clock (timing)
#include <string>
#include <cassert>
#include <cstdlib>        // getopt / argument parsing
// JSON output: write by hand (sprintf/fprintf) — no JSON library needed
// The output JSON is simple enough to write directly without a library
```

Compiler: `g++ -std=c++17 -O3 -march=native` or `clang++ -std=c++17 -O3`

### Python

```
numpy>=1.24          # data generation, file I/O
matplotlib>=3.7      # plotting
pandas>=2.0          # result aggregation in run_experiments.py
```

Install: `pip install numpy matplotlib pandas`

No other Python dependencies. The C++ binary is called via `subprocess`.

---

## 15. Build System

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

## 16. Python Driver Interface

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

## 17. Key Invariants and Edge Cases

Agents must preserve these invariants in all code:

1. **FA never exceeds capacity.** If the sample produces more candidates than
   `fa_capacity`, take only the top `fa_capacity` by estimated aggregate. The
   capacity check is on cache space `Cs = occupied × sizeof(FAEntry)`, not raw
   group count — though in practice these are equivalent when `sizeof(FAEntry) = 16`.

2. **FORCED_SET from MeasureIndex takes priority in FA.** In ext-b and ext-ab,
   the measure index groups always occupy FA slots, even if the sampler would
   have chosen different groups. Remaining slots go to sampler candidates.

3. **Pruning is based on `total_sum`, not `max_value`.** `CA[p].max_value` is the
   largest single row value seen in partition p, not the largest group total. Do not
   use it as the SUM pruning bound — it would be incorrect. Use `CA[p].total_sum`
   for SUM queries. `max_value` is only the correct UB for MAX queries.

4. **All values must be non-negative** for the `total_sum` pruning invariant to hold.
   The generator enforces this. If supporting negative values in future, the pruning
   logic must change.

5. **FA sentinel key must never appear as a real group_id.** The value `UINT64_MAX`
   is reserved as the empty-slot marker in the FA hash table. Mask all generated
   group_ids to `[0, UINT64_MAX - 1]` in the data generator. The C++ Pass 1 loop
   must assert `group_id != UINT64_MAX` in debug builds.

6. **FM sketch uses a hash function independent of the partition hash.** Using the
   same hash function for both the partition assignment and the FM sketch introduces
   correlation that corrupts the distinct count estimate. Use two different hash
   functions — e.g., multiply-shift with different constants.

7. **Brute-force baseline must be single-threaded.** For fair comparison, no
   parallelism anywhere in this prototype.

8. **Timing must exclude dataset loading from disk.** Start the clock after the data
   is in memory. Index build time is measured separately and added to overhead.

---

## 18. What NOT to Implement (Scope Boundaries)

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

## 19. Summary of What We Are Contributing

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