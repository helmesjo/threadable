#pragma once

#include <threadable/allocator.hxx>
#include <threadable/atomic.hxx>
#include <threadable/function.hxx>
#include <threadable/ring_iterator.hxx>
#include <threadable/ring_slot.hxx>
#include <threadable/token.hxx>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <ranges>
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

  /// @brief A subrange of slots `ready` in the buffer.
  /// @details Represents a range of consumed slots ready for processing. It manages the
  ///          lifetime of these slots, releasing them when the subrange is destroyed to prevent
  ///          reuse before processing is complete. Uses a `shared_ptr` with a static dummy
  ///          object to handle cleanup without extra allocation.
  /// @tparam `Iterator` The iterator type (`ring_iterator_t` or `const_ring_iterator_t`).
  template<typename Iterator, typename Slot>
  class active_subrange final : public std::ranges::subrange<Iterator>
  {
  public:
    using base_t = std::ranges::subrange<Iterator>;

    using iterator        = Iterator; // NOLINT
    using value_type      = typename iterator::value_type;
    using difference_type = typename iterator::difference_type;

    using base_t::base_t;

    using base_t::advance;
    using base_t::back;
    using base_t::begin;
    using base_t::empty;
    using base_t::end;
    using base_t::front;
    using base_t::next;
    using base_t::prev;
    using base_t::size;

    active_subrange(active_subrange const&) noexcept = delete;
    active_subrange(active_subrange&& that) noexcept = default;
    ~active_subrange()                               = default;

    active_subrange(base_t&& that) noexcept
      : base_t(std::move(that))
    {}

    auto operator=(active_subrange const&) noexcept -> active_subrange& = delete;
    auto operator=(active_subrange&& that) noexcept -> active_subrange& = default;

  private:
    /// @brief Static dummy object and `shared_ptr` for reference counting to trigger cleanup.
    /// @details Uses a static `dummy` char as a placeholder to avoid memory allocation. The
    ///          `shared_ptr` tracks references, and its custom deleter releases all slots in
    ///          the range when the last reference is destroyed.
    inline static char    dummy = 1;
    std::shared_ptr<void> resetter_ =
      std::shared_ptr<void>(&dummy,
                            [b = base_t::begin().index(), e = base_t::end().index(),
                             d = base_t::begin().data()](void*)
                            {
                              if constexpr (!std::is_const_v<value_type>)
                              {
                                if (b < e)
                                {
                                  for (auto i = b; i < e; ++i)
                                  {
                                    Slot& elem = d[Iterator::mask(i)];
                                    elem.template release<slot_state::ready>();
                                  }
                                }
                              }
                            });
  };

  /// @brief A `fho::function` alias optimized to cache line size for use within a `ring_buffer`.
  /// @details The size of the function object is exactly that of the target system's (deduced)
  ///          cache line size minus `1` byte reserved for the `ring_slot` state handling.
  using fast_func_t = function<details::slot_size>;

  /// @brief A Multi-Producer Single-Consumer (MPSC) ring buffer for managing objects in a
  /// threading environment.
  /// @details This class provides a lock-free ring buffer that allows multiple producers to add
  /// objects concurrently and a single consumer to remove them. It uses atomic operations to
  /// manage the buffer's state, ensuring thread safety. The buffer has a fixed capacity, which
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
  /// @warning Only one consumer should call `consume()` at a time to ensure thread safety.
  ///          Multiple producers can safely call `emplace_back()` concurrently.
  /// @example
  /// Logical view; actual indices unbounded, masked for array access.
  /// When tail or head reaches the end they will wrap around.
  /// ```cpp
  /// // Logical view; actual indices unbounded, masked for array access.
  /// // When tail or head reaches the end they will wrap around.
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
  /// auto range = buffer.consume();
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

  private:
    static constexpr auto index_mask = Capacity - 1u;

    using slot_type    = ring_slot<value_type>;
    using claimed_type = claimed_slot<value_type>;

    using atomic_index_t        = std::atomic_uint_fast64_t;
    using index_t               = typename atomic_index_t::value_type;
    using ring_iterator_t       = ring_iterator<slot_type, index_mask>;
    using const_ring_iterator_t = ring_iterator<slot_type const, index_mask>;

  public:
    using subrange_type = decltype(active_subrange<ring_iterator_t, slot_type>() |
                                   std::views::transform(slot_value_accessor<value_type>));

    using transform_type =
      ring_transform_view<ring_iterator_t, decltype(slot_value_accessor<value_type>)>;
    using const_transform_type =
      ring_transform_view<const_ring_iterator_t, decltype(slot_value_accessor<value_type const>)>;

    using iterator       = std::ranges::iterator_t<transform_type>;       // NOLINT
    using const_iterator = std::ranges::iterator_t<const_transform_type>; // NOLINT

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
      (void)consume();
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
      tail_  = rhs.tail_.load(std::memory_order::relaxed);
      head_  = rhs.head_.load(std::memory_order::relaxed);
      elems_ = std::move(rhs.elems_);
      return *this;
    }

    /// @brief Constructs a value into the buffer with an existing token.
    /// @details Adds a value to the buffer, associating it with a provided token. The process
    ///          involves:
    ///          1. **Claim a slot**: Atomically increments `head_` & acquire the slot.
    //              Blocks via `tail_.wait()` if the buffer is full until space is freed by the
    //              consumer.
    ///          2. **Assign the value**: Constructs the value in the slot and binds the token.
    ///          3. **Commit the slot**: Updates slot `state` to make the slot available, then
    ///             notifies via `head_.notify_one()`.
    /// @param token Token to associate with the slot.
    /// @param args Arguments to construct the value.
    /// @return Reference to the provided token.
    /// @note Thread-safe for multiple producers.
    /// @example
    /// ```cpp
    /// fho::slot_token token;
    /// buffer.emplace_back(token, []() { std::cout << "Task\n"; });
    /// ```
    template<typename... Args>
      requires std::constructible_from<T, Args...>
    auto
    emplace(slot_token& token, Args&&... args) noexcept -> slot_token&
    {
      // 1. Claim a slot

      // @NOTE: Relaxed fetch-add mem order ok,
      //        it's followed by slot acquire.
      auto const slot = head_.fetch_add(1, std::memory_order_relaxed);
      slot_type& elem = elems_[ring_iterator_t::mask(slot)];
      elem.template wait<slot_state::ready | slot_state::claimed>(std::memory_order_acquire);

      assert(elem.template test<slot_state::empty>() and "ring_buffer::emplace_back()");
      elem.claim(slot_state::empty);

      // 2. Assign/Commit `value_type`.
      elem.emplace(FWD(args)...);
      elem.bind(token);

      // 3. Notify slot.
      head_.notify_all();

      return token;
    }

    /// @brief Constructs a value into the buffer and returns a new token.
    /// @details Adds a value to the buffer by creating a new token and delegating to the
    ///          token-based overload.
    /// @param `args` Arguments to construct the value.
    /// @return New token associated with the slot.
    /// @note Thread-safe for multiple producers.
    /// @example
    /// ```cpp
    /// auto token = buffer.emplace_back([]() { std::cout << "Task\n"; });
    /// ```
    template<typename... Args>
      requires std::constructible_from<T, Args...>
    auto
    emplace(Args&&... args) noexcept -> slot_token
    {
      slot_token token;
      (void)emplace(token, FWD(args)...);
      return token;
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
    ///          Thread-safe for multiple consumers. Does not notify waiters; use a separate
    ///          mechanism if notification is needed.
    /// @pre `size() > 0`
    /// @note Popping beyond emplaced items results in undefined behavior.
    void
    pop() noexcept
    {
      assert(size() > 0 and "ring_buffer::pop()");
      index_t tail; // NOLINT
      while (true)
      {
        // 1. Read tail slot.
        tail = tail_.load(std::memory_order_acquire);
        // 2. Acquire tail slot.
        slot_type& elem = elems_[ring_iterator_t::mask(tail)];
        if (elem.try_claim()) [[likely]]
        {
          // 3. Release tail slot.
          elem.template release<slot_state::claimed>();
          auto expected = tail;
          while (!tail_.compare_exchange_weak(expected, tail + 1, std::memory_order_acq_rel,
                                              std::memory_order_acquire))
          {
            expected = tail;
          }
          tail_.notify_all();
          break;
        }
        // 4. Somebody already released tail - Retry.
      }
    }

    /// @brief Claims the first element from the ring buffer.
    /// @details Claims the item at `front()` and advances `tail_`.
    /// The claimed slot is released when it goes out of scope.
    /// Thread-safe for multiple consumers.
    /// @note: Makes only a single attempt to claim next item in line.
    auto
    try_pop() noexcept -> claimed_type
    {
      auto  tail = tail_.load(std::memory_order_acquire);
      auto& elem = elems_[ring_iterator_t::mask(tail)];
      // 1. If element exists, try to claim it & advance tail.
      if (elem.try_claim()) [[likely]]
      {
        if (tail_.compare_exchange_strong(tail, tail + 1, std::memory_order_acq_rel)) [[likely]]
        {
          // Success: own the slot, safe to lend (read value)
          return claimed_type{&elem};
        }
        else [[unlikely]]
        {
          // Fail: contention lost, return slot
          elem.undo_claim();
        }
      }
      return nullptr;
    }

    /// @brief
    /// @details
    auto
    try_steal() noexcept -> claimed_type
    {
      auto  head = head_.load(std::memory_order_acquire);
      auto& elem = elems_[ring_iterator_t::mask(head - 1)];
      // 1. If element exists, try to claim it & recede head.
      if (elem.try_claim()) [[likely]]
      {
        if (head_.compare_exchange_strong(head, head - 1, std::memory_order_acq_rel)) [[likely]]
        {
          // Success: own the slot, safe to access (read value)
          return claimed_type{&elem};
        }
        else [[unlikely]]
        {
          // Fail: contention lost, return slot
          elem.undo_claim();
        }
      }
      return nullptr;
    }

    /// @brief Waits until the buffer has items available.
    /// @details Blocks until `head_ > tail_`, indicating items are ready for consumption.
    void
    wait() const noexcept
    {
      auto const tail = tail_.load(std::memory_order_acquire);
      auto const head = head_.load(std::memory_order_acquire);
      if (ring_iterator_t::mask(head - tail) == 0) [[unlikely]]
      {
        head_.wait(head, std::memory_order_acquire);
      }
    }

    /// @brief Consumes a range of items from the buffer.
    /// @details Retrieves a range of values from `front()` to `end()` (or `max`).
    ///          Returns a `subrange_type` that must be processed within its lifetime; slots are
    ///          released when the subrange is destroyed.
    /// @param `max` Maximum number of items to consume. Defaults to `max_size()`.
    /// @return Subrange of consumed values.
    /// @warning Only one consumer should call this at a time to avoid race conditions.
    /// @example
    /// ```cpp
    /// auto range = buffer.consume(10);
    /// for (auto& value : range) { /* Process value */ }
    /// ```
    auto
    consume(index_t max = max_size()) noexcept
    {
      auto const tail = tail_.load(std::memory_order_acquire);
      auto const head = head_.load(std::memory_order_acquire);
      auto       cap  = tail;

      // @NOTE: Forward-scan until last one ready within cap.
      while (cap < head && (cap - tail) < max)
      {
        auto& elem = elems_[ring_iterator_t::mask(cap)];
        if (!elem.template test<slot_state::ready>(std::memory_order_acquire))
        {
          break;
        }
        ++cap;
      }

      auto b = ring_iterator_t(elems_.data(), tail);
      auto e = ring_iterator_t(elems_.data(), cap);
      tail_.store(cap, std::memory_order_release);
      tail_.notify_all();
      return subrange_type(std::ranges::subrange(b, e), slot_value_accessor<value_type>);
    }

    /// @brief Clears all items from the buffer.
    /// @details Consumes all available items, leaving the buffer empty.
    void
    clear() noexcept
    {
      (void)consume();
    }

    /// @brief Returns a const iterator to the buffer's start.
    /// @details Points to the first consumable item at `tail_`. Returns a const iterator due to
    ///          single-consumer design.
    /// @return Const iterator to the start of the buffer.
    auto
    begin() const noexcept
    {
      auto const tail = tail_.load(std::memory_order_acquire);
      return std::ranges::next(constRange_.begin(), tail);
    }

    /// @brief Returns a const iterator to the buffer's end.
    /// @details Points past the last consumable item at `head_`.
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

    /// @brief Returns the current number of items in the buffer.
    /// @details Computes the direct (unbounded) difference between `head_` and `tail_`.
    /// @return Current number of items.
    auto
    size() const noexcept -> std::size_t
    {
      auto const tail = tail_.load(std::memory_order_acquire);
      auto const head = head_.load(std::memory_order_acquire);
      return head - tail; // circular distance
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
