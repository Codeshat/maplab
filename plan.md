# flatmap-lab — Swiss-Table-Style Hashmap Build Plan

**Goal:** a cache-friendly, SIMD-probed, open-addressing hashmap in modern C++ — framed not as "faster than Boost" but as a **design laboratory**: every major design decision gets implemented, toggled, measured, and written up. The deliverable is the table *plus* an experiments document full of graphs, which is a rarer and more interesting artifact than yet another "blazing fast" repo.

This framing is your headline. A README section titled "Design decisions, measured" — where you show what the H2 filter buys, what load factor costs, what a bad hash does — demonstrates more understanding than a benchmark victory would, and no interviewer can ask "but is it faster than folly?" in a way that hurts you, because that was explicitly never the claim.

**Scope rules:**
- Single-threaded. Concurrent maps are a different (much harder) project; say so in limitations.
- Not allocator-templated beyond the default; no node handles; basic exception guarantees only (document: assumes nothrow-movable types for the fast path, `static_assert` it).
- Reference baselines (`std::unordered_map`, `boost::unordered_flat_map` or `absl::flat_hash_map`, `ankerl::unordered_dense`) are *context lines on graphs*, not targets.
- Reuse everything from simdfix: CMake presets, sanitizer configs, CI workflow, benchmark conventions, RESULTS.md discipline.

**Rough budget:** ~2 weeks part-time. Phases 0–5 are the table (week 1). Phases 6–8 are benchmarks, experiments, and writeup (week 2) — and week 2 is the point of the project, so don't let week 1 eat it.

---

## Phase 0: Repo & tooling (half a day — mostly copy-paste from simdfix)

1. New repo (`flatmap-lab` or similar — the "lab" in the name sets the framing before anyone reads a word). Same layout: `include/flatmap/`, `tests/`, `bench/`, `experiments/`, `.github/workflows/`.
2. Port your CMake presets, warning flags, sanitizer configs, clang-format, CI matrix from simdfix wholesale. This should take an hour, and the fact that it does is your last project paying rent.
3. FetchContent: Catch2, Google Benchmark, plus the baselines — `boost::unordered_flat_map` (via boost-unordered standalone or full Boost), `ankerl::unordered_dense` (single header, trivial). Abseil is a heavier dependency; one of Boost/Abseil is enough, both is stretch.
4. Add a `bench/plots/` Python script (matplotlib) that turns Google Benchmark's JSON output into PNGs. Graphs in the README are non-negotiable for this project — the experiments *are* graphs.

## Phase 1: Design doc before code (half a day)

Write `DESIGN.md` first — one page, committed before the implementation. Interviewers who open the repo and find a design doc written *ahead of* the code will assume you're senior beyond your years, because almost nobody does this.

