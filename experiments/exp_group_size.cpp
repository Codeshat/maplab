// Experiment 6: group width 8 versus 16.
//
// Question:  A 16-byte group is one SSE2 register. An 8-byte group halves the number of
//            control bytes a probe examines but doubles the number of probe steps needed
//            to cover the same ground. Does the wider group win, and by how much?
// Method:    Config::group_width, crossed with the probe policy, at several table sizes.
//            Everything else -- load factor, hashing, layout -- is identical; only the
//            granularity of the scan and therefore the probe sequence's stride change.
// Expect:    A small edge to 16 on the miss path: a wider group is more likely to contain
//            an empty byte, so misses terminate in fewer steps. Crucially this can only
//            show up at a high load factor -- at alpha = 0.5 a group of 8 already contains
//            an empty byte 99.6% of the time, so there is nothing to measure. Each table is
//            therefore filled to its 7/8 ceiling, where 0.875^8 = 34% of groups are full
//            against 0.875^16 = 12%.
//            A null result is still a result: it would say the win comes from the *layout*
//            -- one control byte per slot, scanned in bulk -- and not from the specific
//            register width, which is the part that ports to NEON.
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

struct result {
  double hit_ns;
  double miss_ns;
  double groups_hit;
  double groups_miss;
};

struct width8_stats : maplab::group8_config {
  static constexpr bool stats = true;
};

struct width8_scalar_stats : maplab::group8_scalar_config {
  static constexpr bool stats = true;
};

template<class Config>
using table =
    maplab::flat_map<std::uint64_t, std::uint64_t, maplab::default_hash, std::equal_to<>, Config>;

// Insert until one more insert would rehash, so every configuration is measured at its
// design load rather than wherever reserve() happened to leave it.
template<class Config>
void fill_to_ceiling(table<Config>& m, const std::vector<std::uint64_t>& keys) {
  for (const auto k : keys) {
    m.try_emplace(k, k);
    if (m.growth_left() == 0 && m.size() > keys.size() / 2) break;
  }
}

template<class Stats>
std::pair<double, double> count_groups(const std::vector<std::uint64_t>& keys,
                                       const std::vector<std::uint64_t>& hit_probe,
                                       const std::vector<std::uint64_t>& miss_probe) {
  table<Stats> c;
  fill_to_ceiling(c, keys);
  c.reset_stats();
  for (const auto k : hit_probe) sink(c.find(k) != c.end());
  const double gh = c.stats().mean_groups_per_lookup();
  c.reset_stats();
  for (const auto k : miss_probe) sink(c.find(k) != c.end());
  const double gm = c.stats().mean_groups_per_lookup();
  return {gh, gm};
}

// Four configurations, eight measurements, all interleaved.
std::array<result, 4> measure(std::size_t n) {
  const auto keys = maplab_workloads::make_keys(n, maplab_workloads::key_pattern::random);
  const auto absent =
      maplab_workloads::make_absent_keys(n, maplab_workloads::key_pattern::random, n);
  const auto hit_probe = maplab_workloads::shuffled(keys);
  const auto miss_probe = maplab_workloads::shuffled(absent);

  table<maplab::default_config> w16;
  table<maplab::group8_config> w8;
  table<maplab::scalar_config> w16s;
  table<maplab::group8_scalar_config> w8s;
  // Filled to the growth ceiling rather than reserve()d. reserve(n) leaves the table at
  // roughly alpha = 0.5, where a group of 8 already contains an empty byte 99.6% of the
  // time and the comparison has nothing to measure.
  fill_to_ceiling(w16, keys);
  fill_to_ceiling(w8, keys);
  fill_to_ceiling(w16s, keys);
  fill_to_ceiling(w8s, keys);

  const auto t = time_interleaved(n,
                                  {[&] {
                                     for (const auto k : hit_probe) sink(w16.find(k) != w16.end());
                                   },
                                   [&] {
                                     for (const auto k : hit_probe) sink(w8.find(k) != w8.end());
                                   },
                                   [&] {
                                     for (const auto k : hit_probe)
                                       sink(w16s.find(k) != w16s.end());
                                   },
                                   [&] {
                                     for (const auto k : hit_probe) sink(w8s.find(k) != w8s.end());
                                   },
                                   [&] {
                                     for (const auto k : miss_probe) sink(w16.find(k) != w16.end());
                                   },
                                   [&] {
                                     for (const auto k : miss_probe) sink(w8.find(k) != w8.end());
                                   },
                                   [&] {
                                     for (const auto k : miss_probe)
                                       sink(w16s.find(k) != w16s.end());
                                   },
                                   [&] {
                                     for (const auto k : miss_probe) sink(w8s.find(k) != w8s.end());
                                   }});

  const auto g16 = count_groups<maplab::stats_config>(keys, hit_probe, miss_probe);
  const auto g8 = count_groups<width8_stats>(keys, hit_probe, miss_probe);
  const auto g16s = count_groups<maplab::scalar_stats_config>(keys, hit_probe, miss_probe);
  const auto g8s = count_groups<width8_scalar_stats>(keys, hit_probe, miss_probe);

  return {result{t[0].best, t[4].best, g16.first, g16.second},
          result{t[1].best, t[5].best, g8.first, g8.second},
          result{t[2].best, t[6].best, g16s.first, g16s.second},
          result{t[3].best, t[7].best, g8s.first, g8s.second}};
}

}  // namespace

int main() {
  heading("Experiment 6: group width 8 vs 16");

  csv out(results_dir() + "/exp6_group_size.csv",
          {"width", "probe", "elements", "hit_ns", "miss_ns", "groups_per_hit", "groups_per_miss"});

  const console_table table({{"elements", 10},
                             {"w16 hit", 9},
                             {"w8 hit", 9},
                             {"w16 miss", 10},
                             {"w8 miss", 9},
                             {"grp/miss 16", 12},
                             {"grp/miss 8", 11}});

  for (const std::size_t n :
       {std::size_t{1} << 12, std::size_t{1} << 16, std::size_t{1} << 20, std::size_t{1} << 22}) {
    const auto r = measure(n);
    const auto& w16 = r[0];
    const auto& w8 = r[1];
    const auto& w16s = r[2];
    const auto& w8s = r[3];

    table.row({std::to_string(n),
               fmt(w16.hit_ns),
               fmt(w8.hit_ns),
               fmt(w16.miss_ns),
               fmt(w8.miss_ns),
               fmt(w16.groups_miss, 3),
               fmt(w8.groups_miss, 3)});

    out.row(16,
            "simd",
            n,
            fmt(w16.hit_ns, 4),
            fmt(w16.miss_ns, 4),
            fmt(w16.groups_hit, 4),
            fmt(w16.groups_miss, 4));
    out.row(8,
            "simd",
            n,
            fmt(w8.hit_ns, 4),
            fmt(w8.miss_ns, 4),
            fmt(w8.groups_hit, 4),
            fmt(w8.groups_miss, 4));
    out.row(16,
            "scalar",
            n,
            fmt(w16s.hit_ns, 4),
            fmt(w16s.miss_ns, 4),
            fmt(w16s.groups_hit, 4),
            fmt(w16s.groups_miss, 4));
    out.row(8,
            "scalar",
            n,
            fmt(w8s.hit_ns, 4),
            fmt(w8s.miss_ns, 4),
            fmt(w8s.groups_hit, 4),
            fmt(w8s.groups_miss, 4));
  }

  std::cout << "\nThe groups/miss columns are the mechanism: a narrower group contains an\n"
               "empty byte less often, so a miss takes more steps to prove absence.\n";
  report_timing_noise();
  return 0;
}
