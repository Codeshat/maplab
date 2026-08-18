// The cache sweep: ns per lookup as a function of table size, from L1-resident to DRAM.
//
// This is the single most informative graph the project produces, because it separates
// the two things a hash table costs -- instructions, and cache misses -- by making one of
// them vary over three orders of magnitude while the other stays fixed. Below L2 every
// implementation looks similar and the winner is whoever executes fewer instructions;
// above L3, everything is memory latency and the winner is whoever touches fewer lines.
#include <cstdint>
#include <vector>

#include "bench_common.hpp"

namespace maplab_bench {
namespace {

template<class Tag, bool Hit>
void bm_sweep(benchmark::State& state) {
  const auto n = static_cast<std::size_t>(state.range(0));
  const auto keys = maplab_workloads::make_keys(n, maplab_workloads::key_pattern::random);
  const auto absent =
      maplab_workloads::make_absent_keys(n, maplab_workloads::key_pattern::random, n);

  typename Tag::template map<std::uint64_t, std::uint64_t> m;
  m.reserve(n);
  for (const auto k : keys) m.try_emplace(k, k);

  // Shuffled once, then walked sequentially: the table access stays uniformly random,
  // the key-array access does not add cache misses of its own.
  const auto probe = maplab_workloads::shuffled(Hit ? keys : absent);

  std::size_t i = 0;
  for (auto _ : state) {
    if constexpr (Hit) {
      sink(value_of(m.find(probe[i]), m));
    } else {
      sink(m.find(probe[i]) == m.end());
    }
    if (++i == n) i = 0;
  }
  state.SetItemsProcessed(state.iterations());
}

}  // namespace

void register_cache_sweep() {
  // Roughly 2x per step from 1K to 8M elements: 8M x 17 B is ~140 MB for maplab and about
  // four times that for a node-based map, which is where this machine's 8 GB runs out.
  // 8M elements is still ~23x this CPU's 6 MB L3, so the DRAM plateau is fully visible.
  static const std::vector<std::int64_t> sizes = {1 << 10,
                                                  1 << 12,
                                                  1 << 13,
                                                  1 << 14,
                                                  1 << 15,
                                                  1 << 16,
                                                  1 << 17,
                                                  1 << 18,
                                                  1 << 19,
                                                  1 << 20,
                                                  1 << 21,
                                                  1 << 22,
                                                  1 << 23};

  for_each_impl([](auto tag) {
    using tag_t = decltype(tag);
    auto* hit =
        benchmark::RegisterBenchmark(bench_name("sweep_hit", tag_t::name), bm_sweep<tag_t, true>);
    auto* miss =
        benchmark::RegisterBenchmark(bench_name("sweep_miss", tag_t::name), bm_sweep<tag_t, false>);
    for (const auto s : sizes) {
      hit->Arg(s);
      miss->Arg(s);
    }
    hit->UseRealTime();
    miss->UseRealTime();
  });
}

}  // namespace maplab_bench
