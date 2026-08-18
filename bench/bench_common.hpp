// Shared scaffolding for the benchmark suite.
//
// Conventions, applied everywhere and stated once:
//   * Every map implementation is a tag with a `map` alias, so a workload is written once
//     and instantiated for all of them. Selecting implementations with templates rather
//     than function pointers matters: at these input sizes an indirect call is a
//     measurable fraction of the thing being measured.
//   * Keys are generated from fixed seeds by maplab_workloads, so a graph can be
//     reproduced exactly.
//   * Lookups run in a random order. Looking keys up in insertion order lets the hardware
//     prefetcher hide the cache behaviour that is the entire point of the cache sweep.
//   * `sink()` wraps benchmark::DoNotOptimize because the const-ref overload is deprecated
//     in Benchmark 1.9; taking by value gives a mutable lvalue and selects the live one.
#ifndef MAPLAB_BENCH_COMMON_HPP
#define MAPLAB_BENCH_COMMON_HPP

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

#include <benchmark/benchmark.h>
#include <unordered_dense.h>

#include "maplab/config.hpp"
#include "maplab/flat_map.hpp"
#include "maplab_workloads/workloads.hpp"

#if MAPLAB_HAVE_ABSEIL
#include <absl/container/flat_hash_map.h>
#endif

namespace maplab_bench {

template<class T>
inline void sink(T v) {
  benchmark::DoNotOptimize(v);
}

// Every lookup in the hit benchmarks is a guaranteed hit, by construction of the key set.
// The compiler cannot know that, and at -O3 -Wnull-dereference objects to dereferencing
// an iterator it cannot prove is valid. Branching on it would add a perfectly-predicted
// but nonetheless real compare-and-branch to the measured loop -- for every
// implementation, so it would not bias the comparison, but it would inflate every number.
// Asserting the invariant instead keeps the loop honest.
template<class It, class Map>
inline decltype(auto) value_of(It it, const Map& m) {
  MAPLAB_ASSUME(it != m.end());
  return it->second;
}

// ---- implementation tags ---------------------------------------------------------------

struct maplab_tag {
  static constexpr const char* name = "maplab";
  template<class K, class V>
  using map = maplab::flat_map<K, V>;
};

// The Experiment 1 control group: same layout, same probe order, scalar control scan.
struct maplab_scalar_tag {
  static constexpr const char* name = "maplab-scalar";
  template<class K, class V>
  using map = maplab::flat_map<K, V, maplab::default_hash, std::equal_to<>, maplab::scalar_config>;
};

struct std_tag {
  static constexpr const char* name = "std::unordered_map";
  template<class K, class V>
  using map = std::unordered_map<K, V>;
};

struct ankerl_tag {
  static constexpr const char* name = "ankerl::unordered_dense";
  template<class K, class V>
  using map = ankerl::unordered_dense::map<K, V>;
};

#if MAPLAB_HAVE_ABSEIL
struct absl_tag {
  static constexpr const char* name = "absl::flat_hash_map";
  template<class K, class V>
  using map = absl::flat_hash_map<K, V>;
};
#endif

// ---- registration --------------------------------------------------------------------

// Benchmark names are "workload/impl/size[/variant]" so bench/plots/plot.py can split on
// '/' and group without a lookup table.
inline std::string bench_name(const char* workload, const char* impl) {
  return std::string(workload) + "/" + impl;
}

// Run `f` for every implementation tag. Registering programmatically (rather than with
// BENCHMARK_TEMPLATE) is what lets the names stay machine-readable.
template<class F>
void for_each_impl(F&& f) {
  f(maplab_tag{});
  f(maplab_scalar_tag{});
  f(std_tag{});
  f(ankerl_tag{});
#if MAPLAB_HAVE_ABSEIL
  f(absl_tag{});
#endif
}

// The baselines only; used where including maplab's own ablation would be noise.
template<class F>
void for_each_reference_impl(F&& f) {
  f(maplab_tag{});
  f(std_tag{});
  f(ankerl_tag{});
#if MAPLAB_HAVE_ABSEIL
  f(absl_tag{});
#endif
}

void register_ops();
void register_cache_sweep();
void register_strings();
void register_finance();

}  // namespace maplab_bench

#endif  // MAPLAB_BENCH_COMMON_HPP
