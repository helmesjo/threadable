#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <new>

namespace threadable
{
#if __cpp_lib_hardware_interference_size >= 201603
  // Pretty much no compiler implements this yet
  constexpr auto cache_line_size = std::hardware_destructive_interference_size;
#else
  // TODO: Make portable
  constexpr auto cache_line_size = std::size_t{64};
#endif
  
  using function_t = std::function<void()>;
  struct alignas(cache_line_size) job
  {
    function_t function;
    std::weak_ptr<job> parent;
    std::atomic_size_t unfinished_jobs;
    std::array<char, 8> buffer;
  };
  static_assert(sizeof(job) == cache_line_size);

  class job_queue
  {
    static constexpr auto NUMBER_OF_JOBS = std::size_t{8};
    static constexpr auto MASK = NUMBER_OF_JOBS - std::size_t{1u};
  
  public:
  
    void push(function_t&& fun) noexcept
    {
      std::scoped_lock _{mutex_};

      auto& job = jobs_[bottom_ & MASK];
      job.function = std::move(fun);
      ++bottom_;
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
