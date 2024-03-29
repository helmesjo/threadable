#pragma once

#include <threadable/function.hxx>
#include <threadable/queue.hxx>
#include <threadable/std_concepts.hxx>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <thread>
#if __has_include(<pstld/pstld.h>)
  #include <pstld/pstld.h>
#endif
#ifdef __cpp_lib_execution
  #include <execution>
#else
  #error requires __cpp_lib_execution
#endif

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

namespace threadable
{
  template<std::size_t max_nr_of_jobs = details::default_max_nr_of_jobs>
  class pool
  {
    using clk_t = std::chrono::high_resolution_clock;

  public:
    using queue_t = queue<max_nr_of_jobs>;

    struct alignas(details::cache_line_size) entry
    {
      queue_t         queue;
      std::size_t     last_executed = 0;
      std::size_t     jobs_ready    = 0;
      clk_t::duration avg_dur{1};
    };

    static constexpr auto derp = sizeof(entry);

    using queues_t = std::vector<std::shared_ptr<entry>>;

    pool(bool run = true) noexcept
    {
      details::atomic_clear(quit_);

      if (run)
      {
        thread_ = std::thread(
          [this]
          {
            while (true)
            {
              // 1. Check if quit = true. If so, bail.
              // 2. Wait for jobs to queued.
              // 3. Execute all jobs.
              if (details::atomic_test(quit_, std::memory_order_acquire)) [[unlikely]]
              {
                break;
              }
              else [[likely]]
              {
                details::atomic_wait(ready_, false, std::memory_order_acquire);
              }
              execute();
            }
          });
      }
    }

    pool(pool const&) = delete;
    pool(pool&&)      = delete;

    auto operator=(pool const&) -> pool& = delete;
    auto operator=(pool&&) -> pool&      = delete;

    ~pool()
    {
      details::atomic_set(quit_, std::memory_order_release);
      notify();
      if (thread_.joinable())
      {
        thread_.join();
      }
    }

    [[nodiscard]] auto
    create(execution_policy policy = execution_policy::parallel) noexcept -> queue_t&
    {
      return add(queue_t(policy));
    }

