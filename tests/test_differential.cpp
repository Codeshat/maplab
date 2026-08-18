// The single highest-value test in the repo: run a long random operation sequence against
// both maplab::flat_map and std::unordered_map and assert they never disagree.
//
// A unit test checks the cases its author thought of. This checks the cases the author
// did not: a growth that lands mid-probe-chain, an erase that turns the last element of a
// chain empty rather than deleted, a reinsert that reuses a tombstone three groups away.
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include "configs.hpp"
#include "maplab/flat_map.hpp"
#include "maplab_workloads/workloads.hpp"

namespace {

// A small key space relative to the operation count is deliberate: it forces collisions,
// tombstone reuse and repeated growth/shrink cycles instead of a table that only ever
// grows.
template<class Config>
void run_differential(std::size_t ops, std::uint64_t key_space, std::uint64_t seed) {
  maplab::flat_map<std::uint64_t, std::uint64_t, maplab::default_hash, std::equal_to<>, Config>
      subject;
  std::unordered_map<std::uint64_t, std::uint64_t> reference;
  maplab_workloads::splitmix64 rng(seed);

  const std::size_t check_every = 997;  // prime, so checks do not align with any cycle

  for (std::size_t i = 0; i < ops; ++i) {
    const std::uint64_t key = rng.bounded(key_space);
    const std::uint64_t value = rng();

    const std::uint64_t op = rng.bounded(100);

    if (op < 35) {  // insert
      const auto a = subject.insert({key, value});
      const auto b = reference.insert({key, value});
      REQUIRE(a.second == b.second);
      REQUIRE(a.first->first == key);
      REQUIRE(a.first->second == b.first->second);
    } else if (op < 45) {  // try_emplace
      const auto a = subject.try_emplace(key, value);
      const auto b = reference.try_emplace(key, value);
      REQUIRE(a.second == b.second);
      REQUIRE(a.first->second == b.first->second);
    } else if (op < 55) {  // insert_or_assign
      const auto a = subject.insert_or_assign(key, value);
      const auto b = reference.insert_or_assign(key, value);
      REQUIRE(a.second == b.second);
      REQUIRE(a.first->second == value);
      REQUIRE(b.first->second == value);
    } else if (op < 63) {  // operator[]
      subject[key] = value;
      reference[key] = value;
    } else if (op < 83) {  // erase by key
      REQUIRE(subject.erase(key) == reference.erase(key));
    } else if (op < 85) {  // erase by iterator
      auto it = subject.find(key);
      auto jt = reference.find(key);
      REQUIRE((it == subject.end()) == (jt == reference.end()));
      if (it != subject.end()) {
        subject.erase(it);
        reference.erase(jt);
      }
    } else if (op == 85) {  // reserve
      subject.reserve(subject.size() + rng.bounded(512));
    } else if (op == 86) {  // rehash down to the smallest capacity that still fits
      subject.rehash(subject.size());
    } else if (op == 87) {  // clear, occasionally, so the empty-table paths run too
      if (rng.bounded(8) == 0) {
        subject.clear();
        reference.clear();
      }
    } else if (op < 90) {  // copy round-trip: the copy must be indistinguishable
      auto copy = subject;
      REQUIRE(copy.size() == subject.size());
      for (const auto& [k, v] : copy) {
        const auto it = subject.find(k);
        REQUIRE(it != subject.end());
        REQUIRE(it->second == v);
      }
      subject = std::move(copy);
    } else {  // find
      const auto it = subject.find(key);
      const auto jt = reference.find(key);
      REQUIRE((it == subject.end()) == (jt == reference.end()));
      if (it != subject.end()) REQUIRE(it->second == jt->second);
      REQUIRE(subject.contains(key) == (reference.count(key) != 0));
    }

    if (i % check_every == 0) {
      REQUIRE(subject.size() == reference.size());
      // Invariants that must hold after *every* operation, not just at the end.
      REQUIRE(subject.size() <= subject.capacity());
      if (subject.capacity() != 0) {
        // size + tombstones + growth_left is exactly the growth ceiling, always.
        // This is the accounting identity the whole insert path depends on: break it
        // and the table either rehashes forever or overruns its load factor.
        const std::size_t cap = subject.capacity();
        const std::size_t ceiling =
            cap / Config::max_load_den * Config::max_load_num +
            cap % Config::max_load_den * Config::max_load_num / Config::max_load_den;
        REQUIRE(subject.size() + subject.tombstones() + subject.growth_left() == ceiling);
        REQUIRE(subject.load_factor() <= subject.max_load_factor());
      }
    }
  }

  // Full agreement in both directions at the end.
  REQUIRE(subject.size() == reference.size());
  for (const auto& [k, v] : reference) {
    const auto it = subject.find(k);
    REQUIRE(it != subject.end());
    REQUIRE(it->second == v);
  }
  std::size_t seen = 0;
  for (const auto& [k, v] : subject) {
    const auto it = reference.find(k);
    REQUIRE(it != reference.end());
    REQUIRE(it->second == v);
    ++seen;
  }
  REQUIRE(seen == subject.size());
}

}  // namespace

TEMPLATE_LIST_TEST_CASE("differential vs std::unordered_map, dense key space",
                        "[differential]",
                        maplab_tests::all_configs) {
  run_differential<TestType>(60000, 700, 0xD1FF);
}

TEMPLATE_LIST_TEST_CASE("differential vs std::unordered_map, sparse key space",
                        "[differential]",
                        maplab_tests::all_configs) {
  run_differential<TestType>(60000, 200000, 0x5EED);
}

// The soak test. Skipped by default so `ctest` stays fast; CI runs it by tag on the
// release preset, and the pre-tag checklist runs it at 10M.
TEST_CASE("differential soak", "[differential][.soak]") {
  run_differential<maplab::default_config>(2000000, 20000, 0x50AC);
}
