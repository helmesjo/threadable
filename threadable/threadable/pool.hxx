#pragma once

#include <threadable/queue.hxx>
#include <threadable/std_atomic.hxx>
#include <threadable/std_concepts.hxx>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <thread>
#if __has_include(<execution>)
#include <execution>
#endif
#if __has_include (<pstld/pstld.h>)
  #ifndef __cpp_lib_execution
  #define __cpp_lib_execution 201902L
  #endif

  #ifndef __cpp_lib_parallel_algorithm
  #define __cpp_lib_parallel_algorithm 201603L
  #endif

  #include <pstld/pstld.h>
#endif

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

    pool() noexcept
    {
      details::atomic_clear(quit_);

      thread_ = std::thread([this]{
        while(true)
        {
          // 1. Check if quit = true.
          // 2. Wait for jobs to executed.
          // 3. Execute all jobs.
          if(details::atomic_test(quit_, std::memory_order_acquire))
          UNLIKELY
          {
            // thread exit
            break;
          }
          else
          LIKELY
          {
            details::atomic_wait(readyCount_, std::size_t{0}, std::memory_order_acquire);
          }

          // aquire a copy of queues, and run all jobs available in each
          const auto queues = std::atomic_load_explicit(&queues_, std::memory_order_acquire);
          const auto begin = std::begin(*queues);
          const auto end = std::end(*queues);
#ifdef __cpp_lib_execution
          std::for_each(std::execution::par, begin, end, [this](const auto& q){
            auto executed = q->execute();
            readyCount_.fetch_sub(executed, std::memory_order_release);
          });
#else
          std::for_each(begin, end, [this](const auto& q){
            auto executed = q->execute();
            readyCount_.fetch_sub(executed, std::memory_order_release);
          });
#endif
        }
      });
    }

    ~pool()
    {
      details::atomic_set(quit_, std::memory_order_release);
      notify_jobs(1);
      thread_.join();
    }

    queue_t& create(execution_policy policy = execution_policy::parallel) noexcept
    {
      auto q = std::make_shared<queue_t>(policy);
      add(q);
      return *q;
    }

    bool add(std::shared_ptr<queue_t> q)
    {
      auto _ = std::scoped_lock{queueMutex_};
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

    bool remove(queue_t&& q) noexcept
    {
      auto _ = std::scoped_lock{queueMutex_};
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

    template<execution_policy policy = execution_policy::parallel, std::copy_constructible callable_t, typename... arg_ts>
    auto push(callable_t&& func, arg_ts&&... args) noexcept
      requires requires(queue_t q){ q.push(FWD(func), FWD(args)...); }
    {
      thread_local queue_t& queue = create(policy);
      return queue.push(FWD(func), FWD(args)...);
    }

    template<execution_policy policy = execution_policy::parallel, std::copy_constructible callable_t, typename... arg_ts>
    auto push_slow(callable_t&& func, arg_ts&&... args) noexcept
      requires requires(queue_t q){ q.push_slow(FWD(func), FWD(args)...); }
    {
      thread_local queue_t& queue = create(policy);
      return queue.push_slow(FWD(func), FWD(args)...);
    }

    void wait() const noexcept
    {
      while(readyCount_.load(std::memory_order_relaxed) > 0){ std::this_thread::yield(); };
    }

    std::size_t size() const noexcept
    {
      return readyCount_.load(std::memory_order_acquire);
    }

    static constexpr std::size_t max_size() noexcept
    {
      return max_nr_of_jobs;
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

    std::mutex queueMutex_;
    alignas(details::cache_line_size) details::atomic_flag quit_;
    alignas(details::cache_line_size) std::atomic_size_t readyCount_{0};
    alignas(details::cache_line_size) std::shared_ptr<queues_t> queues_ = std::make_shared<queues_t>();
    std::thread thread_;
  };
}

#undef FWD
#undef LIKELY
#undef UNLIKELY
