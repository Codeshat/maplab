// The claim "the stats build is free when it is off" is a claim, so it gets a test.
#include <cstdint>
#include <functional>
#include <iterator>
#include <string>
#include <type_traits>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "maplab/flat_map.hpp"

using plain = maplab::flat_map<std::uint64_t, std::uint64_t>;
using instrumented = maplab::instrumented_flat_map<std::uint64_t, std::uint64_t>;
using scalar = maplab::flat_map<std::uint64_t,
                                std::uint64_t,
                                maplab::default_hash,
                                std::equal_to<>,
                                maplab::scalar_config>;

// Five words: two pointers and three size_ts. The hasher and comparator are empty and
// carry no storage, and neither does the disabled stats block.
static_assert(sizeof(plain) == 5 * sizeof(void*));
static_assert(sizeof(scalar) == sizeof(plain));
static_assert(sizeof(instrumented) > sizeof(plain));

// Ablation configs must not change the object layout either, or an ablation would be
// measuring two different data structures.
static_assert(sizeof(maplab::flat_map<std::uint64_t,
                                      std::uint64_t,
                                      maplab::default_hash,
                                      std::equal_to<>,
                                      maplab::no_h2_config>) == sizeof(plain));
static_assert(sizeof(maplab::flat_map<std::uint64_t,
                                      std::uint64_t,
                                      maplab::default_hash,
                                      std::equal_to<>,
                                      maplab::group8_config>) == sizeof(plain));

// A stateful hasher does take space, and must not be silently dropped.
struct stateful_hash {
  std::uint64_t seed = 1;

  std::size_t operator()(std::uint64_t k) const noexcept {
    return maplab::default_hash{}(k ^ seed);
  }
};

static_assert(sizeof(maplab::flat_map<std::uint64_t, std::uint64_t, stateful_hash>) >
              sizeof(plain));

TEST_CASE("value_type is the standard one and iterators model forward_iterator", "[traits]") {
  static_assert(std::is_same_v<plain::value_type, std::pair<const std::uint64_t, std::uint64_t>>);
  static_assert(std::is_same_v<plain::key_type, std::uint64_t>);
  static_assert(std::is_same_v<plain::mapped_type, std::uint64_t>);
  static_assert(std::forward_iterator<plain::iterator>);
  static_assert(std::forward_iterator<plain::const_iterator>);
  static_assert(std::is_convertible_v<plain::iterator, plain::const_iterator>);
  static_assert(!std::is_convertible_v<plain::const_iterator, plain::iterator>);
  SUCCEED();
}

TEST_CASE("a slot is exactly the key plus the value, with no per-element overhead", "[traits]") {
  // The memory story: one control byte per slot, and slots that are exactly as big as
  // the pair. std::unordered_map cannot make this claim -- a node carries a next pointer
  // and an allocation header on top.
  maplab::flat_map<std::uint64_t, std::uint64_t> m;
  m.reserve(1024);
  const double bytes_per_slot =
      static_cast<double>(m.memory_usage()) / static_cast<double>(m.capacity());
  REQUIRE(bytes_per_slot > 16.0);  // the pair
  REQUIRE(bytes_per_slot < 17.5);  // plus one control byte and a little rounding
}
