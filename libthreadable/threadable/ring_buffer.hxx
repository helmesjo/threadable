#pragma once

#include <threadable/allocator.hxx>
#include <threadable/atomic.hxx>
#include <threadable/debug.hxx>
#include <threadable/function.hxx>
#include <threadable/ring_iterator.hxx>
#include <threadable/ring_slot.hxx>
#include <threadable/token.hxx>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <memory>
#include <ranges>
#include <thread>
#include <type_traits>
#include <vector>

#ifdef _MSC_VER
  #pragma warning(push)
  #pragma warning(disable : 4324)
#endif

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

namespace fho
{
  namespace details
  {
    inline constexpr auto        slot_size        = cache_line_size - sizeof(fho::atomic_state_t);
    inline constexpr std::size_t default_capacity = 1 << 16;
  }

  template<typename R, typename T, template<typename, typename> typename C>
  concept range_of = std::ranges::range<R> && C<T&, std::ranges::range_value_t<R>>::value;

  /// @brief A `fho::function` alias optimized to cache line size for use within a `ring_buffer`.
  /// @details The size of the function object is exactly that of the target system's (deduced)
  ///          cache line size minus `1` byte reserved for the `ring_slot` state handling.
  using fast_func_t = function<details::slot_size>;

  /// @brief A Multi-Producer Multi-Consumer (MPMC) ring buffer for managing objects in a
  /// threading environment.
  /// @details This class provides a lock-free ring buffer that allows multiple producers to add
  /// objects concurrently and multiple consumer to claim & remove them. It uses atomic operations
  /// to manage the buffer's state, ensuring thread safety. The buffer has a fixed capacity, which
  /// must be a power of 2 greater than 1 for mask-based indexing; full `Capacity` slots usable.
  /// Slots are aligned to cache lines to reduce false sharing and improve performance in concurrent
  /// scenarios.
  /// @tparam `T` The type of elements stored in the buffer. Must be movable and destructible.
  ///             Defaults to `fast_func_t`, a callable optimized for cache line size.
  /// @tparam `Capacity` The size of the ring buffer, must be a power of 2 and greater than 1.
  ///                    Defaults to 65536 (`1 << 16`).
  /// @tparam `Allocator` The allocator type used for the buffer's slots. Defaults to an aligned
  ///                     allocator for `ring_slot<T>`.
  /// @note The buffer uses three atomic indices: `tail_` (next slot to consume), `head_` (next
  ///       slot to produce into), and `next_` (next slot to claim for production).
  /// @warning Only one consumer should call `pop range()` at a time to ensure thread safety.
  ///          Multiple producers can safely call `emplace()` concurrently.
  /// @example
  /// Logical view; actual indices unbounded, masked for array access.
  /// When tail or head reaches the end they will wrap around.
  /// ```cpp
  ///  _
  /// |_|
  /// |_|
  /// |_| ┐→ tail  (next claim) - consumer
  /// |_| │
  /// |_| │
  /// |_| │
  /// |_| │
  /// |_| ┘→ head-1 (last elem) - consumer
  /// |_|  ← head   (next slot) - producer
  /// |_|
  /// auto buffer = fho::ring_buffer<>{}; // Uses fast_func_t by default
  /// auto token = buffer.emplace_back([]() { std::cout << "Hello, World!\n"; });
  /// auto range = buffer.pop_range();
  /// for (auto& func : range) {
  ///     func();
  /// }
  /// token.wait(); // Wait for the task to complete
  /// ```
  template<typename T = fast_func_t, std::size_t Capacity = details::default_capacity,
           typename Allocator = aligned_allocator<ring_slot<T>, details::cache_line_size>>
  class ring_buffer
  {
  public:
    using value_type      = T;
    using size_type       = std::size_t;
    using reference       = value_type&;
    using pointer         = value_type*;
    using const_reference = value_type const&;
    using allocator_type  = Allocator;

    using claimed_type = claimed_slot<value_type>;

  private:
    static constexpr auto index_mask = Capacity - 1u;

    using slot_type = ring_slot<value_type>;

    using atomic_index_t        = std::atomic_uint_fast64_t;
    using index_t               = typename atomic_index_t::value_type;
    using ring_iterator_t       = ring_iterator<slot_type, index_mask>;
    using const_ring_iterator_t = ring_iterator<slot_type const, index_mask>;

  public:
    using transform_type =
      ring_transform_view<ring_iterator_t, decltype(slot_value_accessor<value_type>)>;
    using const_transform_type =
      ring_transform_view<const_ring_iterator_t, decltype(slot_value_accessor<value_type const>)>;

    using iterator       = std::ranges::iterator_t<transform_type>;       // NOLINT
    using const_iterator = std::ranges::iterator_t<const_transform_type>; // NOLINT

    static constexpr bool is_always_lock_free = false; // Waits if Capacity exceeded.

    static_assert(std::is_unsigned_v<index_t>);
    static_assert(Capacity > 1, "capacity must be greater than 1");
    static_assert((Capacity & index_mask) == 0, "capacity must be a power of 2");
    static_assert(sizeof(slot_type) % details::cache_line_size == 0,
                  "buffer slot size must be a multiple of the cache line size");

