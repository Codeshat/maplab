// maplab: a Swiss-table-style open-addressing hash map.
//
// Layout, in one allocation:
//
//   [ ctrl: capacity+1 bytes ][ cloned: width-1 bytes ][ pad ][ slots: capacity * slot ]
//                       ^ ctrl[capacity] = sentinel
//
// `capacity` is always 2^k - 1, and the probe mask *is* the capacity, so probe offsets
// range over [0, 2^k). Offset `capacity` lands on the sentinel byte, which is neither
// empty nor a fingerprint match, so it is simply a position no key can occupy; it costs
// one slot and buys a branch-free end to iteration.
//
// The `cloned` tail repeats ctrl[0 .. width-2] so that a group load starting anywhere in
// [0, capacity] can read `width` bytes without a wraparound branch, and a bit set at a
// cloned position maps back to the real slot by the same masking every other offset uses.
//
// A lookup is: mix the key -> H1 picks the group, H2 is a 7-bit fingerprint -> load 16
// control bytes -> one _mm_cmpeq_epi8 + _mm_movemask_epi8 gives every candidate slot in
// the group as a bitmask -> compare keys only at those positions -> if the group held any
// empty byte, the key is absent, stop. That last clause is the whole miss fast path.
#ifndef MAPLAB_FLAT_MAP_HPP
#define MAPLAB_FLAT_MAP_HPP

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <new>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

#include "maplab/config.hpp"
#include "maplab/control.hpp"
#include "maplab/hash.hpp"
#include "maplab/stats.hpp"

// An occupied control byte implies the table has storage, but the compiler cannot see
// that through the shared empty control group a default-constructed table points at, and
// GCC warns about dereferencing the null slot pointer at -O3. Telling it what we know is
// both the fix and, on the hot path, useful information.
// GCC 13 parses [[assume]] but does not yet feed it to the null-dereference analysis, so
// the __builtin_unreachable form is what actually works there. Clang's __builtin_assume
// is the equivalent. [[assume]] is kept as the portable fallback for compilers that have
// neither builtin.
#if defined(__clang__)
#define MAPLAB_ASSUME(x) __builtin_assume(x)
#elif defined(__GNUC__)
#define MAPLAB_ASSUME(x)               \
  do {                                 \
    if (!(x)) __builtin_unreachable(); \
  } while (false)
#elif defined(__has_cpp_attribute) && __has_cpp_attribute(assume) >= 202207L
#define MAPLAB_ASSUME(x) [[assume(x)]]
#else
#define MAPLAB_ASSUME(x) ((void)0)
#endif

namespace maplab {

namespace detail {

template<class H, class K>
concept hasher_for = requires(const H& h, const K& k) {
  { h(k) } -> std::convertible_to<std::size_t>;
};

template<class E, class A, class B>
concept equality_for = requires(const E& e, const A& a, const B& b) {
  { e(a, b) } -> std::convertible_to<bool>;
};

// Heterogeneous lookup is enabled exactly when both functors opt in, which is the
// standard's rule and also the safe one: a transparent hasher paired with a
// non-transparent comparator would hash `string_view` and compare `string`.
template<class H, class E>
concept transparent_pair = requires {
  typename H::is_transparent;
  typename E::is_transparent;
};

// The control array a default-constructed (capacity 0) table points at. One sentinel
// followed by empties, so `find` on an empty map takes the miss fast path on its first
// group load and never touches the (null) slot pointer. Never written to.
inline ctrl_t* empty_group() noexcept {
  alignas(16) static std::array<ctrl_t, 16> g = {
      ctrl_sentinel, ctrl_empty, ctrl_empty, ctrl_empty, ctrl_empty, ctrl_empty,
      ctrl_empty,    ctrl_empty, ctrl_empty, ctrl_empty, ctrl_empty, ctrl_empty,
      ctrl_empty,    ctrl_empty, ctrl_empty, ctrl_empty};
  return g.data();
}

// Smallest 2^k - 1 that is >= n.
constexpr std::size_t normalize_capacity(std::size_t n) noexcept {
  return n == 0 ? 0 : (~std::size_t{0} >> static_cast<unsigned>(std::countl_zero(n)));
}

template<class Slot, class Value, std::size_t Width, class Policy>
class map_iterator {
  using group = group_t<Width, Policy>;

 public:
  using iterator_category = std::forward_iterator_tag;
  using value_type = std::remove_const_t<Value>;
  using difference_type = std::ptrdiff_t;
  using pointer = Value*;
  using reference = Value&;

  map_iterator() = default;

  map_iterator(ctrl_t* c, Slot* s) noexcept : ctrl_(c), slot_(s) {}