    [[nodiscard]] auto
    add(queue_t&& q) -> queue_t&
    {
      queue_t* queue = nullptr;
      {
        auto _ = std::scoped_lock{queueMutex_};
        queue  = &queues_.emplace_back(std::make_unique<entry>(std::move(q)))->queue;
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

    [[nodiscard]] auto
    remove(queue_t&& queue) noexcept -> bool // NOLINT
    {
      auto _ = std::scoped_lock{queueMutex_};
      if (auto itr = std::find_if(std::begin(queues_), std::end(queues_),
                                  [&queue](auto const& entry)
                                  {
                                    return &entry->queue == &queue;
                                  });
          itr != std::end(queues_))
      {
        queue.shutdown();
        queues_.erase(itr);
        return true;
      }
      else
      {
        return false;
      }
    }

    [[nodiscard]] auto
    queues() const noexcept -> std::size_t
    {
      auto _ = std::scoped_lock{queueMutex_};
      return queues_.size();
    }

    [[nodiscard]] auto
    size() const noexcept -> std::size_t
    {
      auto _ = std::scoped_lock{queueMutex_};
      return std::ranges::count(queues_,
                                [](auto const& queue)
                                {
                                  return queue.size();
                                });
    }

    static constexpr auto
    max_size() noexcept -> std::size_t
    {
      return max_nr_of_jobs;
    }

    auto
    execute()
    {
      using namespace std::chrono;

      ready_.store(false, std::memory_order_relaxed);
      queues_t queues;
      {
        auto _ = std::scoped_lock{queueMutex_};
        queues = queues_;
        if (queues.empty())
        {
          return;
        }
      }
      auto const begin = std::begin(queues);
      auto const end   = std::end(queues);

      std::for_each(begin, end,
                    [](auto& entry)
                    {
                      entry->jobs_ready = entry->queue.size();
                    });

      // calculate the max allowed executation duration for each queue
      auto const fastest = *std::min_element(begin, end,
                                             [](auto const& lhs, auto const& rhs)
                                             {
                                               return lhs->avg_dur * lhs->jobs_ready //
                                                      < rhs->avg_dur * rhs->jobs_ready;
                                             });
      auto const slowest = *std::max_element(begin, end,
                                             [](auto const& lhs, auto const& rhs)
                                             {
                                               return lhs->avg_dur < rhs->avg_dur;
                                             });

      auto const a = clk_t::duration{slowest->avg_dur};
      auto const b = clk_t::duration{fastest->avg_dur * fastest->jobs_ready};
      maxDuration_ = a > b ? a : b;

      auto const exec = [this](auto& entry)
      {
        // given the max allowed duration, calculate how many jobs can be executed
        const auto maxJobs = std::max(clk_t::rep{1}, maxDuration_.count() / entry->avg_dur.count());
        const auto start   = clk_t::now();
        if (entry->last_executed = entry->queue.execute(maxJobs); entry->last_executed > 0)
        {
          entry->avg_dur = (clk_t::now() - start) / entry->last_executed;
        }
        else
        {
          entry->avg_dur = clk_t::duration::max();
        }
      };

      // for (auto const& q : queues)
      // {
      //   auto const maxJobs = std::max(1ll, maxDuration_.count() / q.avg_dur.count());
      //   std::cout << std::format("before: queue: {}\tmax dur: {}\tmax jobs: {}\tavg dur: "
      //                            "{}\tlast executed: {}\tavailable: {}\n",
      //                            (void*)q.queue.get(), duration_cast<microseconds>(maxDuration_),
      //                            maxJobs, duration_cast<microseconds>(q.avg_dur),
      //                            q.last_executed, q.queue->size());
      // }
      std::for_each(std::execution::par, begin, end, exec);
      // for (auto const& q : queues)
      // {
      //   auto const maxJobs = std::max(1ll, maxDuration_.count() / q.avg_dur.count());
      //   std::cout << std::format("after : queue: {}\tmax dur: {}\tmax jobs: {}\tavg dur: "
      //                            "{}\tlast executed: {}\tavailable: {}\n",
      //                            (void*)q.queue.get(), duration_cast<microseconds>(maxDuration_),
      //                            maxJobs, duration_cast<microseconds>(q.avg_dur),
      //                            q.last_executed, q.queue->size());
      // }
      // std::cout << '\n';
      if (std::any_of(begin, end,
                      [](auto const& entry)
                      {
                        return entry->jobs_ready > entry->last_executed;
                      }))
      {
        ready_.store(true, std::memory_order_relaxed);
      }
    }

  private:
    inline void
    notify()
    {
      // whenever a new job is pushed, release executing thread
      ready_.store(true, std::memory_order_release);
      details::atomic_notify_one(ready_);
    }

    alignas(details::cache_line_size) mutable std::mutex queueMutex_;
    alignas(details::cache_line_size) clk_t::duration maxDuration_ = clk_t::duration::max();
    alignas(details::cache_line_size) details::atomic_flag_t quit_;
    alignas(details::cache_line_size) std::atomic_bool ready_{false};
    alignas(details::cache_line_size) queues_t queues_;
    alignas(details::cache_line_size) std::thread thread_;
  };

  namespace details
  {
    using pool_t = threadable::pool<details::default_max_nr_of_jobs>;
    extern auto pool() -> pool_t&;
    using queue_t = pool_t::queue_t;
  }

  template<execution_policy policy = execution_policy::parallel, std::copy_constructible callable_t,
           typename... arg_ts>
  [[nodiscard]] inline auto
  push(callable_t&& func, arg_ts&&... args) noexcept
    requires requires (details::queue_t q) { q.push(FWD(func), FWD(args)...); }
  {
    static auto& queue = details::pool().create(policy); // NOLINT
    return queue.push(FWD(func), FWD(args)...);
  }

  [[nodiscard]] inline auto
  create(execution_policy policy = execution_policy::parallel) noexcept -> details::queue_t&
  {
    return details::pool().create(policy);
  }

  inline auto
  remove(details::queue_t&& queue) noexcept -> bool
  {
    return details::pool().remove(std::move(queue));
  }
}

#undef FWD
