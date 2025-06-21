#pragma once

#include <threadable/allocator.hxx>
#include <threadable/atomic.hxx>
#include <threadable/job.hxx>
#include <threadable/ring_iterator.hxx>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <execution>
#include <iterator>
#include <ranges>
#include <vector>

#if !defined(__cpp_lib_execution) && !defined(__cpp_lib_parallel_algorithm) && \
  __has_include(<pstld/pstld.h>)
  #ifndef PSTLD_HACK_INTO_STD
    #define PSTLD_HACK_INTO_STD
  #endif
  #include <pstld/pstld.h>
  #undef PSTLD_HACK_INTO_STD
#endif

#if __cpp_lib_execution < 201603L || __cpp_lib_parallel_algorithm < 201603L
  #error requires __cpp_lib_execution & __cpp_lib_parallel_algorithm
#endif

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

namespace fho
{
  namespace details
  {
    constexpr std::size_t default_capacity = 1 << 16;
  }

  enum class execution
  {
    sequential,
    parallel
  };

  template<std::size_t capacity = details::default_capacity>
  class ring_buffer
  {
    using value_type                    = job;
    using atomic_index_t                = std::atomic_size_t;
    using index_t                       = typename atomic_index_t::value_type;
    static constexpr auto index_mask    = capacity - 1u;
    static constexpr auto null_callback = [](ring_buffer&) {};

    static_assert(capacity > 1, "capacity must be greater than 1");
    static_assert((capacity & index_mask) == 0, "capacity must be a power of 2");

  public:
    using iterator       = ring_iterator<value_type, index_mask>;       // NOLINT
    using const_iterator = ring_iterator<value_type const, index_mask>; // NOLINT
    static_assert(std::is_const_v<typename const_iterator::value_type>);
    static_assert(std::is_const_v<std::remove_reference_t<typename const_iterator::reference>>);
    static_assert(std::is_const_v<std::remove_pointer_t<typename const_iterator::pointer>>);

    // Make sure iterator is valid for parallelization with the standard algorithms
    static_assert(std::random_access_iterator<iterator>);
    static_assert(std::contiguous_iterator<iterator>);

    ring_buffer(ring_buffer const&) = delete;
    ~ring_buffer()                  = default;

    ring_buffer(execution policy = execution::parallel) noexcept
      : policy_(policy)
    {}

    ring_buffer(ring_buffer&& rhs) noexcept
      : policy_(std::move(rhs.policy_))
      , tail_(rhs.tail_.load(std::memory_order::relaxed))
      , head_(rhs.head_.load(std::memory_order::relaxed))
      , next_(rhs.next_.load(std::memory_order::relaxed))
      , elems_(std::move(rhs.elems_))
    {
      rhs.tail_.store(0, std::memory_order::relaxed);
      rhs.head_.store(0, std::memory_order::relaxed);
      rhs.next_.store(0, std::memory_order::relaxed);
    }

    auto operator=(ring_buffer const&) -> ring_buffer& = delete;

    auto
    operator=(ring_buffer&& rhs) noexcept -> ring_buffer&
    {
      tail_   = rhs.tail_.load(std::memory_order::relaxed);
      head_   = rhs.head_.load(std::memory_order::relaxed);
      next_   = rhs.next_.load(std::memory_order::relaxed);
      policy_ = std::move(rhs.policy_);
      elems_  = std::move(rhs.elems_);
      return *this;
    }

    template<std::copy_constructible callable_t, typename... arg_ts>
      requires std::invocable<callable_t, arg_ts...> ||
               std::invocable<callable_t, job_token&, arg_ts...>
    auto
    push(job_token& token, callable_t&& func, arg_ts&&... args) noexcept -> job_token&
    {
      // 1. Claim a slot.
      auto const slot = next_.fetch_add(1, std::memory_order_relaxed);

      auto& elem = elems_[iterator::mask(slot)];
      auto  exp  = job_state::empty;
      while (!elem.state.compare_exchange_weak(exp, job_state::claimed, std::memory_order_release,
                                               std::memory_order_relaxed)) [[likely]]
      {
        exp = job_state::empty;
      }

      // Wait if slot is occupied.
      if (fho::details::test<job_state::active>(elem.state)) [[unlikely]]
      {
        fho::details::wait<job_state::active, true>(elem.state);
      }

      // 2. Assign `value_type`.
      if constexpr (std::invocable<callable_t, job_token&, arg_ts...>)
      {
        elem.assign(FWD(func), std::ref(token), FWD(args)...);
      }
      else
      {
        elem.assign(FWD(func), FWD(args)...);
      }

      assert(elem);

      token.reassign(elem.state);

      // Check if full before comitting.
      if (auto tail = tail_.load(std::memory_order_relaxed); iterator::mask(slot + 1 - tail) == 0)
        [[unlikely]]
      {
        tail_.wait(tail, std::memory_order_relaxed);
      }

      // 3. Commit slot.
      auto expected = slot;
      while (!head_.compare_exchange_weak(expected, slot + 1, std::memory_order_release,
                                          std::memory_order_relaxed)) [[likely]]
      {
        expected = slot;
      }
      head_.notify_one();
      return token;
    }

