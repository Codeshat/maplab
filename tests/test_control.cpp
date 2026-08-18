// The control layer, tested on its own. If the group scan or the probe sequence is wrong,
// every higher-level test fails in a way that is much harder to read.
#include <algorithm>
#include <array>
#include <bitset>
#include <cstdint>
#include <set>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include "maplab/control.hpp"
#include "maplab_workloads/workloads.hpp"

using namespace maplab;

TEST_CASE("control byte predicates partition the valid alphabet", "[control]") {
  // The alphabet a table ever writes is {empty, deleted, sentinel} plus the 128
  // fingerprints. The remaining 125 byte values (-127..-3) are unreachable by
  // construction, and the fast predicates are allowed to be sloppy about them: notably
  // is_empty_or_deleted() is implemented as `c < sentinel`, which is one signed SIMD
  // compare precisely *because* it does not have to distinguish -3 from -2. This test
  // pins down the alphabet that claim depends on.
  std::vector<ctrl_t> alphabet{ctrl_empty, ctrl_deleted, ctrl_sentinel};
  for (int v = 0; v <= 127; ++v) alphabet.push_back(static_cast<ctrl_t>(v));
  REQUIRE(alphabet.size() == 131);

  int empties = 0;
  int deleted = 0;
  int sentinels = 0;
  int full = 0;
  for (const ctrl_t c : alphabet) {
    const bool e = is_empty(c);
    const bool d = is_deleted(c);
    const bool s = (c == ctrl_sentinel);
    const bool f = is_full(c);
    // Exactly one category per valid byte.
    REQUIRE((static_cast<int>(e) + static_cast<int>(d) + static_cast<int>(s) +
             static_cast<int>(f)) == 1);
    REQUIRE(is_empty_or_deleted(c) == (e || d));
    empties += static_cast<int>(e);
    deleted += static_cast<int>(d);
    sentinels += static_cast<int>(s);
    full += static_cast<int>(f);
  }
  REQUIRE(empties == 1);
  REQUIRE(deleted == 1);
  REQUIRE(sentinels == 1);
  REQUIRE(full == 128);  // the 7-bit fingerprint space

  // The three specials all have the sign bit set, which is what lets mask_full() be a
  // bare movemask with no comparison at all.
  REQUIRE(ctrl_empty < 0);
  REQUIRE(ctrl_deleted < 0);
  REQUIRE(ctrl_sentinel < 0);
  // ...and the sentinel sits above both, so `c < sentinel` selects exactly empty+deleted.
  REQUIRE(ctrl_empty < ctrl_sentinel);
  REQUIRE(ctrl_deleted < ctrl_sentinel);
}

TEST_CASE("H1 and H2 are cut from disjoint bits", "[control]") {
  maplab_workloads::splitmix64 rng(1);
  for (int i = 0; i < 10000; ++i) {
    const std::size_t h = rng();
    REQUIRE(h2(h) < 128);
    // Reassembling proves no bit is used twice and none is dropped.
    REQUIRE(((h1(h) << 7U) | h2(h)) == h);
  }
}

