// maplab: control bytes, bit masks, and the group scan.
//
// One control byte per slot, stored in an array parallel to (and contiguous with) the
// slots. The encoding is chosen so that every predicate the probe loop needs is a single
// signed byte comparison:
//
//   0x80  (-128)  empty          sign bit set, and the only value with low bits 0
//   0xFE  (-2)    deleted        sign bit set  ("tombstone")
//   0xFF  (-1)    sentinel       sign bit set  (one byte, at ctrl[capacity], ends iteration)
//   0x00..0x7F    full, = H2     sign bit clear
//
//   is_full(c)              <=>  c >= 0                       (movemask, inverted)
//   is_empty_or_deleted(c)  <=>  c < sentinel                 (_mm_cmpgt_epi8)
//   matches fingerprint h   <=>  c == h                       (_mm_cmpeq_epi8)
//
// The scalar group below implements the *same* interface over the *same* memory layout,
// byte at a time. It is not dead code: it is the control group for Experiment 1, so the
// only difference measured there is the scan itself.
#ifndef MAPLAB_CONTROL_HPP
#define MAPLAB_CONTROL_HPP

#include <bit>
#include <cstddef>
#include <cstdint>
#include <emmintrin.h>

namespace maplab {

using ctrl_t = signed char;
using h2_t = std::uint8_t;

inline constexpr ctrl_t ctrl_empty = -128;   // 0x80
inline constexpr ctrl_t ctrl_deleted = -2;   // 0xFE
inline constexpr ctrl_t ctrl_sentinel = -1;  // 0xFF

constexpr bool is_full(ctrl_t c) noexcept {
  return c >= 0;
}

constexpr bool is_empty(ctrl_t c) noexcept {
  return c == ctrl_empty;
}

constexpr bool is_deleted(ctrl_t c) noexcept {
  return c == ctrl_deleted;
}

constexpr bool is_empty_or_deleted(ctrl_t c) noexcept {
  return c < ctrl_sentinel;
}

// H1 selects the group, H2 is the 7-bit fingerprint kept in the control byte.
// Splitting rather than reusing bits matters: if H2 were a slice of the bits that also
// choose the position, every key in a group would share a fingerprint and the filter
// would reject nothing.
constexpr std::size_t h1(std::size_t hash) noexcept {
  return hash >> 7U;
}

constexpr h2_t h2(std::size_t hash) noexcept {
  return static_cast<h2_t>(hash & 0x7FU);
}

// --------------------------------------------------------------------------------------
// bit_mask: one bit per slot in a group, iterated low bit first.
// --------------------------------------------------------------------------------------
template<std::uint32_t Width>
class bit_mask {
 public:
  explicit constexpr bit_mask(std::uint32_t mask) noexcept : mask_(mask) {}

  explicit constexpr operator bool() const noexcept { return mask_ != 0; }

  [[nodiscard]] constexpr std::uint32_t bits() const noexcept { return mask_; }

  [[nodiscard]] constexpr std::uint32_t lowest_set() const noexcept {
    return static_cast<std::uint32_t>(std::countr_zero(mask_));
  }

  [[nodiscard]] constexpr std::uint32_t trailing_zeros() const noexcept {
    return static_cast<std::uint32_t>(std::countr_zero(mask_));
  }

  // Counted within the Width-bit field, not within the 32-bit word.
  [[nodiscard]] constexpr std::uint32_t leading_zeros() const noexcept {
    return static_cast<std::uint32_t>(std::countl_zero(mask_)) - (32U - Width);
  }

  // Number of set bits before the first clear one, i.e. the length of the run of
  // matching slots starting at the group's first slot. Used by iteration to skip a whole
  // run of empty/deleted control bytes in one step.
  [[nodiscard]] constexpr std::uint32_t trailing_ones() const noexcept {
    return static_cast<std::uint32_t>(std::countr_zero(mask_ + 1));
  }

  [[nodiscard]] constexpr std::uint32_t count() const noexcept {
    return static_cast<std::uint32_t>(std::popcount(mask_));
  }

  class iterator {
   public:
    explicit constexpr iterator(std::uint32_t mask) noexcept : mask_(mask) {}

    constexpr std::uint32_t operator*() const noexcept {
      return static_cast<std::uint32_t>(std::countr_zero(mask_));
    }

    // Clear the lowest set bit: BLSR on x86, two instructions otherwise.
    constexpr iterator& operator++() noexcept {
      mask_ &= mask_ - 1;
      return *this;
    }

    friend constexpr bool operator==(iterator a, iterator b) noexcept { return a.mask_ == b.mask_; }

   private:
    std::uint32_t mask_;
  };

  constexpr iterator begin() const noexcept { return iterator{mask_}; }

  constexpr iterator end() const noexcept { return iterator{0}; }

 private:
  std::uint32_t mask_;
};

// --------------------------------------------------------------------------------------
// Probe policies. Tags, not function pointers: the group type is a template parameter so
// the scan inlines into the probe loop and the ablation measures code, not indirection.
// --------------------------------------------------------------------------------------
namespace probe_policy {

struct simd {
  static constexpr const char* name = "simd";
};

struct scalar {
  static constexpr const char* name = "scalar";
};

}  // namespace probe_policy

template<std::size_t Width>
struct sse2_group {
  static_assert(Width == 8 || Width == 16, "SSE2 groups are 8 or 16 bytes wide");
  static constexpr std::size_t width = Width;
  static constexpr std::uint32_t valid = Width == 16 ? 0xFFFFU : 0x00FFU;
  using mask_type = bit_mask<static_cast<std::uint32_t>(Width)>;
  using policy = probe_policy::simd;

