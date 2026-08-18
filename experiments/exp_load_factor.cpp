// Experiment 2: the space/time frontier.
//
// Question:  What does raising the maximum load factor actually cost per lookup, and what
//            does it save in memory?
// Method:    Sweep Config::max_load_num/den from 1/2 to 31/32. Every configuration is
//            filled to exactly its own growth ceiling on an array of the *same* capacity,
//            so the only thing that varies between rows is how much of the array holds
//            live elements. All nine tables are built first and their eighteen lookup
//            measurements are then interleaved, because a run long enough to compare nine
//            configurations is also long enough for this machine to change clock. Counters
//            come from stats-enabled twins, never from the timed tables.
// Expect:    Lookup cost rises slowly and then sharply, because the probability that a
//            group contains no empty byte -- which is what forces a second group, and
//            usually a second cache line -- goes as alpha^16. Memory per element falls as
//            1/alpha. The interesting part is where the knee is, and whether 7/8 is on the
//            right side of it.
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "exp_common.hpp"
#include "maplab/flat_map.hpp"
#include "maplab_workloads/workloads.hpp"

namespace {

using namespace maplab_exp;

template<class Config>
using table =
    maplab::flat_map<std::uint64_t, std::uint64_t, maplab::default_hash, std::equal_to<>, Config>;

template<std::size_t Num, std::size_t Den>
struct load_case {
  static constexpr std::size_t num = Num;
  static constexpr std::size_t den = Den;
  using plain = maplab::load_factor_config<Num, Den>;
  using stats = maplab::load_factor_stats_config<Num, Den>;
};

// Fill until the table is exactly at its own growth ceiling at a chosen capacity.
//
// growth_left() == 0 is the precise statement of "one more insert would rehash", so it
// needs no tolerance and no floating point. Waiting for a specific capacity as well is
// what makes the sweep a controlled experiment: every configuration is measured on an
// array of the same size.
template<class Config>
std::size_t fill_to_ceiling(table<Config>& m,
                            const std::vector<std::uint64_t>& keys,
                            std::size_t target_capacity) {
  for (const auto k : keys) {
    m.try_emplace(k, k);
    if (m.capacity() >= target_capacity && m.growth_left() == 0) break;
  }
  if (m.capacity() != target_capacity) {
    throw std::runtime_error("not enough keys to fill capacity " + std::to_string(target_capacity) +
                             " (reached " + std::to_string(m.capacity()) + " with " +
                             std::to_string(m.size()) + " elements)");
  }
  return m.size();
}

struct row {
  std::string label;
  double load_factor;
  std::size_t elements;
  double bytes_per_elem;
  double hit_ns;
  double miss_ns;
  maplab::probe_stats hit_stats;
  maplab::probe_stats miss_stats;
};

template<class Case>
void collect_counters(const std::vector<std::uint64_t>& keys,
                      std::size_t target_capacity,
                      const std::vector<std::uint64_t>& hit_probe,
                      const std::vector<std::uint64_t>& miss_probe,
                      row& r) {
  table<typename Case::stats> c;
  fill_to_ceiling(c, keys, target_capacity);
  c.reset_stats();
  for (const auto k : hit_probe) sink(c.find(k) != c.end());
  r.hit_stats = c.stats();
  c.reset_stats();
  for (const auto k : miss_probe) sink(c.find(k) != c.end());
  r.miss_stats = c.stats();
}

template<class... Cases>
std::vector<row> run_all(std::size_t target_capacity, std::size_t pool) {
  constexpr std::size_t n_cases = sizeof...(Cases);
  const auto keys = maplab_workloads::make_keys(pool, maplab_workloads::key_pattern::random);
  const auto absent =
      maplab_workloads::make_absent_keys(pool, maplab_workloads::key_pattern::random, pool);

  std::tuple<table<typename Cases::plain>...> tables;
  std::array<std::size_t, n_cases> sizes{};

  std::size_t i = 0;
  std::apply([&](auto&... t) { ((sizes[i++] = fill_to_ceiling(t, keys, target_capacity)), ...); },
             tables);

  // Each table is probed over a uniform sample of **its own** residents.
  //
  // The obvious shortcut -- probe the prefix of keys resident in every table -- is a trap,
  // and this experiment fell into it once. Keys are inserted in order, so the common
  // prefix is exactly the set inserted while the table was still mostly empty; every one
  // of them sits at its home position. Probing only those reported 1.000 groups per hit at
  // *every* load factor, which is precisely the quantity the experiment exists to measure.
  // Shuffling each table's full resident set and then truncating gives a uniform sample
  // instead, and each body still performs the same number of lookups.
  const std::size_t probes = *std::min_element(sizes.begin(), sizes.end());
  std::vector<std::vector<std::uint64_t>> hit_probes(n_cases);
  for (std::size_t j = 0; j < n_cases; ++j) {
    std::vector<std::uint64_t> v(keys.begin(), keys.begin() + static_cast<long>(sizes[j]));
    maplab_workloads::shuffle_in_place(v, 0xBEEF + j);
    v.resize(probes);
    hit_probes[j] = std::move(v);
  }
  const auto miss_probe = maplab_workloads::shuffled(
      std::vector<std::uint64_t>(absent.begin(), absent.begin() + static_cast<long>(probes)));

  std::vector<std::function<void()>> bodies;
  bodies.reserve(n_cases * 2);
  std::size_t idx = 0;
  std::apply(
      [&](auto&... t) {
        auto add = [&](auto& tbl) {
          const auto* probe = &hit_probes[idx++];
          bodies.push_back([&tbl, probe] {
            for (const auto k : *probe) sink(tbl.find(k) != tbl.end());
          });
        };
        (add(t), ...);
      },
      tables);
  std::apply(
      [&](auto&... t) {
        (bodies.push_back([&t, &miss_probe] {
          for (const auto k : miss_probe) sink(t.find(k) != t.end());
        }),
         ...);
      },
      tables);

  const auto timings = time_interleaved(probes, bodies);

  std::vector<row> rows;
  rows.reserve(n_cases);
  i = 0;
  std::apply(
      [&](auto&... t) {
        ((rows.push_back(row{"",
                             t.load_factor(),
                             t.size(),
                             static_cast<double>(t.memory_usage()) / static_cast<double>(t.size()),
                             timings[i].best,
                             timings[i + n_cases].best,
                             {},
                             {}}),
          ++i),
         ...);
      },
      tables);

  const std::array<std::string, n_cases> labels{
      (std::to_string(Cases::num) + "/" + std::to_string(Cases::den))...};
  for (std::size_t j = 0; j < n_cases; ++j) rows[j].label = labels[j];

  // Counters, one instrumented table at a time so the peak footprint stays reasonable.
  i = 0;
  ((collect_counters<Cases>(keys, target_capacity, hit_probes[i], miss_probe, rows[i]), ++i), ...);
  return rows;
}

}  // namespace

