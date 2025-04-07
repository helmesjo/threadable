#pragma once

#include <threadable/allocator.hxx>
#include <threadable/circular_iterator.hxx>
#include <threadable/job.hxx>

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

namespace threadable
{
  namespace details
  {
    constexpr std::size_t default_max_nr_of_jobs = 1 << 16;
  }

  enum class execution_policy
  {
    sequential,
    parallel
  };

  template<std::size_t max_nr_of_jobs = details::default_max_nr_of_jobs>
  class queue
  {
    using atomic_index_t                = std::atomic_size_t;
    using index_t                       = typename atomic_index_t::value_type;
    static constexpr auto index_mask    = max_nr_of_jobs - 1u;
    static constexpr auto null_callback = [](queue&) {};

    static_assert(max_nr_of_jobs > 1, "number of jobs must be greater than 1");
    static_assert((max_nr_of_jobs & index_mask) == 0, "number of jobs must be a power of 2");
#ifdef __cpp_lib_constexpr_cmath
    static_assert(max_nr_of_jobs <= std::pow(2, (8 * sizeof(index_t)) - 1),
                  "number of jobs must be <= half the index range");
#endif

  public:
    using iterator       = circular_iterator<job, index_mask>;       // NOLINT
    using const_iterator = circular_iterator<job const, index_mask>; // NOLINT
    static_assert(std::is_const_v<typename const_iterator::value_type>);
    static_assert(std::is_const_v<std::remove_reference_t<typename const_iterator::reference>>);
    static_assert(std::is_const_v<std::remove_pointer_t<typename const_iterator::pointer>>);

    // Make sure iterator is valid for parallelization with the standard algorithms
    static_assert(std::random_access_iterator<iterator>);
    static_assert(std::contiguous_iterator<iterator>);

    using function_t = function<details::job_buffer_size>;

    queue(queue const&) = delete;
    ~queue()            = default;

    queue(execution_policy policy = execution_policy::parallel) noexcept
      : policy_(policy)
    {}

    queue(queue&& rhs) noexcept
      : policy_(std::move(rhs.policy_))
      , tail_(std::move(rhs.tail_))
      , head_(rhs.head_.load(std::memory_order::relaxed))
      , nextSlot_(rhs.nextSlot_.load(std::memory_order::relaxed))
      , jobs_(std::move(rhs.jobs_))
    {
      rhs.tail_ = 0;
      rhs.head_.store(0, std::memory_order::relaxed);
      rhs.nextSlot_.store(0, std::memory_order::relaxed);
    }

    auto
    operator=(queue&& rhs) noexcept -> queue&
    {
      tail_     = std::move(rhs.tail_);
      head_     = rhs.head_.load(std::memory_order::relaxed);
      nextSlot_ = rhs.nextSlot_.load(std::memory_order::relaxed);
      policy_   = std::move(rhs.policy_);
      jobs_     = std::move(rhs.jobs_);
      return *this;
    }

    template<std::copy_constructible callable_t, typename... arg_ts>
      requires std::invocable<callable_t, arg_ts...> ||
               std::invocable<callable_t, job_token&, arg_ts...>
    auto
    push(job_token& token, callable_t&& func, arg_ts&&... args) noexcept -> job_token&
    {
      // 1. Acquire a slot
      index_t const slot = nextSlot_.fetch_add(1, std::memory_order_relaxed);

      auto& job = jobs_[iterator::mask(slot)];
      // Blocks in release if slot is occupied, asserts in debug to catch overflow
      assert(!job);
      if (job) [[unlikely]]
      {
        job.state.wait(job_state::active, std::memory_order_acquire);
      }

      // 2. Assign job
      if constexpr (std::invocable<callable_t, job_token&, arg_ts...>)
      {
        job.set(FWD(func), std::ref(token), FWD(args)...);
      }
      else
      {
        job.set(FWD(func), FWD(args)...);
      }

      assert(job);

      token.reassign(job.state);

      // 3. Commit slot
      index_t expected = slot;
      while (!head_.compare_exchange_weak(expected, slot + 1, std::memory_order_release))
      {
        expected = slot;
      }
      head_.notify_all();
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
      auto const head = nextSlot_.load(std::memory_order_acquire);
      if (iterator::mask(head - tail_) == 0)
      {
        head_.wait(head);
      }
    }

    auto
    consume(std::size_t max = max_nr_of_jobs) noexcept
    {
      auto head = head_.load(std::memory_order_acquire);
      auto b    = iterator(jobs_.data(), tail_);
      auto e    = iterator(jobs_.data(), std::min(tail_ + max, head));
      tail_     = head;
      return std::ranges::subrange(b, e);
    }

    void
    clear() noexcept
    {
      auto range = consume();
      std::for_each(std::execution::par, std::begin(range), std::end(range),
                    [](job& job)
                    {
                      job.reset();
                    });
      tail_ = head_.load(std::memory_order_acquire);
    }

    auto
    begin() const noexcept -> const_iterator
    {
      return const_iterator(jobs_.data(), tail_);
    }

    auto
    end(std::size_t max = max_nr_of_jobs) const noexcept -> const_iterator
    {
      auto head = head_.load(std::memory_order_acquire);
      // Subtract indices, mask to wrap within buffer:
      auto size     = iterator::mask(head - tail_);
      auto endIndex = tail_ + std::min(size, max);
      return const_iterator(jobs_.data(), endIndex);
    }

    auto
    execute(std::ranges::range auto r) const -> std::size_t
    {
      assert(r.data() >= jobs_.data() && r.data() <= jobs_.data() + jobs_.size());
      if (policy_ == execution_policy::parallel) [[likely]]
      {
        std::for_each(std::execution::par, std::begin(r), std::end(r),
                      [](job& job)
                      {
                        job();
                      });
      }
      else [[unlikely]]
      {
        auto b = std::begin(r);
        // make sure previous has been executed
        auto const& prev = *(b - 1);
        prev.state.wait(job_state::active, std::memory_order_acquire);
        std::for_each(b, std::end(r),
                      [](job& job)
                      {
                        job();
                      });
      }
      return r.size();
    }

    auto
    execute(std::size_t max = max_nr_of_jobs) -> std::size_t
    {
      assert(max > 0);
      return execute(consume(max));
    }

    static constexpr auto
    max_size() noexcept -> std::size_t
    {
      return max_nr_of_jobs - 1;
    }

    auto
    size() const noexcept -> std::size_t
    {
      auto const head = head_.load(std::memory_order_relaxed);
      return iterator::mask(head - tail_); // circular distance
    }

    auto
    empty() const noexcept -> bool
    {
      return size() == 0;
    }

    auto
    data() const noexcept -> decltype(auto)
    {
      return jobs_.data();
    }

  private:
    /*
      Circular job buffer. When tail or head
      reaches the end they will wrap around:
       _
      |_|
      |_|
      |_| ┐→ tail  (next claim) - consumer
      |_| │
      |_| │
      |_| │
      |_| │
      |_| ┘→ head-1 (last job)  - consumer
      |_|  ← head   (next slot) - producer
      |_|
    */

    alignas(details::cache_line_size) execution_policy policy_ = execution_policy::parallel;
    alignas(details::cache_line_size) index_t tail_{0};
    alignas(details::cache_line_size) atomic_index_t head_{0};
    alignas(details::cache_line_size) atomic_index_t nextSlot_{0};

    alignas(details::cache_line_size)
      std::vector<job, aligned_allocator<job, details::cache_line_size>> jobs_{max_nr_of_jobs};
  };
}

#undef FWD