    static_assert(
      std::is_same_v<value_type, std::remove_reference_t<decltype(*std::declval<iterator>())>>,
      "Incorrect dereferenced type of non-const iterator");
    static_assert(
      std::is_const_v<std::remove_reference_t<decltype(*std::declval<const_iterator>())>>,
      "Incorrect dereferenced type of const iterator");

    /// @brief Default constructor.
    /// @details Initializes the ring buffer with pre-allocated slots of size `Capacity`.
    ring_buffer() noexcept          = default;
    ring_buffer(ring_buffer const&) = delete;

    /// @brief Destructor.
    /// @details Consumes all remaining items to ensure proper cleanup.
    ~ring_buffer()
    {
      clear();
    }

    /// @brief Move constructor.
    /// @details Transfers ownership of the buffer's contents from another instance.
    /// @param `rhs` The ring buffer to move from.
    ring_buffer(ring_buffer&& rhs) noexcept
      : tail_(rhs.tail_.load(std::memory_order::relaxed))
      , head_(rhs.head_.load(std::memory_order::relaxed))
      , elems_(std::move(rhs.elems_))
    {
      rhs.tail_.store(0, std::memory_order::relaxed);
      rhs.head_.store(0, std::memory_order::relaxed);
    }

    auto operator=(ring_buffer const&) -> ring_buffer& = delete;

    /// @brief Move assignment operator.
    /// @details Transfers ownership of the buffer's contents from another instance.
    /// @param `rhs` The ring buffer to move from.
    /// @return Reference to this ring buffer.
    auto
    operator=(ring_buffer&& rhs) noexcept -> ring_buffer&
    {
      tail_ = rhs.tail_.load(std::memory_order::relaxed);
      head_ = rhs.head_.load(std::memory_order::relaxed);
      rhs.tail_.store(0, std::memory_order::relaxed);
      rhs.head_.store(0, std::memory_order::relaxed);
      elems_ = std::move(rhs.elems_);
      return *this;
    }

    template<slot_state Tags = slot_state::invalid>
    auto
    push_back(slot_token& token, range_of<T, std::is_constructible> auto&& r) noexcept
      -> slot_token&
    {
      auto const s = std::ssize(r);
      assert(s <= Capacity and "ring_buffer::push_back()");
      // 1. Reserv slot(s)
      auto h = head_.fetch_add(s, std::memory_order_relaxed);

      // 2. Construct
      auto idx = h;
      for (auto& v : r)
      {
        auto& s = elems_[ring_iterator_t::mask(idx)];
        // Wait until the consumer has recycled this slot to empty, then lock it.
        // try_lock<empty>() is an acquire-CAS on the slot header.
        while (!s.template try_lock<slot_state::empty>())
        {
          s.template wait<slot_state::empty, false>(std::memory_order_acquire);
        }
        s.template set<Tags, true>(std::memory_order_relaxed);
        s.bind(token);
        s.emplace(fho::stdext::forward_like<decltype(r)>(v));

        // 3. Publish
        s.template commit<slot_state::locked_empty>();
        ++idx;
      }
      assert(h >= tailPop_.load(std::memory_order_acquire) and
             "ring_buffer::emplace_back() - producer would publish 'ready' behind tailPop_");

      // We just committed slot 'h' to READY. Try to extend publication contiguously
      // starting from the current published top (hp), not from head_ (reservations).
      auto hp = h;
      h       = h + s;
      if (!headPop_.compare_exchange_weak(hp, h + 1, std::memory_order_acq_rel,
                                          std::memory_order_acquire))
      {
        do
        {
          if (hp > h)
          {
            break; // our slot became visible → done
          }
          // Load current published top and decide if we're the tip *now*.
          auto hl = head_.load(std::memory_order_acquire);
          assert(hp <= hl and "ring_buffer::emplace_back()");
          auto tip = hl == h + 1;

          // We only ever try to push within a proven contiguous prefix
          // starting at `hp`, capped at our own visibility need (h+1) and reservations (hl).
          auto bound = std::min(hl, h + 1);
          auto scan  = hp;
          for (; scan < bound; ++scan)
          {
            auto& s = elems_[ring_iterator_t::mask(scan)];
            if (!(s.template test<slot_state::ready>(std::memory_order_acquire) &&
                  (s.template test<slot_state::epoch>(std::memory_order_relaxed) ==
                   epoch_of(scan))))
            {
              break; // hole at `scan` → cannot advance beyond it
            }
          }

          if (headPop_.compare_exchange_weak(hp, scan, std::memory_order_acq_rel,
                                             std::memory_order_acquire))
          {
            hp = scan;
          }
        }
        while (true);
      }

      assert(h < headPop_.load(std::memory_order_acquire) and "ring_buffer::emplace_back()");
      return token;
    }

    template<slot_state Tags = slot_state::invalid, typename U>
      requires std::constructible_from<T, U>
    auto
    push_back(slot_token& token, U&& val) noexcept -> slot_token&
    {
      return emplace_back<Tags>(token, FWD(val));
    }

    inline constexpr auto
    epoch_of(index_t p) noexcept -> bool
    {
      return (p >> capacity_bits) & 1;
    }

