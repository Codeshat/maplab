// Experiment 3: what happens when you trust std::hash.
//
// Question:  A power-of-two capacity indexed by masking only ever looks at the low bits of
//            the hash. libstdc++ and libc++ both define std::hash<integer> as the
//            identity. What does that combination cost, and on what inputs?
// Method:    Three hashers (identity, murmur3 fmix64, wyhash) crossed with three key
//            patterns (uniform random, sequential 0..N, strided i<<12). Sequential is not
//            a strawman: order IDs, row IDs, timestamps and file offsets are all
//            sequential, and strided is what you get from aligned pointers or IDs with a
//            fixed low-order field.
// Hypothesis (written before running it, and wrong -- see the note at the end of main):
//            identity + random is fine because the entropy is already in the low bits;
//            identity + sequential is *also* fine, because consecutive integers mask to
//            consecutive slots, which is a perfect permutation with no collisions at all;
//            identity + strided is the catastrophe, because every key shares its low 12
//            bits and therefore its H1 and its H2.
//
//            Two of those three predictions survived contact with the counters. The
//            reasoning about "consecutive integers mask to consecutive slots" forgot that
//            the table does not index by the hash -- it indexes by H1 = hash >> 7.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "exp_common.hpp"
#include "maplab/flat_map.hpp"
#include "maplab_workloads/workloads.hpp"

namespace {

using namespace maplab_exp;
using pattern = maplab_workloads::key_pattern;

template<class Hash, class Config>
using table = maplab::flat_map<std::uint64_t, std::uint64_t, Hash, std::equal_to<>, Config>;

struct row {
  double insert_ns;
  double hit_ns;
  double miss_ns;
  maplab::probe_stats hit_stats;
};

template<class Hash>
maplab::probe_stats count(const std::vector<std::uint64_t>& keys,
                          const std::vector<std::uint64_t>& hit_probe) {
  table<Hash, maplab::stats_config> c;
  c.reserve(keys.size());
  for (const auto k : keys) c.try_emplace(k, k);
  c.reset_stats();
  for (const auto k : hit_probe) sink(c.find(k) != c.end());
  return c.stats();
}

template<class Hash>
double time_insert(const std::vector<std::uint64_t>& keys) {
  table<Hash, maplab::default_config> m;
  m.reserve(keys.size());
  return best_ns_per_op(
      keys.size(),
      [&] {
        m.clear();
        for (const auto k : keys) m.try_emplace(k, k);
      },
      3);
}

// All three hashers are compared within one pattern, with their lookup measurements
// interleaved, because on this machine a run long enough to compare them is also long
// enough to change the clock.
void run_pattern(pattern p, std::size_t n, csv& out, const console_table& console, csv& hist) {
  const auto keys = maplab_workloads::make_keys(n, p);
  const auto absent = maplab_workloads::make_absent_keys(n, p, n);
  const auto hit_probe = maplab_workloads::shuffled(keys);
  const auto miss_probe = maplab_workloads::shuffled(absent);

  table<maplab::identity_hash, maplab::default_config> ident;
  table<maplab::fmix_hash, maplab::default_config> fmix;
  table<maplab::wyhash_hash, maplab::default_config> wy;
  ident.reserve(n);
  fmix.reserve(n);
  wy.reserve(n);
  for (const auto k : keys) {
    ident.try_emplace(k, k);
    fmix.try_emplace(k, k);
    wy.try_emplace(k, k);
  }

  const auto t = time_interleaved(n,
                                  {[&] {
                                     for (const auto k : hit_probe)
                                       sink(ident.find(k) != ident.end());
                                   },
                                   [&] {
                                     for (const auto k : hit_probe)
                                       sink(fmix.find(k) != fmix.end());
                                   },
                                   [&] {
                                     for (const auto k : hit_probe) sink(wy.find(k) != wy.end());
                                   },
                                   [&] {
                                     for (const auto k : miss_probe)
                                       sink(ident.find(k) != ident.end());
                                   },
                                   [&] {
                                     for (const auto k : miss_probe)
                                       sink(fmix.find(k) != fmix.end());
                                   },
                                   [&] {
                                     for (const auto k : miss_probe) sink(wy.find(k) != wy.end());
                                   }});

  const std::array<const char*, 3> names{"identity", "fmix64", "wyhash"};
  const std::array<double, 3> inserts{time_insert<maplab::identity_hash>(keys),
                                      time_insert<maplab::fmix_hash>(keys),
                                      time_insert<maplab::wyhash_hash>(keys)};
  const std::array<maplab::probe_stats, 3> stats{count<maplab::identity_hash>(keys, hit_probe),
                                                 count<maplab::fmix_hash>(keys, hit_probe),
                                                 count<maplab::wyhash_hash>(keys, hit_probe)};

  for (std::size_t i = 0; i < 3; ++i) {
    const auto& st = stats[i];
    console.row({names[i],
                 maplab_workloads::name_of(p),
                 fmt(inserts[i]),
                 fmt(t[i].best),
                 fmt(t[i + 3].best),
                 fmt(st.mean_groups_per_lookup(), 3),
                 std::to_string(st.groups_quantile(0.99)),
                 fmt(st.mean_key_compares_per_lookup(), 3)});
    out.row(names[i],
            maplab_workloads::name_of(p),
            n,
            fmt(inserts[i], 4),
            fmt(t[i].best, 4),
            fmt(t[i + 3].best, 4),
            fmt(st.mean_groups_per_lookup(), 4),
            st.groups_quantile(0.99),
            fmt(st.mean_key_compares_per_lookup(), 4));

    for (std::size_t b = 0; b < maplab::stats_hist_bins; ++b) {
      if (st.groups_hist[b] == 0) continue;
      hist.row(names[i],
               maplab_workloads::name_of(p),
               b + 1,
               fmt(static_cast<double>(st.groups_hist[b]) / static_cast<double>(st.lookups), 6));
    }
  }
}

}  // namespace