    template<std::copy_constructible callable_t, typename... arg_ts>
      requires std::invocable<callable_t, arg_ts...>
    auto
    push(callable_t&& func, arg_ts&&... args) noexcept -> job_token
    {
      job_token token;
      push(token, FWD(func), FWD(args)...);
      return token;
    }

    void
    wait() const noexcept
    {
      auto const tail = tail_.load(std::memory_order_relaxed);
      auto const head = head_.load(std::memory_order_relaxed);
      if (iterator::mask(head - tail) == 0) [[unlikely]]
      {
        head_.wait(head, std::memory_order_relaxed);
      }
    }

    auto
    consume(std::size_t max = capacity) noexcept -> std::ranges::subrange<iterator>
    {
      auto const tail = tail_.load(std::memory_order_acquire);
      auto const head = head_.load(std::memory_order_acquire);
      auto       b    = iterator(elems_.data(), tail);
      auto       e    = iterator(elems_.data(), head);
      tail_.store(head, std::memory_order_release);
      // NOTE: Intentionally not notifying here since we
      //       use spin-locks for better performance.
      // tail_.notify_all();
      return std::ranges::subrange(b, e);
    }

    void
    clear() noexcept
    {
      auto range = consume();
      std::for_each(std::execution::par, std::begin(range), std::end(range),
                    [](value_type& elem)
                    {
                      elem.reset();
                    });
    }

    auto
    begin() const noexcept -> const_iterator
    {
      auto const tail = tail_.load(std::memory_order_acquire);
      return const_iterator(elems_.data(), tail);
    }

    auto
    end() const noexcept -> const_iterator
    {
      auto const head = head_.load(std::memory_order_acquire);
      return begin() + head;
    }

    auto
    execute(std::ranges::range auto r) const -> std::size_t
    {
      assert(r.data() >= jobs_.data() && r.data() <= jobs_.data() + jobs_.size());

      auto const b = std::begin(r);
      auto const e = std::end(r);
      if (policy_ == execution::parallel) [[likely]]
      {
        std::for_each(std::execution::par, b, e,
                      [](value_type& elem)
                      {
                        elem();
                      });
      }
      else [[unlikely]]
      {
        // Make sure previous job has been executed, where
        // `b-1` is `e` if `r` wraps around, or active if it's
        // already consumed but being processed by another
        // thread.
        auto const prev = b - 1;
        if ((prev != e) &&
            fho::details::test<job_state::active>(prev->state, std::memory_order_relaxed))
          [[unlikely]]
        {
          fho::details::wait<job_state::active, true>(prev->state);
        }
        std::for_each(b, e,
                      [](value_type& elem)
                      {
                        elem();
                      });
      }
      return r.size();
    }

    auto
    execute(std::size_t max = capacity) -> std::size_t
    {
      assert(max > 0);
      return execute(consume(max));
    }

    static constexpr auto
    max_size() noexcept -> std::size_t
    {
      return capacity - 1;
    }

    auto
    size() const noexcept -> std::size_t
    {
      auto const tail = tail_.load(std::memory_order_relaxed);
      auto const head = head_.load(std::memory_order_relaxed);
      return iterator::mask(head - tail); // circular distance
    }

    auto
    empty() const noexcept -> bool
    {
      return size() == 0;
    }

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

    alignas(details::cache_line_size) execution policy_ = execution::parallel;
    alignas(details::cache_line_size) atomic_index_t tail_{0};
    alignas(details::cache_line_size) atomic_index_t head_{0};
    alignas(details::cache_line_size) atomic_index_t next_{0};

    alignas(details::cache_line_size)
      std::vector<value_type, aligned_allocator<value_type, details::cache_line_size>> elems_{
        capacity};
  };
}

#undef FWD
