// Shared workload and key generation for tests, benchmarks and experiments.
//
// This is scaffolding, not the library: it allocates freely and is allowed to throw.
// It lives in one place so that a benchmark and the experiment that explains it are
// provably measuring the same keys, and so every number in RESULTS.md can be reproduced
// from a seed.
#ifndef MAPLAB_WORKLOADS_HPP
#define MAPLAB_WORKLOADS_HPP

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

namespace maplab_workloads {

// splitmix64: tiny, deterministic across platforms and compilers, and good enough that a
// weak generator can never be the explanation for a surprising graph.
class splitmix64 {
 public:
  explicit constexpr splitmix64(std::uint64_t seed) noexcept : state_(seed) {}

  constexpr std::uint64_t operator()() noexcept {
    std::uint64_t z = (state_ += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31U);
  }

  constexpr std::uint64_t bounded(std::uint64_t n) noexcept {
    // Lemire's multiply-shift. Slight modulo bias, irrelevant at these scales and much
    // cheaper than rejection sampling inside a benchmark's setup loop.
    return static_cast<std::uint64_t>((static_cast<__uint128_t>(operator()()) * n) >> 64U);
  }

 private:
  std::uint64_t state_;
};

enum class key_pattern {
  random,      // uniform 64-bit values: the friendly case
  sequential,  // 0, 1, 2, ... : what order IDs and row IDs actually look like
  strided,     // 0, 4096, 8192, ...: aligned IDs, the classic low-entropy adversary
};

inline const char* name_of(key_pattern p) noexcept {
  switch (p) {
    case key_pattern::random:
      return "random";
    case key_pattern::sequential:
      return "sequential";
    case key_pattern::strided:
      return "strided";
  }
  return "?";
}

// n distinct keys. `random` is deduplicated by construction (splitmix64 is a bijection
// over 64 bits, so distinct counters give distinct outputs).
inline std::vector<std::uint64_t> make_keys(std::size_t n,
                                            key_pattern pattern,
                                            std::uint64_t seed = 0xC0FFEE) {
  std::vector<std::uint64_t> keys;
  keys.reserve(n);
  switch (pattern) {
    case key_pattern::random: {
      splitmix64 rng(seed);
      for (std::size_t i = 0; i < n; ++i) keys.push_back(rng());
      break;
    }
    case key_pattern::sequential:
      for (std::size_t i = 0; i < n; ++i) keys.push_back(i);
      break;
    case key_pattern::strided:
      for (std::size_t i = 0; i < n; ++i) keys.push_back(i << 12U);
      break;
  }
  return keys;
}

// Keys guaranteed absent from make_keys(n, pattern, seed), for the miss benchmarks. The
// guarantee matters: a "miss" benchmark that accidentally hits 1% of the time measures
// something else entirely.
inline std::vector<std::uint64_t> make_absent_keys(std::size_t n,
                                                   key_pattern pattern,
                                                   std::size_t resident,
                                                   std::uint64_t seed = 0xC0FFEE) {
  std::vector<std::uint64_t> keys;
  keys.reserve(n);
  switch (pattern) {
    case key_pattern::random: {
      // A different seed stream; collision with the resident set has probability ~n*m/2^64.
      splitmix64 rng(seed ^ 0x5DEECE66DULL);
      for (std::size_t i = 0; i < n; ++i) keys.push_back(rng() | 1ULL << 63U);
      break;
    }
    case key_pattern::sequential:
      for (std::size_t i = 0; i < n; ++i) keys.push_back(resident + i);
      break;
    case key_pattern::strided:
      for (std::size_t i = 0; i < n; ++i) {
        keys.push_back((resident + i) << 12U);
      }
      break;
  }
  return keys;
}

// Shuffle a key array once, then walk it sequentially.
//
// This replaces the more obvious "index through a random permutation array" because that
// costs a second random memory access per lookup -- at 16M elements, the permutation
// array and the key array are each larger than L3, so two thirds of the measured cache
// misses belonged to the harness rather than to the table. Shuffling the keys themselves
// keeps the *table* access uniformly random (which is what we are measuring) while making
// the *key* access sequential and therefore prefetchable (which we are not).
//
// The shuffle matters even though the keys are already random: for structured key
// patterns, probing in generation order would walk consecutive table slots and let the
// prefetcher hide precisely the clustering that Experiment 3 exists to expose.
template<class T>
inline void shuffle_in_place(std::vector<T>& v, std::uint64_t seed) {
  splitmix64 rng(seed);
  for (std::size_t i = v.size(); i > 1; --i) {
    const auto j = rng.bounded(i);
    std::swap(v[i - 1], v[j]);
  }
}

template<class T>
inline std::vector<T> shuffled(std::vector<T> v, std::uint64_t seed = 0xBEEF) {
  shuffle_in_place(v, seed);
  return v;
}

// Short strings that fit libstdc++'s 15-byte SSO buffer: no heap indirection, so the
// comparison cost is real but the pointer chase is not.
inline std::vector<std::string> make_short_strings(std::size_t n, std::uint64_t seed = 0xC0FFEE) {
  static constexpr char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  std::vector<std::string> out;
  out.reserve(n);
  splitmix64 rng(seed);
  for (std::size_t i = 0; i < n; ++i) {
    std::string s;
    // 8 random chars plus the index: random-looking, but distinct by construction.
    for (int k = 0; k < 8; ++k) s.push_back(alphabet[rng.bounded(sizeof(alphabet) - 1)]);
    s += std::to_string(i % 1000000);
    s.resize(15, 'x');
    out.push_back(std::move(s));
  }
  return out;
}

// Long strings that force a heap allocation and a memcmp long enough to dominate.
inline std::vector<std::string> make_long_strings(std::size_t n,
                                                  std::size_t len = 64,
                                                  std::uint64_t seed = 0xC0FFEE) {
  static constexpr char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  std::vector<std::string> out;
  out.reserve(n);
  splitmix64 rng(seed);
  for (std::size_t i = 0; i < n; ++i) {
    std::string s;
    s.reserve(len);
    // A shared 32-byte prefix, because real long keys (URLs, paths, FIX tags) share
    // prefixes and that is what makes memcmp expensive rather than cheap.
    for (int k = 0; k < 32; ++k) s.push_back(alphabet[static_cast<std::size_t>(k) % 26]);
    while (s.size() < len) s.push_back(alphabet[rng.bounded(sizeof(alphabet) - 1)]);
    s += std::to_string(i);
    out.push_back(std::move(s));
  }
  return out;
}

// A realistic symbol universe: ~2k tickers, 1-5 uppercase characters, the sort of table a
// market-data process keeps hot for its entire life.
inline std::vector<std::string> make_symbols(std::size_t n = 2000, std::uint64_t seed = 0x5EED) {
  std::vector<std::string> out;
  out.reserve(n);
  splitmix64 rng(seed);
  for (std::size_t i = 0; i < n; ++i) {
    std::string s;
    const std::uint64_t len = 1 + rng.bounded(5);
    for (std::uint64_t k = 0; k < len; ++k) {
      s.push_back(static_cast<char>('A' + rng.bounded(26)));
    }
    s += std::to_string(i);  // keeps them distinct without changing the shape
    out.push_back(std::move(s));
  }
  return out;
}

// An order-book-shaped event stream: new orders arrive with monotonically increasing IDs
// and are erased when they fill, so the table's *size* is stable while its contents churn
// completely. This is the workload that makes tombstones matter.
struct order_event {
  std::uint64_t id;
  bool insert;  // false => erase
};

inline std::vector<order_event> make_order_flow(std::size_t working_set,
                                                std::size_t events,
                                                std::uint64_t seed = 0x0DDE12) {
  std::vector<order_event> out;
  out.reserve(events + working_set);
  std::vector<std::uint64_t> live;
  live.reserve(working_set * 2);
  splitmix64 rng(seed);
  std::uint64_t next_id = 1;

  for (std::size_t i = 0; i < working_set; ++i) {
    out.push_back({next_id, true});
    live.push_back(next_id);
    ++next_id;
  }
  for (std::size_t i = 0; i < events; ++i) {
    if (live.empty() || rng.bounded(2) == 0) {
      out.push_back({next_id, true});
      live.push_back(next_id);
      ++next_id;
    } else {
      // Fills are not FIFO: pick a live order at random, which is what actually
      // scatters the erases across the table.
      const auto j = rng.bounded(live.size());
      out.push_back({live[j], false});
      live[j] = live.back();
      live.pop_back();
    }
  }
  return out;
}

}  // namespace maplab_workloads

#endif  // MAPLAB_WORKLOADS_HPP
