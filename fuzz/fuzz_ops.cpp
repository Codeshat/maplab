// Differential fuzzing of the operation sequence.
//
// The Catch2 differential test drives random operations from a fixed seed, which covers a
// lot of ground but only the ground a uniform distribution happens to reach. libFuzzer
// drives the same comparison from *coverage feedback*, so it deliberately hunts for the
// input that takes the branch nothing else took -- the rehash that lands mid-probe-chain,
// the erase that empties the last slot of a run, the reinsert into a tombstone three
// groups away.
//
// Build and run:
//   CC=clang-19 CXX=clang++-19 cmake --preset fuzz
//   cmake --build --preset fuzz
//   ./build/fuzz/bin/fuzz_ops -max_total_time=60 -print_final_stats=1
#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include "maplab/flat_map.hpp"

namespace {

// A tiny key space relative to the operation count, so collisions, tombstone reuse and
// repeated growth are the common case rather than the rare one.
constexpr std::uint16_t key_mask = 0x01FF;

class byte_reader {
 public:
  byte_reader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

  bool done() const { return pos_ >= size_; }

  std::uint8_t u8() { return pos_ < size_ ? data_[pos_++] : 0; }

  std::uint16_t u16() {
    const auto lo = static_cast<std::uint16_t>(u8());
    return static_cast<std::uint16_t>(lo | (static_cast<std::uint16_t>(u8()) << 8U));
  }

 private:
  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t pos_ = 0;
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > 8192) return 0;  // keep each case fast so the corpus stays wide

  byte_reader in(data, size);
  maplab::flat_map<std::uint32_t, std::uint32_t> subject;
  std::unordered_map<std::uint32_t, std::uint32_t> reference;

  while (!in.done()) {
    const std::uint8_t op = in.u8();
    const std::uint32_t key = in.u16() & key_mask;
    const std::uint32_t value = in.u16();

    switch (op % 8) {
      case 0:
      case 1:
        if (subject.insert({key, value}).second != reference.insert({key, value}).second) {
          __builtin_trap();
        }
        break;
      case 2:
        if (subject.try_emplace(key, value).second != reference.try_emplace(key, value).second) {
          __builtin_trap();
        }
        break;
      case 3:
        subject.insert_or_assign(key, value);
        reference.insert_or_assign(key, value);
        break;
      case 4:
      case 5:
        if (subject.erase(key) != reference.erase(key)) __builtin_trap();
        break;
      case 6:
        subject.reserve(subject.size() + (value & 0xFFU));
        break;
      default: {
        const auto a = subject.find(key);
        const auto b = reference.find(key);
        if ((a == subject.end()) != (b == reference.end())) __builtin_trap();
        if (a != subject.end() && a->second != b->second) __builtin_trap();
        break;
      }
    }

    if (subject.size() != reference.size()) __builtin_trap();
  }

  // Full agreement, in both directions, plus the accounting identity the insert path
  // depends on.
  for (const auto& [k, v] : reference) {
    const auto it = subject.find(k);
    if (it == subject.end() || it->second != v) __builtin_trap();
  }
  std::size_t seen = 0;
  for (const auto& [k, v] : subject) {
    const auto it = reference.find(k);
    if (it == reference.end() || it->second != v) __builtin_trap();
    ++seen;
  }
  if (seen != subject.size()) __builtin_trap();
  if (subject.capacity() != 0 && subject.size() + subject.tombstones() + subject.growth_left() !=
                                     subject.capacity() / 8 * 7 + subject.capacity() % 8 * 7 / 8) {
    __builtin_trap();
  }
  return 0;
}
