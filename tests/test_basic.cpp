// The API surface, and the contracts a caller is entitled to rely on.
#include <algorithm>
#include <bit>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include "configs.hpp"
#include "maplab/flat_map.hpp"

namespace {

template<class Config>
using map_t = maplab::flat_map<int, std::string, maplab::default_hash, std::equal_to<>, Config>;

}  // namespace

TEMPLATE_LIST_TEST_CASE("an empty map touches no storage", "[basic]", maplab_tests::all_configs) {
  map_t<TestType> m;
  REQUIRE(m.empty());
  REQUIRE(m.size() == 0);
  REQUIRE(m.capacity() == 0);
  REQUIRE(m.memory_usage() == 0);  // a default-constructed map has not allocated
  REQUIRE(m.begin() == m.end());
  REQUIRE(m.find(1) == m.end());
  REQUIRE_FALSE(m.contains(1));
  REQUIRE(m.count(1) == 0);
  REQUIRE(m.erase(1) == 0);
  REQUIRE(m.load_factor() == 0.0);
  REQUIRE_THROWS_AS(m.at(1), std::out_of_range);
  // ...and clearing an empty map is not a special case anyone has to remember.
  m.clear();
  REQUIRE(m.empty());
}

TEMPLATE_LIST_TEST_CASE("insert reports whether it inserted",
                        "[basic]",
                        maplab_tests::all_configs) {
  map_t<TestType> m;
  const auto a = m.insert({1, "one"});
  REQUIRE(a.second);
  REQUIRE(a.first->first == 1);
  const auto b = m.insert({1, "uno"});
  REQUIRE_FALSE(b.second);
  REQUIRE(b.first->second == "one");  // insert never overwrites
  REQUIRE(m.size() == 1);

  REQUIRE(m.insert_or_assign(1, "uno").second == false);
  REQUIRE(m.at(1) == "uno");  // ...but insert_or_assign does

  REQUIRE(m.try_emplace(1, "ein").second == false);
  REQUIRE(m.at(1) == "uno");  // ...and try_emplace does not
  REQUIRE(m.try_emplace(2, "two").second == true);
  REQUIRE(m.size() == 2);

  REQUIRE(m.emplace(3, "three").second == true);
  REQUIRE(
      m.emplace(std::piecewise_construct, std::forward_as_tuple(4), std::forward_as_tuple(3, 'x'))
          .second == true);
  REQUIRE(m.at(4) == "xxx");
}

TEMPLATE_LIST_TEST_CASE("operator[] default-constructs, at() throws",
                        "[basic]",
                        maplab_tests::all_configs) {
  map_t<TestType> m;
  REQUIRE(m[7].empty());
  REQUIRE(m.size() == 1);
  m[7] = "seven";
  REQUIRE(m.at(7) == "seven");
  REQUIRE_THROWS_AS(m.at(8), std::out_of_range);
  const auto& cm = m;
  REQUIRE(cm.at(7) == "seven");
  REQUIRE_THROWS_AS(cm.at(8), std::out_of_range);
}

TEMPLATE_LIST_TEST_CASE("iteration visits every element exactly once",
                        "[basic]",
                        maplab_tests::all_configs) {
  maplab::flat_map<int, int, maplab::default_hash, std::equal_to<>, TestType> m;
  constexpr int n = 5000;
  for (int i = 0; i < n; ++i) m.try_emplace(i, i * 3);

  std::vector<int> seen;
  seen.reserve(n);
  for (const auto& [k, v] : m) {
    REQUIRE(v == k * 3);
    seen.push_back(k);
  }
  std::sort(seen.begin(), seen.end());
  REQUIRE(seen.size() == static_cast<std::size_t>(n));
  REQUIRE(std::adjacent_find(seen.begin(), seen.end()) == seen.end());  // no duplicates
  REQUIRE(seen.front() == 0);
  REQUIRE(seen.back() == n - 1);

  // Erasing half of them and iterating again must skip exactly the tombstones.
  for (int i = 0; i < n; i += 2) m.erase(i);
  std::size_t count = 0;
  for (const auto& [k, v] : m) {
    REQUIRE(k % 2 == 1);
    REQUIRE(v == k * 3);
    ++count;
  }
  REQUIRE(count == m.size());
  REQUIRE(count == static_cast<std::size_t>(n / 2));
}

TEMPLATE_LIST_TEST_CASE("erase(iterator) returns the next element",
                        "[basic]",
                        maplab_tests::all_configs) {
  maplab::flat_map<int, int, maplab::default_hash, std::equal_to<>, TestType> m;
  for (int i = 0; i < 200; ++i) m.try_emplace(i, i);
  auto it = m.begin();
  std::size_t erased = 0;
  while (it != m.end()) {
    it = m.erase(it);
    ++erased;
  }
  REQUIRE(erased == 200);
  REQUIRE(m.empty());
  REQUIRE(m.begin() == m.end());
}

