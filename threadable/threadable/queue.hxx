#pragma once

#include <atomic>
#include <threadable/function.hxx>
#include <threadable/std_atomic.hxx>

#include <cassert>
#include <compare>
#include <cstddef>
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
    job() = default;
    job(job&&) = delete;
    job(const job&) = delete;
    auto operator=(job&&) = delete;
    auto operator=(const job&) = delete;
  
    template<typename callable_t, typename... arg_ts>
      requires std::invocable<callable_t, arg_ts...>
    decltype(auto) set(callable_t&& func, arg_ts&&... args) noexcept
    {
      this->func.set(FWD(func), FWD(args)...);
      details::atomic_test_and_set(active, std::memory_order_release);
      details::atomic_notify_all(active);
    }

    void reset() noexcept
    {
      child_active = nullptr;
      details::atomic_clear(active, std::memory_order_release);
      details::atomic_notify_all(active);
    }

    void operator()()
    {
      if(child_active)
      UNLIKELY
      {
        details::atomic_wait(*child_active, true, std::memory_order_acquire);
      }
      func();
      reset();
    }

    operator bool() const noexcept
    {
      return details::atomic_test(active, std::memory_order_acquire);
    }

  private:
    function<details::job_buffer_size> func;
  };
  static_assert(sizeof(job) == details::cache_line_size, "job size must equal cache line size");

  struct job_ref
  {
    job_ref(job* ref):
      ref(ref)
    {}

    job_ref(const job_ref& other) = default;
    job_ref(job_ref&& other):
      ref(other.ref)
    {
      other.ref = nullptr;
    }

    job_ref& operator=(const job_ref& other) = default;
    job_ref& operator=(job_ref&& other)
    {
      ref = other.ref;
      other.ref = nullptr;
      return *this;
    }
    bool operator==(const job_ref& other) const noexcept
    {
      return ref == other.ref;
    }
    bool operator!=(const job_ref& other) const noexcept
    {
      return !(*this == other.ref);
    }

    ~job_ref()
    {
      if(ref)
      UNLIKELY
      {
        ref->reset();
      }
    }

    job& operator*() const
    {
      return *ref;
    }

    void operator()()
    {
      (*ref)();
      ref = nullptr;
    }

    operator bool() const noexcept
    {
      return ref && *ref;
    }

  private:
    job* ref;
  };

  struct job_token
  {
    job_token(const details::atomic_flag& active):
      active(active)
    {}

    bool done() const noexcept
    {
      return !details::atomic_test(active, std::memory_order_acquire);
    }

    void wait() const noexcept
    {
      details::atomic_wait(active, true, std::memory_order_acquire);
    }

  private:
    const details::atomic_flag& active;
  };

  enum class execution_policy
  {
    sequential,
    concurrent
  };

  namespace details
  {
    constexpr std::size_t default_max_nr_of_jobs = 1024;
  }

  template<std::size_t max_nr_of_jobs = details::default_max_nr_of_jobs>
  class queue2
  {
    using atomic_index_t = std::atomic_size_t;
    using index_t = typename atomic_index_t::value_type;
    static constexpr auto index_mask = max_nr_of_jobs - 1u;
    static constexpr auto null_callback = [](queue2&){};

    static_assert(max_nr_of_jobs > 1, "number of jobs must be greater than 1");
    static_assert((max_nr_of_jobs & index_mask) == 0, "number of jobs must be a power of 2");
  public:
    template<std::invocable<queue2&> callable_t>
    queue2(callable_t&& onJobReady) noexcept:
      jobs_(*jobsPtr_)
    {
      set_notify(FWD(onJobReady));
    }
    queue2() noexcept:
      queue2(null_callback)
    {}
    queue2(queue2&&) = delete;
    queue2(const queue2&) = delete;
    auto operator=(queue2&&) = delete;
    auto operator=(const queue2&) = delete;

    struct sentinel
    {
      explicit sentinel(index_t* tail, index_t end) noexcept:
        tail(tail),
        end(end)
      {}
      sentinel() = default;
      sentinel(const sentinel& other):
        end(other.end),
        tail(other.tail)
      {
        // Glorious hack to make sure there is ever only one
        // end-iterator changing the tail value;
        const_cast<sentinel&>(other).tail = nullptr;
      }
      sentinel(sentinel&& other):
        end(other.end),
        tail(other.tail)
      {
        other.tail = nullptr;
      }
      sentinel& operator=(const sentinel& rhs)
      {

        end = rhs.end;
        tail = rhs.tail;
        // Glorious hack to make sure there is ever only one
        // end-iterator changing the tail value;
        const_cast<sentinel&>(rhs).tail = nullptr;
        return *this;
      }
      ~sentinel()
      {
        if(tail)
        UNLIKELY
        {
          *tail = end;
        }
      }
      index_t end = 0;
    private:
      index_t* tail = nullptr;
    };
    struct iterator
    {
      using iterator_category = std::random_access_iterator_tag;
      using difference_type   = std::ptrdiff_t;
      using value_type        = job_ref;
      using pointer           = job_ref;
      using reference         = job_ref&;

      iterator() = default;

      explicit iterator(job* jobs, index_t index) noexcept:
        jobs_(jobs),
        index_(index)
      {}

      explicit iterator(sentinel&& sentinel) noexcept:
        jobs_(nullptr),
        index_(sentinel.end),
        sentinel_(sentinel)
      {}

      job_ref operator*() const noexcept
      {
        return &jobs_[mask(index_)];
      }
      inline job_ref operator[](difference_type rhs) const noexcept { return &jobs_[mask(rhs)]; }

      inline auto operator<=>(const iterator& rhs) const noexcept { return index_ <=> rhs.index_; }
      bool operator==(const iterator& other) const noexcept { return index_ == other.index_; }
      bool operator!=(const iterator& other) const noexcept { return !(*this == other); }

      inline difference_type operator+ (const iterator& rhs) const noexcept { return index_ - rhs.index_; }
      inline difference_type operator- (const iterator& rhs) const noexcept { return index_ - rhs.index_; }
      inline iterator        operator+ (difference_type rhs) const noexcept { return iterator(jobs_, index_ + rhs); }
      inline iterator        operator- (difference_type rhs) const noexcept { return iterator(jobs_, index_ - rhs); }
      friend inline iterator operator+(difference_type lhs, const iterator& rhs) { return iterator(rhs.jobs_, rhs + rhs.index_); }
      friend inline iterator operator-(difference_type lhs, const iterator& rhs) { return iterator(rhs.jobs_, lhs - rhs.index_); }
      inline iterator&       operator+=(difference_type rhs) noexcept { index_ += rhs; return *this; }
      inline iterator&       operator-=(difference_type rhs) noexcept { index_ -= rhs; return *this; }
      inline iterator&       operator++() noexcept { ++index_; return *this; }
      inline iterator&       operator--() noexcept { --index_; return *this; }
      inline iterator        operator++(int) noexcept { iterator tmp(jobs_, index_++); return tmp; }
      inline iterator        operator--(int) noexcept { iterator tmp(jobs_, index_--); return tmp; }

    private:
      job* jobs_ = nullptr;
      index_t index_ = 0;
      sentinel sentinel_;
    };
    // Make sure iterator is valid for parallelization with the standard algorithms
    static_assert(std::random_access_iterator<iterator>, "queue::iterator must be a std::random_access_iterator");

    template<std::invocable<queue2&> callable_t>
    void set_notify(callable_t&& onJobReady) noexcept
    {
      on_job_ready.set(FWD(onJobReady), std::ref(*this));
    }

    void set_notify(std::nullptr_t) noexcept
    {
      on_job_ready.set(null_callback, std::ref(*this));
    }

    // Push jobs to non-claimed slots.
    template<typename callable_t, typename... arg_ts>
      requires std::invocable<callable_t, arg_ts...>
    const job_token push(callable_t&& func, arg_ts&&... args) noexcept
    {
      assert(size() <= max_nr_of_jobs);
      const index_t i = head_.load(std::memory_order_acquire);
      assert(mask(tail_) <= i);

      auto& job = jobs_[mask(i)];
      job.set(FWD(func), FWD(args)...);

      head_.store(i+1, std::memory_order_release);
      on_job_ready();

      return job.active;
    }

    void clear()
    {
      for(auto job : *this){}
    }

    auto begin() noexcept
    {
      return iterator(jobs_.data(), tail_);
    }

    auto end() noexcept
    {
      return iterator(sentinel(&tail_, head_.load(std::memory_order_acquire)));
    }

    std::size_t execute()
    {
      std::size_t executed = 0;
      for(auto job : *this)
      {
        job();
        ++executed;
      }
      return executed;
    }

    std::size_t execute_or_wait()
    {
      (void)next_or_wait();
      return execute();
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

  private:
    static constexpr inline auto mask(index_t val) noexcept
    {
      return val & index_mask;
    }

    auto& next()
    {
      return jobs_[mask(tail_)];
    }

    auto& next_or_wait()
    {
      auto& job = next();
      details::atomic_wait(job.active, false, std::memory_order_acquire);
      return job;
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
      |_| ┘
      |_|  ← head (next push)  - producer
      |_|
      |_|
    */

    // max() is intentional to easily detect wrap-around issues
    index_t tail_{0};
    atomic_index_t head_{0};
    using jobs_t = std::array<job, max_nr_of_jobs>;
    std::unique_ptr<jobs_t> jobsPtr_ = std::make_unique<jobs_t>();
    jobs_t& jobs_;
    function<details::job_buffer_size> on_job_ready;
    // potential bug with clang (14.0.6) where use of vector for jobs (with atomic member)
    // causing noity_all() to not wake thread(s). See completion token test "stress-test"
    // std::vector<job> jobs_{max_nr_of_jobs};
  };
  template<std::size_t max_nr_of_jobs = details::default_max_nr_of_jobs>
  class queue
  {
    static_assert((max_nr_of_jobs& (max_nr_of_jobs - 1)) == 0, "number of jobs must be a power of 2");

    using atomic_index_t = std::atomic_ptrdiff_t;
    using index_t = typename atomic_index_t::value_type;

    static constexpr auto index_mask = max_nr_of_jobs - 1u;
    static constexpr job* null_job = nullptr;
    static constexpr auto null_callback = [](queue&){};

    struct iterator
    {
      using iterator_category = std::forward_iterator_tag;
      using difference_type   = index_t;
      using value_type        = job;
      using pointer           = value_type*;
      using reference         = value_type&;

      iterator() = default;
      iterator(const iterator&) = default;
      iterator(queue& queue, index_t index):
        queue_(&queue),
        index_(index)
      {}

      reference operator*() const
      {
        job_ = queue_->steal();
        return *job_;
      }

      iterator& operator++()
      {
        ++index_;
        return *this;
      }

      iterator operator++(int)
      {
        iterator tmp(*queue_, std::move(*job_));
        ++(*this);
        return tmp;
      }

      bool operator==(const iterator& other) const
      {
        return index_ == other.index_;
      }

      bool operator!=(const iterator& other) const
      {
        return !(*this == other);
      }

    private:
      queue* queue_ = nullptr;
      index_t index_ = 0;
      mutable job_ref job_ = {null_job};
    };
    static_assert(std::forward_iterator<iterator>, "queue::iterator must be a forward_iterator");

  public:
    template<std::invocable<queue&> callable_t>
    queue(execution_policy policy, callable_t&& onJobReady) noexcept:
      policy_(policy),
      jobs_(*jobsPtr_)
    {
      details::atomic_clear(quit_);
      set_notify(FWD(onJobReady));
    }

    queue(execution_policy policy = execution_policy::concurrent) noexcept:
      queue(policy, null_callback)
    {
    }

    template<std::invocable<queue&> callable_t>
    queue(callable_t&& onJobReady) noexcept:
      queue(execution_policy::concurrent, FWD(onJobReady))
    {
    }

    queue(queue&&) = delete;
    queue(const queue&) = delete;
    auto operator=(queue&&) = delete;
    auto operator=(const queue&) = delete;

    ~queue()
    {
      quit();
      for(auto& job : jobs_)
      {
        details::atomic_wait(job.active, true);
      }
    }

    void quit() noexcept
    {
      details::atomic_test_and_set(quit_, std::memory_order_seq_cst);
      details::atomic_notify_all(quit_);

      while(waiters_ > 0)
      {
        // push dummy jobs to release any waiting threads
        if(empty())
        {
          (void)push([]{});
        }
        std::this_thread::yield();
      }
      // pop & discard (reset) remaining jobs
      while(!empty())
      {
        (void)pop();
      }
    }

    template<std::invocable<queue&> callable_t>
    void set_notify(callable_t&& onJobReady) noexcept
    {
      on_job_ready.set(FWD(onJobReady), std::ref(*this));
    }

    void set_notify(std::nullptr_t) noexcept
    {
      on_job_ready.set(null_callback, std::ref(*this));
    }

    template<typename callable_t, typename... arg_ts>
      requires std::invocable<callable_t, arg_ts...>
    const job_token push(callable_t&& func, arg_ts&&... args) noexcept
    {
      assert(size() < max_nr_of_jobs);
    
      const index_t b = bottom_.load(std::memory_order_acquire);

      auto& job = jobs_[index(b)];
      job.set(FWD(func), FWD(args)...);
      if(policy_ == execution_policy::sequential && b > 0)
      UNLIKELY
      {
        const index_t prev = b-1;
        job.child_active = &jobs_[index(prev)].active;
      }

      bottom_.store(b+1, std::memory_order_release);
      details::atomic_notify_one(bottom_);
      on_job_ready();

      return job.active;
    }

    job_ref pop() noexcept
    {
      const index_t b = bottom_.load(std::memory_order_acquire) - 1;
      bottom_.store(b, std::memory_order_release);
      const index_t t = top_.load(std::memory_order_acquire);

      if(t != b+1)
      LIKELY
      {
        // there are jobs available
        auto* job = &jobs_[index(b)];
        if(t != b)
        LIKELY
        {
          // there's still more than one item left in the queue
          return job;
        }
        // this is the last item in the queue

        // whether we win or lose a race against steal() we still set
        // the queue to 'empty' (bottom = new top), because either way it
        // implies that the last job has been taken.
        // NOTE: it's important that it happens before the exchange for 
        //       top & bottom to remain consistent
        index_t previous = t;
        bottom_.store(t+1, std::memory_order_release);
        if(top_.compare_exchange_strong(previous, t+1))
        LIKELY
        {
          // won race against steal()
          return job;
        }
        else
        UNLIKELY
        {
          // lost race against steal()
          return null_job;
        }
      }
      else
      UNLIKELY
      {
        // restore bottom since we never got a job
        bottom_.store(t, std::memory_order_release);
        return null_job;
      }
    }

    job_ref steal() noexcept
    {
      const index_t t = top_.load(std::memory_order_acquire);
      const index_t b = bottom_.load(std::memory_order_acquire);

      // if there are jobs available
      if(t != b)
      LIKELY
      {
        job* job = nullptr;
        index_t previous = t;
        do
        {
          job = &jobs_[index(previous)];
        }
        while(previous != b && !top_.compare_exchange_weak(previous, previous+1));

        if(previous != b)
        LIKELY
        {
          return job;
        }
        else
        UNLIKELY
        {
          // try steal again (with updated values)
          return steal();
        }
      }
      return null_job;
    }

    job_ref steal_or_wait() noexcept
    {
      struct raii
      {
        raii(std::atomic_size_t& counter): counter_(counter) { ++counter_; }
        ~raii() { --counter_; }
        std::atomic_size_t& counter_;
      } waiting(waiters_);

      job_ref job = steal();
      while(!job)
      {
        const index_t t = top_.load(std::memory_order_acquire);
        const index_t b = bottom_.load(std::memory_order_acquire);

        if(const auto quit = details::atomic_test(quit_, std::memory_order_relaxed); !quit && t == b)
        LIKELY
        {
          // wait until bottom changes
          details::atomic_wait(bottom_, b);
        }
        else if(quit)
        UNLIKELY
        {
          return null_job;
        }

        job = steal();
      }

      return job;
    }

    std::size_t size() const noexcept
    {
      const index_t b = bottom_.load(std::memory_order_acquire);
      const index_t t = top_.load(std::memory_order_acquire);
      return static_cast<std::size_t>(b - t);
    }

    constexpr std::size_t max_size() const noexcept
    {
      return max_nr_of_jobs;
    }

    bool empty() const noexcept
    {
      return size() == 0;
    }

    decltype(auto) begin()
    {
      return iterator(*this, 0);
    }

    decltype(auto) end()
    {
      return iterator(*this, size());
    }

  private:
    inline auto index(index_t val) const
    {
      return val & index_mask;
    }

    // max() is intentional to easily detect wrap-around issues
    atomic_index_t top_{std::numeric_limits<index_t>::max()};
    atomic_index_t bottom_{std::numeric_limits<index_t>::max()};
    execution_policy policy_ = execution_policy::concurrent;
    function<details::job_buffer_size> on_job_ready;
    details::atomic_flag quit_;
    using jobs_t = std::array<job, max_nr_of_jobs>;
    std::unique_ptr<jobs_t> jobsPtr_ = std::make_unique<jobs_t>();
    jobs_t& jobs_;
    // potential bug with clang (14.0.6) where use of vector for jobs (with atomic member)
    // causing noity_all() to not wake thread(s). See completion token test "stress-test"
    // std::vector<job> jobs_{max_nr_of_jobs};
    std::atomic_size_t waiters_{0};
  };
}

#undef FWD
#undef LIKELY
#undef UNLIKELY
