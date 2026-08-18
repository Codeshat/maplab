// Slots are raw storage, so element lifetimes are entirely the map's responsibility.
// ASan sees a use-after-free through a raw byte buffer only sometimes; a type that counts
// its own constructions and destructions sees it always.
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include "configs.hpp"
#include "maplab/flat_map.hpp"

namespace {

struct counters {
  int default_ctor = 0;
  int value_ctor = 0;
  int copy_ctor = 0;
  int move_ctor = 0;
  int copy_assign = 0;
  int move_assign = 0;
  int dtor = 0;

  int live() const { return default_ctor + value_ctor + copy_ctor + move_ctor - dtor; }

  void reset() { *this = counters{}; }
};

counters g;

// Nothrow-movable, as flat_map's static_assert requires, and loud about every transition.
struct tracked {
  std::uint64_t v = 0;

  tracked() { ++g.default_ctor; }

  explicit tracked(std::uint64_t x) : v(x) { ++g.value_ctor; }

  tracked(const tracked& o) : v(o.v) { ++g.copy_ctor; }

  tracked(tracked&& o) noexcept : v(o.v) {
    o.v = ~std::uint64_t{0};  // poison, so reading a moved-from slot is visible
    ++g.move_ctor;
  }

  tracked& operator=(const tracked& o) {
    v = o.v;
    ++g.copy_assign;
    return *this;
  }

  tracked& operator=(tracked&& o) noexcept {
    v = o.v;
    o.v = ~std::uint64_t{0};
    ++g.move_assign;
    return *this;
  }

  ~tracked() { ++g.dtor; }

  friend bool operator==(const tracked& a, const tracked& b) { return a.v == b.v; }
};

struct tracked_hash {
  using is_transparent = void;

  std::size_t operator()(const tracked& t) const noexcept { return maplab::default_hash{}(t.v); }
};

}  // namespace

TEMPLATE_LIST_TEST_CASE("every constructed element is destroyed exactly once",
                        "[lifetime]",
                        maplab_tests::all_configs) {
  g.reset();
  {
    maplab::flat_map<tracked, tracked, tracked_hash, std::equal_to<>, TestType> m;
    for (std::uint64_t i = 0; i < 3000; ++i) {
      m.try_emplace(tracked{i}, tracked{i * 2});
    }
    REQUIRE(m.size() == 3000);
    // Growth relocates elements; the count of live objects still equals the map size
    // plus whatever temporaries the test itself is holding (none, here).
    REQUIRE(g.live() == 2 * 3000);

    for (std::uint64_t i = 0; i < 3000; i += 3) m.erase(tracked{i});
    REQUIRE(g.live() == 2 * static_cast<int>(m.size()));

    auto copy = m;
    REQUIRE(g.live() == 4 * static_cast<int>(m.size()));
    copy.clear();
    REQUIRE(g.live() == 2 * static_cast<int>(m.size()));
  }
  REQUIRE(g.live() == 0);  // the destructor cleaned up everything it created
}

TEMPLATE_LIST_TEST_CASE("growth moves elements, it never copies them",
                        "[lifetime]",
                        maplab_tests::all_configs) {
  g.reset();
  {
    maplab::flat_map<std::uint64_t, tracked, maplab::default_hash, std::equal_to<>, TestType> m;
    for (std::uint64_t i = 0; i < 5000; ++i) m.try_emplace(i, tracked{i});
    const int copies = g.copy_ctor + g.copy_assign;
    REQUIRE(copies == 0);
    REQUIRE(g.move_ctor > 5000);  // at least one relocation per growth generation
    for (std::uint64_t i = 0; i < 5000; ++i) REQUIRE(m.at(i).v == i);
  }
  REQUIRE(g.live() == 0);
}