Decisions to make and record (each with a one-sentence rationale — you'll expand them into experiments later):

- **Layout:** open addressing, flat storage. One contiguous allocation: control-byte array + slot array (`std::pair<Key, T>` storage). One control byte per slot: `0x80` empty, `0xFE` tombstone, else `0x00–0x7F` = H2.
- **Hash split:** H1 = hash >> 7 (position), H2 = hash & 0x7F (the 7-bit fingerprint stored in the control byte).
- **Groups of 16** control bytes probed at once with SSE2 (`_mm_cmpeq_epi8` + `_mm_movemask_epi8`). Note in the doc: SSE2 is baseline x86-64, so *no runtime dispatch needed* — a deliberate contrast with your simdfix AVX2 story, and a talking point in itself.
- **Probing:** linear within a group; between groups, triangular-number stride (1, 3, 6, 10… groups), which with power-of-two group counts is guaranteed to visit every group exactly once. Know the proof sketch.
- **Capacity:** power of two, index via mask. Consequence: you *must* mix the hash — record that identity `std::hash<int>` + mask is a known catastrophe you will demonstrate in experiments. Default hasher: a strong finalizer (murmur3 fmix64 is 5 lines) or wyhash.
- **Max load factor 7/8**, growth ×2.
- **Deletion: tombstones** (abseil-style) as the primary; backward-shift as a Phase 7 experiment if time allows.
- **Sentinel/mirror group** at the end of the control array (clone of the first group) so a group load starting near the end never needs a wraparound branch. Small detail, disproportionately impressive to explain.
- **API surface:** `insert`/`emplace`/`try_emplace`, `find`, `contains`, `erase(key)`, `reserve`, `clear`, iterators, `load_factor`. C++20 concepts constraining `Hash`/`KeyEqual`. Heterogeneous lookup (transparent hash) as stretch.
- **Iterator invalidation:** everything invalidates on rehash; erase invalidates the erased iterator only. Write it down; it's a guaranteed interview question.

## Phase 2: Scalar reference table first (1–1.5 days)

Build the whole table with a *scalar* probe loop (byte-at-a-time control scan) before any SIMD. Reasons: (a) it's your correctness reference, (b) it's your ablation baseline — "SIMD probe vs scalar probe on the identical layout" is Experiment #1 and needs this to exist, (c) debugging probing logic and SIMD masks simultaneously is misery.

Order of work:
1. Control array + metadata, `find` on an empty/static table, then `insert` without growth, then growth/rehash, then `erase` with tombstones, then iterators.
2. **The memory-management layer is the real lesson of this phase** — slots are *raw storage*: allocate bytes (`operator new` with `std::align_val_t`), create elements with `std::construct_at`, destroy with `std::destroy_at`, never default-construct the array. Growth = allocate new, move-construct occupied slots over, destroy + deallocate old. This is where you meet placement lifetime rules, `std::launder` discussions, and alignment — UB that sanitizers only partially catch and interviewers love.
3. **Tests, written alongside:**
   - Differential test against `std::unordered_map`: random operation sequences (insert/erase/find/reserve/clear, mixed), apply to both, assert identical contents after every N ops. This is your single highest-value test — port the pattern from simdfix's corpus testing.
   - Targeted cases: growth mid-insert, erase-then-reinsert same key, all-tombstone probe chains, find on empty table, keys that collide in H1, keys that collide in H2 but not full hash (hand-craft these).
   - Non-trivial value types: a counting type that tracks constructions/destructions; assert balance at destruction (catches lifetime bugs ASan can miss on raw storage).
   - Everything under ASan/UBSan in CI.

**Checkpoint:** scalar table passes the differential test for a million random ops. Do not proceed on a table that mostly works.

## Phase 3: SIMD group probing (1 day)

Swap the scalar control scan for the group probe:

```cpp
// probe one group of 16 control bytes
__m128i ctrl  = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ctrl_ + group * 16));
__m128i match = _mm_cmpeq_epi8(ctrl, _mm_set1_epi8(h2));
std::uint32_t candidates = _mm_movemask_epi8(match);   // bits = slots whose H2 matches
// iterate candidates with countr_zero / blsr — same pattern as your FIX scanner
std::uint32_t empties = _mm_movemask_epi8(_mm_cmpeq_epi8(ctrl, _mm_set1_epi8(kEmpty)));
// any empty in group ⇒ key absent ⇒ stop probing (this is the miss fast-path)
```

- The **unsuccessful-lookup fast path** — "group contains an empty slot ⇒ terminate" — is the crown jewel; make sure your miss benchmark later isolates it.
- Keep the scalar probe compiled-in behind a template parameter or policy tag (`probe_policy::scalar` / `probe_policy::simd`), *not* deleted — it's Experiment #1's control group.
- Re-run the full differential test suite against the SIMD build. Any divergence is a mask bug; this is exactly why Phase 2 came first.

> **Interview angles:** Why does one movemask replace up to 16 comparisons *and* why is the false-positive rate per slot 1/128 (7-bit H2)? Expected number of full key comparisons per lookup as a function of load factor? Why is this the same instruction pattern as your FIX delimiter scan (byte-broadcast compare → movemask → bit iteration — data-oriented design is one trick applied everywhere)?

## Phase 4: Hardening details (half a day)

- Mirror group / sentinel at array end (kills the wraparound branch in group loads).
- `reserve()` that actually pre-sizes to avoid rehash during a known-size insert burst (benchmark Phase 6 depends on it).
- Rehash policy for tombstone accumulation: when tombstones exceed a threshold, rehash in place at same capacity ("drain"). Without this, churn workloads degrade forever — and *showing* that degradation before the fix is a great experiment (log it as a candidate for Phase 7).
- `static_assert`s on nothrow-move for the fast growth path; concept-check `Hash` and `KeyEqual`.

## Phase 5: Statistics hooks (half a day — this is what makes it a *lab*)

Add a compile-time-flagged stats mode (`FLATMAP_STATS`) collecting: probe length per operation (histogram), groups touched, H2 false positives (H2 matched, full key didn't), tombstones encountered. Zero cost when off (everything behind `if constexpr`).

Probe-length histograms are the experimental instrument for the entire next phase — "mean probe length vs load factor, measured" turns hand-waving into science, and almost no student project has this.

---

## Phase 6: Benchmark suite (1–1.5 days)

Structure identical to simdfix conventions (pinning, governor, `DoNotOptimize`, fixed seeds, RESULTS.md with machine/compiler/flags).

**Workloads** (each its own benchmark):
1. Insert N unique keys, with and without `reserve` (the gap *is* the cost of rehashing — say so on the graph).
2. Successful lookup (uniform random over resident keys).
3. **Unsuccessful lookup** (keys guaranteed absent) — the miss fast-path showcase; many real workloads are miss-heavy.
4. Erase-heavy churn: sustained insert+erase at constant size (the tombstone stress test).
5. Mixed realistic: 90% find / 9% insert / 1% erase.
6. Finance-flavored workload for the CV tie-in: `std::uint64_t` order-ID keys with insert-on-new-order / erase-on-fill churn patterns, and a `string→T` symbol-table workload with ~2k short string keys. Costs an hour, converts the project from generic to domain-relevant.

**Axes to sweep:**
- **Table size: the cache sweep.** From 1K elements (L1-resident) through L2, L3, to 100M+ (DRAM). Plot ns/lookup vs size on log-x — the cache-cliff staircase graph is the single best artifact this project produces. Annotate your CPU's actual cache sizes as vertical lines.
- **Key/value types:** `u64→u64`, short strings (≤15 chars), long strings (64+). Expected finding worth writing up: with string keys, hashing+comparison dominate and table layout differences compress — knowing *when the table doesn't matter* is itself the insight.

**Baselines on every graph:** `std::unordered_map` (the explained strawman — node-based, pointer-chasing, forced by the standard's pointer-stability and bucket-API guarantees), one of Boost/Abseil, `ankerl::unordered_dense`. You are drawing context lines, not racing. If a baseline beats you — it will, sometimes — the writeup says *why* (e.g., Boost's SIMD-free metadata trick, ankerl's dense storage helping iteration): turning a loss into analysis is the senior move.

**Profiling beyond timing:** `perf stat` per workload — L1d misses/op, LLC misses/op, branch misses/op, instructions/op. Quote these in the writeup; "our miss path executes ~N instructions and ~1 cache miss per lookup at 87% load" is the sentence that proves you profiled rather than timed.

## Phase 7: The experiments (1.5–2 days — the showcase)

Each experiment = a question, a one-line method, a graph, a 3–5 sentence conclusion, collected in `EXPERIMENTS.md`. Do them in this order; stop when you run out of time — the first four are the core.

1. **SIMD vs scalar probe** (same layout, policy toggle). Expect: modest at low load, growing with load factor and on the miss path. Honest nuance to hunt for: at very low load, scalar sometimes ties — say so.
2. **Load factor sweep** (0.5 → 0.875 in steps): lookup cost + memory per element + probe-length histograms on one figure. This is the classic space/time frontier, measured by you.
3. **Hash quality:** identity hash vs fmix64 vs wyhash on sequential integer keys (0..N — the realistic adversary: order IDs *are* sequential). Expect identity + power-of-two-mask to produce catastrophic clustering; the probe-length histogram makes it visceral. Best graph titles write themselves ("what happens when you trust std::hash").
4. **H2 filter ablation:** patch H2 to a constant (filter always passes) and measure full-key-comparison count and lookup time. Directly quantifies what the 7 bits buy.
5. **Tombstones under churn:** workload 4 with and without the drain-rehash policy; show unbounded degradation vs stable. If you implement backward-shift erase, add it as a third line.
6. **Group size 8 vs 16** (template parameter): mostly a wash or slight win for 16 — a null result written up honestly is *still a good experiment* and signals scientific integrity.
7. (Stretch) **Prefetching** the slot group during control probe; **growth factor** ×2 vs ×1.5.

## Phase 8: Writeup & polish (1 day — protected, as always)

- **README:** pitch ("a Swiss-table-style flat hashmap built as a measurement lab"), design summary (from DESIGN.md), the cache-sweep graph up top, links to EXPERIMENTS.md, methodology, limitations (single-threaded, no allocator support, x86-64 SSE2 assumed), roadmap (NEON port, backward-shift, heterogeneous lookup if not done).
- **EXPERIMENTS.md** is the star: graphs + short conclusions, written like a lab notebook.
- Final pass: sanitizers, clang-tidy, differential test at 10M ops, tag `v0.1.0`.
- **CV bullet template** (fill in your numbers):
  > **flatmap-lab** — Swiss-table-style open-addressing hashmap in C++23 (SSE2 group probing, `construct_at`-managed raw storage, 7/8 load factor). Benchmarked and profiled against std/Boost/ankerl maps across cache-residency sweeps; ablation studies quantifying the SIMD probe, H2 filter, load factor, and hash-quality trade-offs (perf-counter level). Differential-tested vs `std::unordered_map`; sanitized CI.

## Stretch goals (priority order)

1. Heterogeneous lookup (`is_transparent`, find `string_view` in `string`-keyed map) — small, modern, high signal.
2. Backward-shift erase variant for Experiment 5.
3. libFuzzer on the operation-sequence differential test.
4. NEON port of the group probe (no movemask on ARM — `vshrn` narrowing trick; you already know this from the simdfix flash cards).

## Interview flash-cards

- Why is `std::unordered_map` slow, and why can't implementations fix it? (node-based ⇒ cache miss per element; the *standard* forces it via pointer/reference stability on insert and the bucket API)
- Walk through a lookup: H1 → group → `cmpeq`/movemask on H2 → candidate slots → full compare → empty-in-group ⇒ miss. Cost model at load factor α.
- Why 7 bits of H2? (1 control byte/slot overhead; 1/128 false-positive rate per slot ⇒ ~one wasted key compare per 128 candidates)
- Why power-of-two + mask requires a good mixer; what identity hashing does to clustering (you have the graph).
- Triangular probing: why it visits every group exactly once at power-of-two sizes.
- Tombstones vs backward-shift: churn degradation + drain-rehash vs more expensive erase; when each wins.
- Raw-storage lifetime rules: why no default-constructed slot array, `construct_at`/`destroy_at`, alignment, what `std::launder` is for.
- Iterator invalidation contract, and why flat maps can't offer `unordered_map`'s stability.
- Why max load factor 7/8 is viable here but would be suicide for plain linear probing without the H2 filter.
- Memory per element vs `unordered_map` (1 control byte + slot vs node + next-pointer + allocator overhead + bucket array).
- What the cache sweep shows and where your CPU's cliffs are — from *your* graph, with numbers.
- When the table design stops mattering (long string keys ⇒ hash/compare dominate) — from your experiment.
