// String keys. The expected finding is worth stating in advance so the result is a test
// of it rather than a story told afterwards: once the key is long enough that hashing and
// memcmp dominate, table layout stops mattering and every implementation converges.
// Knowing *when the data structure stops being the bottleneck* is the useful part.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "bench_common.hpp"

namespace maplab_bench {
namespace {

enum class string_kind { short_sso, long_heap, symbol };

const char* name_of(string_kind k) {
  switch (k) {
    case string_kind::short_sso:
      return "short15";
    case string_kind::long_heap:
      return "long64";
    case string_kind::symbol:
      return "symbols2k";
  }
  return "?";
}

std::vector<std::string> keys_for(string_kind kind, std::size_t n) {
  switch (kind) {
    case string_kind::short_sso:
      return maplab_workloads::make_short_strings(n);
    case string_kind::long_heap:
      return maplab_workloads::make_long_strings(n, 64);
    case string_kind::symbol:
      return maplab_workloads::make_symbols(n);
  }
  return {};
}

template<class Tag>
void bm_string_find_hit(benchmark::State& state, string_kind kind) {
  const auto n = static_cast<std::size_t>(state.range(0));
  const auto keys = keys_for(kind, n);
  const auto hit_probe = maplab_workloads::shuffled(keys);

  typename Tag::template map<std::string, std::uint64_t> m;
  m.reserve(keys.size());
  for (std::size_t i = 0; i < keys.size(); ++i) m.try_emplace(keys[i], i);

  std::size_t i = 0;
  for (auto _ : state) {
    sink(value_of(m.find(hit_probe[i]), m));
    if (++i == hit_probe.size()) i = 0;
  }
  state.SetItemsProcessed(state.iterations());
}

template<class Tag>
void bm_string_insert(benchmark::State& state, string_kind kind) {
  const auto n = static_cast<std::size_t>(state.range(0));
  const auto keys = keys_for(kind, n);
  for (auto _ : state) {
    typename Tag::template map<std::string, std::uint64_t> m;
    m.reserve(keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i) m.try_emplace(keys[i], i);
    sink(m.size());
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(keys.size()));
}

// Heterogeneous lookup, maplab only: find a std::string key from a std::string_view with
// no temporary. There is no baseline line here because the comparison that matters is
// against the same table doing the same lookup *with* the conversion.
void bm_symbol_lookup_transparent(benchmark::State& state) {
  const auto keys = maplab_workloads::make_symbols(2000);
  maplab::flat_map<std::string, std::uint64_t> m;
  for (std::size_t i = 0; i < keys.size(); ++i) m.try_emplace(keys[i], i);

  const auto hit_probe = maplab_workloads::shuffled(keys);
  std::vector<std::string_view> views;
  views.reserve(hit_probe.size());
  for (const auto& k : hit_probe) views.emplace_back(k);

  std::size_t i = 0;
  for (auto _ : state) {
    sink(value_of(m.find(views[i]), m));
    if (++i == views.size()) i = 0;
  }
  state.SetItemsProcessed(state.iterations());
}

void bm_symbol_lookup_converting(benchmark::State& state) {
  const auto keys = maplab_workloads::make_symbols(2000);
  // A non-transparent comparator forces the string_view to become a std::string first,
  // which is exactly the allocation heterogeneous lookup exists to avoid.
  maplab::flat_map<std::string, std::uint64_t, maplab::default_hash, std::equal_to<std::string>> m;
  for (std::size_t i = 0; i < keys.size(); ++i) m.try_emplace(keys[i], i);

  const auto hit_probe = maplab_workloads::shuffled(keys);
  std::vector<std::string_view> views;
  views.reserve(hit_probe.size());
  for (const auto& k : hit_probe) views.emplace_back(k);

  std::size_t i = 0;
  for (auto _ : state) {
    sink(value_of(m.find(std::string{views[i]}), m));
    if (++i == hit_probe.size()) i = 0;
  }
  state.SetItemsProcessed(state.iterations());
}

}  // namespace

void register_strings() {
  for (const auto kind : {string_kind::short_sso, string_kind::long_heap}) {
    for_each_reference_impl([kind](auto tag) {
      using tag_t = decltype(tag);
      const std::string suffix = std::string("/") + name_of(kind);
      benchmark::RegisterBenchmark(
          bench_name("string_find_hit", tag_t::name) + suffix,
          [kind](benchmark::State& s) { bm_string_find_hit<tag_t>(s, kind); })
          ->Arg(1 << 12)
          ->Arg(1 << 18)
          ->UseRealTime();
      benchmark::RegisterBenchmark(
          bench_name("string_insert", tag_t::name) + suffix,
          [kind](benchmark::State& s) { bm_string_insert<tag_t>(s, kind); })
          ->Arg(1 << 12)
          ->Arg(1 << 18)
          ->UseRealTime();
    });
  }

  for_each_reference_impl([](auto tag) {
    using tag_t = decltype(tag);
    benchmark::RegisterBenchmark(
        bench_name("string_find_hit", tag_t::name) + "/symbols2k",
        [](benchmark::State& s) { bm_string_find_hit<tag_t>(s, string_kind::symbol); })
        ->Arg(2000)
        ->UseRealTime();
  });

  benchmark::RegisterBenchmark("symbol_lookup/maplab/transparent", bm_symbol_lookup_transparent)
      ->UseRealTime();
  benchmark::RegisterBenchmark("symbol_lookup/maplab/converting", bm_symbol_lookup_converting)
      ->UseRealTime();
}

}  // namespace maplab_bench
