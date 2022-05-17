#pragma once

#include <threadable/function.hxx>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <concepts>
#include <vector>

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

namespace threadable
{
  namespace details
  {
    using atomic_flag = std::atomic_bool;

    struct job_base
    {
      atomic_flag active;
      atomic_flag* child_active = nullptr;
    };
    static constexpr auto job_buffer_size = cache_line_size - sizeof(job_base) - sizeof(function<0>);

    struct job_token
    {
      job_token(details::atomic_flag& active):
        active(active)
      {}

      bool done() const
      {
        return !active;
      }

      void wait() const
      {
        active.wait(true);
      }

    private:
      details::atomic_flag& active;
    };
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
      active.notify_all();
    }

    void operator()()
    {
      if(child_active)
      {
        child_active->wait(true);
      }
      func();
      reset();
    }

    operator bool() const
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
      {
        ref->reset();
      }
    }

    void operator()()
    {
      (*ref)();
      ref = nullptr;
    }

    operator bool() const
    {
      return ref && *ref;
    }

  private:
    job* ref;
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
    using atomic_index_t = std::atomic_ptrdiff_t;
    using index_t = typename atomic_index_t::value_type;

    static_assert((max_nr_of_jobs & (max_nr_of_jobs - 1)) == 0, "number of jobs must be a power of 2");
    static constexpr auto MASK = max_nr_of_jobs - 1u;
    static constexpr job* null_job = nullptr;

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
        job.active.wait(true);
      }
    }

    template<typename callable_t, typename... arg_ts>
      requires std::invocable<callable_t, arg_ts...>
    details::job_token push(callable_t&& func, arg_ts&&... args) noexcept
    {
      assert(size() < max_nr_of_jobs);
    
      const index_t b = bottom_;
      auto& job = jobs_[b & MASK];
      job.set(FWD(func), FWD(args)...);
      if(policy_ == execution_policy::sequential && b > 0)
      {
        const index_t prev = b-1;
        job.child_active = &jobs_[prev & MASK].active;
      }
      std::atomic_thread_fence(std::memory_order_release);
      bottom_ = b + 1;

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
      {
        // non-empty queue
        job = &jobs_[b & MASK];
        if(t != b)
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
        {
          // lost race against steal()
          job = null_job;
        }
        bottom_ = t+1;
      }
      else
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
      {
        auto* job = &jobs_[t & MASK];
        index_t expected = t;
        if(top_.compare_exchange_weak(expected, t+1))
        {
          // won race against pop()
          return job;
        }
      }
      return null_job;
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
  };
}

#undef FWD