int main() {
  heading("Experiment 3: hash quality on realistic integer keys");
  std::cout << "identity_hash is exactly what libstdc++ and libc++ do for integers.\n"
               "Combined with a power-of-two mask, it is the table's entire defence\n"
               "against structured input -- which is to say, none.\n\n";

  csv out(results_dir() + "/exp3_hash_quality.csv",
          {"hash",
           "pattern",
           "elements",
           "insert_ns",
           "hit_ns",
           "miss_ns",
           "groups_per_hit",
           "groups_p99_hit",
           "keycmp_per_hit"});

  const console_table console({{"hash", 14},
                               {"keys", 11},
                               {"insert ns", 10},
                               {"hit ns", 8},
                               {"miss ns", 8},
                               {"grp/hit", 8},
                               {"p99", 6},
                               {"cmp/hit", 8}});

  constexpr std::size_t n = 1U << 19U;
  csv hist(results_dir() + "/exp3_probe_histogram.csv", {"hash", "pattern", "groups", "fraction"});

  for (const auto p : {pattern::random, pattern::sequential, pattern::strided}) {
    run_pattern(p, n, out, console, hist);
  }

  std::cout
      << "\nThe two identity failures are different failures, and only the counters tell\n"
         "them apart:\n"
         "\n"
         "  sequential  H1 = hash >> 7, so 128 consecutive integers share a home group.\n"
         "              The group is 16 slots wide. The table is 8x oversubscribed at every\n"
         "              home position and the probe walks a huge cluster: watch grp/hit.\n"
         "              This refutes the hypothesis at the top of this file, which assumed\n"
         "              the table indexes by the hash rather than by H1.\n"
         "\n"
         "  strided     H2 = hash & 0x7F, and i << 12 has no low bits, so every key stores\n"
         "              fingerprint 0. Home groups are still well spread (grp/hit stays at\n"
         "              1.0), but the filter matches every occupied slot in the group, so\n"
         "              each lookup pays a full key comparison per resident: watch cmp/hit.\n"
         "              This is the H2 ablation of Experiment 4, arrived at by accident\n"
         "              through the key distribution rather than on purpose through config.\n"
         "\n"
         "Neither is repairable by better probing. This is the argument for the table owning\n"
         "its mixer rather than inheriting whatever std::hash the key type came with.\n";
  report_timing_noise();
  return 0;
}
