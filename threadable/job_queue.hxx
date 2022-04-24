#pragma once

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

  constexpr auto buffer_size = 54;
  struct function
  {
    struct callable
    {
      virtual ~callable() = default;
      virtual void operator()(void*) = 0;
    };

    template<typename func_t>
    struct wrapped final: callable
    {
      void operator()(void* addr) override
      {
        func_t& func = *static_cast<func_t*>(addr);
        func();
      }
    };

    using buffer_t = std::array<std::uint8_t, buffer_size>;
    static constexpr buffer_t zero_buffer{};

    template<typename func_t>
    void set(func_t&& func)
    {
      wrapped<func_t> obj;
      static_assert(sizeof(obj) == sizeof(callable), "wrapped size must be equal to callable size");
      std::memcpy(buffer.data(), reinterpret_cast<void*>(&obj), sizeof(obj));
      std::memcpy(buffer.data() + sizeof(obj), reinterpret_cast<void*>(&func), sizeof(func));
    }

    void operator()()
    {
      callable& obj = reinterpret_cast<callable&>(*buffer.data());
      void* addr = buffer.data() + sizeof(callable);
      obj(addr);
    }

    operator bool() const
    {
      return std::memcmp(zero_buffer.data(), buffer.data(), sizeof(typename buffer_t::value_type) * buffer.size());
    }

    buffer_t buffer;
  };
  static_assert(sizeof(function) < cache_line_size, "function size must be less than cache line size");

  struct alignas(cache_line_size) job
  {
    void operator()()
    {
      func();
    }

    operator bool() const
    {
      return func;
    }

    job* parent = nullptr;
    function func;
    std::atomic_uint16_t unfinished_jobs;
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
      static_assert(sizeof(func) < buffer_size, "callable is too big");
      std::scoped_lock _{mutex_};

      auto& job = jobs_[bottom_ & MASK];
      job.func.set(std::forward<func_t>(func));
      ++bottom_;
    }

    template<typename func_t, typename... arg_ts>
      requires std::invocable<func_t, arg_ts...>
    void push(func_t func, arg_ts&&... args)
    {
      // perfectly forwarded lvalue references are stored by value in lambda,
      // so need to use tuple+apply trick.
      push([func = std::forward<func_t>(func), args = std::tuple<decltype(args)...>(std::forward<decltype(args)>(args)...)]() mutable{
        std::apply([&func](auto&&... args) mutable{
          std::invoke(std::forward<func_t>(func), std::forward<arg_ts>(args)...);
        }, args);
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
