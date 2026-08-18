// maplab: the measurement instrument.
//
// Timing tells you *that* one design is faster. These counters tell you *why*: how many
// 16-byte groups a lookup touched, how many full key comparisons it paid for, and how
// many of those comparisons were H2 false positives that the 7-bit fingerprint failed to
// reject. Every experiment in EXPERIMENTS.md is built on this struct.
//
// Everything here is compiled out entirely unless the table's Config sets `stats = true`;
// the counters live behind `if constexpr` and the disabled struct is empty, so a
// stats-off table is byte-for-byte the same object as one built before this header
// existed. tests/test_zero_cost.cpp asserts the sizeof.
#ifndef MAPLAB_STATS_HPP
#define MAPLAB_STATS_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <ostream>

namespace maplab {

inline constexpr std::size_t stats_hist_bins = 32;

struct probe_stats {
  // Lookups (find/contains/count, and the lookup half of every insert).
  std::uint64_t lookups = 0;
  std::uint64_t lookup_hits = 0;
  std::uint64_t lookup_misses = 0;

  // Groups loaded and scanned. The primary cost signal: one group is one 16-byte
  // control load, and beyond the first group it is usually one more cache line.
  std::uint64_t groups_probed = 0;

  // Full key comparisons performed after an H2 match said "maybe".
  std::uint64_t key_compares = 0;
  // ...of which the key did not actually match. Divided by key_compares this is the
  // measured false-positive rate of the 7-bit filter; the theoretical value is 1/128
  // per occupied slot examined.
  std::uint64_t h2_false_positives = 0;

  // Tombstones stepped over while probing. Grows without bound under churn if the
  // drain policy is disabled: that is Experiment 5.
  std::uint64_t tombstones_probed = 0;

  // Mutations and table events.
  std::uint64_t inserts = 0;
  std::uint64_t erases = 0;
  std::uint64_t tombstones_created = 0;
  std::uint64_t grows = 0;   // rehash into a larger allocation
  std::uint64_t drains = 0;  // rehash at the same capacity purely to drop tombstones
  std::uint64_t slots_moved = 0;

  // Groups probed per lookup. Bin i is "i+1 groups"; the last bin saturates.
  std::array<std::uint64_t, stats_hist_bins> groups_hist{};

  void record_groups(std::uint64_t n) noexcept {
    groups_probed += n;
    const std::size_t bin = n == 0 ? 0 : n - 1;
    groups_hist[bin < stats_hist_bins ? bin : stats_hist_bins - 1] += 1;
  }

  [[nodiscard]] double mean_groups_per_lookup() const noexcept {
    return lookups == 0 ? 0.0 : static_cast<double>(groups_probed) / static_cast<double>(lookups);
  }

  [[nodiscard]] double mean_key_compares_per_lookup() const noexcept {
    return lookups == 0 ? 0.0 : static_cast<double>(key_compares) / static_cast<double>(lookups);
  }

  [[nodiscard]] double h2_false_positive_rate() const noexcept {
    return key_compares == 0
               ? 0.0
               : static_cast<double>(h2_false_positives) / static_cast<double>(key_compares);
  }

  // The tail matters more than the mean: a lookup that touches 4 groups is 4 chances at
  // a cache miss. Returns the smallest group count covering `q` of all lookups.
  [[nodiscard]] std::uint64_t groups_quantile(double q) const noexcept {
    const auto target = static_cast<std::uint64_t>(q * static_cast<double>(lookups));
    std::uint64_t acc = 0;
    for (std::size_t i = 0; i < stats_hist_bins; ++i) {
      acc += groups_hist[i];
      if (acc >= target) return i + 1;
    }
    return stats_hist_bins;
  }

  void reset() noexcept { *this = probe_stats{}; }

  probe_stats& operator+=(const probe_stats& o) noexcept {
    lookups += o.lookups;
    lookup_hits += o.lookup_hits;
    lookup_misses += o.lookup_misses;
    groups_probed += o.groups_probed;
    key_compares += o.key_compares;
    h2_false_positives += o.h2_false_positives;
    tombstones_probed += o.tombstones_probed;
    inserts += o.inserts;
    erases += o.erases;
    tombstones_created += o.tombstones_created;
    grows += o.grows;
    drains += o.drains;
    slots_moved += o.slots_moved;
    for (std::size_t i = 0; i < stats_hist_bins; ++i) groups_hist[i] += o.groups_hist[i];
    return *this;
  }
};

// The disabled counterpart. Empty, and stored with [[no_unique_address]].
struct no_stats {};

std::ostream& operator<<(std::ostream& os, const probe_stats& s);

inline std::ostream& operator<<(std::ostream& os, const probe_stats& s) {
  os << "lookups=" << s.lookups << " (hit " << s.lookup_hits << " / miss " << s.lookup_misses
     << ")\n  groups/lookup   mean=" << s.mean_groups_per_lookup()
     << "  p50=" << s.groups_quantile(0.5) << "  p99=" << s.groups_quantile(0.99)
     << "\n  keycmp/lookup   mean=" << s.mean_key_compares_per_lookup()
     << "  H2 false-positive rate=" << s.h2_false_positive_rate()
     << "\n  tombstones      probed=" << s.tombstones_probed << " created=" << s.tombstones_created
     << "\n  table           inserts=" << s.inserts << " erases=" << s.erases
     << " grows=" << s.grows << " drains=" << s.drains << " slots_moved=" << s.slots_moved << '\n';
  return os;
}

}  // namespace maplab

#endif  // MAPLAB_STATS_HPP
