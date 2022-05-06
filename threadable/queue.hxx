#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <concepts>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <tuple>

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

// Workarounds for unimplemented concepts & type traits (specifically with libc++)
// NOTE: Intentionally placed in 'std' to be easily removed when no longer needed
//       since below definitions aren't as conforming as std equivalents).
#if defined(__clang__) && defined(_LIBCPP_VERSION) // libc++

namespace std
{
#if (__clang_major__ <= 13 && (defined(__APPLE__) || defined(__EMSCRIPTEN__))) || __clang_major__ < 13
  // Credit: https://en.cppreference.com
  template<class F, class... Args>
  concept invocable =
    requires(F&& f, Args&&... args) {
      std::invoke(std::forward<F>(f), std::forward<Args>(args)...);
      /* not required to be equality preserving */
    };
#endif
}
#endif

namespace threadable
{
  namespace details
  {
#if __cpp_lib_hardware_interference_size >= 201603
        // Pretty much no compiler implements this yet
        constexpr auto cache_line_size = std::hardware_destructive_interference_size;
#else
        // TODO: Make portable
        constexpr auto cache_line_size = std::size_t{64};
#endif

      template<typename callable_t>
      void invoke_func(void* addr)
      {
        callable_t& func = *static_cast<callable_t*>(addr);
        std::invoke(func);
        if constexpr(std::is_destructible_v<callable_t>)
        {
          func.~callable_t();
        }
      }
      using invoke_func_t = decltype(&invoke_func<void(*)()>);

      struct job_base
      {
        std::atomic_flag done;
      };
  }

  template<std::size_t buffer_size = details::cache_line_size>
  struct function
  {
    template<typename callable_t>
      requires std::invocable<callable_t>
    void set(callable_t&& func) noexcept
    {
      using callable_value_t = std::remove_reference_t<callable_t>;
      static_assert(sizeof(callable_value_t) <= buffer_size, "callable won't fit in function buffer");
      unwrap_func = std::addressof(details::invoke_func<callable_value_t>);
      void* buffPtr = buffer.data();
      if constexpr(std::is_trivially_copyable_v<callable_value_t>)
      {
        std::memcpy(buffPtr, std::addressof(func), sizeof(func));
      }
      else
      {
        if(::new (buffPtr) callable_value_t(FWD(func)) != buffPtr)
        {
          std::terminate();
        }
      }
    }

    template<typename callable_t, typename... arg_ts>
      requires std::invocable<callable_t, arg_ts...>
    void set(callable_t&& func, arg_ts&&... args) noexcept
    {
      set([func = FWD(func), ...args = FWD(args)] mutable noexcept {
        std::invoke(FWD(func), FWD(args)...);
      });
    }

    void reset() noexcept
    {
      unwrap_func = nullptr;
    }

    template<typename callable_t, typename... arg_ts>
      requires std::invocable<callable_t, arg_ts...> 
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

    void operator()()
    {
      unwrap_func(buffer.data());
      reset();
    }

    operator bool() const noexcept
    {
      return unwrap_func;
    }

  private:
    details::invoke_func_t unwrap_func = nullptr;
    std::array<std::uint8_t, buffer_size> buffer;
  };

  namespace details
  {
    static constexpr auto job_buffer_size = cache_line_size - sizeof(job_base) - sizeof(function<0>);
  }

  struct alignas(details::cache_line_size) job final: details::job_base
  {
    constexpr job() = default;
    job(job&&) = delete;
    job(const job&) = delete;
    auto operator=(job&&) = delete;
    auto operator=(const job&) = delete;
  
    template<typename callable_t, typename... arg_ts>
      requires std::invocable<callable_t, arg_ts...>
    decltype(auto) set(callable_t&& func, arg_ts&&... args) noexcept
    {
      this->func.set(FWD(func), FWD(args)...);
    }

    void reset() noexcept
    {
      func.reset();
    }

    void operator()()
    {
      func();
      done.test_and_set();
      done.notify_all();
    }

    operator bool() const
    {
      return func;
    }

  private:
    function<details::job_buffer_size> func;
  };
  static_assert(sizeof(job) == details::cache_line_size, "job size must equal cache line size");
  static job null_job;

  struct job_ref
  {
    job_ref(job& ref):
      ref(ref)
    {}

    ~job_ref()
    {
      ref.reset();
    }

    void operator()()
    {
      ref();
    }

    operator bool() const
    {
      return ref;
    }

  private:
    job& ref;
  };

  struct job_token
  {
    job_token(std::atomic_flag& flag):
      flag(flag)
    {}
  
    bool done() const
    {
      return flag.test();
    }

    void wait() const
    {
      flag.wait(false);
    }

  private:
    std::atomic_flag& flag;
  };

  template<std::size_t max_nr_of_jobs = 4096>
  class queue
  {
    static_assert((max_nr_of_jobs & (max_nr_of_jobs - 1)) == 0, "number of jobs must be a power of 2");
    static constexpr auto MASK = max_nr_of_jobs - 1u;

  public:
    constexpr queue() noexcept = default;
    queue(queue&&) = delete;
    queue(const queue&) = delete;
    auto operator=(queue&&) = delete;
    auto operator=(const queue&) = delete;
  
    ~queue()
    {
      // pop & execute remaining jobs
      while(!empty())
      {
        if(auto job = pop())
        {
          job();
        }
      }
      // wait for any active (stolen) jobs
      for(auto& job : jobs_)
      {
        if(job)
        {
          job.done.wait(false);
        }
      }
    }

    template<typename callable_t, typename... arg_ts>
      requires std::invocable<callable_t, arg_ts...>
    job_token push(callable_t&& func, arg_ts&&... args) noexcept
    {
      const auto b = bottom_.load();
      auto& job = jobs_[b & MASK];
      job.set(FWD(func), FWD(args)...);
      std::atomic_thread_fence(std::memory_order_release);
      bottom_ = b + 1;

      return job.done;
    }

    job_ref pop() noexcept
    {
      const auto b = bottom_.load() - 1;
      bottom_ = b;
      std::atomic_thread_fence(std::memory_order::seq_cst);
      const auto t = top_.load();

      if(t <= b)
      {
        // non-empty queue
        auto* job = &jobs_[b & MASK];
        if(t != b)
        {
          // there's still more than one item left in the queue
          return *job;
        }

        // this is the last item in the queue
        //
        // whether we win or lose a race against steal() we still set
        // the queue to 'empty' (bottom = newTop), because either way it
        // implies that the last job has been taken.
        auto expected = t;
        if(!top_.compare_exchange_weak(expected, t+1))
        {
          // lost race against steal()
          job = &null_job;
        }
        bottom_ = t+1;
        return *job;
      }
      else
      {
        bottom_ = t;
      }
      return null_job;
    }

    job_ref steal() noexcept
    {
      const auto t = top_.load();
      std::atomic_thread_fence(std::memory_order_release);
      const auto b = bottom_.load();

      if(t < b)
      {
        auto& job = jobs_[t & MASK];
        auto expected = t;
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
    std::array<job, max_nr_of_jobs> jobs_;
    std::atomic_ptrdiff_t top_{0};
    std::atomic_ptrdiff_t bottom_{0};
  };
}

#undef FWD