// maplab: compile-time table configuration.
//
// Every design decision this project claims to have "measured" is a member of a config
// struct, so an experiment is a template argument rather than a patch, a #ifdef, or a
// separate branch. Two tables that differ only in `probe` share the entire rest of the
// implementation by construction, which is what makes the ablations honest.
//
// Configs compose by inheritance: a derived struct redeclares only what it changes and
// name hiding does the rest.
//
//   struct my_config : maplab::default_config {
//     using probe = maplab::probe_policy::scalar;
//   };
#ifndef MAPLAB_CONFIG_HPP
#define MAPLAB_CONFIG_HPP

#include <cstddef>

#include "maplab/control.hpp"

namespace maplab {

struct default_config {
  // Control bytes examined per probe step. 16 is one SSE2 register and one control-array
  // cache line quarter; 8 is the ablation (Experiment 6).
  static constexpr std::size_t group_width = 16;

  // simd => sse2_group, scalar => scalar_group. Identical layout, identical algorithm,
  // different scan. This is Experiment 1's only degree of freedom.
  using probe = probe_policy::simd;

  // Maximum load factor as an exact rational; the table grows when it would be exceeded.
  // 7/8 is viable only because the H2 filter makes long probe chains cheap to walk.
  static constexpr std::size_t max_load_num = 7;
  static constexpr std::size_t max_load_den = 8;

  // When false, the group is chosen from the whole hash instead of from H1 = hash >> 7,
  // so the position bits and the fingerprint bits overlap. Everything still *works*; the
  // filter just stops filtering. Kept as a knob because "H1 and H2 must be disjoint" is
  // the kind of claim that is much more convincing with a number attached.
  static constexpr bool split_hash = true;

  // When false, every occupied control byte stores the fingerprint 0 instead of H2, so
  // the group match degenerates to "every occupied slot is a candidate" and each match
  // costs a full key comparison. The layout, the probe order and the load factor are
  // untouched: this isolates exactly what the 7 bits buy (Experiment 4).
  static constexpr bool h2_filter = true;

  // Rehash at the *same* capacity when tombstones dominate, instead of only ever growing.
  // Without it, an insert/erase churn workload degrades without bound (Experiment 5).
  static constexpr bool drain_tombstones = true;

  // Prefetch the slot line while the control group is still being scanned.
  static constexpr bool prefetch = false;

  // Compile in the probe_stats counters. Off by default and free when off.
  static constexpr bool stats = false;
};

// ---- Named configurations used by the tests and experiments ---------------------------

struct scalar_config : default_config {
  using probe = probe_policy::scalar;
};

struct group8_config : default_config {
  static constexpr std::size_t group_width = 8;
};

struct group8_scalar_config : default_config {
  static constexpr std::size_t group_width = 8;
  using probe = probe_policy::scalar;
};

// H2 defeated: the filter always passes.
struct no_h2_config : default_config {
  static constexpr bool h2_filter = false;
};

// H1 and H2 cut from overlapping bits of the hash: the filter still runs, but the
// candidates it sees are highly correlated.
struct overlapping_hash_config : default_config {
  static constexpr bool split_hash = false;
};

struct overlapping_hash_stats_config : overlapping_hash_config {
  static constexpr bool stats = true;
};

// Tombstones accumulate forever; only growth ever clears them.
struct no_drain_config : default_config {
  static constexpr bool drain_tombstones = false;
};

struct prefetch_config : default_config {
  static constexpr bool prefetch = true;
};

struct stats_config : default_config {
  static constexpr bool stats = true;
};

struct scalar_stats_config : scalar_config {
  static constexpr bool stats = true;
};

struct no_h2_stats_config : no_h2_config {
  static constexpr bool stats = true;
};

struct no_drain_stats_config : no_drain_config {
  static constexpr bool stats = true;
};

// Load-factor sweep (Experiment 2). Num/Den is the ceiling the table grows at.
template<std::size_t Num, std::size_t Den>
struct load_factor_config : default_config {
  static constexpr std::size_t max_load_num = Num;
  static constexpr std::size_t max_load_den = Den;
};

template<std::size_t Num, std::size_t Den>
struct load_factor_stats_config : load_factor_config<Num, Den> {
  static constexpr bool stats = true;
};

}  // namespace maplab

#endif  // MAPLAB_CONFIG_HPP
