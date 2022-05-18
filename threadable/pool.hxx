#pragma once

#include <threadable/queue.hxx>
#include <concepts>
#include <cstddef>
#include <thread>

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

namespace threadable
{
  template<std::size_t max_nr_of_jobs = details::default_max_nr_of_jobs>
  class pool
  {
  public:
    pool(std::size_t threads)
    {
      for(std::size_t i = 0; i < threads; ++i)
      {
        threads_.emplace_back([this]{
          while(run_)
          {
            if(auto job = queue_.steal_or_wait())
            {
              job();
            }
          }
        });
      }
    }

    ~pool()
    {
      run_ = false;
      queue_.quit();
      for(auto& thread : threads_)
      {
        thread.join();
      }
    }

    template<typename callable_t, typename... arg_ts>
      requires std::invocable<callable_t, arg_ts...>
    decltype(auto) push(callable_t&& func, arg_ts&&... args) noexcept
    {
      return queue_.push(FWD(func), FWD(args)...);
    }

  private:
    std::atomic_bool run_{true};
    queue<max_nr_of_jobs> queue_;
    std::vector<std::thread> threads_;
  };
}

#undef FWD