int main() {
  heading("Experiment 2: load factor sweep");
  std::cout << "Every configuration is filled to its own growth ceiling on an array of the\n"
               "same capacity, and all nine are measured with their repetitions\n"
               "interleaved.\n\n";

  csv out(results_dir() + "/exp2_load_factor.csv",
          {"max_load",
           "load_factor",
           "elements",
           "bytes_per_elem",
           "hit_ns",
           "miss_ns",
           "groups_per_hit",
           "groups_per_miss",
           "groups_p99_miss",
           "keycmp_per_hit"});
  csv hist(results_dir() + "/exp2_probe_histogram.csv",
           {"max_load", "lookup", "groups", "fraction"});

  const console_table console({{"max load", 9},
                               {"actual", 7},
                               {"elements", 9},
                               {"B/elem", 7},
                               {"hit ns", 8},
                               {"miss ns", 8},
                               {"grp/hit", 8},
                               {"grp/miss", 9},
                               {"p99 miss", 9},
                               {"cmp/hit", 8}});

  // A 2^19 - 1 slot array is ~8.9 MB of control bytes and slots, just past this machine's
  // 6 MB L3, so the miss path's extra group probes land in DRAM where they are visible.
  // The key pool must reach the ceiling of the most aggressive configuration.
  constexpr std::size_t target_capacity = (1U << 19U) - 1;
  constexpr std::size_t pool = 530000;

  const auto rows = run_all<load_case<1, 2>,
                            load_case<5, 8>,
                            load_case<11, 16>,
                            load_case<3, 4>,
                            load_case<13, 16>,
                            load_case<7, 8>,
                            load_case<29, 32>,
                            load_case<15, 16>,
                            load_case<31, 32>>(target_capacity, pool);

  for (const auto& r : rows) {
    console.row({r.label,
                 fmt(r.load_factor, 3),
                 std::to_string(r.elements),
                 fmt(r.bytes_per_elem, 1),
                 fmt(r.hit_ns),
                 fmt(r.miss_ns),
                 fmt(r.hit_stats.mean_groups_per_lookup(), 3),
                 fmt(r.miss_stats.mean_groups_per_lookup(), 3),
                 std::to_string(r.miss_stats.groups_quantile(0.99)),
                 fmt(r.hit_stats.mean_key_compares_per_lookup(), 3)});
    out.row(r.label,
            fmt(r.load_factor, 4),
            r.elements,
            fmt(r.bytes_per_elem, 3),
            fmt(r.hit_ns, 4),
            fmt(r.miss_ns, 4),
            fmt(r.hit_stats.mean_groups_per_lookup(), 4),
            fmt(r.miss_stats.mean_groups_per_lookup(), 4),
            r.miss_stats.groups_quantile(0.99),
            fmt(r.hit_stats.mean_key_compares_per_lookup(), 4));

    // The histogram matters more than the mean: a lookup touching 3 groups is 3 chances
    // to miss cache.
    const auto emit = [&](const char* which, const maplab::probe_stats& s) {
      for (std::size_t b = 0; b < maplab::stats_hist_bins; ++b) {
        if (s.groups_hist[b] == 0) continue;
        hist.row(r.label,
                 which,
                 b + 1,
                 fmt(static_cast<double>(s.groups_hist[b]) / static_cast<double>(s.lookups), 6));
      }
    };
    emit("hit", r.hit_stats);
    emit("miss", r.miss_stats);
  }

  std::cout << "\nThe miss column is the one that moves: a miss must find an empty control\n"
               "byte, and the chance that a group of 16 contains none grows as alpha^16.\n";
  report_timing_noise();
  return 0;
}