TEMPLATE_LIST_TEST_CASE("copy, move, swap and assignment", "[basic]", maplab_tests::all_configs) {
  map_t<TestType> a;
  for (int i = 0; i < 100; ++i) a.try_emplace(i, std::to_string(i));

  map_t<TestType> b(a);
  REQUIRE(b.size() == a.size());
  for (int i = 0; i < 100; ++i) REQUIRE(b.at(i) == std::to_string(i));
  b[1000] = "extra";
  REQUIRE_FALSE(a.contains(1000));  // deep copy, not a shared buffer

  map_t<TestType> c(std::move(b));
  REQUIRE(c.size() == 101);
  REQUIRE(b.empty());          // NOLINT(bugprone-use-after-move) -- moved-from is empty
  REQUIRE(b.capacity() == 0);  // and has released its allocation
  b.try_emplace(1, "usable");  // and is still usable
  REQUIRE(b.size() == 1);

  map_t<TestType> d;
  d = c;
  REQUIRE(d.size() == c.size());
  d = std::move(c);
  REQUIRE(d.size() == 101);

  a.swap(d);
  REQUIRE(a.size() == 101);
  REQUIRE(d.size() == 100);
  swap(a, d);
  REQUIRE(a.size() == 100);

  // Self-assignment must not destroy the map.
  auto& ref = a;
  a = ref;
  REQUIRE(a.size() == 100);
}

TEMPLATE_LIST_TEST_CASE("reserve pre-sizes so a known-size burst never rehashes",
                        "[basic]",
                        maplab_tests::all_configs) {
  maplab::flat_map<int, int, maplab::default_hash, std::equal_to<>, TestType> m;
  constexpr std::size_t n = 10000;
  m.reserve(n);
  const std::size_t cap = m.capacity();
  REQUIRE(cap >= n);
  for (std::size_t i = 0; i < n; ++i) m.try_emplace(static_cast<int>(i), 0);
  REQUIRE(m.capacity() == cap);  // no rehash happened
  REQUIRE(m.size() == n);
  // One more than reserved is allowed to grow; reserve promises n, not n+1.
  REQUIRE(m.load_factor() <= m.max_load_factor());
}

TEMPLATE_LIST_TEST_CASE("clear keeps the allocation, rehash releases it",
                        "[basic]",
                        maplab_tests::all_configs) {
  maplab::flat_map<int, int, maplab::default_hash, std::equal_to<>, TestType> m;
  for (int i = 0; i < 1000; ++i) m.try_emplace(i, i);
  const std::size_t cap = m.capacity();
  m.clear();
  REQUIRE(m.empty());
  REQUIRE(m.capacity() == cap);
  REQUIRE(m.tombstones() == 0);  // clear resets tombstones, unlike erase
  m.rehash(0);
  REQUIRE(m.capacity() < cap);
  REQUIRE(m.empty());
  // rehash(0) on an empty table releases the allocation outright rather than shrinking
  // to a minimum-size one that nothing would ever free.
  REQUIRE(m.capacity() == 0);
  REQUIRE(m.memory_usage() == 0);
  m.try_emplace(1, 1);  // and the table is immediately usable again
  REQUIRE(m.size() == 1);
}

TEST_CASE("construction from a range and an initializer_list", "[basic]") {
  const std::vector<std::pair<int, int>> src{{1, 10}, {2, 20}, {3, 30}, {1, 99}};
  maplab::flat_map<int, int> a(src.begin(), src.end());
  REQUIRE(a.size() == 3);
  REQUIRE(a.at(1) == 10);  // first wins

  maplab::flat_map<int, int> b{{1, 10}, {2, 20}};
  REQUIRE(b.size() == 2);
  b.insert({{3, 30}, {4, 40}});
  REQUIRE(b.size() == 4);

  maplab::flat_map<int, int> c(1000);
  REQUIRE(c.capacity() >= 1000);
}

TEST_CASE("the growth ceiling is never exceeded", "[basic]") {
  maplab::flat_map<std::uint64_t, std::uint64_t> m;
  for (std::uint64_t i = 0; i < 200000; ++i) {
    m.try_emplace(i, i);
    if (i % 1024 == 0) REQUIRE(m.load_factor() <= m.max_load_factor());
  }
  REQUIRE(m.load_factor() <= m.max_load_factor());
  // Capacity is 2^k - 1 at every size, which is what makes masking legal.
  const std::size_t cap = m.capacity();
  REQUIRE(std::has_single_bit(cap + 1));
}
