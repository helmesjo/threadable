#pragma once

#include <threadable/function.hxx>
#include <threadable/queue.hxx>
#include <threadable/std_concepts.hxx>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <random>
#include <ranges>
#include <thread>

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

namespace fho
{
  class executor
  {
  public:
    executor()
      : work_(execution_policy::sequential)
      , thread_(
          [this]
          {
            run();
          })
    {}

    ~executor()
    {
      stop();
      if (thread_.joinable())
      {
        thread_.join();
      }
    }

    executor(executor const&)                    = delete;
    executor(executor&&)                         = delete;
    auto operator=(executor const&) -> executor& = delete;
    auto operator=(executor&&) -> executor&      = delete;

    template<typename T>
    auto
    submit(std::ranges::subrange<T> range) noexcept
      requires std::invocable<std::ranges::range_value_t<decltype(range)>>
    {
      return work_.push(
        [r = std::move(range)]
        {
          for (auto& j : r)
          {
            j();
          }
        });
    }

    auto
    submit(std::invocable auto&& work) noexcept
    {
      return work_.push(FWD(work));
    }

    void
    stop() noexcept
    {
      stop_.store(true, std::memory_order_relaxed);
      std::ignore = work_.consume();
    }

  private:
    void
    run()
    {
      while (!stop_.load(std::memory_order_relaxed)) [[likely]]
      {
        if (auto c = work_.execute(); c == 0) [[unlikely]]
        {
          std::this_thread::sleep_for(std::chrono::nanoseconds{1});
        }
      }
    }

    std::atomic_bool stop_{false};
    queue<>          work_;
    std::thread      thread_;
  };

  template<std::size_t max_nr_of_jobs = details::default_max_nr_of_jobs>
  class pool
  {
    using clk_t = std::chrono::high_resolution_clock;

  public:
    using queue_t = queue<max_nr_of_jobs>;

    struct alignas(details::cache_line_size) worker
    {
      std::thread thread;
      queue_t     work;
    };

    using queues_t = std::vector<std::shared_ptr<queue_t>>;

    pool(unsigned int workers = std::thread::hardware_concurrency() - 1) noexcept
    {
      // start worker threads
      for (std::size_t i = 0; i < workers; ++i)
      {
        executors_.emplace_back(std::make_unique<executor>());
      }

      // start scheduler thread
      scheduler_ = std::thread(
        [this, workers]
        {
          auto const mt    = workers > 0;
          auto       rd    = std::random_device{};
          auto       gen   = std::mt19937(rd());
          auto       distr = std::uniform_int_distribution<std::size_t>(0, mt ? workers - 1 : 0);

          while (true)
          {
            // 1. Check if quit = true. If so, bail.
            // 2. Distribute queues to workers.
            if (details::atomic_test(stop_, std::memory_order_acquire)) [[unlikely]]
            {
              break;
            }

            queues_t queues;
            {
              auto _ = std::scoped_lock{queueMutex_};
              queues = queues_;
            }

            auto rand      = distr(gen);
            auto submitted = false;
            for (auto& queue : queues)
            {
              if (auto range = queue->consume(); !range.empty()) [[likely]]
              {
                submitted = true;
                if (mt) [[likely]]
                {
                  // assign to (random) worker
                  // @TODO: Implement a proper load balancer, both
                  //        for range size & executor selection.
                  executor& e = *executors_[rand];
                  e.submit(std::move(range));
                }
                else [[unlikely]]
                {
                  std::for_each(range.begin(), range.end(),
                                [](auto& j)
                                {
                                  j();
                                });
                }
                rand = distr(gen);
              }
            }
            if (!submitted)
            {
              std::this_thread::sleep_for(std::chrono::nanoseconds{1});
            }
          }
        });
    }

    pool(pool const&) = delete;
    pool(pool&&)      = delete;

    auto operator=(pool const&) -> pool& = delete;
    auto operator=(pool&&) -> pool&      = delete;

    ~pool()
    {
      stop_.store(true, std::memory_order_release);
      if (scheduler_.joinable())
      {
        scheduler_.join();
      }
      for (auto& w : executors_)
      {
        w->stop();
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
        queue  = queues_.emplace_back(std::make_unique<queue_t>(std::move(q))).get();
      }

      return *queue;
    }

    [[nodiscard]] auto
    remove(queue_t&& queue) noexcept -> bool // NOLINT
    {
      auto _ = std::scoped_lock{queueMutex_};
      if (auto itr = std::find_if(std::begin(queues_), std::end(queues_),
                                  [&queue](auto const& q)
                                  {
                                    return q.get() == &queue;
                                  });
          itr != std::end(queues_))
      {
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

  private:
    alignas(details::cache_line_size) mutable std::mutex queueMutex_;
    alignas(details::cache_line_size) details::atomic_flag_t stop_{false};
    alignas(details::cache_line_size) queues_t queues_;
    alignas(details::cache_line_size) std::thread scheduler_;
    alignas(details::cache_line_size) std::vector<std::unique_ptr<executor>> executors_;
  };

  namespace details
  {
    using pool_t = fho::pool<>;
    extern auto pool() -> pool_t&;
    using queue_t = pool_t::queue_t;
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

  template<execution_policy policy = execution_policy::parallel, std::copy_constructible callable_t,
           typename... arg_ts>
  [[nodiscard]] inline auto
  push(callable_t&& func, arg_ts&&... args) noexcept
    requires requires (details::queue_t q) { q.push(FWD(func), FWD(args)...); }
  {
    static auto& queue = create(policy); // NOLINT
    return queue.push(FWD(func), FWD(args)...);
  }
}

#undef FWD