    template<slot_state Tags = slot_state::invalid, typename U>
      requires std::constructible_from<T, U>
    auto
    push_back(U&& u) noexcept -> slot_token
    {
      auto t = slot_token{};
      auto s = std::span{std::addressof(u), 1};
      push_back<Tags>(t, fho::stdext::forward_like<U>(s));
      return t;
    }

    template<slot_state Tags = slot_state::invalid, typename... Args>
      requires std::constructible_from<T, Args...>
    auto
    emplace_back(slot_token& token, Args&&... args) noexcept -> slot_token&
    {
      // 1. Reserv slot(s)
      auto const h = head_.fetch_add(1, std::memory_order_relaxed);

      // 2. Construct
      auto& s = elems_[ring_iterator_t::mask(h)];
      // Wait until the consumer has recycled this slot to empty, then lock it.
      while (!s.template try_lock<slot_state::empty>())
      {
        // Wait on the slot state changing away from non-empty (acquire).
        s.template wait<slot_state::empty, false>(std::memory_order_acquire);
      }
      s.template set<Tags, true>(std::memory_order_relaxed);
      // s.template set<slot_state::epoch>(epoch_of(h), std::memory_order_relaxed);
      dbg::verify<slot_state::epoch>(s, epoch_of(h) ? slot_state::epoch : slot_state::invalid);
      s.bind(token);
      s.emplace(FWD(args)...);

      // assert(h >= tailPop_.load(std::memory_order_acquire) and
      //        "ring_buffer::emplace_back() - producer would publish 'ready' behind tailPop_");
      // 3. Publish
      s.template commit<slot_state::locked_empty>();

      // We just committed slot 'h' to READY. Try to extend publication contiguously
      // starting from the current published top (hp), not from head_ (reservations).
      auto hp = h;
      if (!headPop_.compare_exchange_weak(hp, h + 1, std::memory_order_acq_rel,
                                          std::memory_order_acquire))
      {
        do
        {
          if (hp > h)
          { // slot published by another; done
            break;
          }

          auto hl = head_.load(std::memory_order_acquire); // current reservation bound
          assert(h <= hl && "ring_buffer::emplace_back(): new head ahead of head_");
          assert(hp <= hl && "ring_buffer::emplace_back(): headPop_ ahead of head_");

          // Scan backward from h to hp to verify contiguous ready slots
          auto scan  = hl;
          auto ready = true;
          for (; scan > hp; --scan)
          {
            auto const  pos = scan - 1;
            auto const& s1  = elems_[ring_iterator_t::mask(pos)];
            auto const  sameEpoch =
              s1.template test<slot_state::epoch>(std::memory_order_acquire) == epoch_of(pos);

            if (sameEpoch && !s1.template test<slot_state::ready>(std::memory_order_acquire))
            {
              ready = false;
              break; // Same epoch, not ready.
            }
            if (!sameEpoch)
            {
              break; // Different epoch, not consumed.
            }
          }

          if (!ready)
          {
            std::this_thread::yield();
            hp = headPop_.load(std::memory_order_acquire);
            continue;
          }

          if (scan < hp)
          {
            assert(scan >= hp and "ring_buffer::emplace_back() - Bad back-tracking");
          }
          // If contiguous to hp, try to publish up to h+1 (or hl if smaller)
          if (headPop_.compare_exchange_weak(hp, hl, std::memory_order_acq_rel,
                                             std::memory_order_acquire))
          {
            hp = hl;
          }
          // CAS failed; hp already reloaded, so retry
        }
        while (true);
      }
      else
      {
        hp = h + 1;
      }

      if (h >= hp)
      {
        assert(h < hp and "ring_buffer::emplace_back() - Premature publish");
      }
      return token;
    }

    template<slot_state Tags = slot_state::invalid, typename... Args>
      requires std::constructible_from<T, Args...>
    auto
    emplace_back(Args&&... args) noexcept -> slot_token
    {
      auto t = slot_token{};
      emplace_back<Tags>(t, FWD(args)...);
      return t;
    }

    static constexpr auto claim_view = std::ranges::views::transform(
      [](slot_type& s)
      {
        return claimed_type{&s};
      });

    auto
    try_pop_back2() noexcept -> claimed_type
    {
      if (auto r = try_pop_back2(1); !r.empty())
      {
        assert(r.size() == 1 and "ring_buffer::try_pop_front()");
        return std::move(*r.begin());
      }
      return nullptr;
    }

