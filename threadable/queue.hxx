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
#if __cpp_lib_hardware_interference_size >= 201603
  // Pretty much no compiler implements this yet
  constexpr auto cache_line_size = std::hardware_destructive_interference_size;
#else
  // TODO: Make portable
  constexpr auto cache_line_size = std::size_t{64};
#endif

  namespace details
  {
      template<typename func_t>
      void invoke_func(void* addr)
      {
        func_t& func = *static_cast<func_t*>(addr);
        std::invoke(func);
        if constexpr(std::is_destructible_v<func_t>)
        {
          func.~func_t();
        }
      }
      using invoke_func_t = decltype(&invoke_func<void(*)()>);

      template<std::size_t buffer_size>
      struct function
      {
        using buffer_t = std::array<std::uint8_t, buffer_size>;

        constexpr function() noexcept = default;

        template<typename func_t>
        void set(func_t&& func) noexcept
        {
          static_assert(sizeof(func_t) <= buffer_size, "callable won't fit in function buffer");
          unwrap_func = std::addressof(invoke_func<func_t>);
          if constexpr(std::is_trivially_copyable_v<func_t>)
          {
            std::memcpy(buffer.data(), std::addressof(func), sizeof(func));
          }
          else
          {
            void* ptr = buffer.data();
            if(::new ((void*)buffer.data()) func_t(FWD(func)) != ptr)
            {
              std::terminate();
            }
          }
        }

        void operator()()
        {
          unwrap_func(buffer.data());
          unwrap_func = nullptr;
        }

        operator bool() const noexcept
        {
          return unwrap_func;
        }

        invoke_func_t unwrap_func = nullptr;
        buffer_t buffer;
      };

      struct job_base
      {
        job_base* parent = nullptr;
        std::atomic_uint16_t unfinished_jobs;
      };
      static constexpr auto job_buffer_size = cache_line_size - sizeof(job_base) - sizeof(function<0>);
  }

  struct alignas(cache_line_size) job final: details::job_base
  {
      constexpr job() noexcept = default;

      template<typename func_t>
      void set(func_t&& func) noexcept
      {
        this->func.set(FWD(func));
      }

      void operator()()
      {
        func();
      }

      operator bool() const
      {
        return func;
      }

      details::function<details::job_buffer_size> func;
  };
  static job null_job;
  static_assert(sizeof(job) == cache_line_size, "job size must equal cache line size");

  template<std::size_t max_nr_of_jobs = 4096>
  class queue
  {
    static_assert((max_nr_of_jobs & (max_nr_of_jobs - 1)) == 0, "number of jobs must be a power of 2");
    static constexpr auto MASK = max_nr_of_jobs - std::size_t{1u};

  public:
    constexpr queue() noexcept = default;

    template<typename func_t>
    void push(func_t&& func) noexcept
      requires std::invocable<func_t>
    {
      static_assert(sizeof(func) <= details::job_buffer_size, "callable is too big");

      const auto b = bottom_.load();
      auto& job = jobs_[b & MASK];
      job.set(FWD(func));
      std::atomic_thread_fence(std::memory_order_release);
      bottom_ = b + 1;
    }

    template<typename func_t, typename... arg_ts>
      requires std::invocable<func_t, arg_ts...>
    void push(func_t&& func, arg_ts&&... args) noexcept
    {
      push([func = FWD(func), ...args = FWD(args)]() mutable{
        std::invoke(FWD(func), FWD(args)...);
      });
    }

    job& pop() noexcept
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
        // the queue to 'empty' (buttom = top), because either way it
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

    job& steal() noexcept
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
    std::atomic_size_t top_{0};
    std::atomic_size_t bottom_{0};
  };
}

#undef FWD