  // iterator -> const_iterator, never the other way.
  template<class V2>
    requires(std::is_const_v<Value> && std::is_same_v<V2, std::remove_const_t<Value>>)
  map_iterator(const map_iterator<Slot, V2, Width, Policy>& other) noexcept
      : ctrl_(other.ctrl()), slot_(other.slot()) {}

  reference operator*() const noexcept { return slot_->value; }

  pointer operator->() const noexcept { return std::addressof(slot_->value); }

  map_iterator& operator++() noexcept {
    ++ctrl_;
    ++slot_;
    skip_to_full();
    return *this;
  }

  map_iterator operator++(int) noexcept {
    auto tmp = *this;
    ++*this;
    return tmp;
  }

  friend bool operator==(const map_iterator& a, const map_iterator& b) noexcept {
    return a.ctrl_ == b.ctrl_;
  }

  [[nodiscard]] ctrl_t* ctrl() const noexcept { return ctrl_; }

  [[nodiscard]] Slot* slot() const noexcept { return slot_; }

  // Advance to the next occupied slot, or to end(). Skips whole runs of empty/deleted
  // bytes a group at a time rather than one byte at a time, so iterating a table that
  // has just been drained of a million tombstones is still one pass over the control
  // array. Terminates on the sentinel, which is neither empty nor deleted.
  void skip_to_full() noexcept {
    while (is_empty_or_deleted(*ctrl_)) {
      const std::uint32_t shift = group{ctrl_}.mask_empty_or_deleted().trailing_ones();
      ctrl_ += shift;
      slot_ += shift;
    }
    if (*ctrl_ == ctrl_sentinel) {
      ctrl_ = nullptr;
      slot_ = nullptr;
    }
  }

 private:
  ctrl_t* ctrl_ = nullptr;
  Slot* slot_ = nullptr;
};

}  // namespace detail

// --------------------------------------------------------------------------------------
// flat_map
// --------------------------------------------------------------------------------------
template<class Key,
         class T,
         class Hash = default_hash,
         class KeyEqual = std::equal_to<>,
         class Config = default_config>
class flat_map {
  static constexpr std::size_t width = Config::group_width;
  static constexpr std::size_t cloned = width - 1;
  using group = group_t<width, typename Config::probe>;

 public:
  using key_type = Key;
  using mapped_type = T;
  using value_type = std::pair<const Key, T>;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;
  using hasher = Hash;
  using key_equal = KeyEqual;
  using config_type = Config;
  using reference = value_type&;
  using const_reference = const value_type&;
  using pointer = value_type*;
  using const_pointer = const value_type*;

 private:
  using mutable_value_type = std::pair<Key, T>;

  // Slots are raw storage. The map never default-constructs a slot; it constructs
  // elements with std::construct_at and destroys them with std::destroy_at, so a
  // `flat_map<int, std::mutex>` would be as legal as the element type allows and an
  // empty table costs no element constructions at all.
  //
  // The union exists for relocation. Rehashing must *move* the key, but the live member
  // is `pair<const Key, T>` whose key cannot be moved from; `mutable_value` is the same
  // object viewed without the const so growth is O(n) moves instead of O(n) copies. See
  // DESIGN.md, "Standard-conformance notes".
  union slot_type {
    value_type value;
    mutable_value_type mutable_value;

    slot_type() noexcept {}

    slot_type(const slot_type&) = delete;
    slot_type(slot_type&&) = delete;
    slot_type& operator=(const slot_type&) = delete;
    slot_type& operator=(slot_type&&) = delete;

    ~slot_type() {}
  };

  static_assert(detail::hasher_for<Hash, Key>,
                "Hash must be invocable with Key and return something convertible to "
                "std::size_t");
  static_assert(detail::equality_for<KeyEqual, Key, Key>,
                "KeyEqual must be invocable with two Keys and return something convertible "
                "to bool");
  static_assert(std::is_nothrow_move_constructible_v<mutable_value_type>,
                "maplab::flat_map requires nothrow-movable Key and T: growth relocates every "
                "element and there is nowhere to unwind to if a move throws halfway through. "
                "This is a documented scope limitation, not an oversight (see DESIGN.md). Mark "
                "your move constructor noexcept, or store a unique_ptr/shared_ptr.");
  static_assert(width == group::width);

 public:
  using iterator = detail::map_iterator<slot_type, value_type, width, typename Config::probe>;
  using const_iterator =
      detail::map_iterator<slot_type, const value_type, width, typename Config::probe>;

  static constexpr size_type npos = ~size_type{0};

  // ---- construction -------------------------------------------------------------------

  flat_map() = default;

