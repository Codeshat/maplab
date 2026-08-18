# maplab — design

This document was written to be argued with. Every decision below is one that could have
gone the other way, and most of them are measured in [EXPERIMENTS.md](EXPERIMENTS.md)
rather than asserted here.

---

## 1. Goal, and what follows from it

maplab is a Swiss-table-style open-addressing hash map built as a **measurement
laboratory**. The goal is not to be the fastest map; it is to make every major design
decision toggleable at compile time and then measure what each one is worth.

Two consequences run through the whole design:

1. **Design decisions are template parameters, not code paths.** A scalar probe loop, a
   defeated H2 filter and a 1/2 load factor are configurations of one implementation, not
   three implementations. If they were separate code, an ablation would be comparing two
   programs rather than isolating one variable.
2. **The table can count what it does.** A `probe_stats` block behind `if constexpr`
   records groups probed, key comparisons and fingerprint false positives. Timing alone
   tells you which design is faster; the counters tell you why, and they caught two real
   bugs during development that timing did not.

### Non-goals

- **Concurrency.** Single-threaded. A concurrent flat map is a different and much harder
  project, and pretending otherwise would be the least honest thing in this repo.
- **Allocator support.** One `operator new` with an alignment, no `Allocator` template
  parameter, no `pmr`.
- **Node handles / pointer stability.** Structurally impossible here; see §11.
- **Beating Boost or Abseil.** Their numbers appear on the graphs as context lines. Where
  they win, [EXPERIMENTS.md](EXPERIMENTS.md) says so and says why.

---

## 2. Layout

One allocation holds everything:

```
      ctrl_                              slots_
        |                                  |
        v                                  v
        +----------------+---+--------+----+-------------------------------+
        | control bytes  | S | cloned |pad | slots: pair<Key,T> * capacity  |
        +----------------+---+--------+----+-------------------------------+
         <-- capacity --> ^   <- w-1 ->
                          |
                          ctrl[capacity] = sentinel
```

- `capacity` is always `2^k - 1`, and the probe mask **is** the capacity, so offsets range
  over `[0, 2^k)`.
- Offset `capacity` lands on the sentinel byte, which is neither empty nor a fingerprint
  match. It is therefore a position no key can occupy: it costs one slot and buys a
  branch-free termination for iteration.
- The `cloned` tail repeats `ctrl[0 .. w-2]`. A group load starting anywhere in
  `[0, capacity]` can then read `w` bytes with no wraparound branch, and a bit set at a
  cloned position maps back to the real slot under the same mask every other offset uses.
  Every control-byte write goes through `set_ctrl`, which writes the clone too.

**Why one allocation rather than two.** The control array and the slot array are always
used together and always freed together. One allocation halves the allocator traffic on
growth, guarantees they cannot end up in wildly different pages, and makes
`memory_usage()` an exact number rather than an estimate — which matters, because
Experiment 7 quotes it.

---

## 3. The control byte

One byte per slot, in an array parallel to the slots:

| value | meaning | sign bit |
|---|---|---|
| `0x80` (-128) | empty | set |
| `0xFE` (-2) | deleted (tombstone) | set |
| `0xFF` (-1) | sentinel (one, at `ctrl[capacity]`) | set |
| `0x00`–`0x7F` | occupied; the value **is** H2 | clear |

The encoding is chosen so every predicate the probe loop needs is one signed byte
comparison:

- `is_full(c)` ⟺ `c >= 0` — a bare `movemask`, no comparison at all.
- `is_empty_or_deleted(c)` ⟺ `c < sentinel` — one `_mm_cmpgt_epi8`.
- fingerprint match ⟺ `c == h2` — one `_mm_cmpeq_epi8`.

Note that `is_empty_or_deleted` is *not* `c == empty || c == deleted`; it is a range test
that happens to be equivalent given the alphabet above. The 125 unassigned byte values
(-127..-3) are unreachable by construction, and `tests/test_control.cpp` pins that
invariant down, because it is exactly the sort of assumption that quietly stops being true.

**Why 7 bits and not 8?** One byte per slot is the overhead budget; the sign bit has to
distinguish occupied from free. Seven bits give a 1/128 ≈ 0.78% false-positive rate per
occupied slot examined — measured at 0.37%–0.39% in Experiment 4, which is what the
theory predicts once you account for probes that stop at the first match.

