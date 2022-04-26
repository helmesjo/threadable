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
          std::invoke(std::forward<func_t>(func));
      }
      using invoke_func_t = decltype(&invoke_func<void>);

      template<std::size_t buffer_size>
      struct function
      {
        using buffer_t = std::array<std::uint8_t, buffer_size>;
        static constexpr buffer_t zero_buffer{};

        template<typename func_t>
        void set(func_t&& func)
        {
          unwrap_func = std::addressof(invoke_func<func_t>);
          // TODO:
          //    Check out 'placement new' & std::align instead of memcpy for none trivially copyable types,
          //    else this will explode some day.
          if constexpr(std::is_trivially_copyable_v<std::remove_reference_t<func_t>>)
          {
            std::memcpy(buffer.data(), std::addressof(func), sizeof(func));
          }
        }

        void operator()()
        {
          unwrap_func(buffer.data());
        }

        operator bool() const
        {
          return std::memcmp(zero_buffer.data(), buffer.data(), buffer.size() * sizeof(typename buffer_t::value_type));
        }

        invoke_func_t unwrap_func;
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
      job.func.set(std::forward<func_t>(func));
      ++bottom_;
    }

    template<typename func_t, typename... arg_ts>
      requires std::invocable<func_t, arg_ts...>
    void push(func_t func, arg_ts&&... args)
    {
      push([func = std::forward<func_t>(func), ...args = std::forward<decltype(args)>(args)]() mutable{
        std::invoke(std::forward<func_t>(func), std::forward<arg_ts>(args)...);
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