  explicit flat_map(size_type bucket_hint,
                    const Hash& hash = Hash{},
                    const KeyEqual& equal = KeyEqual{})
      : hash_(hash), eq_(equal) {
    if (bucket_hint != 0) reserve(bucket_hint);
  }

  template<class InputIt>
  flat_map(InputIt first,
           InputIt last,
           size_type bucket_hint = 0,
           const Hash& hash = Hash{},
           const KeyEqual& equal = KeyEqual{})
      : flat_map(bucket_hint, hash, equal) {
    insert(first, last);
  }

  flat_map(std::initializer_list<value_type> init,
           size_type bucket_hint = 0,
           const Hash& hash = Hash{},
           const KeyEqual& equal = KeyEqual{})
      : flat_map(bucket_hint == 0 ? init.size() : bucket_hint, hash, equal) {
    insert(init.begin(), init.end());
  }

  flat_map(const flat_map& other) : hash_(other.hash_), eq_(other.eq_) {
    if (other.capacity_ == 0) return;
    allocate(other.capacity_);
    // Copying the control array wholesale preserves probe positions, so no element is
    // rehashed and iteration order survives the copy.
    std::memcpy(ctrl_, other.ctrl_, ctrl_bytes(other.capacity_));
    size_type constructed = 0;
    try {
      for (size_type i = 0; i != capacity_; ++i) {
        if (is_full(ctrl_[i])) {
          construct_slot(slots_ + i, other.slots_[i].value);
          ++constructed;
        }
      }
    } catch (...) {
      for (size_type i = 0; i != capacity_ && constructed != 0; ++i) {
        if (is_full(ctrl_[i])) {
          destroy_slot(slots_ + i);
          --constructed;
        }
      }
      deallocate(ctrl_, capacity_);
      ctrl_ = detail::empty_group();
      slots_ = nullptr;
      capacity_ = 0;
      throw;
    }
    size_ = other.size_;
    growth_left_ = other.growth_left_;
  }

  flat_map(flat_map&& other) noexcept
      : ctrl_(other.ctrl_),
        slots_(other.slots_),
        size_(other.size_),
        capacity_(other.capacity_),
        growth_left_(other.growth_left_),
        hash_(std::move(other.hash_)),
        eq_(std::move(other.eq_)) {
    other.ctrl_ = detail::empty_group();
    other.slots_ = nullptr;
    other.size_ = 0;
    other.capacity_ = 0;
    other.growth_left_ = 0;
  }

  flat_map& operator=(const flat_map& other) {
    if (this != &other) {
      flat_map tmp(other);
      swap(tmp);
    }
    return *this;
  }

  flat_map& operator=(flat_map&& other) noexcept {
    if (this != &other) {
      destroy_and_release();
      ctrl_ = other.ctrl_;
      slots_ = other.slots_;
      size_ = other.size_;
      capacity_ = other.capacity_;
      growth_left_ = other.growth_left_;
      hash_ = std::move(other.hash_);
      eq_ = std::move(other.eq_);
      other.ctrl_ = detail::empty_group();
      other.slots_ = nullptr;
      other.size_ = 0;
      other.capacity_ = 0;
      other.growth_left_ = 0;
    }
    return *this;
  }

  ~flat_map() { destroy_and_release(); }

  void swap(flat_map& other) noexcept {
    using std::swap;
    swap(ctrl_, other.ctrl_);
    swap(slots_, other.slots_);
    swap(size_, other.size_);
    swap(capacity_, other.capacity_);
    swap(growth_left_, other.growth_left_);
    swap(hash_, other.hash_);
    swap(eq_, other.eq_);
  }

  friend void swap(flat_map& a, flat_map& b) noexcept { a.swap(b); }

  // ---- iteration ----------------------------------------------------------------------

  iterator begin() noexcept {
    iterator it{ctrl_, slots_};
    it.skip_to_full();
    return it;
  }

  iterator end() noexcept { return iterator{}; }

  const_iterator begin() const noexcept {
    const_iterator it{ctrl_, slots_};
    it.skip_to_full();
    return it;
  }

  const_iterator end() const noexcept { return const_iterator{}; }

  const_iterator cbegin() const noexcept { return begin(); }

  const_iterator cend() const noexcept { return end(); }

  // ---- capacity -----------------------------------------------------------------------

  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

  [[nodiscard]] size_type size() const noexcept { return size_; }

  [[nodiscard]] size_type capacity() const noexcept { return capacity_; }

  [[nodiscard]] double load_factor() const noexcept {
    return capacity_ == 0 ? 0.0 : static_cast<double>(size_) / static_cast<double>(capacity_);
  }

  [[nodiscard]] static constexpr double max_load_factor() noexcept {
    return static_cast<double>(Config::max_load_num) / static_cast<double>(Config::max_load_den);
  }

