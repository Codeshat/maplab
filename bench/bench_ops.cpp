// The core u64 -> u64 workloads: insert, hit, miss, churn, mixed, iterate.
#include <cstdint>
#include <string>
#include <vector>

#include "bench_common.hpp"

namespace maplab_bench {
namespace {

using workloads = maplab_workloads::key_pattern;

// ---- 1. insert N unique keys, with and without reserve ---------------------------------
//
// The gap between the two *is* the cost of rehashing: same keys, same final table, the
// only difference is whether the table had to grow log2(N/16) times on the way there.
template<class Tag>
void bm_insert(benchmark::State& state) {
  const auto n = static_cast<std::size_t>(state.range(0));
  const bool presize = state.range(1) != 0;
  const auto keys = maplab_workloads::make_keys(n, workloads::random);

  for (auto _ : state) {
    typename Tag::template map<std::uint64_t, std::uint64_t> m;
    if (presize) m.reserve(n);
    for (const auto k : keys) m.try_emplace(k, k);
    sink(m.size());
    // Destruction is inside the timed region on purpose: freeing one block versus freeing
    // N nodes is a real difference between flat and node-based maps, and hiding it would
    // flatter us.
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}

// ---- 2. successful lookup --------------------------------------------------------------
template<class Tag>
void bm_find_hit(benchmark::State& state) {
  const auto n = static_cast<std::size_t>(state.range(0));
  const auto keys = maplab_workloads::make_keys(n, workloads::random);
  const auto probe = maplab_workloads::shuffled(keys);

  typename Tag::template map<std::uint64_t, std::uint64_t> m;
  m.reserve(n);
  for (const auto k : keys) m.try_emplace(k, k);

  std::size_t i = 0;
  for (auto _ : state) {
    sink(value_of(m.find(probe[i]), m));
    if (++i == n) i = 0;
  }
  state.SetItemsProcessed(state.iterations());
}

// ---- 3. unsuccessful lookup ------------------------------------------------------------
//
// The showcase for the miss fast path: an empty control byte anywhere in the first group
// ends the probe with zero key comparisons and zero slot loads. Many real workloads --
// caches, dedup filters, "have I seen this order id" -- are miss-dominated.
template<class Tag>
void bm_find_miss(benchmark::State& state) {
  const auto n = static_cast<std::size_t>(state.range(0));
  const auto keys = maplab_workloads::make_keys(n, workloads::random);
  const auto absent = maplab_workloads::make_absent_keys(n, workloads::random, n);
  const auto probe = maplab_workloads::shuffled(absent);

  typename Tag::template map<std::uint64_t, std::uint64_t> m;
  m.reserve(n);
  for (const auto k : keys) m.try_emplace(k, k);

  std::size_t i = 0;
  for (auto _ : state) {
    sink(m.find(probe[i]) == m.end());
    if (++i == n) i = 0;
  }
  state.SetItemsProcessed(state.iterations());
}

// ---- 4. erase-heavy churn at constant size ---------------------------------------------
//
// Insert one, erase one, forever, with the table size pinned. Nothing grows, so any
// slowdown over time is tombstone accumulation and nothing else.
template<class Tag>
void bm_churn(benchmark::State& state) {
  const auto n = static_cast<std::size_t>(state.range(0));
  typename Tag::template map<std::uint64_t, std::uint64_t> m;
  m.reserve(n);
  for (std::uint64_t k = 0; k < n; ++k) m.try_emplace(k, k);

  std::uint64_t next = n;
  std::uint64_t oldest = 0;
  for (auto _ : state) {
    m.try_emplace(next, next);
    m.erase(oldest);
    ++next;
    ++oldest;
    sink(m.size());
  }
  state.SetItemsProcessed(state.iterations());
}

// ---- 5. mixed realistic: 90% find / 9% insert / 1% erase --------------------------------
template<class Tag>
void bm_mixed(benchmark::State& state) {
  const auto n = static_cast<std::size_t>(state.range(0));
  const auto keys = maplab_workloads::make_keys(n, workloads::random);

  typename Tag::template map<std::uint64_t, std::uint64_t> m;
  m.reserve(n);
  for (const auto k : keys) m.try_emplace(k, k);

  maplab_workloads::splitmix64 rng(0x1234);
  for (auto _ : state) {
    const std::uint64_t r = rng.bounded(100);
    const std::uint64_t k = keys[rng.bounded(n)];
    if (r < 90) {
      const auto it = m.find(k);
      sink(it == m.end() ? 0 : it->second);
    } else if (r < 99) {
      sink(m.try_emplace(k ^ 0x9E3779B9ULL, k).second);
    } else {
      sink(m.erase(k));
    }
  }
  state.SetItemsProcessed(state.iterations());
}

// ---- 6. iteration ----------------------------------------------------------------------
//
// Where a node-based map pays for its pointer chase and a flat map does not -- but also
// where maplab pays for its 1/8 empty slots relative to a densely-stored map.
template<class Tag>
void bm_iterate(benchmark::State& state) {
  const auto n = static_cast<std::size_t>(state.range(0));
  const auto keys = maplab_workloads::make_keys(n, workloads::random);
  typename Tag::template map<std::uint64_t, std::uint64_t> m;
  m.reserve(n);
  for (const auto k : keys) m.try_emplace(k, k);

  for (auto _ : state) {
    std::uint64_t sum = 0;
    for (const auto& kv : m) sum += kv.second;
    sink(sum);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}

// Sizes chosen to straddle this machine's cache levels; the cache sweep in
// bench_cache.cpp covers the axis properly, these are the headline points.
constexpr std::int64_t small = 1 << 10;   // comfortably L1
constexpr std::int64_t medium = 1 << 16;  // L2/L3
constexpr std::int64_t large = 1 << 22;   // DRAM

template<class Tag, class Fn>
void reg(Fn fn, const char* workload, Tag /*tag*/) {
  auto* b = benchmark::RegisterBenchmark(bench_name(workload, Tag::name), fn);
  b->Arg(small)->Arg(medium)->Arg(large)->UseRealTime();
}

}  // namespace

void register_ops() {
  for_each_impl([](auto tag) {
    using tag_t = decltype(tag);
    benchmark::RegisterBenchmark(bench_name("insert_no_reserve", tag_t::name), bm_insert<tag_t>)
        ->Args({small, 0})
        ->Args({medium, 0})
        ->Args({large, 0})
        ->UseRealTime();
    benchmark::RegisterBenchmark(bench_name("insert_reserved", tag_t::name), bm_insert<tag_t>)
        ->Args({small, 1})
        ->Args({medium, 1})
        ->Args({large, 1})
        ->UseRealTime();
    reg(bm_find_hit<tag_t>, "find_hit", tag);
    reg(bm_find_miss<tag_t>, "find_miss", tag);
    reg(bm_churn<tag_t>, "churn", tag);
    reg(bm_mixed<tag_t>, "mixed_90_9_1", tag);
    reg(bm_iterate<tag_t>, "iterate", tag);
  });
}

}  // namespace maplab_bench
