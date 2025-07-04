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

#ifdef _WIN32
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
  /// @details The size of the function object is exactly that of the target systems (deduced)
  ///          cache line size minus `1` byte reserved for the `ring_slot` state handling.
  using fast_func_t = function<details::slot_size>;

  /// @brief A Multi-Producer Single-Consumer (MPSC) ring buffer for managing objects in a
  /// threading environment.
  /// @details This class provides a lock-free ring buffer that allows multiple producers to add
  /// objects concurrently and a single consumer to consume. It uses atomic
  /// operations to manage the buffer's state. The buffer is templated on its capacity, which must
  /// be a power of 2 and greater than 1.
  /// @tparam `T` The value type.
  /// @tparam `Capacity` The size of the ring buffer, must be a power of 2 and greater than 1.
  /// Defaults to 65536 (`1 << 16`).
  /// @tparam `Allocator` The allocator type.
  /// @example
  /// ```cpp
  /// auto buffer = fho::ring_buffer<int>{};
  /// buffer.push(1);
  /// auto range = buffer.consume();
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
    template<typename Iterator>
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
      /// @details This is a hack to use `shared_ptr<void>` for reference counting but without
      /// allocating memory. The static `dummy` char serves as a valid, non-owned placeholder
      /// with program-long lifetime. The `shared_ptr`'s control block tracks references, and the
      /// custom deleter runs when the last instance is destroyed, releasing all `slot_type`
      /// elements in the range.
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
                                      slot_type& elem = d[ring_iterator_t::mask(i)];
                                      elem.release();
                                    }
                                  }
                                }
                              });
    };

    inline static constexpr auto value_accessor =
      [](auto&& a) -> std::add_lvalue_reference_t<value_type>
    {
      return FWD(a).value();
    };

    inline static constexpr auto const_value_accessor =
      [](auto&& a) -> std::add_lvalue_reference_t<std::add_const_t<value_type>>
    {
      return FWD(a).value();
    };

    using transform_type =
      std::ranges::transform_view<std::ranges::subrange<ring_iterator_t>, decltype(value_accessor)>;
    using const_transform_type =
      std::ranges::transform_view<std::ranges::subrange<const_ring_iterator_t>,
                                  decltype(const_value_accessor)>;

    using subrange_type =
      decltype(active_subrange<ring_iterator_t>() | std::views::transform(value_accessor));

    using const_subrange_type = decltype(active_subrange<const_ring_iterator_t>() |
                                         std::views::transform(const_value_accessor));

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
    /// @details Initializes the (pre-allocated) ring buffer.
    ring_buffer() noexcept          = default;
    ring_buffer(ring_buffer const&) = delete;

    ~ring_buffer()
    {
      std::ignore = consume();
    }

    /// @brief Move constructor.
    /// @details Moves the contents of another ring buffer into this one.
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
    /// @details Moves the contents of another ring buffer into this one.
    /// @param `rhs` The ring buffer to move from.
    /// @return A reference to this ring buffer.
    auto
    operator=(ring_buffer&& rhs) noexcept -> ring_buffer&
    {
      tail_  = rhs.tail_.load(std::memory_order::relaxed);
      head_  = rhs.head_.load(std::memory_order::relaxed);
      next_  = rhs.next_.load(std::memory_order::relaxed);
      elems_ = std::move(rhs.elems_);
      return *this;
    }

    /// @brief Pushes a value into the ring buffer with a token.
    /// @details Adds a callable to the buffer, associating it with a token for state
    /// monitoring.
    /// @tparam `Func` The type of the callable.
    /// @tparam `Args` The types of the arguments.
    /// @param `token` The token to associate with the slot.
    /// @param `func` The callable to add to the buffer.
    /// @param `args` The arguments to pass to the callable.
    /// @return A reference to the token.
    /// @example
    /// ```cpp
    /// auto token = fho::slot_token{};
    /// buffer.push(token, val);
    /// ```
    template<std::move_constructible Func, typename... Args>
      requires std::invocable<Func, Args...> || std::invocable<Func, slot_token&, Args...>
    auto
    push(slot_token& token, Func&& func, Args&&... args) noexcept -> slot_token&
    {
      // 1. Claim a slot.
      auto const slot = next_.fetch_add(1, std::memory_order_acquire);
      slot_type& elem = elems_[ring_iterator_t::mask(slot)];
      elem.acquire();

      // 2. Assign `value_type`.
      if constexpr (std::invocable<Func, slot_token&, Args...>)
      {
        elem.assign(FWD(func), std::ref(token), FWD(args)...);
      }
      else
      {
        elem.assign(FWD(func), FWD(args)...);
      }

      elem.token(token);

      // 3. Commit slot.
      index_t expected; // NOLINT
      do
      {
        // Check if full before comitting.
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

    /// @brief Pushes a value into the ring buffer.
    /// @details Adds a callable to the buffer and returns a new token.
    /// @tparam `Func` The type of the callable.
    /// @tparam `Args` The types of the arguments.
    /// @param `func` The callable to add to the buffer.
    /// @param `args` The arguments to pass to the callable.
    /// @return A new token for the acquired slot.
    /// @example
    /// ```cpp
    /// auto token = buffer.push(val);
    /// ```
    template<std::move_constructible Func, typename... Args>
      requires std::invocable<Func, Args...> || std::invocable<Func, slot_token&, Args...>
    auto
    push(Func&& func, Args&&... args) noexcept -> slot_token
    {
      slot_token token;
      std::ignore = push(token, FWD(func), FWD(args)...);
      return token;
    }

    /// @brief Waits until there are values available in the buffer.
    /// @details Blocks until the buffer is not empty.
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

    /// @brief Consumes values from the buffer.
    /// @details Retrieves a range of values from the buffer for execution.
    /// @param `max` The maximum number of values to consume. Defaults to `max_size()`.
    /// @return A subrange pointing to the consumed values.
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
      return subrange_type(std::ranges::subrange(b, e), value_accessor);
    }

    /// @brief Clears all values from the buffer.
    /// @details Resets all values in the buffer to an empty state.
    void
    clear() noexcept
    {
      std::ignore = consume();
    }

    /// @brief Returns an iterator to the beginning (tail) of the buffer.
    /// @details Provides access to the first value in the buffer.
    /// @return A const iterator to the beginning of the buffer.
    auto
    begin() const noexcept
    {
      auto const tail = tail_.load(std::memory_order_acquire);
      return std::ranges::next(constRange_.begin(), tail);
    }

    /// @brief Returns an iterator to the end (head) of the buffer.
    /// @details Provides access to the position after the last value in the buffer.
    /// @return A const iterator to the end of the buffer.
    auto
    end() const noexcept
    {
      auto const head = head_.load(std::memory_order_acquire);
      return std::ranges::next(constRange_.begin(), head);
    }

    /// @brief Returns the maximum size of the buffer.
    /// @details The maximum number of values the buffer can hold, which is `capacity - 1`.
    /// @return The maximum size of the buffer.
    static constexpr auto
    max_size() noexcept -> std::size_t
    {
      return Capacity - 1;
    }

    /// @brief Returns the current number of values in the buffer.
    /// @details Calculates the number of values currently in the buffer.
    /// @return The number of values in the buffer.
    auto
    size() const noexcept -> std::size_t
    {
      auto const tail = tail_.load(std::memory_order_acquire);
      auto const head = head_.load(std::memory_order_acquire);
      return ring_iterator_t::mask(head - tail); // circular distance
    }

    /// @brief Checks if the buffer is empty.
    /// @details Returns true if there are no values in the buffer, false otherwise.
    /// @return True if the buffer is empty, false otherwise.
    auto
    empty() const noexcept -> bool
    {
      return size() == 0;
    }

    /// @brief Returns a pointer to the underlying data.
    /// @details Provides direct access to the buffer's data.
    /// @return A pointer to the first element in the buffer.
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
      const_transform_type(std::ranges::subrange(begin_, end_), const_value_accessor);
  };
}

#undef FWD

#ifdef _WIN32
  #pragma warning(pop)
#endif