TEST_CASE("bit_mask iterates set bits low to high", "[control]") {
  const bit_mask<16> m{0b0000'0000'1010'0100U};
  std::vector<std::uint32_t> got;
  for (const std::uint32_t i : m) got.push_back(i);
  REQUIRE(got == std::vector<std::uint32_t>{2, 5, 7});
  REQUIRE(m.lowest_set() == 2);
  REQUIRE(m.count() == 3);
  REQUIRE(m.trailing_zeros() == 2);
  REQUIRE(m.leading_zeros() == 8);  // counted inside the 16-bit field, not the word
  REQUIRE(bit_mask<16>{0}.leading_zeros() == 16);
  REQUIRE(bit_mask<16>{0b0000'0000'0000'0111U}.trailing_ones() == 3);
  REQUIRE(bit_mask<16>{0xFFFFU}.trailing_ones() == 16);
  REQUIRE_FALSE(static_cast<bool>(bit_mask<16>{0}));
}

// The central claim of Experiment 1: the SIMD group and the scalar group are two ways to
// compute the same four bitmasks. Randomised over control arrays that contain every kind
// of byte in every position.
TEMPLATE_TEST_CASE_SIG(
    "simd and scalar groups agree on every mask", "[control][simd]", ((std::size_t W), W), 8, 16) {
  maplab_workloads::splitmix64 rng(0xA11CE);
  std::array<ctrl_t, 64> ctrl{};
  for (int trial = 0; trial < 20000; ++trial) {
    for (auto& c : ctrl) {
      switch (rng.bounded(4)) {
        case 0:
          c = ctrl_empty;
          break;
        case 1:
          c = ctrl_deleted;
          break;
        case 2:
          c = ctrl_sentinel;
          break;
        default:
          c = static_cast<ctrl_t>(rng.bounded(128));
          break;
      }
    }
    const std::size_t pos = rng.bounded(64 - W);
    const auto h = static_cast<h2_t>(rng.bounded(128));
    const sse2_group<W> simd{ctrl.data() + pos};
    const scalar_group<W> scalar{ctrl.data() + pos};

    REQUIRE(simd.match(h).bits() == scalar.match(h).bits());
    REQUIRE(simd.mask_empty().bits() == scalar.mask_empty().bits());
    REQUIRE(simd.mask_empty_or_deleted().bits() == scalar.mask_empty_or_deleted().bits());
    REQUIRE(simd.mask_full().bits() == scalar.mask_full().bits());

    // ...and the masks agree with the byte-by-byte definition, so "both wrong the same
    // way" is not an available explanation.
    std::uint32_t expect_match = 0;
    std::uint32_t expect_full = 0;
    for (std::size_t i = 0; i < W; ++i) {
      const ctrl_t c = ctrl[pos + i];
      expect_match |= static_cast<std::uint32_t>(c == static_cast<ctrl_t>(h)) << i;
      expect_full |= static_cast<std::uint32_t>(is_full(c)) << i;
    }
    REQUIRE(simd.match(h).bits() == expect_match);
    REQUIRE(simd.mask_full().bits() == expect_full);
  }
}

// The triangular-probing guarantee, checked rather than asserted: with a power-of-two
// number of positions, the sequence visits every group exactly once, so probing always
// terminates and always terminates having examined every slot.
TEMPLATE_TEST_CASE_SIG("triangular probing visits every group exactly once",
                       "[control]",
                       ((std::size_t W), W),
                       8,
                       16) {
  for (std::size_t k = 4; k <= 12; ++k) {
    const std::size_t positions = std::size_t{1} << k;
    const std::size_t mask = positions - 1;  // == capacity
    const std::size_t groups = positions / W;

    for (std::size_t start : {std::size_t{0}, std::size_t{1}, mask, positions / 3}) {
      probe_seq<W> seq{start, mask};
      std::set<std::size_t> seen;
      std::vector<bool> covered(positions, false);
      for (std::size_t i = 0; i < groups; ++i) {
        const std::size_t off = seq.offset();
        REQUIRE(seen.insert(off).second);  // never revisits a group
        for (std::size_t j = 0; j < W; ++j) covered[(off + j) & mask] = true;
        seq.next();
      }
      REQUIRE(seen.size() == groups);
      REQUIRE(std::all_of(covered.begin(), covered.end(), [](bool b) { return b; }));

      // After a full cycle the sequence lands half the table away rather than back at
      // the start: W * T_g = positions * (g+1) / 2 with g a power of two, so g+1 is odd
      // and the residue is positions/2. The full period is 2g, but every slot has
      // already been examined after g steps, which is the property that matters -- the
      // probe loop always exits before then, because the load factor guarantees an empty
      // byte somewhere in the table.
      if (groups >= 2) {
        REQUIRE(seq.offset() == ((start + positions / 2) & mask));
      }
    }
  }
}
