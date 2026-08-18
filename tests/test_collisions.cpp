// Cases the random differential test would take a very long time to stumble into, so we
// construct them on purpose: keys engineered to collide in H1 but not H2, in H2 but not
// H1, and probe chains made entirely of tombstones.
//
// These use a hasher we control completely, which is the only way to *guarantee* a
// collision rather than hope for one.
#include <cstdint>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "maplab/flat_map.hpp"

namespace {

// Returns the key's own low bits as the hash, so a test can dictate H1 and H2 exactly.
// H1 = hash >> 7 chooses the group, H2 = hash & 0x7F is the fingerprint.
struct dictated_hash {
  using is_transparent = void;

  std::size_t operator()(std::uint64_t k) const noexcept { return static_cast<std::size_t>(k); }
};

using dictated_map = maplab::flat_map<std::uint64_t, std::uint64_t, dictated_hash, std::equal_to<>>;

constexpr std::uint64_t make_hash(std::uint64_t group_bits, std::uint64_t fingerprint) {
  return (group_bits << 7U) | (fingerprint & 0x7FU);
}

}  // namespace

TEST_CASE("keys that share H1 but differ in H2", "[collisions]") {
  // Same group, 100 different fingerprints. Every lookup should find its key with one
  // group probe and exactly one full key comparison: this is the case the filter is for.
  dictated_map m;
  m.reserve(200);
  for (std::uint64_t i = 0; i < 100; ++i) m.try_emplace(make_hash(42, i), i);
  REQUIRE(m.size() == 100);
  for (std::uint64_t i = 0; i < 100; ++i) {
    const auto it = m.find(make_hash(42, i));
    REQUIRE(it != m.end());
    REQUIRE(it->second == i);
  }
  REQUIRE_FALSE(m.contains(make_hash(42, 100)));
}

TEST_CASE("keys that share H2 but differ in H1", "[collisions]") {
  // Same fingerprint, different groups: the filter cannot help, and correctness rests
  // entirely on the full key comparison.
  dictated_map m;
  m.reserve(2000);
  for (std::uint64_t g = 0; g < 1000; ++g) m.try_emplace(make_hash(g, 0x55), g);
  REQUIRE(m.size() == 1000);
  for (std::uint64_t g = 0; g < 1000; ++g) REQUIRE(m.at(make_hash(g, 0x55)) == g);
}

TEST_CASE("keys with identical H1 and identical H2", "[collisions]") {
  // The worst case the structure allows: distinct keys with a *completely* identical
  // hash, so they share both the group and the fingerprint. Every candidate the filter
  // returns costs a full key comparison and the probe walks the triangular sequence.
  // It must still be correct, and it must still terminate.
  //
  // Dropping the low 20 bits is what makes distinct keys collide totally; no amount of
  // fingerprinting can separate keys whose hashes are equal.
  struct colliding_hash {
    using is_transparent = void;

    std::size_t operator()(std::uint64_t k) const noexcept { return k >> 20U; }
  };

  maplab::flat_map<std::uint64_t, std::uint64_t, colliding_hash, std::equal_to<>> m;

  constexpr std::uint64_t clique = 500;
  for (std::uint64_t i = 0; i < clique; ++i) m.try_emplace(i, i);  // all hash to 0
  REQUIRE(m.size() == clique);
  for (std::uint64_t i = 0; i < clique; ++i) REQUIRE(m.at(i) == i);

  // A miss inside the colliding clique must terminate rather than probe forever.
  REQUIRE_FALSE(m.contains(clique + 1));

  for (std::uint64_t i = 0; i < clique; ++i) REQUIRE(m.erase(i) == 1);
  REQUIRE(m.empty());
}

TEST_CASE("a probe chain made entirely of tombstones still finds the key", "[collisions]") {
  // Fill one group's worth of keys, delete all but the last, then look the last one up.
  // The lookup has to walk past a run of tombstones without concluding "absent".
  dictated_map m;
  m.reserve(4096);
  std::vector<std::uint64_t> keys;
  // Distinct keys, all landing in group 11, cycling through the 128 fingerprints.
  for (std::uint64_t i = 0; i < 300; ++i) keys.push_back(make_hash(11, i) + (i << 32U));

  for (std::size_t i = 0; i < keys.size(); ++i) m.try_emplace(keys[i], i);
  const std::uint64_t survivor = keys.back();
  for (std::size_t i = 0; i + 1 < keys.size(); ++i) REQUIRE(m.erase(keys[i]) == 1);

  REQUIRE(m.size() == 1);
  REQUIRE(m.at(survivor) == keys.size() - 1);
  REQUIRE_FALSE(m.contains(keys.front()));
  // And a miss that has to walk the same tombstone chain must terminate.
  REQUIRE_FALSE(m.contains(make_hash(11, 3) + (9999ULL << 32U)));
}

TEST_CASE("erase then reinsert the same key reuses the chain", "[collisions]") {
  maplab::flat_map<std::uint64_t, std::uint64_t> m;
  for (std::uint64_t round = 0; round < 50; ++round) {
    for (std::uint64_t i = 0; i < 1000; ++i) m.try_emplace(i, round * 1000 + i);
    REQUIRE(m.size() == 1000);
    for (std::uint64_t i = 0; i < 1000; ++i) REQUIRE(m.erase(i) == 1);
    REQUIRE(m.empty());
  }
  // 50 rounds of complete churn must not have grown the table without bound: the drain
  // policy has to be reclaiming tombstones.
  REQUIRE(m.capacity() <= 4095);
}

TEST_CASE("the table survives a fully adversarial hash", "[collisions]") {
  // Every key hashes to zero. This is a linked list with extra steps, and the only thing
  // being tested is that it is a *correct* linked list with extra steps.
  struct constant_hash {
    using is_transparent = void;

    std::size_t operator()(std::uint64_t) const noexcept { return std::size_t{0}; }
  };

  maplab::flat_map<std::uint64_t, std::uint64_t, constant_hash, std::equal_to<>> m;
  for (std::uint64_t i = 0; i < 2000; ++i) m.try_emplace(i, i * 7);
  REQUIRE(m.size() == 2000);
  for (std::uint64_t i = 0; i < 2000; ++i) REQUIRE(m.at(i) == i * 7);
  REQUIRE_FALSE(m.contains(std::uint64_t{9999}));
  for (std::uint64_t i = 0; i < 2000; i += 2) REQUIRE(m.erase(i) == 1);
  for (std::uint64_t i = 1; i < 2000; i += 2) REQUIRE(m.at(i) == i * 7);
}

TEST_CASE("growth triggered in the middle of a probe chain", "[collisions]") {
  // Insert exactly up to the growth boundary, then insert a key whose chain is long, so
  // the rehash happens with a half-walked probe sequence in flight.
  for (std::size_t start = 1; start < 64; ++start) {
    maplab::flat_map<std::uint64_t, std::uint64_t> m;
    m.reserve(start);
    const std::size_t ceiling = m.capacity() / 8 * 7;
    for (std::uint64_t i = 0; i < ceiling + 20; ++i) {
      m.try_emplace(i, i);
      REQUIRE(m.size() == i + 1);
    }
    for (std::uint64_t i = 0; i < ceiling + 20; ++i) REQUIRE(m.at(i) == i);
  }
}
