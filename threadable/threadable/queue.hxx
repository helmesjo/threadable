#pragma once

#include <threadable/job.hxx>

#include <atomic>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <compare>
#include <concepts>
#include <cstddef>
#if __has_include(<execution>)
  #include <execution>
#endif
#if __has_include (<pstld/pstld.h>)
  #include <atomic> // missing include
  #include <pstld/pstld.h>
#endif
#include <iterator>
#include <limits>
#include <vector>

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

#if __has_cpp_attribute(likely) && __has_cpp_attribute(unlikely)
#define LIKELY [[likely]]
#define UNLIKELY [[unlikely]]
#else
#define LIKELY
#define UNLIKELY
#endif

namespace threadable
{

  namespace details
  {
    constexpr std::size_t default_max_nr_of_jobs = 1024;
  }

  enum class execution_policy
  {
    sequential,
    parallel
  };

  template<std::size_t max_nr_of_jobs = details::default_max_nr_of_jobs>
  class queue
  {
    using atomic_index_t = std::atomic_size_t;
    using index_t = typename atomic_index_t::value_type;
    static constexpr auto index_mask = max_nr_of_jobs - 1u;
    static constexpr auto null_callback = [](queue&){};

    static_assert(max_nr_of_jobs > 1, "number of jobs must be greater than 1");
    static_assert((max_nr_of_jobs & index_mask) == 0, "number of jobs must be a power of 2");
#ifdef __cpp_lib_constexpr_cmath
    static_assert(max_nr_of_jobs <= std::pow(2, (8 * sizeof(index_t)) - 1), "number of jobs must be <= half the index range");
#endif

// Workaround for bug in MSVC with nested types accessing outer (private) members
#ifdef _WIN32
public:
#endif
    static constexpr inline auto mask(index_t val) noexcept
    {
      return val & index_mask;
    }

  public:

    queue(execution_policy policy = execution_policy::parallel) noexcept
    : policy_(policy)
    {
      set_notify(null_callback);
    }

    queue(queue&&) = delete;
    queue(const queue&) = delete;
    auto operator=(queue&&) = delete;
    auto operator=(const queue&) = delete;

    struct iterator
    {
      using iterator_category = std::random_access_iterator_tag;
      using iterator_concept  = std::contiguous_iterator_tag;
      using difference_type   = std::ptrdiff_t;
      using value_type        = job;
      using pointer           = value_type*;
      using reference         = value_type&;
      using element_type      = value_type;

      iterator() = default;

      explicit iterator(job* jobs, index_t index) noexcept:
        jobs_(jobs),
        index_(index)
      {}

      inline reference operator*() noexcept
      {
        return jobs_[mask(index_)];
      }
      inline pointer operator->() const noexcept
      {
        return &jobs_[mask(index_)];
      }
      inline reference operator[](difference_type rhs) const noexcept
      {
        return jobs_[mask(index_ + rhs)];
      }

      friend inline reference operator*(const iterator& it) { return it.jobs_[mask(it.index_)]; }

      inline auto operator<=>(const iterator& rhs) const noexcept { return index_ <=> rhs.index_; }
      inline bool operator== (const iterator& other) const noexcept { return index_ == other.index_; }

      // todo: Add tests and make sure iterator works for wrap-around
      inline difference_type operator+ (const iterator& rhs) const noexcept { return index_ + rhs.index_; }
      inline difference_type operator- (const iterator& rhs) const noexcept { return index_ - rhs.index_; }
      inline iterator        operator+ (difference_type rhs) const noexcept { return iterator(jobs_, index_ + rhs); }
      inline iterator        operator- (difference_type rhs) const noexcept { return iterator(jobs_, index_ - rhs); }
      friend inline iterator operator+ (difference_type lhs, const iterator& rhs) { return iterator(rhs.jobs_, lhs + rhs.index_); }
      friend inline iterator operator- (difference_type lhs, const iterator& rhs) { return iterator(rhs.jobs_, lhs - rhs.index_); }
      inline iterator&       operator+=(difference_type rhs) noexcept { index_ += rhs; return *this; }
      inline iterator&       operator-=(difference_type rhs) noexcept { index_ -= rhs; return *this; }
      inline iterator&       operator++() noexcept { ++index_; return *this; }
      inline iterator&       operator--() noexcept { --index_; return *this; }
      inline iterator        operator++(int) noexcept { return iterator(jobs_, index_++); }
      inline iterator        operator--(int) noexcept { return iterator(jobs_, index_--); }

    private:
      job* jobs_ = nullptr;
      index_t index_ = 0;
    };
    // Make sure iterator is valid for parallelization with the standard algorithms
    static_assert(std::random_access_iterator<iterator>);
    static_assert(std::contiguous_iterator<iterator>);

