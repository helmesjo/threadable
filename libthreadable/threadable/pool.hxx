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
#include <thread>

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

namespace threadable
{
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

    pool(unsigned int workers = std::thread::hardware_concurrency()) noexcept
    {
      workers = std::max(2u, workers);
      details::atomic_clear(quit_);

      // start worker threads
      for (std::size_t i = 0; i < workers - 1; ++i)
      {
        auto w    = std::make_unique<worker>();
        w->thread = std::thread(
          [](std::atomic_bool& quit, queue_t& work)
          {
            while (true)
            {
              // 1. Check if quit = true. If so, bail.
              if (details::atomic_test(quit, std::memory_order_acquire)) [[unlikely]]
              {
                break;
              }
              // 2. Wait for jobs to ready.
              else [[likely]]
              {
                work.wait();
              }
              // 3. Execute all jobs.
              work.execute();
            }
          },
          std::ref(quit_), std::ref(w->work));
        workers_.push_back(std::move(w));
      }

      // start scheduler thread
      scheduler_ = std::thread(
        [this]
        {
          auto rd    = std::random_device{};
          auto gen   = std::mt19937(rd());
          auto distr = std::uniform_int_distribution<std::size_t>(std::size_t{0}, workers_.size());

          while (true)
          {
            // 1. Check if quit = true. If so, bail.
            // 2. Distribute queues to workers.
            if (details::atomic_test(quit_, std::memory_order_acquire)) [[unlikely]]
            {
              break;
            }

            queues_t queues;
            {
              auto _ = std::scoped_lock{queueMutex_};
              queues = queues_;
            }

            auto rand = distr(gen);
            if (queues.size() == 1)
            {
              (void)queues[0]->execute();
            }
            else
            {
              for (auto& queue : queues)
              {
                if (auto range = queue->consume(); !range.empty())
                {
                  // assign to (random) worker
                  // @TODO: Implement a proper load balancer.
                  if (rand < workers_.size()) [[likely]]
                  {
                    worker& w = *workers_[rand];
                    w.work.push(
                      [queue, range = std::move(range)]
                      {
                        queue->execute(range);
                      });
                  }
                  else [[unlikely]]
                  {
                    queue->execute(range);
                  }
                  auto prev = rand;
                  while ((rand = distr(gen)) != prev)
                    ;
                }
              }
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
      details::atomic_set(quit_, std::memory_order_release);
      details::atomic_notify_all(quit_);
      if (scheduler_.joinable())
      {
        scheduler_.join();
      }

      for (auto& w : workers_)
      {
        w->work.push([] {});
        w->thread.join();
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
    alignas(details::cache_line_size) details::atomic_flag_t quit_;
    alignas(details::cache_line_size) queues_t queues_;
    alignas(details::cache_line_size) std::thread scheduler_;
    alignas(details::cache_line_size) std::vector<std::unique_ptr<worker>> workers_;
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