  explicit sse2_group(const ctrl_t* pos) noexcept {
    if constexpr (Width == 16) {
      // Unaligned by design: probe positions are hash-derived, not group-aligned, and
      // the cloned tail group (see flat_map::set_ctrl) makes a load at capacity-1 legal.
      v_ = _mm_loadu_si128(reinterpret_cast<const __m128i*>(pos));
    } else {
      v_ = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(pos));
    }
  }

  mask_type match(h2_t h) const noexcept {
    const __m128i eq = _mm_cmpeq_epi8(_mm_set1_epi8(static_cast<char>(h)), v_);
    return mask_type{static_cast<std::uint32_t>(_mm_movemask_epi8(eq)) & valid};
  }

  mask_type mask_empty() const noexcept {
    const __m128i eq = _mm_cmpeq_epi8(_mm_set1_epi8(static_cast<char>(ctrl_empty)), v_);
    return mask_type{static_cast<std::uint32_t>(_mm_movemask_epi8(eq)) & valid};
  }

  mask_type mask_empty_or_deleted() const noexcept {
    // sentinel(-1) > c  <=>  c is empty(-128) or deleted(-2). One signed compare.
    const __m128i gt = _mm_cmpgt_epi8(_mm_set1_epi8(static_cast<char>(ctrl_sentinel)), v_);
    return mask_type{static_cast<std::uint32_t>(_mm_movemask_epi8(gt)) & valid};
  }

  mask_type mask_full() const noexcept {
    // Full is "sign bit clear", so the raw movemask inverted. No compare needed at all.
    return mask_type{~static_cast<std::uint32_t>(_mm_movemask_epi8(v_)) & valid};
  }

 private:
  __m128i v_;
};

template<std::size_t Width>
struct scalar_group {
  static_assert(Width == 8 || Width == 16, "kept parallel to sse2_group for the ablation");
  static constexpr std::size_t width = Width;
  using mask_type = bit_mask<static_cast<std::uint32_t>(Width)>;
  using policy = probe_policy::scalar;

  explicit scalar_group(const ctrl_t* pos) noexcept : p_(pos) {}

  mask_type match(h2_t h) const noexcept {
    return build([h](ctrl_t c) { return c == static_cast<ctrl_t>(h); });
  }

  mask_type mask_empty() const noexcept {
    return build([](ctrl_t c) { return is_empty(c); });
  }

  mask_type mask_empty_or_deleted() const noexcept {
    return build([](ctrl_t c) { return is_empty_or_deleted(c); });
  }

  mask_type mask_full() const noexcept {
    return build([](ctrl_t c) { return is_full(c); });
  }

 private:
  template<class Pred>
  mask_type build(Pred pred) const noexcept {
    std::uint32_t m = 0;
    for (std::size_t i = 0; i < Width; ++i) {
      m |= static_cast<std::uint32_t>(pred(p_[i]) ? 1U : 0U) << i;
    }
    return mask_type{m};
  }

  const ctrl_t* p_;
};

template<std::size_t Width, class Policy>
struct group_for;

template<std::size_t Width>
struct group_for<Width, probe_policy::simd> {
  using type = sse2_group<Width>;
};

template<std::size_t Width>
struct group_for<Width, probe_policy::scalar> {
  using type = scalar_group<Width>;
};

template<std::size_t Width, class Policy>
using group_t = typename group_for<Width, Policy>::type;

// --------------------------------------------------------------------------------------
// probe_seq: triangular probing over groups.
//
//   offset_k = (offset_0 + Width * k(k+1)/2) mod capacity
//
// With capacity a power of two and a multiple of Width, every offset in the sequence is
// congruent to offset_0 modulo Width, so the sequence walks capacity/Width distinct group
// starts indexed by the triangular numbers T_k modulo the power of two capacity/Width.
// Triangular numbers modulo 2^m form a complete residue system, so the walk visits every
// group exactly once before repeating: probing terminates, and it terminates having seen
// every slot. (Quadratic probing with an arbitrary stride gives no such guarantee, which
// is why it needs a lower load factor.)
// --------------------------------------------------------------------------------------
template<std::size_t Width>
class probe_seq {
 public:
  probe_seq(std::size_t hash, std::size_t mask) noexcept : mask_(mask), offset_(hash & mask) {}

  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

  [[nodiscard]] std::size_t offset(std::size_t i) const noexcept { return (offset_ + i) & mask_; }

  void next() noexcept {
    index_ += Width;
    offset_ = (offset_ + index_) & mask_;
  }

  // Groups consumed so far. Only read by the stats build and by tests.
  [[nodiscard]] std::size_t index() const noexcept { return index_; }

 private:
  std::size_t mask_;
  std::size_t offset_;
  std::size_t index_ = 0;
};

}  // namespace maplab

#endif  // MAPLAB_CONTROL_HPP