  // Insertions available before the next rehash. Tombstones consume this budget, which
  // is why a churn workload without the drain policy rehashes forever (Experiment 5).
  [[nodiscard]] size_type growth_left() const noexcept { return growth_left_; }

  [[nodiscard]] size_type tombstones() const noexcept {
    return capacity_ == 0 ? 0 : max_size_for(capacity_) - size_ - growth_left_;
  }

  // Bytes in the single allocation. Divided by size() this is the honest memory-per-element
  // number the README quotes; it includes the control byte and the unused 1/8.
  [[nodiscard]] size_type memory_usage() const noexcept { return capacity_ == 0 ? 0 : alloc_bytes(capacity_); }

  // ---- lookup -------------------------------------------------------------------------

  iterator find(const key_type& key) {
    const size_type i = find_index(key, hash_(key));
    return i == npos ? end() : iterator_at(i);
  }

  const_iterator find(const key_type& key) const {
    const size_type i = find_index(key, hash_(key));
    return i == npos ? end() : const_iterator_at(i);
  }

  template<class K>
    requires detail::transparent_pair<Hash, KeyEqual> && detail::hasher_for<Hash, K> &&
             detail::equality_for<KeyEqual, Key, K> &&
             (!std::is_same_v<std::remove_cvref_t<K>, key_type>)
  iterator find(const K& key) {
    const size_type i = find_index(key, hash_(key));
    return i == npos ? end() : iterator_at(i);
  }

  template<class K>
    requires detail::transparent_pair<Hash, KeyEqual> && detail::hasher_for<Hash, K> &&
             detail::equality_for<KeyEqual, Key, K> &&
             (!std::is_same_v<std::remove_cvref_t<K>, key_type>)
  const_iterator find(const K& key) const {
    const size_type i = find_index(key, hash_(key));
    return i == npos ? end() : const_iterator_at(i);
  }

  bool contains(const key_type& key) const { return find_index(key, hash_(key)) != npos; }

  template<class K>
    requires detail::transparent_pair<Hash, KeyEqual> && detail::hasher_for<Hash, K> &&
             detail::equality_for<KeyEqual, Key, K> &&
             (!std::is_same_v<std::remove_cvref_t<K>, key_type>)
  bool contains(const K& key) const {
    return find_index(key, hash_(key)) != npos;
  }

  size_type count(const key_type& key) const { return contains(key) ? 1 : 0; }

  template<class K>
    requires detail::transparent_pair<Hash, KeyEqual> && detail::hasher_for<Hash, K> &&
             detail::equality_for<KeyEqual, Key, K> &&
             (!std::is_same_v<std::remove_cvref_t<K>, key_type>)
  size_type count(const K& key) const {
    return contains(key) ? 1 : 0;
  }

  std::pair<iterator, iterator> equal_range(const key_type& key) {
    auto it = find(key);
    if (it == end()) return {end(), end()};
    auto next = it;
    ++next;
    return {it, next};
  }

  std::pair<const_iterator, const_iterator> equal_range(const key_type& key) const {
    auto it = find(key);
    if (it == end()) return {end(), end()};
    auto next = it;
    ++next;
    return {it, next};
  }

  mapped_type& at(const key_type& key) {
    const size_type i = find_index(key, hash_(key));
    if (i == npos) throw std::out_of_range("maplab::flat_map::at: key not found");
    return slots_[i].value.second;
  }

  const mapped_type& at(const key_type& key) const {
    const size_type i = find_index(key, hash_(key));
    if (i == npos) throw std::out_of_range("maplab::flat_map::at: key not found");
    return slots_[i].value.second;
  }

  mapped_type& operator[](const key_type& key) { return try_emplace(key).first->second; }

  mapped_type& operator[](key_type&& key) { return try_emplace(std::move(key)).first->second; }

  // ---- modification -------------------------------------------------------------------

  std::pair<iterator, bool> insert(const value_type& value) {
    const auto slot = find_or_prepare_insert(value.first);
    if (slot.second) construct_checked(slot.first, value);
    return {iterator_at(slot.first), slot.second};
  }

  std::pair<iterator, bool> insert(value_type&& value) {
    const auto slot = find_or_prepare_insert(value.first);
    if (slot.second) construct_checked(slot.first, std::move(value));
    return {iterator_at(slot.first), slot.second};
  }

  template<class P>
    requires std::is_constructible_v<value_type, P&&> &&
             (!std::is_same_v<std::remove_cvref_t<P>, value_type>)
  std::pair<iterator, bool> insert(P&& value) {
    return emplace(std::forward<P>(value));
  }