    auto
    try_pop_back2(index_t max) noexcept
    {
      for (;;)
      {
        // 1. Validate boundaries/early bail
        auto const tp = tailPop_.load(std::memory_order_acquire); // published start
        auto const hp = headPop_.load(std::memory_order_acquire); // published end
        if (hp <= tp)
        {
          break;
        }

        // 2. FAA to reserve (optimistically)
        auto const diff = hp - tp;
        auto const lim  = std::min(max, diff);
        // retreat headPop_ by lim
        auto const res = headPop_.fetch_sub(lim, std::memory_order_acq_rel);
        // new published end after reservation
        auto const cap = res - lim;

        // 3. Iterate & try-lock reserved range (backward from reserved end)
        auto locked   = index_t{0};
        bool gapFound = false;
        for (auto i = lim; i > 0; --i)
        {
          auto const pos  = cap + i - 1; // logical pos from back
          auto&      slot = elems_[ring_iterator_t::mask(pos)];

          // Attempt lock first, then check epoch for correctness
          if (!slot.template try_lock<slot_state::ready>())
          {
            gapFound = true;
            break; // gap or contention; stop claiming
          }
          // Verify correct epoch
          if (slot.template test<slot_state::epoch>(std::memory_order_acquire) != epoch_of(pos))
          {
            slot.template unlock<slot_state::locked_ready>();
            gapFound = true;
            break;
          }
          // If insertion required "previous empty" on back, then for front
          // we require "next empty" (and same-lap) when tag_seq is set.
          if (slot.template test<slot_state::tag_seq>(std::memory_order_acquire)) [[unlikely]]
          {
            auto const  ppos = pos - 1;
            auto const& prev = elems_[ring_iterator_t::mask(ppos)];

            if (!prev.template test<slot_state::empty>(std::memory_order_acquire) &&
                prev.template test<slot_state::epoch>(std::memory_order_relaxed) == epoch_of(ppos))
            {
              slot.template unlock<slot_state::locked_ready>();
              break;
            }
          }
          ++locked;
        }

        // Handle claims or gaps
        if (locked > 0)
        {
          // Restore over-reserved slots
          auto over = lim - locked;
          if (over > 0)
          {
            assert(cap + over <= res and
                   "ring_buffer::try_pop_back() - Restored published head would go past head");
            headPop_.fetch_add(over, std::memory_order_acq_rel);
          }
          // Return claimed range
          auto b = ring_iterator_t(elems_.data(), cap);
          auto e = ring_iterator_t(elems_.data(), cap + locked);
          return std::ranges::subrange(b, e) | claim_view;
        }

        // 4. Handle zero claims with gap: validate as empty for safe trim
        if (gapFound)
        {
          // Try to lock and validate gap as empty or stale
          auto trimmed = index_t{0};
          for (auto i = lim; i > 0; --i)
          {
            auto const pos  = cap + i - 1;
            auto&      slot = elems_[ring_iterator_t::mask(pos)];
            if (!slot.template try_lock<slot_state::empty>())
            {
              break; // not empty; stop trimming
            }
            if (slot.template test<slot_state::epoch>(std::memory_order_acquire) == epoch_of(pos))
            {
              slot.template unlock<slot_state::locked_empty>(); // still in current lap; may become
                                                                // ready
              break;
            }
            ++trimmed;                                          // confirmed stale/empty
            slot.template unlock<slot_state::locked_empty>();
          }

          if (trimmed == 0)
          {
            // No gaps trimmed, no claims: bail
            break;
          }

          // Restore non-trimmed portion
          if (auto restore = lim - trimmed; restore > 0)
          {
            assert(cap + restore <= res and
                   "ring_buffer::try_pop_back() - Restored published head would go past head");
            auto p = headPop_.fetch_add(restore, std::memory_order_acq_rel);
            assert(p + restore <= head_.load(std::memory_order_relaxed) and
                   "ring_buffer::try_pop_back() - Published head surpassed head");
          }
        }
        else
        {
          // No claims, no gap: lower bound (empty); restore and exit
          headPop_.fetch_add(lim, std::memory_order_acq_rel);
          break;
        }
      }

      // No claims after attempts; return empty
      return std::ranges::subrange(ring_iterator_t(elems_.data(), 0),
                                   ring_iterator_t(elems_.data(), 0)) |
             claim_view;
    }

    auto
    try_pop_front2() noexcept -> claimed_type
    {
      if (auto r = try_pop_front2(1); !r.empty())
      {
        assert(r.size() == 1 and "ring_buffer::try_pop_front()");
        return std::move(*r.begin());
      }
      return nullptr;
    }

