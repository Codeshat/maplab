// Experiment 7: bytes per element, measured rather than quoted.
//
// Question:  A flat map's memory story is usually told as "no node, no next pointer".
//            What is the actual number, including the control byte and the empty slots the
//            load factor requires, and how does it compare to a node-based map?
// Method:    A counting global allocator. Crucially it counts the *allocator's* cost, not
//            the requested size: on glibc a 24-byte node request occupies a 32-byte heap
//            chunk, so counting requests would credit a node-based map with 25% memory it
//            does not have. malloc_usable_size(p) + 8 is exactly the glibc chunk size.
//            A "sizeof(node) x N" calculation misses this entirely.
// Expect:    maplab should sit near (sizeof(pair) + 1) / load_factor. std::unordered_map
//            should be roughly twice that: a node holds the pair plus a next pointer, each
//            node is a separate allocation with its own header, and the bucket array is
//            additional.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

#if defined(__GLIBC__)
#include <malloc.h>
#define MAPLAB_EXACT_HEAP_ACCOUNTING 1
#else
#define MAPLAB_EXACT_HEAP_ACCOUNTING 0
#endif
#include <string>
#include <unordered_map>
#include <vector>

#include <unordered_dense.h>

#include "exp_common.hpp"
#include "maplab/flat_map.hpp"
#include "maplab_workloads/workloads.hpp"

namespace {

// A process-wide allocation counter. Single-threaded by construction (the whole project
// is), so a plain global is honest here rather than merely convenient.
std::size_t g_live_bytes = 0;
bool g_counting = false;

// What one allocation actually costs the heap, as opposed to what was asked for.
std::size_t footprint(void* p, std::size_t requested) {
#if MAPLAB_EXACT_HEAP_ACCOUNTING
  // glibc stores an 8-byte size field ahead of each chunk and rounds the chunk to 16
  // bytes with a 32-byte minimum, so usable_size + 8 is the chunk size exactly.
  (void)requested;
  return malloc_usable_size(p) + sizeof(std::size_t);
#else
  (void)p;
  return requested;
#endif
}

void note_alloc(void* p, std::size_t requested) {
  if (!g_counting) return;
  g_live_bytes += footprint(p, requested);
}

void note_free(void* p, std::size_t requested) {
  if (!g_counting) return;
  g_live_bytes -= footprint(p, requested);
}

}  // namespace

void* operator new(std::size_t n) {
  void* p = std::malloc(n);
  if (p == nullptr) throw std::bad_alloc();
  note_alloc(p, n);
  return p;
}

void operator delete(void* p, std::size_t n) noexcept {
  if (p != nullptr) note_free(p, n);
  std::free(p);
}

// The unsized form has to account too: libstdc++ uses it in places, and an allocation
// counted on the way in but not on the way out turns a growth into a permanent leak in
// the numbers.
void operator delete(void* p) noexcept {
  if (p != nullptr) note_free(p, 0);
  std::free(p);
}

void* operator new(std::size_t n, std::align_val_t a) {
  const auto align = static_cast<std::size_t>(a);
  void* p = std::aligned_alloc(align, ((n + align - 1) / align) * align);
  if (p == nullptr) throw std::bad_alloc();
  note_alloc(p, n);
  return p;
}

void operator delete(void* p, std::size_t n, std::align_val_t) noexcept {
  if (p != nullptr) note_free(p, n);
  std::free(p);
}

void operator delete(void* p, std::align_val_t) noexcept {
  if (p != nullptr) note_free(p, 0);
  std::free(p);
}