  template<class InputIt>
  void insert(InputIt first, InputIt last) {
    if constexpr (std::forward_iterator<InputIt>) {
      reserve(size_ + static_cast<size_type>(std::distance(first, last)));
    }
    for (; first != last; ++first) insert(*first);
  }

  void insert(std::initializer_list<value_type> init) { insert(init.begin(), init.end()); }

  // Generic emplace has to materialise the value before it can see the key, so it costs
  // one extra move relative to try_emplace. That is a real cost, so try_emplace is the
  // documented fast path rather than a convenience wrapper.
  template<class... Args>
  std::pair<iterator, bool> emplace(Args&&... args) {
    mutable_value_type tmp(std::forward<Args>(args)...);
    const auto slot = find_or_prepare_insert(tmp.first);
    if (slot.second) construct_checked(slot.first, std::move(tmp));
    return {iterator_at(slot.first), slot.second};
  }

  template<class... Args>
  std::pair<iterator, bool> try_emplace(const key_type& key, Args&&... args) {
    return try_emplace_impl(key, std::forward<Args>(args)...);
  }

  template<class... Args>
  std::pair<iterator, bool> try_emplace(key_type&& key, Args&&... args) {
    return try_emplace_impl(std::move(key), std::forward<Args>(args)...);
  }

  template<class M>
  std::pair<iterator, bool> insert_or_assign(const key_type& key, M&& obj) {
    return insert_or_assign_impl(key, std::forward<M>(obj));
  }

  template<class M>
  std::pair<iterator, bool> insert_or_assign(key_type&& key, M&& obj) {
    return insert_or_assign_impl(std::move(key), std::forward<M>(obj));
  }

  size_type erase(const key_type& key) { return erase_by_key(key); }

  template<class K>
    requires detail::transparent_pair<Hash, KeyEqual> && detail::hasher_for<Hash, K> &&
             detail::equality_for<KeyEqual, Key, K> &&
             (!std::is_same_v<std::remove_cvref_t<K>, key_type>)
  size_type erase(const K& key) {
    return erase_by_key(key);
  }

  iterator erase(const_iterator pos) {
    const auto index = static_cast<size_type>(pos.ctrl() - ctrl_);
    destroy_slot(slots_ + index);
    erase_meta_only(index);
    iterator it{ctrl_ + index, slots_ + index};
    it.skip_to_full();
    return it;
  }

  void clear() noexcept {
    destroy_all_slots();
    size_ = 0;
    if (capacity_ != 0) {
      reset_ctrl();
      growth_left_ = max_size_for(capacity_);
    }
  }

  // Guarantee that `n` elements can be inserted without a rehash. The whole point of the
  // reserve/no-reserve pair of insert benchmarks is that the gap between them *is* the
  // cost of rehashing, so this must genuinely pre-size.
  void reserve(size_type n) {
    const size_type want = capacity_for(n);
    if (want > capacity_) resize(want);
  }

  void rehash(size_type n) {
    const size_type want = capacity_for(n < size_ ? size_ : n);
    if (want != capacity_) resize(want);
  }

  [[nodiscard]] hasher hash_function() const { return hash_; }

  [[nodiscard]] key_equal key_eq() const { return eq_; }

  // ---- instrumentation ----------------------------------------------------------------

  const probe_stats& stats() const noexcept
    requires(Config::stats)
  {
    return stats_;
  }

  void reset_stats() noexcept
    requires(Config::stats)
  {
    stats_.reset();
  }

 private:
  // ---- geometry -----------------------------------------------------------------------

  static constexpr std::size_t slot_align = alignof(slot_type);
  static constexpr std::size_t block_align = slot_align > 16 ? slot_align : 16;
  static constexpr std::size_t min_capacity = width - 1;

  static constexpr size_type ctrl_bytes(size_type cap) noexcept { return cap + 1 + cloned; }

  static constexpr size_type slot_offset(size_type cap) noexcept {
    return ((ctrl_bytes(cap) + slot_align - 1) / slot_align) * slot_align;
  }

  static constexpr size_type alloc_bytes(size_type cap) noexcept {
    return slot_offset(cap) + (cap * sizeof(slot_type));
  }

  // cap * num / den, computed so that it cannot overflow for any capacity that fits in
  // memory: the quotient and the remainder are scaled separately.
  static constexpr size_type max_size_for(size_type cap) noexcept {
    return ((cap / Config::max_load_den) * Config::max_load_num) +
           (((cap % Config::max_load_den) * Config::max_load_num) / Config::max_load_den);
  }

  static constexpr size_type capacity_for(size_type n) noexcept {
    if (n == 0) return 0;
    size_type cap = std::max(detail::normalize_capacity(n), min_capacity);
    while (max_size_for(cap) < n) cap = (cap * 2) + 1;
    return cap;
  }

