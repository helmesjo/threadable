#pragma once

#include <threadable/queue.hxx>
#include <threadable/std_atomic.hxx>
#include <threadable/std_concepts.hxx>

#include <atomic>
#include <cstddef>
#include <thread>

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

#if __has_cpp_attribute(likely) && __has_cpp_attribute(unlikely)
#define LIKELY [[likely]]
#define UNLIKELY [[unlikely]]
#else
#define LIKELY
#define UNLIKELY
#endif

namespace threadable
{
  template<std::size_t max_nr_of_jobs = details::default_max_nr_of_jobs>
  class pool
  {
  public:
    using queue_t = queue<max_nr_of_jobs>;
    using queues_t = std::vector<std::shared_ptr<queue_t>>;

    pool(std::size_t threads) noexcept
    {
      details::atomic_clear(quit_);
      defaultQueue_ = create();

      for(std::size_t i = 0; i < threads; ++i)
      {
        threads_.emplace_back([this]{
          auto prevCount = readyCount_.load(std::memory_order_acquire);
          while(true)
          {
            // claim job as soon as it becomes available, and wait in the meantime
            details::atomic_wait(readyCount_, std::size_t{0});
            while(prevCount == 0 || !readyCount_.compare_exchange_weak(prevCount, prevCount-1))
            {
              if(details::atomic_test(quit_, std::memory_order_relaxed))
              UNLIKELY
              {
                // thread exit
                return;
              }
              else if(prevCount == 0)
              LIKELY
              {
                details::atomic_wait(readyCount_, std::size_t{0});
                prevCount = readyCount_.load(std::memory_order_acquire);
              }
            }

            const auto queues = std::atomic_load_explicit(&queues_, std::memory_order_acquire);
            if(queues->empty())
            UNLIKELY
            {
              continue;
            }
            // now we have reserved one job, so run until we acqire it.
            bool ran = false;
            while(!ran && !details::atomic_test(quit_, std::memory_order_relaxed))
            LIKELY
            {
              for(auto& queue : *queues)
              {
                if(auto job = queue->steal())
                LIKELY
                {
                  job();
                  ran = true;
                  break;
                }
              }
            }
          }
        });
      }
    }

    ~pool()
    {
      details::atomic_test_and_set(quit_, std::memory_order_seq_cst);
      // release all waiting threads
      readyCount_.fetch_add(threads_.size() * 1000, std::memory_order_release);
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
        notify_jobs(1);
      }));
      std::atomic_store_explicit(&queues_, newQueues, std::memory_order_release);
      return newQueues->back();
    }

    bool add(std::shared_ptr<queue_t> q)
    {
      if(std::find_if(std::begin(*queues_), std::end(*queues_), [&q](const auto& q2){
        return q2.get() == q.get();
      }) != std::end(*queues_))
      {
        // queue already exists
        return false;
      }

      q->set_notify([this](...){
        notify_jobs(1);
      });

      // create copy of queues & append queue, then atomically swap
      const auto jobs = q->size();
      auto newQueues = copy_queues();
      newQueues->emplace_back(std::move(q));
      std::atomic_store_explicit(&queues_, newQueues, std::memory_order_release);
      readyCount_.fetch_add(jobs, std::memory_order_release);
      details::atomic_notify_all(readyCount_);
      return true;
    }

    bool remove(queue_t& q) noexcept
    {
      // create copy of queues & remove queue, then atomically swap
      auto newQueues = copy_queues();
      if(std::erase_if(*newQueues, [&q](const auto& q2){ return q2.get() == &q; }) > 0)
      {
        q.set_notify(nullptr);
        const auto jobs = q.size();
        auto old = readyCount_.load(std::memory_order_acquire);
        std::atomic_store_explicit(&queues_, newQueues, std::memory_order_release);
        while(!readyCount_.compare_exchange_weak(old, old - std::min(old, jobs)));
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

    void wait() const noexcept
    {
      while(readyCount_.load(std::memory_order_relaxed) > 0){ std::this_thread::yield(); };
    }

    std::size_t size() const noexcept
    {
      return readyCount_.load(std::memory_order_acquire);
    }

  private:
    inline void notify_jobs(std::size_t jobs)
    {
      // whenever a new job is pushed, release a thread (increment counter)
      readyCount_.fetch_add(jobs, std::memory_order_release);
      details::atomic_notify_one(readyCount_);
    }

    auto copy_queues() noexcept
    {
      // make a new list of queues, copy from old list
      auto oldQueues = std::atomic_load_explicit(&queues_, std::memory_order_acquire);
      auto newQueues = std::make_shared<queues_t>();
      std::copy(std::cbegin(*oldQueues), std::cend(*oldQueues), std::back_inserter(*newQueues));
      return newQueues;
    }

    details::atomic_flag quit_;
    std::atomic_size_t readyCount_{0};
    std::shared_ptr<queue<max_nr_of_jobs>> defaultQueue_;
    std::shared_ptr<queues_t> queues_ = std::make_shared<queues_t>();
    std::vector<std::thread> threads_;
  };
}

#undef FWD
#undef LIKELY
#undef UNLIKELY