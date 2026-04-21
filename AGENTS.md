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
    Section 16). Physical partitioning (lines 29–33) is out of scope.
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
| `sample_duration_ms` | Time for the sampling + CI validation phase | Baseline overhead of Algorithm 2 |
| `index_build_duration_ms` | Time to build GroupIndex / MeasureIndex | Extension overhead cost |
| `pass1_duration_ms` | Wall-clock time for Pass 1 (full data scan) | Dominant cost in most runs |
| `pass2plus_duration_ms` | Wall-clock time for all passes after Pass 1 | Should shrink with better extensions |
| `topKBound_after_pass1` | Value of topKBound immediately after Pass 1 pruning step | Higher = better pruning; directly shows extension impact |
| `fa_candidates_count` | Number of groups placed in FA before Pass 1 | Sanity check: should equal min(fa_capacity, CI-qualifying groups) |
| `is_optimizable` | Whether Zippy's skew validation passed | If false, fell back to brute-force |
| `sample_size_actual` | Actual number of rows sampled | Should equal max(formula bound, sample_frac × N) |

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