  // Which bits choose the group. H1 = hash >> 7 deliberately excludes the 7 bits that
  // become the fingerprint: if the two overlapped, every slot reachable in one group
  // would share most of its fingerprint bits and the filter would reject almost nothing.
  // Measured, that mistake costs a 30x higher H2 false-positive rate -- it is Experiment
  // 4b, and `split_hash = false` reproduces it.
  static constexpr size_type probe_start(size_type hash) noexcept {
    if constexpr (Config::split_hash) {
      return h1(hash);
    } else {
      return hash;
    }
  }

  static constexpr h2_t fingerprint(size_type hash) noexcept {
    if constexpr (Config::h2_filter) {
      return h2(hash);
    } else {
      // Every occupied byte carries the same fingerprint, so the group match degenerates
      // to "every occupied slot is a candidate" without changing anything else.
      return h2_t{0};
    }
  }

  // ---- raw storage --------------------------------------------------------------------

  template<class... Args>
  static void construct_slot(slot_type* s, Args&&... args) {
    std::construct_at(std::addressof(s->value), std::forward<Args>(args)...);
  }

  static void destroy_slot(slot_type* s) noexcept { std::destroy_at(std::addressof(s->value)); }

  // Relocate one element during a rehash: move-construct at the destination, then destroy
  // the source. Reads through `mutable_value` so the key moves instead of copying.
  static void transfer_slot(slot_type* dst, slot_type* src) noexcept {
    std::construct_at(std::addressof(dst->value), std::move(src->mutable_value));
    std::destroy_at(std::addressof(src->value));
  }

  void allocate(size_type cap) {
    void* block = ::operator new(alloc_bytes(cap), std::align_val_t{block_align});
    ctrl_ = static_cast<ctrl_t*>(block);
    slots_ = reinterpret_cast<slot_type*>(static_cast<std::byte*>(block) + slot_offset(cap));
    capacity_ = cap;
    reset_ctrl();
    growth_left_ = max_size_for(cap);
  }

  static void deallocate(ctrl_t* ctrl, size_type cap) noexcept {
    ::operator delete(static_cast<void*>(ctrl), alloc_bytes(cap), std::align_val_t{block_align});
  }

  void reset_ctrl() noexcept {
    std::memset(
        ctrl_, static_cast<int>(static_cast<unsigned char>(ctrl_empty)), ctrl_bytes(capacity_));
    ctrl_[capacity_] = ctrl_sentinel;
  }

  // Writing a control byte also writes its clone in the tail group, so the two views of
  // the same slot never disagree. For i >= cloned the mirror is the byte itself.
  void set_ctrl(size_type i, ctrl_t h) noexcept {
    const size_type mirrored = ((i - cloned) & capacity_) + (cloned & capacity_);
    ctrl_[i] = h;
    ctrl_[mirrored] = h;
  }

  void destroy_all_slots() noexcept {
    if constexpr (!std::is_trivially_destructible_v<value_type>) {
      if (capacity_ == 0 || size_ == 0) return;
      for (size_type i = 0; i != capacity_; ++i) {
        if (is_full(ctrl_[i])) destroy_slot(slots_ + i);
      }
    }
  }

  void destroy_and_release() noexcept {
    destroy_all_slots();
    if (capacity_ != 0) deallocate(ctrl_, capacity_);
    ctrl_ = detail::empty_group();
    slots_ = nullptr;
    size_ = 0;
    capacity_ = 0;
    growth_left_ = 0;
  }

  // ---- probing ------------------------------------------------------------------------

  template<class K>
  size_type find_index(const K& key, size_type hash) const {
    const h2_t fp = fingerprint(hash);
    probe_seq<width> seq{probe_start(hash), capacity_};
    std::uint64_t groups = 0;
    while (true) {
      ++groups;
      const group g{ctrl_ + seq.offset()};
      if constexpr (Config::prefetch) {
        __builtin_prefetch(static_cast<const void*>(slots_ + seq.offset()));
      }
      for (const std::uint32_t i : g.match(fp)) {
        const size_type index = seq.offset(i);
        MAPLAB_ASSUME(slots_ != nullptr);
        if (eq_(slots_[index].value.first, key)) [[likely]] {
          if constexpr (Config::stats) {
            ++stats_.key_compares;
            ++stats_.lookups;
            ++stats_.lookup_hits;
            stats_.record_groups(groups);
          }
          return index;
        }
        if constexpr (Config::stats) {
          ++stats_.key_compares;
          ++stats_.h2_false_positives;
        }
      }
      // The miss fast path: an empty byte anywhere in this group means the probe chain
      // for this key ended here, so the key cannot be further along.
      if (g.mask_empty()) [[likely]] {
        if constexpr (Config::stats) {
          ++stats_.lookups;
          ++stats_.lookup_misses;
          stats_.record_groups(groups);
        }
        return npos;
      }
      if constexpr (Config::stats) {
        stats_.tombstones_probed += g.mask_empty_or_deleted().count();
      }
      seq.next();
    }
  }

