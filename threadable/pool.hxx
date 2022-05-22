#pragma once

#include <threadable/queue.hxx>

#include <atomic>
#include <concepts>
#include <cstddef>
#include <iterator>
#include <semaphore>
#include <thread>

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

namespace threadable
{
  template<std::size_t max_nr_of_jobs = details::default_max_nr_of_jobs>
  class pool
  {
    using queue_t = queue<max_nr_of_jobs>;
    using queues_t = std::vector<std::shared_ptr<queue_t>>;
  public:

    pool(std::size_t threads) noexcept
    {
      defaultQueue_ = create();

      for(std::size_t i = 0; i < threads; ++i)
      {
        threads_.emplace_back([this]{
          while(run_)
          {
            bool ranJob = false;
            // aquire available jobs (decrement counter)
            sem_.acquire();
            auto queues = std::atomic_load_explicit(&queues_, std::memory_order_acquire);
            for(auto& queue : *queues)
            {
              if(auto job = queue->steal())
              {
                ranJob = true;
                job();
                break;
              }
            }
            if(!ranJob)
            {
              sem_.release();
            }
          }
        });
      }
    }

    ~pool()
    {
      run_ = false;
      for(auto& queue : *queues_)
      {
        queue->quit();
      }
      sem_.release(threads_.size());
      for(auto& thread : threads_)
      {
        thread.join();
      }
    }

    auto create(execution_policy policy = threadable::execution_policy::concurrent) noexcept
    {
      auto oldQueues = std::atomic_load_explicit(&queues_, std::memory_order_acquire);
      auto newQueues = std::make_shared<queues_t>();
      std::copy(std::cbegin(*oldQueues), std::cend(*oldQueues), std::back_inserter(*newQueues));
      newQueues->emplace_back(std::make_shared<queue_t>(policy, [this](...){
        // whenever a new job is pushed, release a thread (increment counter)
        sem_.release();
      }));
      std::atomic_store_explicit(&queues_, newQueues, std::memory_order_release); 
      return newQueues->back();
    }

    template<typename callable_t, typename... arg_ts>
      requires std::invocable<callable_t, arg_ts...>
    decltype(auto) push(callable_t&& func, arg_ts&&... args) noexcept
    {
      return defaultQueue_->push(FWD(func), FWD(args)...);
    }

  private:
    std::shared_ptr<queue<max_nr_of_jobs>> defaultQueue_;
    std::shared_ptr<queues_t> queues_ = std::make_shared<queues_t>();
    std::counting_semaphore<max_nr_of_jobs> sem_{0};
    std::atomic_bool run_{true};
    std::vector<std::thread> threads_;
  };
}

#undef FWD
