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

  /// @brief A subrange of active slots in the buffer.
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
                                    elem.release();
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
  /// must be a power of 2 greater than 1, and uses a mask to handle index wrapping. Slots are
  /// aligned to cache lines to reduce false sharing and improve performance in concurrent
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
  ///          Multiple producers can safely call `push()` concurrently.
  /// @example
  /// ```cpp
  /// auto buffer = fho::ring_buffer<>{}; // Uses fast_func_t by default
  /// auto token = buffer.push([]() { std::cout << "Hello, World!\n"; });
  /// auto range = buffer.consume();
  /// for (auto& func : range) {
  ///     func();
  /// }
  /// token.wait(); // Wait for the job to complete
  /// ```
  template<typename T = fast_func_t, std::size_t Capacity = details::default_capacity,
           typename Allocator = aligned_allocator<ring_slot<T>, details::cache_line_size>>
  class ring_buffer
  {
    static constexpr auto index_mask = Capacity - 1u;

    using slot_type             = ring_slot<T>;
    using allocator_type        = Allocator;
    using value_type            = T;
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

    static_assert(
      std::is_same_v<value_type, std::remove_reference_t<decltype(*std::declval<iterator>())>>);
    static_assert(
      std::is_const_v<std::remove_reference_t<decltype(*std::declval<const_iterator>())>>);

    static_assert(Capacity > 1, "capacity must be greater than 1");
    static_assert((Capacity & index_mask) == 0, "capacity must be a power of 2");

#if __cpp_static_assert >= 202306L && __cpp_lib_constexpr_format
    static_assert(sizeof(T) <= details::slot_size,
                  std::format("T (size: {}) does not fit ring buffer slot (free : {}) ", sizeof(T),
                              details::slot_size));
#else
    static_assert(sizeof(T) <= details::slot_size, "T does not fit ring buffer slot");
#endif

    static_assert(sizeof(slot_type) % details::cache_line_size == 0,
                  "Buffer slot size must be a multiple of the cache line size");

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
      , next_(rhs.next_.load(std::memory_order::relaxed))
      , elems_(std::move(rhs.elems_))
    {
      rhs.tail_.store(0, std::memory_order::relaxed);
      rhs.head_.store(0, std::memory_order::relaxed);
      rhs.next_.store(0, std::memory_order::relaxed);
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
      next_  = rhs.next_.load(std::memory_order::relaxed);
      elems_ = std::move(rhs.elems_);
      return *this;
    }

    /// @brief Pushes a value into the buffer with an existing token.
    /// @details Adds a value to the buffer, associating it with a provided token. The process
    ///          involves:
    ///          1. **Claim a slot**: Atomically increments `next_` to reserve a slot.
    ///          2. **Assign the value**: Constructs the value in the slot and sets it active.
    ///          3. **Commit the slot**: Updates `head_` to make the slot available.
    ///          Blocks if the buffer is full until space is freed by the consumer.
    /// @param `token` Token to associate with the slot.
    /// @param `val` Value to add to the buffer.
    /// @param `args` Additional arguments.
    /// @return Reference to the token.
    /// @note Thread-safe for multiple producers.
    /// @example
    /// ```cpp
    /// auto token = fho::slot_token{};
    /// buffer.push(token, []() { std::cout << "Task\n"; });
    /// ```
    auto
    push(slot_token& token, auto&& val, auto&&... args) noexcept -> slot_token&
    {
      // 1. Claim a slot.
      auto const slot = next_.fetch_add(1, std::memory_order_acquire);
      slot_type& elem = elems_[ring_iterator_t::mask(slot)];
      elem.acquire();

      // 2. Assign `value_type`.
      if constexpr (std::invocable<decltype(val), slot_token&, decltype(args)...>)
      {
        elem.emplace(FWD(val), std::ref(token), FWD(args)...);
      }
      else
      {
        elem.emplace(FWD(val), FWD(args)...);
      }

      elem.bind(token);

      // 3. Commit slot.
      index_t expected; // NOLINT
      do
      {
        // Check if full before committing.
        if (auto tail = tail_.load(std::memory_order_acquire);
            ring_iterator_t::mask(slot + 1 - tail) == 0) [[unlikely]]
        {
          tail_.wait(tail, std::memory_order_acquire);
        }
        expected = slot;
      }
      while (!head_.compare_exchange_weak(expected, slot + 1, std::memory_order_release,
                                          std::memory_order_relaxed));
      head_.notify_one();
      return token;
    }

    /// @brief Pushes a value into the buffer and returns a new token.
    /// @details Adds a value to the buffer and returns a new token for tracking.
    /// @param `val` Value to add to the buffer.
    /// @param `args` Additional arguments.
    /// @return New token associated with the slot.
    /// @note Thread-safe for multiple producers.
    /// @example
    /// ```cpp
    /// auto token = buffer.push([]() { std::cout << "Task\n"; });
    /// ```
    auto
    push(auto&& val, auto&&... args) noexcept -> slot_token
    {
      slot_token token;
      (void)push(token, FWD(val), FWD(args)...);
      return token;
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
    /// @details Retrieves a range of values from `tail_` to `head_`, up to a specified maximum.
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
      auto const cap  = tail + std::min<index_t>(max, head - tail); // Cap range size
      auto       b    = ring_iterator_t(elems_.data(), tail);
      auto       e    = ring_iterator_t(elems_.data(), cap);
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
    /// @details Returns `Capacity - 1`, as one slot is reserved to distinguish full from empty.
    /// @return Maximum number of items the buffer can hold.
    static constexpr auto
    max_size() noexcept -> std::size_t
    {
      return Capacity - 1;
    }

    /// @brief Returns the current number of items in the buffer.
    /// @details Computes the masked difference between `head_` and `tail_`.
    /// @return Current number of items.
    auto
    size() const noexcept -> std::size_t
    {
      auto const tail = tail_.load(std::memory_order_acquire);
      auto const head = head_.load(std::memory_order_acquire);
      return ring_iterator_t::mask(head - tail); // circular distance
    }

    /// @brief Checks if the buffer is empty.
    /// @details Returns true if `size() == 0`.
    /// @return True if empty, false otherwise.
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
    /*
      Circular `value_type` buffer. When tail or head
      reaches the end they will wrap around:
       _
      |_|
      |_|
      |_| ┐→ tail  (next claim) - consumer
      |_| │
      |_| │
      |_| │
      |_| │
      |_| ┘→ head-1 (last elem) - consumer
      |_|  ← head   (next slot) - producer
      |_|
    */

    alignas(details::cache_line_size) atomic_index_t tail_{0};
    alignas(details::cache_line_size) atomic_index_t head_{0};
    alignas(details::cache_line_size) atomic_index_t next_{0};

    alignas(details::cache_line_size) std::vector<slot_type, allocator_type> elems_{Capacity};

    /// @brief Pre-created to ensure iterator compatibility.
    /// NOTE: MSVC in debug specifically does not like iterators from temporarily created &
    /// differing ranges
    const_ring_iterator_t const begin_ = const_ring_iterator_t(elems_.data(), 0);          // NOLINT
    const_ring_iterator_t const end_   = const_ring_iterator_t(elems_.data(), max_size()); // NOLINT
    const_transform_type const  constRange_ =                                              // NOLINT
      const_transform_type(std::ranges::subrange(begin_, end_),
                           slot_value_accessor<value_type const>);
  };
}

#undef FWD

#ifdef _MSC_VER
  #pragma warning(pop)
#endif