  // First slot in the probe chain that can accept a new element: empty, or a tombstone
  // we can reuse. Always terminates, because the load factor keeps at least one empty
  // slot in the table at all times.
  size_type find_first_non_full(size_type hash) const noexcept {
    probe_seq<width> seq{probe_start(hash), capacity_};
    while (true) {
      const group g{ctrl_ + seq.offset()};
      const auto mask = g.mask_empty_or_deleted();
      if (mask) return seq.offset(mask.lowest_set());
      seq.next();
    }
  }

  template<class K>
  std::pair<size_type, bool> find_or_prepare_insert(const K& key) {
    const size_type hash = hash_(key);
    const h2_t fp = fingerprint(hash);
    probe_seq<width> seq{probe_start(hash), capacity_};
    std::uint64_t groups = 0;
    while (true) {
      ++groups;
      const group g{ctrl_ + seq.offset()};
      for (const std::uint32_t i : g.match(fp)) {
        const size_type index = seq.offset(i);
        MAPLAB_ASSUME(slots_ != nullptr);
        if (eq_(slots_[index].value.first, key)) [[likely]] {
          if constexpr (Config::stats) {
            ++stats_.key_compares;
            ++stats_.lookups;
            ++stats_.lookup_hits;
            stats_.record_groups(groups);
          }
          return {index, false};
        }
        if constexpr (Config::stats) {
          ++stats_.key_compares;
          ++stats_.h2_false_positives;
        }
      }
      if (g.mask_empty()) break;
      seq.next();
    }
    if constexpr (Config::stats) {
      ++stats_.lookups;
      ++stats_.lookup_misses;
      stats_.record_groups(groups);
    }
    return {prepare_insert(hash), true};
  }

  size_type prepare_insert(size_type hash) {
    size_type target = find_first_non_full(hash);
    // Inserting into a tombstone consumes no growth budget: the budget was spent when
    // that slot was first filled and never returned.
    if (growth_left_ == 0 && !is_deleted(ctrl_[target])) {
      rehash_and_grow_if_necessary();
      target = find_first_non_full(hash);
    }
    ++size_;
    if (is_empty(ctrl_[target])) --growth_left_;
    set_ctrl(target, static_cast<ctrl_t>(fingerprint(hash)));
    if constexpr (Config::stats) ++stats_.inserts;
    return target;
  }

  void rehash_and_grow_if_necessary() {
    if (capacity_ == 0) {
      resize(min_capacity);
      return;
    }
    if constexpr (Config::drain_tombstones) {
      // Rehashing at the same capacity is only worth it when tombstones, not elements,
      // are what used up the growth budget. Phrasing the threshold in terms of the
      // *ceiling* rather than the capacity is what makes it independent of the configured
      // load factor: abseil's equivalent test (size * 32 <= capacity * 25) silently
      // assumes a 7/8 ceiling, and at a 1/2 ceiling it is true unconditionally -- so the
      // table drains forever and never grows. The load-factor sweep in the test matrix
      // found exactly that.
      //
      // Progress is guaranteed: a drain clears every tombstone, so growth_left afterwards
      // is at least ceiling/8 >= 1.
      const size_type ceiling = max_size_for(capacity_);
      const size_type dead = ceiling - size_;  // == tombstones, since growth_left is 0
      if (capacity_ > width && dead != 0 && dead * 8 >= ceiling) {
        if constexpr (Config::stats) ++stats_.drains;
        resize(capacity_);
        return;
      }
    }
    if constexpr (Config::stats) ++stats_.grows;
    // Capacity is always 2^k - 1, so the growth factor is forced to 2. A 1.5x factor
    // would need a non-power-of-two capacity and therefore a modulo (or Lemire
    // reduction) instead of a mask on every single probe. See DESIGN.md.
    resize((capacity_ * 2) + 1);
  }

