#pragma once

#include <threadable/job.hxx>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#if __has_include(<pstld/pstld.h>)
  #include <pstld/pstld.h>
#endif
#ifdef __cpp_lib_execution
  #include <execution>
#else
  #error requires __cpp_lib_execution
#endif
#include <iterator>
#include <vector>

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

// Workaround for bug in MSVC with nested types accessing outer (private) members
#ifdef _WIN32

  public:
#endif
    inline static constexpr auto
    mask(index_t index) noexcept
    {
      return index & index_mask;
    }

  public:
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

      explicit iterator(job* jobs, index_t index) noexcept
        : jobs_(jobs)
        , index_(index)
      {}

      inline auto
      operator*() noexcept -> reference
      {
        return jobs_[mask(index_)];
      }

      inline auto
      operator->() const noexcept -> pointer
      {
        return &jobs_[mask(index_)];
      }

      inline auto
      operator[](difference_type rhs) const noexcept -> reference
      {
        return jobs_[mask(index_ + rhs)];
      }

      friend inline auto
      operator*(iterator const& it) -> reference
      {
        return it.jobs_[mask(it.index_)];
      }

      inline auto
      operator<=>(iterator const& rhs) const noexcept
      {
        return index_ <=> rhs.index_;
      }

      inline auto
      operator==(iterator const& other) const noexcept -> bool
      {
        return index_ == other.index_;
      }

      // todo: Add tests and make sure iterator works for wrap-around
      inline auto
      operator+(iterator const& rhs) const noexcept -> difference_type
      {
        return index_ + rhs.index_;
      }

      inline auto
      operator-(iterator const& rhs) const noexcept -> difference_type
      {
        return index_ - rhs.index_;
      }

      inline auto
      operator+(difference_type rhs) const noexcept -> iterator
      {
        return iterator(jobs_, index_ + rhs);
      }

      inline auto
      operator-(difference_type rhs) const noexcept -> iterator
      {
        return iterator(jobs_, index_ - rhs);
      }

      friend inline auto
      operator+(difference_type lhs, iterator const& rhs) -> iterator
      {
        return iterator(rhs.jobs_, lhs + rhs.index_);
      }

      friend inline auto
      operator-(difference_type lhs, iterator const& rhs) -> iterator
      {
        return iterator(rhs.jobs_, lhs - rhs.index_);
      }

      inline auto
      operator+=(difference_type rhs) noexcept -> iterator&
      {
        index_ += rhs;
        return *this;
      }

      inline auto
      operator-=(difference_type rhs) noexcept -> iterator&
      {
        index_ -= rhs;
        return *this;
      }

      inline auto
      operator++() noexcept -> iterator&
      {
        ++index_;
        return *this;
      }

      inline auto
      operator--() noexcept -> iterator&
      {
        --index_;
        return *this;
      }

      inline auto
      operator++(int) noexcept -> iterator
      {
        return iterator(jobs_, index_++);
      }

      inline auto
      operator--(int) noexcept -> iterator
      {
        return iterator(jobs_, index_--);
      }

      inline auto
      index() const
      {
        return index_;
      }

    private:
      job*    jobs_  = nullptr;
      index_t index_ = 0;
    };

    // Make sure iterator is valid for parallelization with the standard algorithms
    static_assert(std::random_access_iterator<iterator>);
    static_assert(std::contiguous_iterator<iterator>);

    using function_t = function<details::job_buffer_size>;

    queue(queue const&) = delete;
    ~queue()            = default;

    queue(execution_policy policy = execution_policy::parallel) noexcept
      : policy_(policy)
    {
      set_notify(null_callback);
    }

    queue(queue&& rhs) noexcept
      : tail_(std::move(rhs.tail_))
      , head_(rhs.head_.load(std::memory_order::relaxed))
      , nextSlot_(rhs.nextSlot_.load(std::memory_order::relaxed))
      , policy_(std::move(rhs.policy_))
      , onJobReady_(std::move(rhs.onJobReady_))
      , jobs_(std::move(rhs.jobs_))
    {
      rhs.tail_ = 0;
      rhs.head_.store(0, std::memory_order::relaxed);
      rhs.nextSlot_.store(0, std::memory_order::relaxed);
    }

    auto
    operator=(queue&& rhs) noexcept -> queue&
    {
      tail_       = std::move(rhs.tail_);
      head_       = rhs.head_.load(std::memory_order::relaxed);
      nextSlot_   = rhs.nextSlot_.load(std::memory_order::relaxed);
      policy_     = std::move(rhs.policy_);
      onJobReady_ = std::move(rhs.onJobReady_);
      jobs_       = std::move(rhs.jobs_);
      return *this;
    }

    template<std::invocable<queue&> callable_t>
    void
    set_notify(callable_t&& callback) noexcept
    {
      onJobReady_ = std::make_shared<function_t>(FWD(callback), std::ref(*this));
    }

    void
    set_notify(std::nullptr_t) noexcept
    {
      set_notify(null_callback);
    }

    template<std::copy_constructible callable_t, typename... arg_ts>
      requires std::invocable<callable_t, arg_ts...> ||
               std::invocable<callable_t, job_token&, arg_ts...>
    void
    push(job_token& token, callable_t&& func, arg_ts&&... args) noexcept
    {
      // 1. Acquire a slot
      index_t const slot = nextSlot_.fetch_add(1, std::memory_order_relaxed);
      assert(mask(slot + 1) != mask(tail_));

      auto& job = jobs_[mask(slot)];
      assert(!job);

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

      std::atomic_thread_fence(std::memory_order_release);

      // 3. Commit slot
      index_t expected = 0;
      do
      {
        expected = slot;
      }
      while (!head_.compare_exchange_weak(expected, slot + 1, std::memory_order_relaxed));

      notify();
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

    auto
    consume(std::size_t max = max_nr_of_jobs) -> std::pair<iterator, iterator>
    {
      auto b = begin();
      auto e = end<false>(max);
      tail_  = e.index();
      return {b, e};
    }

    void
    clear()
    {
      std::for_each(std::execution::par, begin(), end(),
                    [](job& job)
                    {
                      job.reset();
                    });
      tail_ = head_.load(std::memory_order_acquire);
    }

    auto
    begin() noexcept
    {
      return iterator(jobs_.data(), tail_);
    }

    template<bool consume = true>
    auto
    end(std::size_t max = max_nr_of_jobs) noexcept
    {
      auto head = head_.load(std::memory_order_acquire);
      head      = std::min(tail_ + max, head);
      if constexpr (consume)
      {
        if (head > 0) [[likely]]
        {
          auto& lastJob = jobs_[mask(head - 1)];
          if (mask(head - tail_) > 0 && lastJob) [[likely]]
          {
            lastJob.set(function_dyn(
              [this, head, func = function_dyn(lastJob.get())]() mutable
              {
                func();
                tail_ = head;
              }));
          }
        }
      }
      return iterator(nullptr, head);
    }

    auto
    execute(iterator b, iterator e) -> std::size_t
    {
      auto const dis = e - b;
      if (dis > 0) [[likely]]
      {
        e = b + dis;
        assert(b != e);
        if (policy_ == execution_policy::parallel) [[likely]]
        {
          std::for_each(std::execution::par, b, e,
                        [](job& job)
                        {
                          job();
                        });
        }
        else [[unlikely]]
        {
          // make sure previous has been executed
          auto const& prev = *(b - 1);
          details::wait<job_state::active, true>(prev.state, std::memory_order_acquire);
          std::for_each(b, e,
                        [](job& job)
                        {
                          job();
                        });
        }
      }
      return dis;
    }

    auto
    execute(std::size_t max = max_nr_of_jobs) -> std::size_t
    {
      assert(max > 0);
      auto const b = begin();
      auto       e = end(max);
      return execute(b, e);
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
      return mask(head - tail_);
    }

    auto
    empty() const noexcept -> bool
    {
      return size() == 0;
    }

    void
    shutdown() noexcept
    {
      set_notify(nullptr);
    }

  private:
    void
    notify() noexcept
    {
      // aqcuire reference while notifying
      if (auto receiver = onJobReady_)
      {
        (*receiver)();
      }
    }

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
      |_|  ← head (next slot)   - producer
      |_|
    */

    alignas(details::cache_line_size) index_t tail_{0};
    alignas(details::cache_line_size) atomic_index_t head_{0};
    alignas(details::cache_line_size) atomic_index_t nextSlot_{0};

    alignas(details::cache_line_size) execution_policy policy_ = execution_policy::parallel;
    alignas(details::cache_line_size) std::shared_ptr<function_t> onJobReady_;
    // potential bug with clang (14.0.6) where use of vector for jobs (with atomic member)
    // causing noity_all() to not wake thread(s). See completion token test "stress-test"
    alignas(details::cache_line_size) std::vector<job> jobs_{max_nr_of_jobs};
  };
}

#undef FWD