    auto
    try_pop_front2(index_t max) noexcept
    {
      for (;;)
      {
        // 1. Validate boundaries/early bail
        auto const tp = tailPop_.load(std::memory_order_acquire); // published start
        auto const hp = headPop_.load(std::memory_order_acquire); // published end
        if (tp >= hp)
        {
          break;
        }

        // 2. FAA to reserve (optimistically)
        auto const diff = mask(hp - tp);
        auto const lim  = std::min(max, diff);
        // advance tailPop_ by lim
        auto const res = tailPop_.fetch_add(lim, std::memory_order_acq_rel);
        // new published start after reservation
        auto const cap = res;

        // 3. Iterate & try-lock reserved range (forward from reserved start)
        auto locked   = index_t{0};
        bool gapFound = false;
        for (auto i = index_t{0}; i < lim; ++i)
        {
          auto const pos  = cap + i; // logical pos from front
          auto&      slot = elems_[ring_iterator_t::mask(pos)];

          // Attempt lock first, then check epoch for correctness
          if (!slot.template try_lock<slot_state::ready>())
          {
            gapFound = true;
            break; // gap or contention; stop claiming
          }
          // Verify correct epoch
          if (slot.template test<slot_state::epoch>(std::memory_order_acquire) != epoch_of(pos))
          {
            slot.template unlock<slot_state::locked_ready>();
            gapFound = true;
            break;
          }
          // If tag_seq is set, require "next empty" (and same-lap) for front
          if (slot.template test<slot_state::tag_seq>(std::memory_order_acquire)) [[unlikely]]
          {
            auto const  ppos = pos - 1;
            auto const& prev = elems_[ring_iterator_t::mask(ppos)];
            if (!prev.template test<slot_state::empty>(std::memory_order_acquire) &&
                prev.template test<slot_state::epoch>(std::memory_order_acquire) == epoch_of(ppos))
            {
              slot.template unlock<slot_state::locked_ready>();
              break;
            }
          }
          ++locked;
        }

        // 4. Handle claims or gaps
        if (locked > 0)
        {
          // Restore over-reserved slots
          auto over = lim - locked;
          if (over > 0)
          {
            assert(cap >= over &&
                   "ring_buffer::try_pop_front() - Restored published tail would go below tail");
            tailPop_.fetch_sub(over, std::memory_order_acq_rel);
          }
          // Return claimed range
          auto b = ring_iterator_t(elems_.data(), cap);
          auto e = ring_iterator_t(elems_.data(), cap + locked);
          return std::ranges::subrange(b, e) | claim_view;
        }

        // 5. Handle zero claims with gap: validate as empty for safe trim
        if (gapFound)
        {
          auto trimmed = index_t{0};
          for (auto i = index_t{0}; i < lim; ++i)
          {
            auto const pos  = cap + i;
            auto&      slot = elems_[ring_iterator_t::mask(pos)];
            if (!slot.template try_lock<slot_state::empty>())
            {
              break; // not empty; stop trimming
            }
            if (slot.template test<slot_state::epoch>(std::memory_order_acquire) == epoch_of(pos))
            {
              slot.template unlock<slot_state::locked_empty>(); // still in current lap; may become
                                                                // ready
              break;
            }
            ++trimmed;                                          // confirmed stale/empty
            slot.template unlock<slot_state::locked_empty>();
          }

          if (trimmed == 0)
          {
            // No gaps trimmed, no claims: bail
            break;
          }

          // Restore non-trimmed portion
          if (auto restore = lim - trimmed; restore > 0)
          {
            assert(lim >= restore &&
                   "ring_buffer::try_pop_front() - Restored published tail would go below tail");
            auto p = tailPop_.fetch_sub(restore, std::memory_order_acq_rel);
            assert(p >= tail_.load(std::memory_order_relaxed) &&
                   "ring_buffer::try_pop_front() - Undershot published tail below tail");
          }
        }
        else
        {
          // No claims, no gap: upper bound (empty); restore and exit
          tailPop_.fetch_sub(lim, std::memory_order_acq_rel);
          break;
        }
      }

      // No claims after attempts; return empty
      return std::ranges::subrange(ring_iterator_t(elems_.data(), 0),
                                   ring_iterator_t(elems_.data(), 0)) |
             claim_view;
    }

    /// @brief Accesses the first element in the ring buffer.
    /// @details Returns a const reference to the oldest element (at `tail_`). The buffer must not
    ///          be empty.
    /// @return Const reference to the front element.
    /// @pre `size() > 0`
    [[nodiscard]] inline auto
    front() const noexcept -> const_reference
    {
      assert(size() > 0 and "ring_buffer::front()");
      auto const tail = tail_.load(std::memory_order_acquire);
      return elems_[ring_iterator_t::mask(tail)];
    }

    /// @brief Accesses the last element in the ring buffer.
    /// @details Returns a const reference to the most recently added element (at `head_ - 1`). The
    ///          buffer must not be empty.
    /// @return Const reference to the back element.
    /// @pre `size() > 0`
    [[nodiscard]] inline auto
    back() const noexcept -> const_reference
    {
      assert(size() > 0 and "ring_buffer::back()");
      auto const head = head_.load(std::memory_order_acquire);
      return elems_[ring_iterator_t::mask(head - 1)];
    }

    /// @brief Removes the first element from the ring buffer.
    /// @details Releases the slot at the front (at `tail_`) and advances `tail_`. The buffer must
    ///          not be empty.
    ///          Thread-safe for multiple consumers.
    /// @pre `size() > 0`
    /// @note Popping beyond emplaced items results in undefined behavior.
    void
    pop_front() noexcept
    {
      assert(size() > 0 and "ring_buffer::pop()");
      index_t tail; // NOLINT
      while (true)
      {
        // 1. Read tail slot.
        tail = tail_.load(std::memory_order_acquire);
        // 2. Acquire tail slot.
        slot_type& elem = elems_[ring_iterator_t::mask(tail)];
        if (elem.template try_lock<slot_state::ready>()) [[likely]]
        {
          // 3. Release tail slot.
          elem.template release<slot_state::locked_ready>();
          auto exp = tail;
          while (!tail_.compare_exchange_weak(exp, tail + 1, std::memory_order_acq_rel,
                                              std::memory_order_acquire))
          {
            exp = tail;
          }
          tailPop_.store(tail + 1, std::memory_order_release);
          break;
        }
        // 4. Somebody already released tail - Retry.
      }
    }

