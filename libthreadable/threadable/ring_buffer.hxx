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
#include <iterator>
#include <ranges>
#include <type_traits>
#include <vector>

#if defined(_MSC_VER) && !defined(__clang__)
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
  ///       slot to produce into).
  /// @example
  /// ```cpp
  /// auto buffer = fho::ring_buffer<>{}; // Uses fast_func_t by default
  /// auto token = buffer.emplace_back([]() { std::cout << "Hello, World!\n"; });
  /// while (auto t = buffer.try_pop_front())
  /// {
  ///   t(); // prints "Hello, World!"
  /// }
  /// token.wait(); // Wait for a task to complete
  /// ```
  template<typename T = fast_func_t, std::size_t Capacity = details::default_capacity,
           typename Allocator = aligned_allocator<ring_slot<T>, details::cache_line_size>>
  class ring_buffer
  {
    static constexpr auto index_mask = Capacity - 1u;

    static_assert(Capacity > 1, "capacity must be greater than 1");
    static_assert((Capacity & index_mask) == 0, "capacity must be a power of 2");

#if 0
  #if __cpp_static_assert >= 202306L && __cpp_lib_constexpr_format
    static_assert(sizeof(T) <= details::slot_size,
                  std::format("T (size: {}) does not fit ring buffer slot (free : {}) ", sizeof(T),
                              details::slot_size));
  #else
    static_assert(sizeof(T) <= details::slot_size, "T does not fit ring buffer slot");
  #endif
#endif

    using value_type      = T;
    using slot_type       = ring_slot<value_type>;
    using size_type       = std::size_t;
    using reference       = value_type&;
    using const_reference = value_type const&;
    using allocator_type  = Allocator;

    using atomic_index_t        = std::atomic_uint_fast64_t;
    using index_t               = typename atomic_index_t::value_type;
    using ring_iterator_t       = ring_iterator<slot_type, index_mask>;
    using const_ring_iterator_t = ring_iterator<slot_type const, index_mask>;

  public:
    static constexpr auto is_always_lock_free = slot_type::is_always_lock_free;

    using claimed_type = claimed_slot<value_type>;

    using transform_type =
      ring_transform_view<ring_iterator_t, decltype(slot_value_accessor<value_type>)>;
    using const_transform_type =
      ring_transform_view<const_ring_iterator_t, decltype(slot_value_accessor<value_type const>)>;

    using iterator       = std::ranges::iterator_t<transform_type>;       // NOLINT
    using const_iterator = std::ranges::iterator_t<const_transform_type>; // NOLINT

    static_assert(
      std::is_same_v<value_type, std::remove_reference_t<decltype(*std::declval<iterator>())>>,
      "Incorrect dereferenced type of non-const iterator");
    static_assert(
      std::is_const_v<std::remove_reference_t<decltype(*std::declval<const_iterator>())>>,
      "Incorrect dereferenced type of const iterator");

    static_assert(sizeof(slot_type) % details::cache_line_size == 0,
                  "Buffer slot size must be a multiple of the cache line size");

    static constexpr auto
    mask(index_t i) noexcept -> decltype(auto)
    {
      return ring_iterator_t::mask(i);
    }

    static constexpr auto
    epoch_of(index_t i) noexcept -> decltype(auto)
    {
      return ring_iterator_t::epoch_of(i);
    }

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
      tail_  = rhs.tail_.load(std::memory_order::relaxed);
      head_  = rhs.head_.load(std::memory_order::relaxed);
      elems_ = std::move(rhs.elems_);
      return *this;
    }

    /// @brief Constructs a value into the buffer with an existing token.
    /// @details Adds a value to the buffer, associating it with a provided token. The process
    ///          involves:
    ///          1. **Claim a slot**: Atomically increments `head_` to reserve a slot.
    ///          2. **Assign the value**: Constructs the value in the slot and binds the token.
    ///          3. **Commit the slot**: Updates `head_` using a compare-exchange loop to make the
    ///          slot available. Blocks via `tail_.wait()` if the buffer is full until space is
    ///          freed by the consumer, then notifies via `head_.notify_one()`.
    /// @param token Token to associate with the slot.
    /// @param args Arguments to construct the value.
    /// @return Reference to the provided token.
    /// @note Thread-safe for multiple producers.
    /// @example
    /// ```cpp
    /// fho::slot_token token;
    /// buffer.emplace_back(token, []() { std::cout << "Task\n"; });
    /// ```
    template<slot_state Tags = slot_state::invalid, typename... Args>
      requires std::constructible_from<T, Args...>
    auto
    emplace_back(slot_token& token, Args&&... args) noexcept -> slot_token&
    {
      // 1. Claim a slot.
      auto const h   = head_.fetch_add(1, std::memory_order_acquire);
      auto const pos = mask(h);
      slot_type& s   = elems_[pos];

      while (!s.template try_lock<slot_state::empty>(std::memory_order_acq_rel))
      {
        s.template wait<slot_state::empty>(false, std::memory_order_acquire);
      }

      // 2. Assign `value_type`.
      s.emplace(FWD(args)...);
      s.template set<Tags, true>(std::memory_order_relaxed);
      dbg::verify<slot_state::epoch>(s, epoch_of(h) ? slot_state::epoch : slot_state::invalid);

      // NOTE: Bind the token to the expected _old_ state so that it
      //       correctly waits until bits 'locked' and 'ready' are 0.
      s.bind(token, slot_state::locked_ready);
      // NOTE: Always bind _before_ comitting. This is especially
      //       important for repeat_async(token,) + token.wait().
      //       If this is modified, wrap repeat_async() test in
      //       a while(true) and let it spin for a while.
      s.template commit<slot_state::locked_empty>(std::memory_order_acq_rel,
                                                  std::memory_order_acquire);

      head_.notify_one();
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
    template<slot_state Tags = slot_state::invalid, typename... Args>
      requires std::constructible_from<T, Args...>
    auto
    emplace_back(Args&&... args) noexcept -> slot_token
    {
      auto t = slot_token{};
      emplace_back<Tags>(t, FWD(args)...);
      return t;
    }

    /// @brief Accesses the first element in the ring buffer.
    /// @details Returns a const reference to the oldest element (at `tail_`). The buffer must not
    ///          be empty.
    /// @return Const reference to the front element.
    /// @pre `size() > 0`
    [[nodiscard]] inline auto
    front() const noexcept -> const_reference
    {
      assert(size() > 0);
      auto const tail = tail_.load(std::memory_order_acquire);
      return elems_[mask(tail)];
    }

    /// @brief Accesses the last element in the ring buffer.
    /// @details Returns a const reference to the most recently added element (at `head_ - 1`). The
    ///          buffer must not be empty.
    /// @return Const reference to the back element.
    /// @pre `size() > 0`
    [[nodiscard]] inline auto
    back() const noexcept -> const_reference
    {
      assert(size() > 0);
      auto const head = head_.load(std::memory_order_acquire);
      return elems_[mask(head - 1)];
    }

    /// @brief Removes the first element from the ring buffer.
    /// @details Releases the slot at the front (at `tail_`) and advances `tail_`. The buffer must
    ///          not be empty.
    ///          Thread-safe for a single consumer. Does not notify waiters; use a separate
    ///          mechanism if notification is needed.
    /// @pre `size() > 0`
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
        slot_type& elem = elems_[mask(tail)];
        if (elem.template try_lock<slot_state::ready>()) [[likely]]
        {
          // 3. Release tail slot.
          tail_.fetch_add(1, std::memory_order_acq_rel);
          elem.template release<slot_state::locked_ready>();
          break;
        }
        // 4. Somebody already released tail - Retry.
      }
    }

    auto
    try_pop_front() noexcept -> claimed_slot<T>
    {
      auto const t = tail_.load(std::memory_order_acquire);
      auto&      s = elems_[mask(t)];
      if (s.template try_lock<slot_state::ready>()) [[likely]]
      {
        // If insertion required "previous empty" (tag_seq is set)
        // then check if previous slot is set to 'empty' iff in the
        // same epoch.
        if (s.template test<slot_state::tag_seq>(std::memory_order_acquire)) [[unlikely]]
        {
          auto const  ppos = t - 1;
          auto const& p    = elems_[mask(ppos)];

          if (!p.template test<slot_state::empty>(std::memory_order_acquire) &&
              p.template test<slot_state::epoch>(std::memory_order_relaxed) == epoch_of(ppos))
          {
            s.template unlock<slot_state::locked_ready>();
            return {nullptr};
          }
        }
        auto exp = t;
        if (tail_.compare_exchange_strong(exp, t + 1, std::memory_order_acq_rel,
                                          std::memory_order_acquire)) [[likely]]
        {
          return {&s};
        }
        s.template unlock<slot_state::locked_ready>();
      }
      return {nullptr};
    }

    auto
    try_pop_back() noexcept -> claimed_slot<T>
    {
      auto const h = head_.load(std::memory_order_acquire);
      auto const t = h < Capacity ? 0 : h - Capacity;
      for (auto i = h; i > t; --i)
      {
        auto const pos = i - 1;
        auto&      s   = elems_[mask(pos)];

        if (s.template try_lock<slot_state::ready>()) [[likely]]
        {
          // If insertion required "previous empty" (tag_seq is set)
          // then check if previous slot is set to 'empty' iff in the
          // same epoch.
          if (s.template test<slot_state::tag_seq>(std::memory_order_acquire)) [[unlikely]]
          {
            auto const  ppos = pos - 1;
            auto const& p    = elems_[mask(ppos)];

            if (!p.template test<slot_state::empty>(std::memory_order_acquire) &&
                p.template test<slot_state::epoch>(std::memory_order_relaxed) == epoch_of(ppos))
            {
              s.template unlock<slot_state::locked_ready>();
              break;
            }
          }
          return {&s};
        }
      }
      return {nullptr};
    }

    /// @brief Waits until the buffer is empty.
    /// @details Blocks until there are no slots in state `ready`.
    void
    wait() const noexcept
    {
      auto next = elems_.begin();
      while (next != elems_.end())
      {
        auto t = slot_token{};
        next->bind(t, slot_state::locked_ready);
        t.wait();
        next = std::ranges::find_if(elems_,
                                    [](auto const& e)
                                    {
                                      return e.template test<slot_state::ready>(
                                        std::memory_order_acquire);
                                    });
      }
    }

    /// @brief Clears all items from the buffer.
    /// @details Consumes all available items, leaving the buffer empty.
    void
    clear() noexcept
    {
      if (elems_.size() == 0) [[unlikely]]
      {
        return;
      }
      while (auto s = try_pop_front())
        ;
      tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
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
    /// @details Computes the number of items in state `ready` O(N).
    /// @return Current number of items.
    auto
    size() const noexcept -> std::size_t
    {
      if (elems_.size() == 0) [[unlikely]]
      {
        return 0;
      }
      // TODO: Probably just add a size_ atomic and keep track
      //       of that, because this can get very slow.
      auto const s = std::ranges::count_if(elems_,
                                           [](auto const& e)
                                           {
                                             return e.template test<slot_state::ready>(
                                               std::memory_order_acquire);
                                           });
      return s;
    }

    /// @brief Checks if the buffer is empty.
    /// @details Returns true if `size() == 0`.
    /// @return True if empty, false otherwise.
    auto
    empty() const noexcept -> bool
    {
      if (elems_.size() == 0) [[unlikely]]
      {
        return true;
      }
      // TODO: Probably just add a size_ atomic and keep track
      //       of that, because this can get very slow.
      // auto const head = head_.load(std::memory_order_acquire);
      return !std::ranges::any_of(elems_,
                                  [](auto const& e)
                                  {
                                    return e.template test<slot_state::ready>(
                                      std::memory_order_acquire);
                                  });
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

    alignas(details::cache_line_size) std::vector<slot_type, allocator_type> elems_{Capacity};

    /// @brief Pre-created to ensure iterator compatibility.
    /// @note MSVC in debug specifically does not like iterators from temporarily created &
    /// differing ranges
    const_ring_iterator_t const begin_ = const_ring_iterator_t(elems_.data(), 0);          // NOLINT
    const_ring_iterator_t const end_   = const_ring_iterator_t(elems_.data(), max_size()); // NOLINT
    const_transform_type const  constRange_ =                                              // NOLINT
      const_transform_type(std::ranges::subrange(begin_, end_),
                           slot_value_accessor<value_type const>);
  };
}

#undef FWD

#if defined(_MSC_VER) && !defined(__clang__)
  #pragma warning(pop)
#endif
