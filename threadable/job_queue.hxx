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
      using invoke_func_t = decltype(&invoke_func<void>);

      template<std::size_t buffer_size>
      struct function
      {
        using buffer_t = std::array<std::uint8_t, buffer_size>;
        static constexpr buffer_t zero_buffer{};

        template<typename func_t>
        void set(func_t&& func) noexcept
        {
          unwrap_func = std::addressof(invoke_func<func_t>);
          if constexpr(std::is_trivially_copyable_v<func_t>)
          {
            std::memcpy(buffer.data(), std::addressof(func), sizeof(func));
          }
          else
          {
            void* ptr = buffer.data();
            auto size = buffer.size();
            // TODO: Use the first byte of the buffer to represent ptr offset
            if (std::align(alignof(func_t), sizeof(func_t), ptr, size))
            {
              (void)::new (ptr) func_t(FWD(func));
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
  static_assert(sizeof(job) == cache_line_size, "job size must equal cache line size");

  class job_queue
  {
    static constexpr auto NUMBER_OF_JOBS = std::size_t{8};
    static constexpr auto MASK = NUMBER_OF_JOBS - std::size_t{1u};

  public:

    template<typename func_t>
    void push(func_t&& func) noexcept
      requires std::invocable<func_t>
    {
      static_assert(sizeof(func) <= details::job_buffer_size, "callable is too big");
      std::scoped_lock _{mutex_};

      auto& job = jobs_[bottom_ & MASK];
      job.func.set(FWD(func));
      ++bottom_;
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
      std::scoped_lock _{mutex_};
      if (const auto jobCount = bottom_ - top_; jobCount <= 0)
      {
          // no job left in the queue
          std::terminate();
      }
 
      --bottom_;
      return jobs_[bottom_ & MASK];
    }

    job* steal() noexcept
    {
      std::scoped_lock _{mutex_};
      if (const auto jobCount = bottom_ - top_; jobCount <= 0)
      {
          // no job there to steal
          return nullptr;
      }

      auto& job = jobs_[top_ & MASK];
      ++top_;
      return &job;
    }

    std::size_t size() const noexcept
    {
      std::scoped_lock _{mutex_};
      return bottom_ - top_;
    }
  private:
    mutable std::mutex mutex_;
    std::array<job, 8> jobs_;
    std::size_t top_;
    std::size_t bottom_;
  };
}

#undef FWD