    /// @brief Consumes a range of items from the buffer.
    /// @details Retrieves a range of values from `front()` to `end()` (or `max`).
    ///          The `subrange_type` must be processed within its lifetime; slots are
    ///          released when the subrange is destroyed.
    /// @param `max` Maximum number of items to consume. Defaults to `max_size()`.
    /// @return Subrange of consumed values.
    /// @warning Only one consumer should call this at a time to avoid race conditions.
    /// @example
    /// ```cpp
    /// auto range = buffer.pop_range(10);
    /// for (auto& value : range) { /* Process value */ }
    /// ```
    auto
    try_pop_front(index_t max) noexcept
    {
      static constexpr auto claim_view = std::ranges::views::transform(
        [](slot_type& s)
        {
          return claimed_type{&s};
        });

      for (;;)
      {
        auto const tp = tailPop_.load(std::memory_order_acquire);
        auto const hp = headPop_.load(std::memory_order_acquire);

        auto const diff = hp - tp;
        max             = std::min(diff, max);
        auto const lim  = tp + max;
        auto       cap  = tp;

        assert(cap <= lim and "ring_buffer::try_pop_front()");
        assert(lim <= hp and "ring_buffer::try_pop_front()");

        // @NOTE: Forward-scan & claim until failure, or limit reached.
        while (cap < lim)
        {
          auto const pos  = cap;
          slot_type& slot = elems_[ring_iterator_t::mask(pos)];

          if (!slot.template try_lock<slot_state::ready>())
          {
            break;
          }

          if (slot.template test<slot_state::epoch>(std::memory_order_acquire) != epoch_of(pos))
          {
            slot.template unlock<slot_state::locked_ready>();
            break;
          }

          // Mirrored rare-path rule: if insertion required "previous empty" on back,
          // then for front we require "next empty" (and same-lap) when tag_seq is set.
          if (slot.template test<slot_state::tag_seq>(std::memory_order_acquire)) [[unlikely]]
          {
            auto const  ppos = pos - 1;
            auto const& prev = elems_[ring_iterator_t::mask(ppos)];

            if (!prev.template test<slot_state::empty>(std::memory_order_acquire) &&
                prev.template test<slot_state::epoch>(std::memory_order_relaxed) == epoch_of(ppos))
            {
              slot.template unlock<slot_state::locked_ready>();
              break;
            }
          }
          ++cap;
        }

        // We failed to lock any bottom cell: try to trim *only* slots that are
        // certainly not part of the current lap anymore.
        if (cap == tp)
        {
          // Scan upward while the physical bottom prefix is *not published for this lap*.
          // Use epoch to disambiguate wraps.
          auto k = tp;
          for (; k < hp; ++k)
          {
            auto& slot = elems_[ring_iterator_t::mask(k)];

            // “Published?” for tail-pop means: state==READY.
            // If not, we can exclude this cell from the published window.
            // Stop at first non-empty: could be READY/LOCKED → must not skip.
            if (!slot.template try_lock<slot_state::empty>())
            {
              break;
            }

            // Empty but epoch matches this logical pos → producer may still commit here.
            if (slot.template test<slot_state::epoch>(std::memory_order_acquire) == epoch_of(k))
            {
              break;
            }
          }

          if (k == tp)
          {
            // No claim & no fix, return empty.
            assert(tp == cap and "ring_buffer::try_pop_front()");
            break;
          }

          assert(k > tp);
          assert(k <= hp);
          assert(k <= headPop_.load(std::memory_order_acquire));
          auto exp = tp;
          // Move tailPop_ forward to exclude the prefix of non-published cells.
          auto cas = tailPop_.compare_exchange_strong(exp, k, std::memory_order_acq_rel,
                                                      std::memory_order_acquire);
          // Unlock scanned range.
          for (auto i = k; i < tp; --i)
          {
            auto const pos  = i;
            auto&      slot = elems_[ring_iterator_t::mask(pos)];
            slot.template unlock<slot_state::locked_empty>();
          }
          if (cas)
          {
            assert(k <= headPop_.load(std::memory_order_acquire));
            // tail fixed; retry outer loop
            continue;
          }
        }

        auto exp = tp;
        while (exp < cap && !tailPop_.compare_exchange_weak(exp, cap, std::memory_order_acq_rel,
                                                            std::memory_order_acquire))
        {
          // We already hold [tp, cap) in locked_ready. Do not roll back.
          // Decide whether we still need to publish, but always keep and return the range.
          if (exp >= cap)
          {
            break; // someone else advanced past our end; nothing more to do
          }
          std::this_thread::yield();
        }

        auto b = ring_iterator_t(elems_.data(), tp);
        auto e = ring_iterator_t(elems_.data(), cap);
        return std::ranges::subrange(b, e) | claim_view;
      }

      auto e = ring_iterator_t(elems_.data(), 0);
      return std::ranges::subrange(e, e) | claim_view;
    }

    /// @brief Attempts to claim the first element from the ring buffer.
    /// @details Tries to lock and claim the item at `front()` and advances `tail_` if successful.
    ///          If `prevReady` is `true`, ensures the previous slot is `empty` before claiming,
    ///          enforcing sequential execution for single-edge DAG tasks within the queue.
    ///          The claimed slot is automatically released when the returned `claimed_type` goes
    ///          out of scope. Thread-safe for multiple consumers.
    /// @param prevReady If `true`, checks that the previous slot is `empty` before claiming the
    ///                  current slot, ensuring the prior task has completed (used for
    ///                  `execution::seq`). If `false`, no dependency check is performed.
    /// @return A `claimed_type` (e.g., `claimed_slot<T>`) containing the claimed slot, or `nullptr`
    ///         if the claim fails due to contention, an empty buffer, or an uncompleted previous
    ///         task when `prevReady` is `true`.
    /// @note Makes a single attempt to claim the next item in line.
    auto
    try_pop_front() noexcept -> claimed_type
    {
      if (auto r = try_pop_front(1); !r.empty())
      {
        assert(r.size() == 1 and "ring_buffer::try_pop_front()");
        return std::move(*r.begin());
      }
      return nullptr;
    }

