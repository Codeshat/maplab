// maplab: hash functions.
//
// A power-of-two capacity indexed by masking means the table only ever looks at the
// *low* bits of the hash (after H2 is split off). That makes the mixer part of the
// data structure, not a detail of the user's key type: `std::hash<int>` is the
// identity on libstdc++ and libc++, and identity + mask is a documented catastrophe.
// We therefore mix by default, and ship `identity_hash` so the catastrophe can be
// measured rather than asserted (see experiments/exp_hash_quality.cpp).
#ifndef MAPLAB_HASH_HPP
#define MAPLAB_HASH_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>

namespace maplab {

// maplab targets 64-bit x86-64 (see the SSE2 requirement in CMakeLists.txt), so the hash
// space and std::size_t are the same width and the hashers can return their mixer output
// directly rather than truncating it.
static_assert(sizeof(std::size_t) == sizeof(std::uint64_t), "maplab assumes a 64-bit std::size_t");

namespace detail {

// MurmurHash3's 64-bit finalizer. Five lines, ~1.5 ns, and it passes avalanche:
// flipping any input bit flips each output bit with probability ~1/2.
constexpr std::uint64_t fmix64(std::uint64_t k) noexcept {
  k ^= k >> 33U;
  k *= 0xff51afd7ed558ccdULL;
  k ^= k >> 33U;
  k *= 0xc4ceb9fe1a85ec53ULL;
  k ^= k >> 33U;
  return k;
}

// wyhash's mixing primitive: a 64x64->128 multiply folded down to 64 bits. One
// `mulx` on x86-64, and stronger per-cycle than fmix64's shift/multiply chain.
inline std::uint64_t wymix(std::uint64_t a, std::uint64_t b) noexcept {
  const __uint128_t r = static_cast<__uint128_t>(a) * b;
  return static_cast<std::uint64_t>(r) ^ static_cast<std::uint64_t>(r >> 64U);
}

inline constexpr std::uint64_t wy_secret_0 = 0xa0761d6478bd642fULL;
inline constexpr std::uint64_t wy_secret_1 = 0xe7037ed1a0b428dbULL;
inline constexpr std::uint64_t wy_secret_2 = 0x8ebc6af09c88c6e3ULL;
inline constexpr std::uint64_t wy_secret_3 = 0x589965cc75374cc3ULL;

inline std::uint64_t read_u64(const std::byte* p) noexcept {
  std::uint64_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

inline std::uint64_t read_u32(const std::byte* p) noexcept {
  std::uint32_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

// Bytes 0..3 of a short run, read with two overlapping loads so that any length in
// [1,3] costs the same three loads and no branch per byte.
inline std::uint64_t read_short(const std::byte* p, std::size_t n) noexcept {
  return (static_cast<std::uint64_t>(std::to_integer<unsigned>(p[0])) << 16U) |
         (static_cast<std::uint64_t>(std::to_integer<unsigned>(p[n >> 1U])) << 8U) |
         static_cast<std::uint64_t>(std::to_integer<unsigned>(p[n - 1]));
}

// wyhash v4 (final), condensed. Chosen over fmix64-over-bytes because string keys are
// one of the workloads and this is where the difference actually shows up.
inline std::uint64_t wyhash_bytes(const void* key,
                                  std::size_t len,
                                  std::uint64_t seed = wy_secret_0) noexcept {
  const auto* p = static_cast<const std::byte*>(key);
  seed ^= wymix(seed ^ wy_secret_0, wy_secret_1);
  std::uint64_t a = 0;
  std::uint64_t b = 0;

  if (len <= 16) [[likely]] {
    if (len >= 4) {
      a = (read_u32(p) << 32U) | read_u32(p + ((len >> 3U) << 2U));
      b = (read_u32(p + len - 4) << 32U) | read_u32(p + len - 4 - ((len >> 3U) << 2U));
    } else if (len > 0) {
      a = read_short(p, len);
      b = 0;
    }
  } else {
    std::size_t i = len;
    if (i > 48) {
      std::uint64_t see1 = seed;
      std::uint64_t see2 = seed;
      do {
        seed = wymix(read_u64(p) ^ wy_secret_1, read_u64(p + 8) ^ seed);
        see1 = wymix(read_u64(p + 16) ^ wy_secret_2, read_u64(p + 24) ^ see1);
        see2 = wymix(read_u64(p + 32) ^ wy_secret_3, read_u64(p + 40) ^ see2);
        p += 48;
        i -= 48;
      } while (i > 48);
      seed ^= see1 ^ see2;
    }
    while (i > 16) {
      seed = wymix(read_u64(p) ^ wy_secret_1, read_u64(p + 8) ^ seed);
      i -= 16;
      p += 16;
    }
    a = read_u64(p + i - 16);
    b = read_u64(p + i - 8);
  }
  a ^= wy_secret_1;
  b ^= seed;
  return wymix(a ^ wy_secret_0 ^ len, b);
}

template<class T>
concept string_like = std::is_convertible_v<const T&, std::string_view>;

// Written as a requires-expression on purpose: std::hash's primary template exists for
// every type but is unusable for most of them, and only a requires-expression turns that
// into "this overload does not apply" instead of a hard error inside a concept check.
template<class T>
concept std_hashable = requires(const T& v) {
  { std::hash<T>{}(v) } -> std::convertible_to<std::size_t>;
};

}  // namespace detail

// --------------------------------------------------------------------------------------
// Hashers
// --------------------------------------------------------------------------------------

// The adversary. Exactly what libstdc++/libc++ `std::hash` does for integers, and
// exactly what you must not combine with power-of-two masking. Kept in the public
// header so Experiment 3 is a template argument rather than a patch.
struct identity_hash {
  using is_transparent = void;

  template<class T>
    requires std::is_integral_v<T> || std::is_enum_v<T>
  constexpr std::size_t operator()(T v) const noexcept {
    return static_cast<std::uint64_t>(v);
  }
};

// MurmurHash3 finalizer over the raw integer bits. Cheapest respectable choice.
struct fmix_hash {
  using is_transparent = void;

  template<class T>
    requires std::is_integral_v<T> || std::is_enum_v<T>
  constexpr std::size_t operator()(T v) const noexcept {
    return detail::fmix64(static_cast<std::uint64_t>(v));
  }

  template<detail::string_like T>
  std::size_t operator()(const T& s) const noexcept {
    const std::string_view sv{s};
    // Deliberately the same byte hash as wyhash_hash: this hasher exists to isolate
    // *integer* mixing strength, so strings must not be a confounding variable.
    return detail::wyhash_bytes(sv.data(), sv.size());
  }
};

// wyhash for everything. One 64x64->128 multiply for integers.
struct wyhash_hash {
  using is_transparent = void;

  template<class T>
    requires std::is_integral_v<T> || std::is_enum_v<T>
  std::size_t operator()(T v) const noexcept {
    return detail::wymix(static_cast<std::uint64_t>(v) ^ detail::wy_secret_1, detail::wy_secret_0);
  }

  template<detail::string_like T>
  std::size_t operator()(const T& s) const noexcept {
    const std::string_view sv{s};
    return detail::wyhash_bytes(sv.data(), sv.size());
  }
};

// The default. Transparent (so `map<std::string, V>::find(std::string_view)` works with
// no temporary), avalanching for integers, wyhash for bytes, and `std::hash` run through
// fmix64 for anything else so that a user's weak `std::hash` specialisation still cannot
// cluster the table.
struct default_hash {
  using is_transparent = void;

  template<class T>
    requires std::is_integral_v<T> || std::is_enum_v<T>
  std::size_t operator()(T v) const noexcept {
    return wyhash_hash{}(v);
  }

  // Constrained away from character pointers, which the string overload owns; without
  // this, `default_hash{}("literal")` is ambiguous rather than obvious.
  template<class T>
    requires(!detail::string_like<T*>)
  std::size_t operator()(T* p) const noexcept {
    return detail::fmix64(reinterpret_cast<std::uintptr_t>(p));
  }

  template<detail::string_like T>
  std::size_t operator()(const T& s) const noexcept {
    const std::string_view sv{s};
    return detail::wyhash_bytes(sv.data(), sv.size());
  }

  template<class T>
    requires(!std::is_integral_v<T> && !std::is_enum_v<T> && !detail::string_like<T> &&
             !std::is_pointer_v<T> && detail::std_hashable<T>)
  std::size_t operator()(const T& v) const {
    return detail::fmix64(std::hash<T>{}(v));
  }
};

}  // namespace maplab

#endif  // MAPLAB_HASH_HPP