---

## 4. Splitting the hash

```
H1 = hash >> 7   chooses the group
H2 = hash & 0x7F is the fingerprint stored in the control byte
```

The two must be **disjoint bit ranges**. This looks like a detail and is not. If the group
were chosen from the whole hash, then the 16 slots reachable in one group would differ
only in their low 4 bits — which are also fingerprint bits — so every candidate in a group
would share most of its fingerprint and the filter would reject almost nothing.

This project shipped that bug for about an hour. The stats block caught it: the measured
H2 false-positive rate was 26% against a theoretical 0.78%. It is now a configuration
(`split_hash = false`) and a measured result: **20% false-positive rate versus 0.4%, a 50x
degradation, from storing exactly the same seven bits**.

---

## 5. Group probing

```cpp
__m128i ctrl  = _mm_loadu_si128(...);                  // 16 control bytes
__m128i match = _mm_cmpeq_epi8(ctrl, _mm_set1_epi8(h2));
uint32_t candidates = _mm_movemask_epi8(match);        // one bit per matching slot
// iterate with countr_zero / blsr
uint32_t empties = _mm_movemask_epi8(_mm_cmpeq_epi8(ctrl, _mm_set1_epi8(kEmpty)));
// any empty in the group => the key is absent => stop
```

**SSE2, deliberately, with no runtime dispatch.** SSE2 is part of the x86-64 baseline, so
there is no CPU to dispatch for and no scalar fallback to maintain. A wider AVX2 group
would double the control bytes per probe but also double the slots a group spans, which is
a *second* cache line for the slot array — the trade is not obviously favourable, and
Experiment 6 (8 vs 16) suggests the win comes from the layout rather than the register
width.

**The miss fast path is the crown jewel.** "This group contains an empty byte, therefore
the key is not in the table" is a single instruction, and it terminates the great majority
of unsuccessful lookups after one group load with zero slot accesses. Experiment 4
measures the miss path at **8.4 ns with the filter and 89.9 ns without** — a 10.7x
difference, far larger than the effect on hits.

The scalar group implements the identical interface over the identical memory, one byte at
a time. It is not dead code: it is the control group for Experiment 1, and it is in the
differential test matrix, because an ablation baseline that is allowed to be wrong is not
a baseline.

---

## 6. Probe sequence: triangular numbers

Between groups, the stride grows by the group width each step:

```
offset_k = (offset_0 + w * k(k+1)/2) mod 2^m      where capacity = 2^m - 1
```

**Why it visits every group exactly once.** Every offset is congruent to `offset_0` modulo
`w`, so the sequence walks `2^m / w` group starts indexed by the triangular numbers
`T_k mod (2^m / w)`. Triangular numbers modulo a power of two form a complete residue
system, so the first `2^m / w` probes hit distinct groups and together cover all `2^m`
positions. Probing therefore always terminates, and always terminates having examined
every slot.

A detail worth knowing: the sequence's full period is `2 * (2^m / w)`, not `2^m / w` — it
lands half the table away after one pass, because `w * T_g = 2^m * (g+1) / 2` with `g+1`
odd. `tests/test_control.cpp` checks both facts. The probe loop never gets that far
anyway: the load factor guarantees an empty byte exists.

Plain quadratic probing offers no such coverage guarantee, which is why it needs a lower
load factor to stay safe.

---

## 7. Capacity, load factor, growth

- **Capacity is `2^k - 1`**, indexed by masking. This is the single decision the rest of
  the design bends around.
- **Consequence: the growth factor is forced to 2.** A 1.5x factor needs a
  non-power-of-two capacity and therefore a modulo (or a Lemire reduction) on *every
  probe*, on the hottest path there is. That trade is not worth 25% peak memory, so
  "growth factor 1.5 vs 2" is not an experiment in this repo — it is a decision made
  upstream by the choice of masking, and this is the honest place to say so.
- **Consequence: the hash must be mixed.** Masking only ever looks at low bits.
  `std::hash<int>` is the identity on libstdc++ and libc++, so a table that trusts the
  key's own hash and masks is one structured input away from collapse. maplab therefore
  owns its mixer, and ships `identity_hash` so the collapse can be measured (Experiment 3)
  rather than merely warned about.
