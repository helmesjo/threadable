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

  /// @brief A Multi-Producer Single-Consumer (MPSC) ring buffer for managing jobs in a threading
  /// environment.
  /// @details This class provides a lock-free ring buffer that allows multiple producers to add
  /// jobs concurrently and a single consumer to consume & execute them. It uses atomic operations
  /// to manage the buffer's state and supports both sequential and parallel execution of jobs based
  /// on the specified policy. The buffer is templated on its capacity, which must be a power of 2
  /// and greater than 1.
  /// @tparam `capacity` The size of the ring buffer, must be a power of 2 and greater than 1.
  /// Defaults to 65536 (`1 << 16`).
  /// @example
  /// ```cpp
  /// auto buffer = fho::ring_buffer<>{fho::execution::parallel};
  /// buffer.push([]() { cout << "Job executed!\n"; });
  /// auto range = buffer.consume();
  /// buffer.execute(range);
  /// ```
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
    /// @brief Default constructor.
    /// @details Initializes the ring buffer with the specified execution policy. Defaults to
    /// parallel execution.
    /// @param `policy` The execution policy for job execution. Can be `execution::sequential` or
    /// `execution::parallel`.
    ring_buffer(ring_buffer const&) = delete;
    ~ring_buffer()                  = default;

    ring_buffer(execution policy = execution::parallel) noexcept
      : policy_(policy)
    {}

    /// @brief Move constructor.
    /// @details Moves the contents of another ring buffer into this one.
    /// @param `rhs` The ring buffer to move from.
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

    /// @brief Move assignment operator.
    /// @details Moves the contents of another ring buffer into this one.
    /// @param `rhs` The ring buffer to move from.
    /// @return A reference to this ring buffer.
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

    /// @brief Pushes a job into the ring buffer with a job token.
    /// @details Adds a callable to the buffer, associating it with a job token for state
    /// monitoring.
    /// @tparam `callable_t` The type of the callable.
    /// @tparam `arg_ts` The types of the arguments.
    /// @param `token` The job token to associate with the job.
    /// @param `func` The callable to add to the buffer.
    /// @param `args` The arguments to pass to the callable.
    /// @return A reference to the job token.
    /// @example
    /// ```cpp
    /// auto token = fho::job_token{};
    /// buffer.push(token, []() { cout << "Job executed!\n"; });
    /// ```
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
      if (elem.state.template test<job_state::active>()) [[unlikely]]
      {
        elem.state.template wait<job_state::active, true>();
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

    /// @brief Pushes a job into the ring buffer.
    /// @details Adds a callable to the buffer and returns a new job token.
    /// @tparam `callable_t` The type of the callable.
    /// @tparam `arg_ts` The types of the arguments.
    /// @param `func` The callable to add to the buffer.
    /// @param `args` The arguments to pass to the callable.
    /// @return A new job token for the added job.
    /// @example
    /// ```cpp
    /// auto token = buffer.push([]() { cout << "Job executed!\n"; });
    /// ```
    template<std::copy_constructible callable_t, typename... arg_ts>
      requires std::invocable<callable_t, arg_ts...>
    auto
    push(callable_t&& func, arg_ts&&... args) noexcept -> job_token
    {
      job_token token;
      push(token, FWD(func), FWD(args)...);
      return token;
    }

    /// @brief Waits until there are jobs available in the buffer.
    /// @details Blocks until the buffer is not empty.
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

    /// @brief Consumes jobs from the buffer.
    /// @details Retrieves a range of jobs from the buffer for execution.
    /// @param `max` The maximum number of jobs to consume. Defaults to the buffer capacity.
    /// @return A subrange of iterators pointing to the consumed jobs.
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

    /// @brief Clears all jobs from the buffer.
    /// @details Resets all jobs in the buffer to an empty state.
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

    /// @brief Returns an iterator to the beginning (tail) of the buffer.
    /// @details Provides access to the first job in the buffer.
    /// @return A const iterator to the beginning of the buffer.
    auto
    begin() const noexcept -> const_iterator
    {
      auto const tail = tail_.load(std::memory_order_acquire);
      return const_iterator(elems_.data(), tail);
    }

    /// @brief Returns an iterator to the end (head) of the buffer.
    /// @details Provides access to the position after the last job in the buffer.
    /// @return A const iterator to the end of the buffer.
    auto
    end() const noexcept -> const_iterator
    {
      auto const head = head_.load(std::memory_order_acquire);
      return begin() + head;
    }

    /// @brief Executes a range of jobs.
    /// @details Executes the jobs in the provided range, either sequentially or in parallel based
    /// on the execution policy.
    /// @tparam `R` The type of the range.
    /// @param `r` The range of jobs to execute.
    /// @return The number of jobs executed.
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
        if ((prev != e) && prev->state.template test<job_state::active>(std::memory_order_relaxed))
          [[unlikely]]
        {
          prev->state.template wait<job_state::active, true>();
        }
        std::for_each(b, e,
                      [](value_type& elem)
                      {
                        elem();
                      });
      }
      return r.size();
    }

    /// @brief Executes up to a specified number of jobs.
    /// @details Consumes and executes up to `max` jobs from the buffer.
    /// @param max The maximum number of jobs to execute. Defaults to the buffer capacity.
    /// @return The number of jobs executed.
    /// @example
    /// ```cpp
    /// buffer.execute(10); // Execute up to 10 jobs
    /// ```
    auto
    execute(std::size_t max = capacity) -> std::size_t
    {
      assert(max > 0);
      return execute(consume(max));
    }

    /// @brief Returns the maximum size of the buffer.
    /// @details The maximum number of jobs the buffer can hold, which is `capacity - 1`.
    /// @return The maximum size of the buffer.
    static constexpr auto
    max_size() noexcept -> std::size_t
    {
      return capacity - 1;
    }

    /// @brief Returns the current number of jobs in the buffer.
    /// @details Calculates the number of jobs currently in the buffer.
    /// @return The number of jobs in the buffer.
    auto
    size() const noexcept -> std::size_t
    {
      auto const tail = tail_.load(std::memory_order_relaxed);
      auto const head = head_.load(std::memory_order_relaxed);
      return iterator::mask(head - tail); // circular distance
    }

    /// @brief Checks if the buffer is empty.
    /// @details Returns true if there are no jobs in the buffer, false otherwise.
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
