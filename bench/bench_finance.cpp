// A domain-shaped workload: an order table keyed by 64-bit order ID.
//
// Two properties make this different from the synthetic churn benchmark, and both are
// properties of the real thing rather than of the benchmark:
//   * order IDs are *sequential*, not random -- which is precisely the input that
//     punishes a table that trusts std::hash and masks the low bits (Experiment 3);
//   * orders are erased when they fill, so the table's size is stable while its contents
//     turn over completely -- which is the input that punishes tombstones (Experiment 5).
#include <cstdint>
#include <vector>

#include "bench_common.hpp"

namespace maplab_bench {
namespace {

struct order {
  std::uint64_t price;
  std::uint32_t quantity;
  std::uint32_t flags;
};

template<class Tag>
void bm_order_flow(benchmark::State& state) {
  const auto working_set = static_cast<std::size_t>(state.range(0));
  const auto events = maplab_workloads::make_order_flow(working_set, working_set * 8);

  for (auto _ : state) {
    typename Tag::template map<std::uint64_t, order> m;
    m.reserve(working_set * 2);
    for (const auto& e : events) {
      if (e.insert) {
        m.try_emplace(e.id, order{e.id * 100, 50, 0});
      } else {
        m.erase(e.id);
      }
    }
    sink(m.size());
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(events.size()));
}

// The lookup half: "do I have this order?" against a live book. Sequential IDs, random
// probe order.
template<class Tag>
void bm_order_lookup(benchmark::State& state) {
  const auto n = static_cast<std::size_t>(state.range(0));
  const auto keys = maplab_workloads::make_keys(n, maplab_workloads::key_pattern::sequential, 1);
  const auto probe = maplab_workloads::shuffled(keys);

  typename Tag::template map<std::uint64_t, order> m;
  m.reserve(n);
  for (const auto k : keys) m.try_emplace(k, order{k * 100, 50, 0});

  std::size_t i = 0;
  for (auto _ : state) {
    sink(value_of(m.find(probe[i]), m).price);
    if (++i == n) i = 0;
  }
  state.SetItemsProcessed(state.iterations());
}

}  // namespace

void register_finance() {
  for_each_reference_impl([](auto tag) {
    using tag_t = decltype(tag);
    benchmark::RegisterBenchmark(bench_name("order_flow", tag_t::name), bm_order_flow<tag_t>)
        ->Arg(1 << 12)
        ->Arg(1 << 16)
        ->UseRealTime();
    benchmark::RegisterBenchmark(bench_name("order_lookup", tag_t::name), bm_order_lookup<tag_t>)
        ->Arg(1 << 12)
        ->Arg(1 << 16)
        ->Arg(1 << 20)
        ->UseRealTime();
  });
}

}  // namespace maplab_bench
