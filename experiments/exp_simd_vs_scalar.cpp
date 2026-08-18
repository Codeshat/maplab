// Experiment 1: what does the SIMD group probe actually buy?
//
// Question:  On an identical layout, identical probe order and identical load factor, how
//            much does replacing the 16-byte SSE2 control scan with a byte-at-a-time
//            scalar scan cost?
// Method:    One template parameter. maplab::default_config and maplab::scalar_config
//            differ in exactly one member, so both tables allocate the same bytes, place
//            the same keys in the same slots, and probe the same groups in the same order.
//            The stats build confirms the group counts are identical, which is what makes
//            this an ablation rather than a comparison of two data structures.
// Expect:    A modest win at small sizes where everything is L1-resident and the scalar
//            loop's extra instructions are exposed; a *shrinking* relative win as the
//            table leaves cache and memory latency dominates both. The miss path should
//            favour SIMD more than the hit path, because a miss's whole job is to test 16
//            bytes for "any empty" -- one instruction in SIMD, a loop in scalar.
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "exp_common.hpp"
#include "maplab/flat_map.hpp"
#include "maplab_workloads/workloads.hpp"

namespace {

using namespace maplab_exp;

struct variant_result {
  double hit_ns;
  double miss_ns;
  double groups_per_hit;
  double groups_per_miss;
};

template<class Config>
using table =
    maplab::flat_map<std::uint64_t, std::uint64_t, maplab::default_hash, std::equal_to<>, Config>;

// Counters for one configuration, from a stats-enabled table filled identically to the
// timed one. Never timed: see the methodology note in exp_common.hpp.
template<class Stats>
std::pair<double, double> count_groups(const std::vector<std::uint64_t>& keys,
                                       const std::vector<std::uint64_t>& hit_probe,
                                       const std::vector<std::uint64_t>& miss_probe) {
  table<Stats> c;
  c.reserve(keys.size());
  for (const auto k : keys) c.try_emplace(k, k);

  c.reset_stats();
  for (const auto k : hit_probe) sink(c.find(k) != c.end());
  const double per_hit = c.stats().mean_groups_per_lookup();
  c.reset_stats();
  for (const auto k : miss_probe) sink(c.find(k) != c.end());
  const double per_miss = c.stats().mean_groups_per_lookup();
  return {per_hit, per_miss};
}

// Both probe policies are built, then all four measurements are interleaved, so a
// frequency change partway through cannot advantage one of them.
std::pair<variant_result, variant_result> measure(std::size_t n) {
  const auto keys = maplab_workloads::make_keys(n, maplab_workloads::key_pattern::random);
  const auto absent =
      maplab_workloads::make_absent_keys(n, maplab_workloads::key_pattern::random, n);
  const auto hit_probe = maplab_workloads::shuffled(keys);
  const auto miss_probe = maplab_workloads::shuffled(absent);

  table<maplab::default_config> simd;
  table<maplab::scalar_config> scalar;
  simd.reserve(n);
  scalar.reserve(n);
  for (const auto k : keys) {
    simd.try_emplace(k, k);
    scalar.try_emplace(k, k);
  }

  const auto t = time_interleaved(n,
                                  {[&] {
                                     for (const auto k : hit_probe)
                                       sink(value_of(simd.find(k), simd));
                                   },
                                   [&] {
                                     for (const auto k : hit_probe)
                                       sink(value_of(scalar.find(k), scalar));
                                   },
                                   [&] {
                                     for (const auto k : miss_probe)
                                       sink(simd.find(k) == simd.end());
                                   },
                                   [&] {
                                     for (const auto k : miss_probe)
                                       sink(scalar.find(k) == scalar.end());
                                   }});

  const auto simd_groups = count_groups<maplab::stats_config>(keys, hit_probe, miss_probe);
  const auto scalar_groups = count_groups<maplab::scalar_stats_config>(keys, hit_probe, miss_probe);

  return {variant_result{t[0].best, t[2].best, simd_groups.first, simd_groups.second},
          variant_result{t[1].best, t[3].best, scalar_groups.first, scalar_groups.second}};
}

}  // namespace

int main() {
  heading("Experiment 1: SIMD group probe vs scalar control scan");
  std::cout << "Same layout, same probe order, same load factor. The only difference is\n"
               "how 16 control bytes are turned into a candidate bitmask.\n\n";

  csv out(results_dir() + "/exp1_simd_vs_scalar.csv",
          {"size", "probe", "lookup", "ns_per_op", "groups_per_lookup", "speedup_vs_scalar"});

  const console_table table({{"elements", 10},
                             {"simd hit", 10},
                             {"scalar hit", 11},
                             {"hit x", 7},
                             {"simd miss", 10},
                             {"scalar miss", 12},
                             {"miss x", 7},
                             {"groups/hit", 11}});

  // Capped at 2^22 (~70 MB of table, plus four key arrays and a second table for the
  // counter pass) because the reference machine has 8 GB. bench_cache.cpp carries the size
  // axis further, with one implementation resident at a time.
  for (const std::size_t n : {std::size_t{1} << 10,
                              std::size_t{1} << 13,
                              std::size_t{1} << 16,
                              std::size_t{1} << 19,
                              std::size_t{1} << 22}) {
    const auto [simd, scalar] = measure(n);

    // The ablation is only valid if both tables really did probe the same groups.
    const bool same_shape = std::abs(simd.groups_per_hit - scalar.groups_per_hit) < 1e-9 &&
                            std::abs(simd.groups_per_miss - scalar.groups_per_miss) < 1e-9;

    table.row({std::to_string(n),
               fmt(simd.hit_ns),
               fmt(scalar.hit_ns),
               fmt(scalar.hit_ns / simd.hit_ns),
               fmt(simd.miss_ns),
               fmt(scalar.miss_ns),
               fmt(scalar.miss_ns / simd.miss_ns),
               fmt(simd.groups_per_hit, 3) + (same_shape ? "" : " MISMATCH")});

    out.row(n,
            "simd",
            "hit",
            fmt(simd.hit_ns, 4),
            fmt(simd.groups_per_hit, 4),
            fmt(scalar.hit_ns / simd.hit_ns, 4));
    out.row(n, "scalar", "hit", fmt(scalar.hit_ns, 4), fmt(scalar.groups_per_hit, 4), "1.0");
    out.row(n,
            "simd",
            "miss",
            fmt(simd.miss_ns, 4),
            fmt(simd.groups_per_miss, 4),
            fmt(scalar.miss_ns / simd.miss_ns, 4));
    out.row(n, "scalar", "miss", fmt(scalar.miss_ns, 4), fmt(scalar.groups_per_miss, 4), "1.0");
  }

  std::cout << "\nThe groups/lookup column is identical for both probes by construction:\n"
               "the ablation changes how a group is scanned, never which groups are\n"
               "scanned. Any difference in the time column is therefore the scan itself.\n";
  report_timing_noise();
  return 0;
}
