#pragma once

#include <threadable/queue.hxx>
#include <threadable/std_atomic_wait.hxx>
#include <threadable/std_concepts.hxx>

#include <atomic>
#include <cstddef>
#include <iterator>
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

            std::size_t old;

            // claim job as soon as it becomes available
            do
            {
              old = readyCount_.load(std::memory_order_acquire);
              if(old == 0)
              {
                details::atomic_wait(readyCount_, old);
              }
            }
            while(old == 0 || !readyCount_.compare_exchange_weak(old, old-1, std::memory_order_release));

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
              // if we never ran a job, we need to reverse our decrement
              readyCount_.fetch_add(1, std::memory_order_release);
              details::atomic_notify_one(readyCount_);
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

      // release all waiting threads
      readyCount_.fetch_add(threads_.size(), std::memory_order_release);
      details::atomic_notify_all(readyCount_);
      for(auto& thread : threads_)
      {
        thread.join();
      }
    }

    auto create(execution_policy policy = threadable::execution_policy::concurrent) noexcept
    {
      // create copy of queues & append queue, then atomically swap
      auto newQueues = copy_queues();
      newQueues->emplace_back(std::make_shared<queue_t>(policy, [this](...){
        // whenever a new job is pushed, release a thread (increment counter)
        readyCount_.fetch_add(1, std::memory_order_release);
        details::atomic_notify_one(readyCount_);
      }));
      std::atomic_store_explicit(&queues_, newQueues, std::memory_order_release); 
      return newQueues->back();
    }

    bool remove(queue<max_nr_of_jobs>& q) noexcept
    {
      // create copy of queues & remove queue, then atomically swap
      auto newQueues = copy_queues();
      if(std::erase_if(*newQueues, [&q](const auto& q2){ return q2.get() == &q; }) > 0)
      {
        std::atomic_store_explicit(&queues_, newQueues, std::memory_order_release);
        q.set_notify(nullptr);
        return true;
      }
      else
      {
        return false;
      }
    }

    template<typename callable_t, typename... arg_ts>
      requires std::invocable<callable_t, arg_ts...>
    decltype(auto) push(callable_t&& func, arg_ts&&... args) noexcept
    {
      return defaultQueue_->push(FWD(func), FWD(args)...);
    }

    std::size_t size() const noexcept
    {
      return readyCount_;
    }

  private:

    auto copy_queues() noexcept
    {
      // make a new list of queues, copy from old list
      auto oldQueues = std::atomic_load_explicit(&queues_, std::memory_order_acquire);
      auto newQueues = std::make_shared<queues_t>();
      std::copy(std::cbegin(*oldQueues), std::cend(*oldQueues), std::back_inserter(*newQueues));
      return newQueues;
    }

    std::atomic_bool run_{true};
    std::atomic_size_t readyCount_{0};
    std::shared_ptr<queue<max_nr_of_jobs>> defaultQueue_;
    std::shared_ptr<queues_t> queues_ = std::make_shared<queues_t>();
    std::vector<std::thread> threads_;
  };
}

#undef FWD