- **Max load factor 7/8**, as an exact rational in the config. This is only viable because
  the H2 filter makes long probe chains cheap to walk: at α = 0.875 a lookup still averages
  1.09 group probes and 1.02 key comparisons. Plain linear probing at 7/8 would be
  catastrophic. Experiment 2 sweeps 1/2 → 31/32 and shows where the knee is.

`size + tombstones + growth_left == ceiling` is an exact accounting identity that holds
after every operation; the differential test asserts it every 997 operations, in every
configuration.

---

## 8. Deletion

An occupied slot cannot simply be emptied: an empty byte terminates probe chains, and any
chain running *through* this slot would be truncated, losing elements that are still in
the table.

**The rule.** If the run of occupied slots containing this one is shorter than a group,
then every probe that could have reached this slot already saw an empty byte in the same
group load and stopped there. Nothing depends on this slot staying non-empty, so it can go
back to empty and return its growth budget. Otherwise it becomes a tombstone.

**Tombstones consume the growth budget.** That is the whole problem: a table churning at
constant size fills with tombstones, exhausts `growth_left`, and *grows* even though its
element count never changed. Memory climbs, the working set leaves cache, and lookups get
slower forever.

**The drain policy.** When the growth budget is exhausted and tombstones account for at
least 1/8 of the ceiling, rehash at the *same* capacity instead of growing. Two notes:

- The threshold is expressed against the **ceiling**, not the capacity. Abseil's
  equivalent test (`size * 32 <= capacity * 25`) silently assumes a 7/8 ceiling; at a 1/2
  ceiling it is true unconditionally, so the table drains forever and never grows. maplab
  had exactly that bug, and the load-factor entries in the test matrix found it — an
  ablation config earning its keep as a *test*, not just as an experiment.
- maplab drains by rehashing into a fresh allocation of the same size. Abseil does it
  in place, avoiding the transient double footprint. That is a real advantage we do not
  have, and it is on the roadmap rather than pretended away.

Experiment 5 runs the churn workload with the policy on and off.

**Backward-shift deletion** (compact the chain instead of leaving a tombstone) is the other
classic answer. It makes erase more expensive and lookups cheaper, and it interacts badly
with the group-at-a-time probe. Not implemented; noted in the roadmap.

---

## 9. Memory management and lifetimes

Slots are **raw storage**. The map never default-constructs a slot array:

- allocate bytes with `::operator new(n, std::align_val_t{...})`,
- create elements with `std::construct_at`,
- destroy them with `std::destroy_at`,
- on growth: allocate new, move-construct the occupied slots over, destroy and deallocate
  the old block.

This is why `flat_map<int, std::mutex>` is as legal as the element type allows, and why an
empty table performs zero element constructions.

**The union, and the one deviation it embodies.** `value_type` is
`std::pair<const Key, T>`, as the standard containers define it. But rehashing must *move*
elements, and a `const Key` cannot be moved from — a `std::string` key would be deep-copied
on every growth. The slot is therefore a union:

```cpp
union slot_type {
  std::pair<const Key, T> value;          // what the user sees
  std::pair<Key, T>       mutable_value;  // how relocation moves the key
};
```

Relocation constructs `dst.value` from `std::move(src.mutable_value)`. This is the same
technique Abseil uses. It relies on the two pair types being layout-compatible and on the
common-initial-sequence rule, which is universal practice and not airtight standardese.
The alternative — making `value_type` be `pair<Key, T>` with a non-const key, as
`ankerl::unordered_dense` does — is arguably cleaner but changes the public type and lets
callers mutate a key out from under the table. **This is a deliberate, documented choice,
not an oversight.**

**`static_assert(is_nothrow_move_constructible_v<pair<Key, T>>)`.** Growth relocates every
element and there is nowhere to unwind to if a move throws halfway through. Rather than
implement a slow `move_if_noexcept` path nobody would exercise, maplab rejects such types
at compile time with a message that says so. A documented scope limitation.

---

## 10. Exception safety

Basic guarantee. Specifically:

- If an element's constructor throws during insertion, the control byte that
  `prepare_insert` already claimed is given back and the size restored, so the table
  remains a valid table with its previous contents. `tests/test_lifetime.cpp` exercises
  this with a type that throws on copy, and checks the table is still iterable, still the
  right size, and still usable afterwards.
- The `try`/`catch` is compiled out entirely when the construction is `noexcept`.
- Copy construction destroys anything it already built before rethrowing.
- Growth cannot throw (see the `nothrow_move` assert), so it is either complete or never
  started.

---

## 11. API surface and the iterator contract

`insert` / `emplace` / `try_emplace` / `insert_or_assign` / `find` / `contains` / `count` /
`at` / `operator[]` / `equal_range` / `erase(key)` / `erase(iterator)` / `reserve` /
`rehash` / `clear` / `swap` / iterators / `load_factor` / `capacity` / `memory_usage`.

- **Heterogeneous lookup** is enabled when *both* the hasher and the comparator are
  transparent — the standard's rule, and the safe one: a transparent hasher with a
  non-transparent comparator would hash a `string_view` and compare a `string`. The default
  comparator is `std::equal_to<>`, so `map<string, V>::find(string_view)` works out of the
  box with no temporary. With a non-transparent comparator there is simply no viable
  overload, which is better than a silent allocation per lookup.
- **`emplace` costs one extra move** relative to `try_emplace`, because with a general
  argument pack it has to materialise the value before it can see the key. `try_emplace` is
  documented as the zero-waste path rather than presented as a convenience wrapper.

**Iterator invalidation.**

| operation | effect |
|---|---|
| `insert`, `emplace`, `try_emplace`, `operator[]` | **all** iterators, pointers and references invalidated **if** a rehash occurs (`growth_left() == 0`); otherwise none |
| `erase(key)`, `erase(iterator)` | only iterators/pointers/references to the erased element |
| `reserve`, `rehash` | all, if the capacity changes |
| `clear` | all |

Note the asymmetry with `std::unordered_map`, which guarantees pointer and reference
stability across *every* operation. It can do that only because each element lives in its
own node — and that node, its `next` pointer, and its separate allocation are precisely
what makes it slow. **A flat map cannot offer that guarantee; the guarantee is the
performance.** The `bucket` API in the standard's interface locks this in even further,
which is why no conforming `std::unordered_map` can be implemented as a flat table.

---

## 12. Configuration architecture

Each design decision is a member of a config struct; configs compose by inheritance and
name hiding:

```cpp
struct scalar_config : maplab::default_config {
  using probe = maplab::probe_policy::scalar;
};
```

| knob | default | measured by |
|---|---|---|
| `group_width` | 16 | Experiment 6 |
| `probe` | `simd` | Experiment 1 |
| `max_load_num/den` | 7/8 | Experiment 2 |
| `split_hash` | true | Experiment 4b |
| `h2_filter` | true | Experiment 4 |
| `drain_tombstones` | true | Experiment 5 |
| `prefetch` | false | — |
| `stats` | false | — |

**Every configuration in this table is in the differential-test matrix.** An ablation
variant that is only exercised by a benchmark is a variant nobody has checked for
correctness, and its numbers mean nothing.

`sizeof(flat_map)` is five words with stats off, and `tests/test_zero_cost.cpp`
static-asserts that every ablation config has the same layout — so an ablation is
measuring a code change, not a different data structure.

---

## 13. Preprocessor surface

maplab defines exactly one macro, `MAPLAB_ASSUME`, used to tell the compiler that a
matching fingerprint implies the table has storage. GCC at `-O3` cannot see this through
the shared empty control group and warns about a null dereference that cannot happen.

---

## 14. Known limitations

1. Single-threaded.
2. x86-64 / SSE2 only. A NEON port needs a different way to get a movemask (the `vshrn`
   narrowing trick); the layout and the algorithm are unchanged, which Experiment 6
   supports by showing the win is in the layout rather than the register width.
3. No allocator support, no `pmr`.
4. Drain rehashes into a fresh allocation rather than in place, so a drain transiently
   doubles the footprint.
5. `value_type` uses the union/common-initial-sequence technique described in §9.
6. Types with throwing move constructors are rejected at compile time.
7. Erase does not shrink the table. `rehash(size())` does, explicitly.
