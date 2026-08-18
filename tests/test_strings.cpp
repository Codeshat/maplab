// String keys, heterogeneous lookup, and the hashers.
#include <algorithm>
#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "maplab/flat_map.hpp"
#include "maplab_workloads/workloads.hpp"

namespace {

template<class M, class K>
concept has_find = requires(M& m, const K& k) { m.find(k); };

}  // namespace

TEST_CASE("string keys round-trip through growth", "[strings]") {
  maplab::flat_map<std::string, int> m;
  const auto keys = maplab_workloads::make_short_strings(20000, 7);
  for (std::size_t i = 0; i < keys.size(); ++i) m.try_emplace(keys[i], static_cast<int>(i));
  REQUIRE(m.size() == keys.size());
  for (std::size_t i = 0; i < keys.size(); ++i) REQUIRE(m.at(keys[i]) == static_cast<int>(i));

  const auto absent = maplab_workloads::make_long_strings(1000, 80, 99);
  for (const auto& k : absent) REQUIRE_FALSE(m.contains(k));
}

TEST_CASE("heterogeneous lookup finds a string key from a string_view", "[strings]") {
  maplab::flat_map<std::string, int> m;
  m.try_emplace("alpha", 1);
  m.try_emplace("beta", 2);

  // The point is that no std::string temporary is constructed here. That is not directly
  // observable, so what we assert is that the transparent path agrees with the
  // homogeneous one -- i.e. the hasher hashes the same bytes either way.
  const std::string_view sv = "alpha";
  REQUIRE(m.find(sv) != m.end());
  REQUIRE(m.find(sv)->second == 1);
  REQUIRE(m.contains(sv));
  REQUIRE(m.count(sv) == 1);
  REQUIRE(m.find(std::string_view{"gamma"}) == m.end());

  const char* cstr = "beta";
  REQUIRE(m.find(std::string_view{cstr}) != m.end());

  REQUIRE(m.erase(std::string_view{"alpha"}) == 1);
  REQUIRE(m.size() == 1);

  const auto& cm = m;
  REQUIRE(cm.find(std::string_view{"beta"}) != cm.end());
}

TEST_CASE("a non-transparent comparator disables heterogeneous lookup", "[strings]") {
  // std::equal_to<std::string> has no is_transparent, so the constrained overload must
  // not be selected -- otherwise a string_view would be silently converted, allocating
  // exactly the temporary the feature exists to avoid.
  using strict_map =
      maplab::flat_map<std::string, int, maplab::default_hash, std::equal_to<std::string>>;
  // string_view -> string is an *explicit* conversion, so with a non-transparent
  // comparator there is simply no viable find() -- which is the desired outcome: a
  // silent allocation per lookup would be worse than a compile error.
  static_assert(!has_find<strict_map, std::string_view>);
  static_assert(has_find<maplab::flat_map<std::string, int>, std::string_view>);
  strict_map m;
  m.try_emplace("x", 1);
  REQUIRE(m.contains("x"));
}

TEST_CASE("the hashers avalanche", "[strings][hash]") {
  // Not a statistical test suite -- just enough to catch a hasher that has stopped
  // mixing at all, which is the failure mode that silently destroys the table.
  const auto bit_spread = [](auto hasher) {
    std::array<int, 64> ones{};
    constexpr int n = 20000;
    for (std::uint64_t i = 0; i < n; ++i) {
      const std::size_t h = hasher(i);
      for (int b = 0; b < 64; ++b)
        ones[static_cast<std::size_t>(b)] += static_cast<int>((h >> b) & 1U);
    }
    int worst = n;
    int best = 0;
    for (const int c : ones) {
      worst = std::min(worst, c);
      best = std::max(best, c);
    }
    // Every output bit should be set roughly half the time over sequential inputs.
    return std::pair{worst, best};
  };

  for (auto [lo, hi] : {bit_spread(maplab::fmix_hash{}),
                        bit_spread(maplab::wyhash_hash{}),
                        bit_spread(maplab::default_hash{})}) {
    REQUIRE(lo > 9000);
    REQUIRE(hi < 11000);
  }

  // The adversary, by contrast, leaves the high bits entirely unset for small inputs.
  const auto [ilo, ihi] = bit_spread(maplab::identity_hash{});
  REQUIRE(ilo == 0);
}

TEST_CASE("the same bytes hash the same however they are spelled", "[strings][hash]") {
  const std::string s = "a moderately long string key that exceeds the SSO buffer";
  const std::string_view sv{s};
  REQUIRE(maplab::default_hash{}(s) == maplab::default_hash{}(sv));
  REQUIRE(maplab::wyhash_hash{}(s) == maplab::wyhash_hash{}(sv));

  // ...and distinct strings do not collide, at every length wyhash special-cases: 1-3
  // bytes (the overlapping-load path), 4-8, 9-16, and >16 (the block loop). A bug in any
  // one of those branches typically shows up as whole groups of keys hashing alike.
  for (std::size_t len = 1; len <= 40; ++len) {
    // Only 26^len distinct keys exist at this length, so ask for what is available.
    std::size_t space = 1;
    for (std::size_t i = 0; i < len && space < 500; ++i) space *= 26;
    const std::size_t n = std::min<std::size_t>(500, space);

    std::set<std::string> keys;
    std::set<std::size_t> hashes;
    for (std::size_t i = 0; i < n; ++i) {
      std::string k(len, 'a');
      for (std::size_t v = i, p = 0; v != 0 && p < len; v /= 26, ++p) {
        k[p] = static_cast<char>('a' + v % 26);
      }
      keys.insert(k);
      hashes.insert(maplab::default_hash{}(k));
    }
    INFO("length " << len << ", " << keys.size() << " distinct keys");
    REQUIRE(hashes.size() == keys.size());  // zero collisions expected over 64 bits
  }
}

TEST_CASE("symbol-table workload", "[strings]") {
  // ~2k short symbols, the shape a market-data process actually keeps hot.
  const auto symbols = maplab_workloads::make_symbols(2000);
  maplab::flat_map<std::string, std::uint64_t> m;
  for (std::size_t i = 0; i < symbols.size(); ++i) m.try_emplace(symbols[i], i);
  REQUIRE(m.size() == symbols.size());
  for (std::size_t i = 0; i < symbols.size(); ++i) {
    REQUIRE(m.find(std::string_view{symbols[i]})->second == i);
  }
}