namespace {

using namespace maplab_exp;

// Bytes the heap is holding on this map's behalf once `keys` are in it.
template<class Map>
std::size_t measure_bytes(const std::vector<std::uint64_t>& keys, std::size_t* elements) {
  g_live_bytes = 0;
  g_counting = true;
  std::size_t held = 0;
  {
    Map m;
    for (const auto k : keys) m.try_emplace(k, k);
    held = g_live_bytes;
    *elements = m.size();
  }
  g_counting = false;
  return held;
}

// maplab's best case: filled to exactly its growth ceiling, where none of the array is
// wasted beyond the 1/8 the load factor reserves. A table sampled at a random moment sits
// somewhere between this and twice this, because capacity doubles on growth.
std::size_t measure_at_ceiling(const std::vector<std::uint64_t>& keys, std::size_t* elements) {
  g_live_bytes = 0;
  g_counting = true;
  std::size_t held = 0;
  {
    maplab::flat_map<std::uint64_t, std::uint64_t> m;
    for (const auto k : keys) {
      m.try_emplace(k, k);
      if (m.growth_left() == 0 && m.size() > keys.size() / 2) break;
    }
    held = g_live_bytes;
    *elements = m.size();
  }
  g_counting = false;
  return held;
}

}  // namespace

int main() {
  heading("Experiment 7: memory per element");
  std::cout << "Counted with a global allocator that charges each allocation what it costs\n"
               "the heap (glibc chunk size), not what was requested. On glibc a 24-byte\n"
               "node occupies 32 bytes, so counting requests would credit a node-based map\n"
               "with 25% memory it does not have.\n";
#if !MAPLAB_EXACT_HEAP_ACCOUNTING
  std::cout << "\nNOTE: malloc_usable_size is unavailable here, so this run counts requested\n"
               "bytes and understates every per-allocation overhead.\n";
#endif
  std::cout << '\n';

  csv out(results_dir() + "/exp7_memory.csv",
          {"impl", "elements", "bytes", "bytes_per_elem", "allocations_note"});

  const console_table console(
      {{"implementation", 26}, {"elements", 10}, {"bytes", 12}, {"B/elem", 9}, {"vs maplab", 10}});

  for (const std::size_t n : {std::size_t{1} << 14, std::size_t{1} << 18, std::size_t{1} << 21}) {
    const auto keys = maplab_workloads::make_keys(n, maplab_workloads::key_pattern::random);

    std::size_t mine_n = 0;
    std::size_t theirs_n = 0;
    std::size_t dense_n = 0;
    std::size_t ceil_n = 0;
    const std::size_t mine =
        measure_bytes<maplab::flat_map<std::uint64_t, std::uint64_t>>(keys, &mine_n);
    const std::size_t dense =
        measure_bytes<ankerl::unordered_dense::map<std::uint64_t, std::uint64_t>>(keys, &dense_n);
    const std::size_t theirs =
        measure_bytes<std::unordered_map<std::uint64_t, std::uint64_t>>(keys, &theirs_n);
    const std::size_t at_ceiling = measure_at_ceiling(keys, &ceil_n);

    const auto emit =
        [&](const char* label, std::size_t bytes, std::size_t elems, const char* note) {
          const double per = static_cast<double>(bytes) / static_cast<double>(elems);
          console.row({label,
                       std::to_string(elems),
                       std::to_string(bytes),
                       fmt(per, 2),
                       fmt(static_cast<double>(bytes) / static_cast<double>(mine), 2) + "x"});
          out.row(label, elems, bytes, fmt(per, 4), note);
        };
    emit("maplab::flat_map", mine, mine_n, "after inserting n keys");
    emit("maplab (at 7/8 ceiling)", at_ceiling, ceil_n, "filled to the growth ceiling");
    emit("ankerl::unordered_dense", dense, dense_n, "after inserting n keys");
    emit("std::unordered_map", theirs, theirs_n, "after inserting n keys");
    std::cout << '\n';
  }

  std::cout << "maplab stores sizeof(pair) + 1 control byte per slot, so at its 7/8 ceiling\n"
               "the floor is (16 + 1) / 0.875 = 19.4 bytes per element -- and up to twice\n"
               "that immediately after a rehash, which is what the first row samples. That\n"
               "range is the honest statement: a power-of-two capacity means a flat table's\n"
               "density depends on where in the growth cycle you look.\n\n"
               "std::unordered_map's cost is structural instead of cyclical: a next pointer\n"
               "and a separate heap chunk per element, neither of which a load factor can\n"
               "reclaim. ankerl::unordered_dense stores its values in a dense vector with a\n"
               "separate index array, which is why it beats both here and pays for it on\n"
               "erase instead.\n";
  report_timing_noise();
  return 0;
}