    template<std::invocable<queue&> callable_t>
    void set_notify(callable_t&& onJobReady) noexcept
    {
      on_job_ready.set(FWD(onJobReady), std::ref(*this));
    }

    void set_notify(std::nullptr_t) noexcept
    {
      on_job_ready.set(null_callback, std::ref(*this));
    }

    template<std::copy_constructible callable_t, typename... arg_ts>
      requires std::invocable<callable_t, arg_ts...>
            || std::invocable<callable_t, job_token&, arg_ts...>
    void push(job_token& token, callable_t&& func, arg_ts&&... args) noexcept
    {
      // 1. Acquire a slot
      const index_t slot = nextSlot_.fetch_add(1, std::memory_order_relaxed);
      assert(mask(slot+1) != mask(tail_));

      auto& job = jobs_[mask(slot)];
      assert(!job);

      // 2. Assign job
      if constexpr(std::invocable<callable_t, job_token&, arg_ts...>)
      {
        job.set(FWD(func), std::ref(token), FWD(args)...);
      }
      else
      {
        job.set(FWD(func), FWD(args)...);
      }

      assert(job);

      if(policy_ == execution_policy::sequential)
      UNLIKELY
      {
        if(auto& childJob = jobs_[mask(slot-1)])
        LIKELY
        {
          job.wait_for(childJob.states);
        }
      }

      token = job.states;

      std::atomic_thread_fence(std::memory_order_release);

      // 3. Commit slot
      index_t expected;
      do
      {
        expected = slot;
      }
      while(!head_.compare_exchange_weak(expected, slot+1, std::memory_order_relaxed));

      on_job_ready();
    }

    template<std::copy_constructible callable_t, typename... arg_ts>
      requires std::invocable<callable_t, arg_ts...>
    job_token push(callable_t&& func, arg_ts&&... args) noexcept
    {
      job_token token;
      push(token, FWD(func), FWD(args)...);
      return token;
    }

    void clear()
    {
#ifdef __cpp_lib_execution
        if(policy_ == execution_policy::parallel)
        LIKELY
        {
          std::for_each(std::execution::par, begin(), end(), [](job& job){
            job.reset();
          });
        }
        else
        UNLIKELY
        {
          std::for_each(begin(), end(), [](job& job){
            job.reset();
          });
        }
#else
        std::for_each(begin(), end(), [](job& job){
          job.reset();
        });
#endif
      tail_ = head_.load(std::memory_order_acquire);
    }

    auto begin() noexcept
    {
      return iterator(jobs_.data(), tail_);
    }

    template<bool consume = true>
    auto end() noexcept
    {
      auto head = head_.load(std::memory_order_acquire);
      if constexpr(consume)
      {
        if(auto& lastJob = jobs_[mask(head-1)])
        LIKELY
        {
          lastJob.set(function_dyn([this, head, func = function_dyn(lastJob.get())]() mutable {
            func();
            tail_ = head;
          }));
        }
      }
      return iterator(nullptr, head);
    }

    std::size_t execute()
    {
      const auto b = begin();
      const auto e = end();
      const auto dis = e - b;
      if(dis > 0)
      LIKELY
      {
        assert(b != e);
#ifdef __cpp_lib_execution
        if(policy_ == execution_policy::parallel)
        LIKELY
        {
          std::for_each(std::execution::par, b, e, [](job& job){
            job();
          });
        }
        else
        UNLIKELY
        {
          std::for_each(b, e, [](job& job){
            job();
          });
        }
#else
        std::for_each(b, e, [](job& job){
          job();
        });
#endif
      }
      return dis;
    }

    static constexpr std::size_t max_size() noexcept
    {
      return max_nr_of_jobs - 1;
    }

    std::size_t size() const noexcept
    {
      const auto head = head_.load(std::memory_order_relaxed);
      return mask(head - tail_);
    }

    bool empty() const noexcept
    {
      return size() == 0;
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
      |_| ┘
      |_|  ← head (next push)   - producer
      |_|
      |_|
    */

    // max() is intentional to easily detect wrap-around issues
    alignas(details::cache_line_size) index_t tail_{0};
    alignas(details::cache_line_size) atomic_index_t head_{0};
    alignas(details::cache_line_size) atomic_index_t nextSlot_{0};

    execution_policy policy_ = execution_policy::parallel;
    function<details::job_buffer_size> on_job_ready;
    // potential bug with clang (14.0.6) where use of vector for jobs (with atomic member)
    // causing noity_all() to not wake thread(s). See completion token test "stress-test"
    std::vector<job> jobs_{max_nr_of_jobs};
  };
}

#undef FWD
#undef LIKELY
#undef UNLIKELY