  void resize(size_type new_capacity) {
    if (new_capacity == 0) {
      // Only reachable from rehash(0) on an empty table. Release the storage outright and
      // go back to pointing at the shared empty control group -- allocating a zero-slot
      // table would still allocate the control bytes, and nothing would ever free them.
      destroy_and_release();
      return;
    }
    ctrl_t* old_ctrl = ctrl_;
    slot_type* old_slots = slots_;
    const size_type old_capacity = capacity_;

    allocate(new_capacity);
    growth_left_ -= size_;

    if (old_capacity != 0) {
      for (size_type i = 0; i != old_capacity; ++i) {
        if (is_full(old_ctrl[i])) {
          const size_type hash = hash_(old_slots[i].value.first);
          const size_type target = find_first_non_full(hash);
          set_ctrl(target, static_cast<ctrl_t>(fingerprint(hash)));
          transfer_slot(slots_ + target, old_slots + i);
          if constexpr (Config::stats) ++stats_.slots_moved;
        }
      }
      deallocate(old_ctrl, old_capacity);
    }
  }

  // Turn an occupied slot back into a free one. The question is whether it may become
  // *empty* (which ends probe chains) or must become a tombstone (which does not).
  //
  // If the run of occupied slots containing this one is shorter than a group, then every
  // probe that could reach this slot already saw an empty byte in the same group load and
  // stopped there, so nothing depends on this slot staying non-empty and we can hand the
  // growth budget back. Otherwise a probe chain may run through it and we must leave a
  // tombstone.
  void erase_meta_only(size_type index) noexcept {
    --size_;
    const size_type before = (index - width) & capacity_;
    const auto empty_after = group{ctrl_ + index}.mask_empty();
    const auto empty_before = group{ctrl_ + before}.mask_empty();
    const bool was_never_full =
        static_cast<bool>(empty_before) && static_cast<bool>(empty_after) &&
        (empty_after.trailing_zeros() + empty_before.leading_zeros()) < width;
    set_ctrl(index, was_never_full ? ctrl_empty : ctrl_deleted);
    if (was_never_full) {
      ++growth_left_;
    } else if constexpr (Config::stats) {
      ++stats_.tombstones_created;
    }
    if constexpr (Config::stats) ++stats_.erases;
  }

  template<class K>
  size_type erase_by_key(const K& key) {
    const size_type index = find_index(key, hash_(key));
    if (index == npos) return 0;
    destroy_slot(slots_ + index);
    erase_meta_only(index);
    return 1;
  }

  template<class... Args>
  void construct_checked(size_type index, Args&&... args) {
    if constexpr (std::is_nothrow_constructible_v<value_type, Args...>) {
      construct_slot(slots_ + index, std::forward<Args>(args)...);
    } else {
      try {
        construct_slot(slots_ + index, std::forward<Args>(args)...);
      } catch (...) {
        // The control byte was already claimed by prepare_insert. Give it back so the
        // table is still a valid table (the basic guarantee).
        erase_meta_only(index);
        throw;
      }
    }
  }

  template<class K, class... Args>
  std::pair<iterator, bool> try_emplace_impl(K&& key, Args&&... args) {
    const auto slot = find_or_prepare_insert(key);
    if (slot.second) {
      construct_checked(slot.first,
                        std::piecewise_construct,
                        std::forward_as_tuple(std::forward<K>(key)),
                        std::forward_as_tuple(std::forward<Args>(args)...));
    }
    return {iterator_at(slot.first), slot.second};
  }

  template<class K, class M>
  std::pair<iterator, bool> insert_or_assign_impl(K&& key, M&& obj) {
    const auto slot = find_or_prepare_insert(key);
    if (slot.second) {
      construct_checked(slot.first,
                        std::piecewise_construct,
                        std::forward_as_tuple(std::forward<K>(key)),
                        std::forward_as_tuple(std::forward<M>(obj)));
    } else {
      slots_[slot.first].value.second = std::forward<M>(obj);
    }
    return {iterator_at(slot.first), slot.second};
  }

  iterator iterator_at(size_type index) noexcept { return iterator{ctrl_ + index, slots_ + index}; }

  const_iterator const_iterator_at(size_type index) const noexcept {
    return const_iterator{ctrl_ + index, slots_ + index};
  }

  // ---- state --------------------------------------------------------------------------

  ctrl_t* ctrl_ = detail::empty_group();
  slot_type* slots_ = nullptr;
  size_type size_ = 0;
  size_type capacity_ = 0;
  size_type growth_left_ = 0;
  [[no_unique_address]] Hash hash_{};
  [[no_unique_address]] KeyEqual eq_{};
  [[no_unique_address]] mutable std::conditional_t<Config::stats, probe_stats, no_stats> stats_{};
};

// Convenience alias: the table with counters compiled in.
template<class Key, class T, class Hash = default_hash, class KeyEqual = std::equal_to<>>
using instrumented_flat_map = flat_map<Key, T, Hash, KeyEqual, stats_config>;

}  // namespace maplab

#endif  // MAPLAB_FLAT_MAP_HPP
