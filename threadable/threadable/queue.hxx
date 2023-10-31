#pragma once

#include <cmath>
#include <threadable/function.hxx>
#include <threadable/std_atomic.hxx>

#include <algorithm>
#include <cassert>
#include <compare>
#include <cstddef>
#if __has_include(<execution>)
#include <execution>
#endif
#include <iterator>
#include <limits>
#include <thread>
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
    struct job_base
    {
      atomic_flag active;
      atomic_flag* child_active = nullptr;
    };
    static constexpr auto job_buffer_size = cache_line_size - sizeof(job_base) - sizeof(function<0>);
  }

  struct alignas(details::cache_line_size) job final: details::job_base
  {
    using function_t = function<details::job_buffer_size>;

    job() = default;
    job(job&&) = delete;
    job(const job&) = delete;
    auto operator=(job&&) = delete;
    auto operator=(const job&) = delete;

    template<typename callable_t, typename... arg_ts>
      requires std::invocable<callable_t, arg_ts...>
    decltype(auto) set(callable_t&& func, arg_ts&&... args) noexcept
    {
      this->func_.set(FWD(func), FWD(args)...);
      details::atomic_set(active, std::memory_order_release);
      // NOTE: Intentionally not notifying here since that is redundant (and costly),
      //       it is designed to be waited on (checking state true -> false)
      // details::atomic_notify_all(active);
    }

    template<typename callable_t>
      requires std::invocable<callable_t>
    auto& operator=(callable_t&& func) noexcept
    {
      set(FWD(func));
      return *this;
    }

    auto& operator=(std::nullptr_t) noexcept
    {
      reset();
      return *this;
    }

    void reset() noexcept
    {
      func_.reset();
      child_active = nullptr;
      details::atomic_clear(active, std::memory_order_release);
      details::atomic_notify_all(active);
    }

    bool done() const noexcept
    {
      return !details::atomic_test(active, std::memory_order_acquire);
    }

    void operator()()
    {
      assert(func_);
      assert(!done());

      if(child_active)
      UNLIKELY
      {
        details::atomic_wait(*child_active, true, std::memory_order_acquire);
      }
      func_();
      reset();
    }

    operator bool() const noexcept
    {
      return !done();
    }

    auto& get() noexcept
    {
      return func_;
    }

  private:
    function_t func_;
  };
  static_assert(sizeof(job) == details::cache_line_size, "job size must equal cache line size");

  struct job_token
  {
    job_token():
      job_token(null_flag)
    {}

    job_token(details::atomic_flag& active):
      active(&active)
    {
    }

    job_token(job_token&& rhs) noexcept:
      active(rhs.active.load(std::memory_order_acquire))
    {
      rhs.active = nullptr;
    }

    auto& operator=(job_token&& rhs) noexcept
    {
      active.store(rhs.active, std::memory_order_release);
      return *this;
    }

    bool done() const noexcept
    {
      return !details::atomic_test(*active.load(std::memory_order_acquire), std::memory_order_acquire);
    }

    auto cancel() noexcept
    {
      return details::atomic_test_and_clear(*active.load(std::memory_order_acquire), std::memory_order_acq_rel);
    }

    void wait() const noexcept
    {
      details::atomic_wait(*active.load(std::memory_order_acquire), true, std::memory_order_acquire);
    }

  private:
    std::atomic<details::atomic_flag*> active = nullptr;
    inline static details::atomic_flag null_flag;
  };

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

    // Push jobs to non-claimed slots.
    template<std::copy_constructible callable_t, typename... arg_ts>
      requires std::invocable<callable_t, arg_ts...>
            || std::invocable<callable_t, const job&, arg_ts...>
    job_token push(callable_t&& func, arg_ts&&... args) noexcept
    {
      const index_t i = head_.load(std::memory_order_acquire);

      auto& job = jobs_[mask(i)];
      assert(!job);

      if constexpr(std::invocable<callable_t&&, const threadable::job&, arg_ts&&...>)
      {
        job.set(FWD(func), std::ref(job), FWD(args)...);
      }
      else
      {
        job.set(FWD(func), FWD(args)...);
      }

      assert(job);

      if(policy_ == execution_policy::sequential)
      {
        job.child_active = &jobs_[mask(i-1)].active;
      }

      head_.store(i+1, std::memory_order_release);
      on_job_ready();

      return job.active;
    }

    template<std::copy_constructible callable_t, typename... arg_ts>
      requires std::invocable<callable_t, arg_ts...>
    job_token push_slow(callable_t&& func, arg_ts&&... args) noexcept
    {
      auto pack = std::make_tuple(FWD(func), FWD(args)...);
      return push([argPack = std::make_shared<decltype(pack)>(std::move(pack))]
      {
        std::apply([](auto&& func, auto&&... args){
          func(FWD(args)...);
        }, *argPack);
      });
    }

    void clear()
    {
      for(auto& job : *this){ (void)job.reset(); }
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
        if(tail_ < head)
        {
          auto& lastJob = jobs_[mask(head-1)];
          lastJob.set([this, head = head, func = function_dyn(lastJob.get())]() mutable {
            func();
            tail_ = head;
          });
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
      {
#ifdef __cpp_lib_execution
        std::for_each(std::execution::par, b, e, [](job& job){
          job();
        });
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
      return max_nr_of_jobs;
    }

    std::size_t size() const noexcept
    {
      const auto head = head_.load(std::memory_order_acquire);
      return head - tail_;
    }

    bool empty() const noexcept
    {
      return size() == 0;
    }

    static constexpr inline auto mask(index_t val) noexcept
    {
      return val & index_mask;
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
