#pragma once

#include <threadable/queue.hxx>
#include <threadable/std_concepts.hxx>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <thread>
#if __cpp_lib_execution
  #include <execution>
#endif
#if __has_include(<pstld/pstld.h>)
  #include <pstld/pstld.h>
#endif

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

namespace threadable
{
  template<std::size_t max_nr_of_jobs = details::default_max_nr_of_jobs>
  class pool
  {
  public:
    using queue_t  = queue<max_nr_of_jobs>;
    using queues_t = std::vector<std::shared_ptr<queue_t>>;

    pool() noexcept
    {
      details::atomic_clear(quit_);

      thread_ = std::thread(
        [this]
        {
          while (true)
          {
            // 1. Check if quit = true.
            // 2. Wait for jobs to executed.
            // 3. Execute all jobs.
            if (details::atomic_test(quit_, std::memory_order_acquire)) [[unlikely]]
            {
              // thread exit.
              break;
            }
            else [[likely]]
            {
              details::atomic_wait(ready_, false, std::memory_order_acquire);
            }

            queues_t queues;
            {
              auto _ = std::scoped_lock{queueMutex_};
              queues = queues_;
            }
            const auto begin = std::begin(queues);
            const auto end   = std::end(queues);

            // at this point we know all jobs are getting executed.
            ready_.store(false, std::memory_order_relaxed);
#ifdef __cpp_lib_execution
            std::for_each(std::execution::par, begin, end,
                          [this](const auto& q)
                          {
                            while (q->execute() > 0)
                              ;
                          });
#else
            std::for_each(begin, end,
                          [this](const auto& q)
                          {
                            while (q->execute() > 0)
                              ;
                          });
#endif
          }
        });
    }

    ~pool()
    {
      details::atomic_set(quit_, std::memory_order_release);
      notify();
      thread_.join();
    }

    [[nodiscard]]
    queue_t& create(execution_policy policy = execution_policy::parallel) noexcept
    {
      return add(std::make_unique<queue_t>(policy));
    }

    [[nodiscard]]
    queue_t& add(std::unique_ptr<queue_t> q)
    {
      queue_t* queue = nullptr;
      {
        auto _ = std::scoped_lock{queueMutex_};
        queue  = queues_.emplace_back(std::move(q)).get();
        queue->set_notify(
          [this](...)
          {
            notify();
          });
      }

      if (!queue->empty())
      {
        notify();
      }
      return *queue;
    }

    bool remove(queue_t&& q) noexcept
    {
      auto itr = std::find_if(std::begin(queues_), std::end(queues_),
                              [&q](auto const& q2)
                              {
                                return q2.get() == &q;
                              });
      if (itr != std::end(queues_))
      {
        q.shutdown();
        auto _ = std::scoped_lock{queueMutex_};
        queues_.erase(itr);
        return true;
      }
      else
      {
        return false;
      }
    }

    std::size_t queues() const noexcept
    {
      return queues_.size();
    }

    std::size_t size() const noexcept
    {
      return std::ranges::count(queues_,
                                [](auto const& queue)
                                {
                                  return queue->size();
                                });
    }

    static constexpr std::size_t max_size() noexcept
    {
      return max_nr_of_jobs;
    }

  private:
    inline void notify()
    {
      // whenever a new job is pushed, release a thread (increment counter)
      ready_.store(true, std::memory_order_release);
      details::atomic_notify_one(ready_);
    }

    std::recursive_mutex queueMutex_;
    alignas(details::cache_line_size) details::atomic_flag_t quit_;
    alignas(details::cache_line_size) std::atomic_bool ready_{false};
    alignas(details::cache_line_size) queues_t queues_;
    std::thread thread_;
  };

  namespace details
  {
    using pool_t = threadable::pool<(1 << 16)>;
    extern pool_t& pool();
    using queue_t = pool_t::queue_t;
  }

  template<execution_policy policy = execution_policy::parallel, std::copy_constructible callable_t,
           typename... arg_ts>
  inline auto push(callable_t&& func, arg_ts&&... args) noexcept
    requires requires (details::queue_t q) { q.push(FWD(func), FWD(args)...); }
  {
    static auto& queue = details::pool().create(policy);
    return queue.push(FWD(func), FWD(args)...);
  }

  inline details::queue_t& create(execution_policy policy = execution_policy::parallel) noexcept
  {
    return details::pool().create(policy);
  }

  inline bool remove(details::queue_t&& queue) noexcept
  {
    return details::pool().remove(std::move(queue));
  }
}

#undef FWD
