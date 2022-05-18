#pragma once

#include <threadable/function.hxx>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <concepts>
#include <thread>
#include <vector>

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

namespace threadable
{
  namespace details
  {
    using atomic_flag = std::atomic_bool;

#if __cpp_lib_atomic_wait >= 201907 && !defined(__APPLE__)
    template<typename atomic_t, typename obj_t>
    inline void atomic_wait(atomic_t&& atomic, obj_t&& old) noexcept
    {
      FWD(atomic).wait(FWD(old));
    }

    template<typename atomic_t>
    inline void atomic_notify_one(atomic_t&& atomic) noexcept
    {
      FWD(atomic).notify_one();
    }

    template<typename atomic_t>
    inline void atomic_notify_all(atomic_t&& atomic) noexcept
    {
      FWD(atomic).notify_all();
    }
#else
    template<typename atomic_t, typename obj_t>
    inline void atomic_wait(atomic_t&& atomic, obj_t&& old) noexcept
    {
      while(FWD(atomic).load() == old);
    }

    template<typename atomic_t>
    inline void atomic_notify_one(atomic_t&& atomic) noexcept
    {
    }

    template<typename atomic_t>
    inline void atomic_notify_all(atomic_t&& atomic) noexcept
    {
    }
#endif

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
      active = true;
    }

    void reset() noexcept
    {
      child_active = nullptr;
      active = false;
      details::atomic_notify_all(active);
    }

    void operator()()
    {
      if(child_active)
      [[unlikely]]
      {
        details::atomic_wait(*child_active, true);
      }
      func();
      reset();
    }

    operator bool() const noexcept
    {
      return active;
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

    ~job_ref()
    {
      if(ref)
      [[unlikely]]
      {
        ref->reset();
      }
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
      return !active;
    }

    void wait() const noexcept
    {
      details::atomic_wait(active, true);
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
  class queue
  {
    static_assert((max_nr_of_jobs& (max_nr_of_jobs - 1)) == 0, "number of jobs must be a power of 2");

    using atomic_index_t = std::atomic_ptrdiff_t;
    using index_t = typename atomic_index_t::value_type;

    static constexpr auto index_mask = max_nr_of_jobs - 1u;
    static constexpr job* null_job = nullptr;
    static constexpr details::atomic_flag null_flag{false};

  public:
    queue(execution_policy policy = execution_policy::concurrent) noexcept:
      policy_(policy)
    {
    }
    queue(queue&&) = delete;
    queue(const queue&) = delete;
    auto operator=(queue&&) = delete;
    auto operator=(const queue&) = delete;

    ~queue()
    {
      // pop & discard (reset) remaining jobs
      while(!empty())
      {
        (void)pop();
      }
      // wait for any active (stolen) jobs
      for(auto& job : jobs_)
      {
        details::atomic_wait(job.active, true);
      }
      quit();
    }

    void quit() noexcept
    {
      bottom_ = -1;
      top_ = -1;
      // release potentially waiting threads
      details::atomic_notify_all(bottom_);
      details::atomic_notify_all(top_);

      while(waiters_ > 0)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds{ 1 });
      }
    }

    template<typename callable_t, typename... arg_ts>
      requires std::invocable<callable_t, arg_ts...>
    const job_token push(callable_t&& func, arg_ts&&... args) noexcept
    {
      assert(size() < max_nr_of_jobs);
    
      const index_t b = bottom_;

      if(b == -1)
      [[unlikely]]
      {
        return null_flag;
      }

      auto& job = jobs_[b & index_mask];
      job.set(FWD(func), FWD(args)...);
      if(policy_ == execution_policy::sequential && b > 0)
      [[unlikely]]
      {
        const index_t prev = b-1;
        job.child_active = &jobs_[prev & index_mask].active;
      }
      std::atomic_thread_fence(std::memory_order_release);
      bottom_ = b + 1;
      details::atomic_notify_one(bottom_);

      return job.active;
    }

    job_ref pop() noexcept
    {
      const index_t b = bottom_ - 1;
      bottom_ = b;
      std::atomic_thread_fence(std::memory_order::seq_cst);
      const index_t t = top_;

      auto* job = null_job;
      if(t <= b)
      [[likely]]
      {
        // non-empty queue
        job = &jobs_[b & index_mask];
        if(t != b)
        [[likely]]
        {
          // there's still more than one item left in the queue
          return job;
        }

        // this is the last item in the queue
        //
        // whether we win or lose a race against steal() we still set
        // the queue to 'empty' (bottom = newTop), because either way it
        // implies that the last job has been taken.
        index_t expected = t;
        if(!top_.compare_exchange_weak(expected, t+1))
        [[unlikely]]
        {
          // lost race against steal()
          job = null_job;
        }
        bottom_ = t+1;
      }
      else
      [[unlikely]]
      {
        bottom_ = t;
      }
      return job;
    }

    job_ref steal() noexcept
    {
      const index_t t = top_;
      std::atomic_thread_fence(std::memory_order_release);
      const index_t b = bottom_;

      if(t < b)
      [[likely]]
      {
        auto* job = &jobs_[t & index_mask];
        index_t expected = t;
        if(top_.compare_exchange_weak(expected, t+1))
        [[likely]]
        {
          // won race against pop()
          return job;
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

      const index_t t = top_;
      std::atomic_thread_fence(std::memory_order_release);
      const index_t b = bottom_;

      if(t == b && b != -1)
      {
        details::atomic_wait(bottom_, b);
      }

      return steal();
    }

    std::size_t size() const noexcept
    {
      return bottom_ - top_;
    }

    bool empty() const noexcept
    {
      return size() == 0;
    }

  private:
    execution_policy policy_ = execution_policy::concurrent;
    std::vector<job> jobs_{max_nr_of_jobs};
    atomic_index_t top_{0};
    atomic_index_t bottom_{0};
    std::atomic_size_t waiters_;
  };
}

#undef FWD