    static constexpr auto capacity_bits = std::countr_zero(Capacity);

    /// @brief Claim the last element from the ring buffer.
    /// @details Attempts to claim items from `back()` and decrements `head_`.
    /// The claimed slot is released when it goes out of scope.
    /// Thread-safe for multiple consumers.
    /// @note: Makes only a single attempt to claim next item in line.
    auto
    try_pop_back(index_t max) noexcept
    {
      static constexpr auto claim_view = std::ranges::views::transform(
        [](slot_type& s)
        {
          return claimed_type{&s};
        });

      for (;;)
      {
        auto const tp = tailPop_.load(std::memory_order_acquire);
        auto const hp = headPop_.load(std::memory_order_acquire);

        auto const diff = hp - tp;
        max             = std::min(diff, max);
        auto const lim  = hp - max;
        auto       cap  = hp;

        assert(cap >= lim and "ring_buffer::try_pop_back()");
        assert(lim <= hp and "ring_buffer::try_pop_back()");

        // @NOTE: Backward-scan & claim until failure, or limit reached.
        while (cap > lim)
        {
          auto const pos  = cap - 1;
          slot_type& slot = elems_[ring_iterator_t::mask(pos)];

          if (!slot.template try_lock<slot_state::ready>())
          {
            break;
          }

          if (slot.template test<slot_state::epoch>(std::memory_order_acquire) != epoch_of(pos))
          {
            slot.template unlock<slot_state::locked_ready>();
            break;
          }

          // Mirrored rare-path rule: if insertion required "previous empty" on back,
          // then for front we require "next empty" (and same-lap) when tag_seq is set.
          if (slot.template test<slot_state::tag_seq>(std::memory_order_acquire)) [[unlikely]]
          {
            auto const  ppos = pos - 1;
            auto const& prev = elems_[ring_iterator_t::mask(ppos)];

            if (!prev.template test<slot_state::empty>(std::memory_order_acquire) &&
                prev.template test<slot_state::epoch>(std::memory_order_relaxed) == epoch_of(ppos))
            {
              slot.template unlock<slot_state::locked_ready>();
              break;
            }
          }
          --cap;
        }

        // We failed to lock any bottom cell: try to trim *only* slots that are
        // certainly not part of the current lap anymore.
        if (cap == hp)
        {
          // Scan backward while the physical tail prefix is *not published for this lap*.
          // Use epoch to disambiguate wraps.
          auto k = hp;
          for (; k > tp;)
          {
            auto const pos  = k - 1;
            auto&      slot = elems_[ring_iterator_t::mask(pos)];

            // “Published?” for head-pop means: state==READY.
            // If not, we can exclude this cell from the published window.
            // Stop at first non-empty: could be READY/LOCKED → must not skip.
            if (!slot.template try_lock<slot_state::empty>())
            {
              break;
            }

            // Empty but epoch matches this logical pos → producer may still commit here.
            if (slot.template test<slot_state::epoch>(std::memory_order_acquire) == epoch_of(pos))
            {
              slot.template unlock<slot_state::locked_empty>();
              break;
            }
            --k;
          }

          if (k == hp)
          {
            // No claim & no fix, return empty.
            assert(hp == cap and "ring_buffer::try_pop_back()");
            break;
          }

          assert(k < hp);
          assert(k >= tp);
          assert(k >= tailPop_.load(std::memory_order_acquire));
          auto exp = hp;
          // Move headPop_ forward to exclude the prefix of non-published cells.
          auto cas = headPop_.compare_exchange_strong(exp, k, std::memory_order_acq_rel,
                                                      std::memory_order_acquire);
          // Unlock scanned range.
          for (auto i = k; i < hp; ++i)
          {
            auto const pos  = i;
            auto&      slot = elems_[ring_iterator_t::mask(pos)];
            slot.template unlock<slot_state::locked_empty>();
          }
          if (cas)
          {
            assert(k >= tailPop_.load(std::memory_order_acquire));
            // head fixed; retry outer loop
            continue;
          }
        }

        auto exp = hp;
        while (exp > cap && !headPop_.compare_exchange_weak(exp, cap, std::memory_order_acq_rel,
                                                            std::memory_order_acquire))
        {
          // We already hold [cap, hp) in locked_ready. Do not roll back.
          // Decide whether we still need to publish, but always keep and return the range.
          if (exp >= cap)
          {
            break; // someone else advanced past our end; nothing more to do
          }
          std::this_thread::yield();
        }

        auto b = ring_iterator_t(elems_.data(), cap);
        auto e = ring_iterator_t(elems_.data(), hp);
        return std::ranges::subrange(b, e) | claim_view;
      }

      auto e = ring_iterator_t(elems_.data(), 0);
      return std::ranges::subrange(e, e) | claim_view;
    }

