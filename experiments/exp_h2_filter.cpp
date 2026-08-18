// Experiment 4: what do the 7 fingerprint bits buy?
//
// Question:  A control byte could just say "occupied". It instead spends 7 of its 8 bits
//            on a hash fingerprint. How much work does that save?
// Method:    Three configurations that differ only in what goes into the control byte:
//              * default          -- H2 = hash & 0x7F, H1 = hash >> 7 (disjoint bits)
//              * no_h2            -- every occupied byte stores 0, so the group match
//                                    returns every occupied slot as a candidate
//              * overlapping_hash -- H2 is still stored, but the group is chosen from the
//                                    whole hash instead of from H1, so slots within a
//                                    group share most of their fingerprint bits
//            Layout, probe order and load factor are untouched in all three.
// Expect:    no_h2 turns one key comparison per lookup into roughly alpha*16 of them, and
//            each comparison is a load from the slot array -- a different cache line from
//            the control array. The overlapping variant is the more interesting one: it
//            *looks* like it has a filter, and measuring the false-positive rate is the
//            only way to discover that it does not.
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "exp_common.hpp"
#include "maplab/flat_map.hpp"
#include "maplab_workloads/workloads.hpp"

namespace {

using namespace maplab_exp;

template<class Config>
using table =
    maplab::flat_map<std::uint64_t, std::uint64_t, maplab::default_hash, std::equal_to<>, Config>;

struct counters {
  double keycmp_hit;
  double keycmp_miss;
  double fp_rate;
  double groups_hit;
};

template<class Stats>
counters count(const std::vector<std::uint64_t>& keys,
               const std::vector<std::uint64_t>& hit_probe,
               const std::vector<std::uint64_t>& miss_probe) {
  table<Stats> c;
  c.reserve(keys.size());
  for (const auto k : keys) c.try_emplace(k, k);

  c.reset_stats();
  for (const auto k : hit_probe) sink(c.find(k) != c.end());
  const auto hit = c.stats();
  c.reset_stats();
  for (const auto k : miss_probe) sink(c.find(k) != c.end());
  const auto miss = c.stats();
  return {hit.mean_key_compares_per_lookup(),
          miss.mean_key_compares_per_lookup(),
          hit.h2_false_positive_rate(),
          hit.mean_groups_per_lookup()};
}

// All three variants are built, then their six measurements are interleaved, so a
// frequency change partway through cannot advantage whichever ran first.
void run_size(std::size_t n, csv& out, const console_table& console) {
  const auto keys = maplab_workloads::make_keys(n, maplab_workloads::key_pattern::random);
  const auto absent =
      maplab_workloads::make_absent_keys(n, maplab_workloads::key_pattern::random, n);
  const auto hit_probe = maplab_workloads::shuffled(keys);
  const auto miss_probe = maplab_workloads::shuffled(absent);

  table<maplab::default_config> with_h2;
  table<maplab::no_h2_config> without_h2;
  table<maplab::overlapping_hash_config> overlapping;
  with_h2.reserve(n);
  without_h2.reserve(n);
  overlapping.reserve(n);
  for (const auto k : keys) {
    with_h2.try_emplace(k, k);
    without_h2.try_emplace(k, k);
    overlapping.try_emplace(k, k);
  }

  const auto t = time_interleaved(n,
                                  {[&] {
                                     for (const auto k : hit_probe)
                                       sink(with_h2.find(k) != with_h2.end());
                                   },
                                   [&] {
                                     for (const auto k : hit_probe)
                                       sink(without_h2.find(k) != without_h2.end());
                                   },
                                   [&] {
                                     for (const auto k : hit_probe)
                                       sink(overlapping.find(k) != overlapping.end());
                                   },
                                   [&] {
                                     for (const auto k : miss_probe)
                                       sink(with_h2.find(k) != with_h2.end());
                                   },
                                   [&] {
                                     for (const auto k : miss_probe)
                                       sink(without_h2.find(k) != without_h2.end());
                                   },
                                   [&] {
                                     for (const auto k : miss_probe)
                                       sink(overlapping.find(k) != overlapping.end());
                                   }});

  const std::array<counters, 3> stats{
      count<maplab::stats_config>(keys, hit_probe, miss_probe),
      count<maplab::no_h2_stats_config>(keys, hit_probe, miss_probe),
      count<maplab::overlapping_hash_stats_config>(keys, hit_probe, miss_probe)};
  const std::array<const char*, 3> labels{
      "H2 (7 bits)", "no H2 (occupied only)", "H2 from H1's bits"};

  for (std::size_t i = 0; i < 3; ++i) {
    const auto& c = stats[i];
    console.row({labels[i],
                 fmt(t[i].best),
                 fmt(t[i + 3].best),
                 fmt(c.keycmp_hit, 3),
                 fmt(c.keycmp_miss, 3),
                 fmt(c.fp_rate * 100.0, 2) + "%",
                 fmt(c.groups_hit, 3)});
    out.row(labels[i],
            n,
            fmt(t[i].best, 4),
            fmt(t[i + 3].best, 4),
            fmt(c.keycmp_hit, 4),
            fmt(c.keycmp_miss, 4),
            fmt(c.fp_rate, 6),
            fmt(c.groups_hit, 4));
  }
}

}  // namespace

int main() {
  heading("Experiment 4: the H2 fingerprint ablation");
  std::cout << "Three tables with the same layout, the same probe order and the same\n"
               "load factor, differing only in what the control byte carries.\n\n";

  csv out(results_dir() + "/exp4_h2_filter.csv",
          {"variant",
           "elements",
           "hit_ns",
           "miss_ns",
           "keycmp_per_hit",
           "keycmp_per_miss",
           "h2_false_positive_rate",
           "groups_per_hit"});

  const std::vector<std::pair<std::string, int>> columns{{"variant", 22},
                                                         {"hit ns", 8},
                                                         {"miss ns", 8},
                                                         {"cmp/hit", 8},
                                                         {"cmp/miss", 9},
                                                         {"H2 FP rate", 11},
                                                         {"grp/hit", 8}};

  for (const std::size_t n : {std::size_t{1} << 16, std::size_t{1} << 22}) {
    std::cout << "\n-- " << n << " elements\n";
    const console_table console(columns);
    run_size(n, out, console);
  }

  std::cout << "\nThe theoretical false-positive rate of a 7-bit fingerprint is 1/128 =\n"
               "0.78% per occupied slot examined. The first row should land near it; the\n"
               "third should not, despite storing exactly the same 7 bits.\n";
  report_timing_noise();
  return 0;
}