TEMPLATE_LIST_TEST_CASE("clear destroys elements without freeing storage",
                        "[lifetime]",
                        maplab_tests::all_configs) {
  g.reset();
  {
    maplab::flat_map<std::uint64_t, tracked, maplab::default_hash, std::equal_to<>, TestType> m;
    for (std::uint64_t i = 0; i < 1000; ++i) m.try_emplace(i, tracked{i});
    REQUIRE(g.live() == 1000);
    m.clear();
    REQUIRE(g.live() == 0);
    REQUIRE(m.capacity() != 0);
    // Reusing the cleared storage must construct fresh objects, not resurrect old ones.
    for (std::uint64_t i = 0; i < 1000; ++i) m.try_emplace(i, tracked{i + 100});
    REQUIRE(g.live() == 1000);
    for (std::uint64_t i = 0; i < 1000; ++i) REQUIRE(m.at(i).v == i + 100);
  }
  REQUIRE(g.live() == 0);
}

TEST_CASE("move-only mapped types work end to end", "[lifetime]") {
  maplab::flat_map<int, std::unique_ptr<int>> m;
  for (int i = 0; i < 2000; ++i) m.try_emplace(i, std::make_unique<int>(i));
  REQUIRE(m.size() == 2000);
  for (int i = 0; i < 2000; ++i) REQUIRE(*m.at(i) == i);

  auto moved = std::move(m);
  REQUIRE(moved.size() == 2000);
  for (int i = 0; i < 2000; ++i) REQUIRE(*moved.at(i) == i);
  moved.erase(5);
  REQUIRE_FALSE(moved.contains(5));
}

TEST_CASE("a string-keyed table does not leak on growth", "[lifetime]") {
  // std::string is the practical case for the union's `mutable_value` view: without it,
  // every rehash would deep-copy every key instead of moving it.
  maplab::flat_map<std::string, std::string> m;
  for (int i = 0; i < 20000; ++i) {
    m.try_emplace("key-that-is-long-enough-to-heap-allocate-" + std::to_string(i),
                  "value-" + std::to_string(i));
  }
  REQUIRE(m.size() == 20000);
  for (int i = 0; i < 20000; i += 97) {
    REQUIRE(m.at("key-that-is-long-enough-to-heap-allocate-" + std::to_string(i)) ==
            "value-" + std::to_string(i));
  }
}

namespace {

int throw_after = -1;

struct throwing {
  std::uint64_t v = 0;

  explicit throwing(std::uint64_t x) : v(x) {
    if (throw_after == 0) throw std::runtime_error("boom");
    if (throw_after > 0) --throw_after;
    ++g.value_ctor;
  }

  throwing(const throwing& o) : v(o.v) {
    if (throw_after == 0) throw std::runtime_error("boom");
    if (throw_after > 0) --throw_after;
    ++g.copy_ctor;
  }

  throwing(throwing&& o) noexcept : v(o.v) { ++g.move_ctor; }

  throwing& operator=(const throwing&) = default;
  throwing& operator=(throwing&&) noexcept = default;

  ~throwing() { ++g.dtor; }
};

}  // namespace

TEST_CASE("a throwing element constructor leaves a valid table", "[lifetime]") {
  // The basic guarantee: if constructing the element throws, the control byte the insert
  // had already claimed must be given back, or the table reports a size it cannot iterate.
  g.reset();
  throw_after = -1;
  {
    maplab::flat_map<std::uint64_t, throwing> m;
    for (std::uint64_t i = 0; i < 100; ++i) m.try_emplace(i, throwing{i});
    const std::size_t before = m.size();

    // The throw has to happen *inside* the slot, after prepare_insert has already
    // written the control byte -- so pass an lvalue and make the copy the thing that
    // fails. A throwing temporary would blow up before the table was touched at all,
    // and would prove nothing.
    const throwing lvalue{999};
    throw_after = 0;
    REQUIRE_THROWS_AS(m.try_emplace(999, lvalue), std::runtime_error);
    throw_after = -1;

    REQUIRE(m.size() == before);
    REQUIRE_FALSE(m.contains(999));
    std::size_t seen = 0;
    for (const auto& kv : m) {
      REQUIRE(kv.first < 100);
      ++seen;
    }
    REQUIRE(seen == before);
    // The table still works afterwards, and the slot the failed insert had claimed is
    // available again.
    m.try_emplace(999, lvalue);
    REQUIRE(m.contains(999));
    REQUIRE(m.size() == before + 1);
  }
  REQUIRE(g.live() == 0);
}