    auto
    try_pop_back() noexcept -> claimed_type
    {
      if (auto r = try_pop_back(1); !r.empty())
      {
        assert(r.size() == 1 and "ring_buffer::try_pop_front()");
        return std::move(*r.begin());
      }
      return nullptr;
    }

    /// @brief Clears all items from the buffer.
    /// @details Consumes all available items, leaving the buffer empty.
    void
    clear() noexcept
    {
      for (auto& e : elems_)
      {
        if (e.template try_lock<slot_state::ready>())
        {
          e.template release<slot_state::locked_ready>();
        }
      }
      head_ = headPop_ = tail_ = tailPop_ = 0;
      // while (!empty())
      // {
      //   for (auto elem : try_pop_front(max_size()))
      //   {
      //     (void)elem; // iterate to auto-release slots.
      //   }
      // }
    }

    /// @brief Returns a const iterator to the buffer's start.
    /// @return Const iterator to the start of the buffer.
    auto
    begin() const noexcept
    {
      auto const tail = tail_.load(std::memory_order_acquire);
      return std::ranges::next(constRange_.begin(), tail);
    }

    /// @brief Returns a const iterator to the buffer's end.
    /// @return Const iterator to the end of the buffer.
    auto
    end() const noexcept
    {
      auto const head = head_.load(std::memory_order_acquire);
      return std::ranges::next(constRange_.begin(), head);
    }

    /// @brief Returns the maximum capacity of the buffer.
    /// @details Returns `Capacity`.
    /// @return Maximum number of items the buffer can hold.
    static constexpr auto
    max_size() noexcept -> std::size_t
    {
      return Capacity;
    }

    /// @brief Returns the maximum capacity of the buffer.
    /// @details Returns `Capacity`.
    /// @return Maximum number of items the buffer can hold.
    static constexpr auto
    mask(index_t i) noexcept -> std::size_t
    {
      return ring_iterator_t::mask(i);
    }

    /// @brief Returns the current number of items in the buffer.
    /// @details Computes the direct (unbounded) difference between `head_` and `tail_`.
    /// @return Current number of items.
    auto
    size() const noexcept -> std::size_t
    {
      auto const tail = tailPop_.load(std::memory_order_acquire);
      auto const head = headPop_.load(std::memory_order_acquire);
      return mask(head - tail); // circular distance
    }

    /// @brief Checks if the buffer is empty.
    /// @details Returns true if `size() == 0`.
    /// @return True if empty, false otherwise.
    /// @note Uses `size() == 0` to distinguish empty (`head==tail`) from full
    ///       (`head-tail == Capacity`).
    auto
    empty() const noexcept -> bool
    {
      return size() == 0;
    }

    /// @brief Provides direct access to the underlying data.
    /// @details Returns a pointer to the buffer's slot array.
    /// @return Pointer to the first slot.
    /// @warning Direct access may not be thread-safe; use with caution.
    auto
    data() const noexcept -> decltype(auto)
    {
      return elems_.data();
    }

  private:
    alignas(details::cache_line_size) atomic_index_t tail_{0};
    alignas(details::cache_line_size) atomic_index_t head_{0};
    alignas(details::cache_line_size) atomic_index_t tailPop_{0};
    alignas(details::cache_line_size) atomic_index_t headPop_{0};

    alignas(details::cache_line_size) std::vector<slot_type, allocator_type> elems_{Capacity};

    /// @brief Pre-created to ensure iterator compatibility.
    /// @note MSVC in debug specifically does not like iterators from temporarily created &
    /// differing ranges
    const_ring_iterator_t const begin_ = const_ring_iterator_t(elems_.data(), 0);        // NOLINT
    const_ring_iterator_t const end_   = const_ring_iterator_t(elems_.data(), Capacity); // NOLINT
    const_transform_type const  constRange_ =                                            // NOLINT
      const_transform_type(std::ranges::subrange(begin_, end_),
                           slot_value_accessor<value_type const>);
  };

  /// @brief Ensures the value type fits within the slot size. See `ring_buffer` for details.
  /// @details Identical to `ring_buffer` but statically checks that the size of type `T` does not
  ///          exceed the available slot size (`details::slot_size`, typically cache line size minus
  ///          `sizeof(atomic_state_t)`). Provides a detailed error message if supported by C++23
  ///          and `std::format`.
  template<typename T = fast_func_t, std::size_t Capacity = details::default_capacity,
           typename Allocator = aligned_allocator<ring_slot<T>, details::cache_line_size>>
  class fixed_ring_buffer : public ring_buffer<T, Capacity, Allocator>
  {
    static constexpr auto index_mask = Capacity - 1u;

#if __cpp_static_assert >= 202306L && __cpp_lib_constexpr_format
    static_assert(sizeof(T) <= details::slot_size,
                  std::format("T (size: {}) does not fit ring buffer slot (free : {}) ", sizeof(T),
                              details::slot_size));
#else
    static_assert(sizeof(T) <= details::slot_size, "T does not fit ring buffer slot");
#endif
  };
}

#undef FWD

#ifdef _MSC_VER
  #pragma warning(pop)
#endif
