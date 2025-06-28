#pragma once

#include <threadable/execution.hxx>
#include <threadable/function.hxx>
#include <threadable/ring_buffer.hxx>
#include <threadable/std_concepts.hxx>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <random>
#include <thread>

#ifdef _WIN32
  #pragma warning(push)
  #pragma warning(disable : 4324)
#endif

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

namespace fho
{
  namespace details
  {
    using job_t = function<details::slot_size>;
  }

  /// @brief A thread pool class that manages multiple executors and job queues.
  /// @details The `pool` class manages a collection of `executor` instances and multiple job queues
  /// (instances of `ring_buffer`). It includes a scheduler thread that distributes jobs from the
  /// queues to the executors. The number of worker threads (executors) is by default set as the
  /// number of hardware threads minus one. The pool can create new queues, add existing queues &
  /// remove queues.
  /// @example
  /// ```cpp
  /// auto pool = fho::pool();
  /// auto& queue = pool.create();
  /// auto t = queue.push([]() { std::cout << "Job executed!\n"; });
  /// t.wait();
  /// ```
  template<std::size_t Capacity = details::default_capacity>
  class pool
  {
  public:
    using queue_t = ring_buffer<details::job_t, Capacity>;

    struct job_queue
    {
      alignas(details::cache_line_size) execution policy;
      alignas(details::cache_line_size) queue_t buffer;
    };

    using queues_t = std::vector<std::shared_ptr<job_queue>>;

    /// @brief Constructor that initializes the thread pool with a specified number of worker
    /// threads.
    /// @details Creates the specified number of `executor` instances and starts a scheduler thread
    /// that distributes jobs from the queues to these executors. If no number is specified, it
    /// defaults to the number of hardware threads minus one (the scheduler).
    /// @param `threads` The number of worker threads (executors) to create. Defaults to
    /// `std::thread::hardware_concurrency() - 1`.
    pool(unsigned int threads = std::thread::hardware_concurrency() - 1) noexcept
    {
      // start worker threads
      for (std::size_t i = 0; i < threads; ++i)
      {
        executors_.emplace_back(std::make_unique<executor>());
      }

      // start scheduler thread
      scheduler_ = std::thread(
        [this, threads]
        {
          auto const mt    = threads > 0;
          auto       rd    = std::random_device{};
          auto       gen   = std::mt19937(rd());
          auto       distr = std::uniform_int_distribution<std::size_t>(0, mt ? threads - 1 : 0);

          while (true)
          {
            // 1. Check if quit = true. If so, bail.
            // 2. Distribute queues' jobs to executors.
            if (stop_.load(std::memory_order_acquire)) [[unlikely]]
            {
              break;
            }

            queues_t queues;
            {
              auto _ = std::scoped_lock{queueMutex_};
              queues = queues_;
            }

            for (auto& queue : queues)
            {
              auto& [policy, buffer] = *queue;
              if (auto range = buffer.consume(); !range.empty()) [[likely]]
              {
                if (mt) [[likely]]
                {
                  // assign to (random) worker
                  // @TODO: Implement a proper load balancer, both
                  //        for range size & executor selection.
                  executor& e = *executors_[distr(gen)];
                  e.submit(std::move(range), policy);
                }
                else [[unlikely]]
                {
                  for (auto& j : range)
                  {
                    j();
                  }
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

    /// @brief Destructor that stops the thread pool and joins all threads.
    /// @details Sets the stop flag, joins the scheduler thread, and stops & joins all executors.
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

    /// @brief Creates a new job queue with the specified execution policy.
    /// @details Adds a new `ring_buffer` to the pool with the given execution policy (sequential or
    /// parallel).
    /// @param `policy` The execution policy for the new queue. Defaults to `execution::parallel`.
    /// @return A reference to the newly created queue.
    [[nodiscard]] auto
    create(execution policy = execution::par) noexcept -> queue_t&
    {
      return add(queue_t{}, policy);
    }

    /// @brief Adds an existing queue to the pool.
    /// @details Appends the given queue to the pool's list of queues, making it available for the
    /// scheduler to distribute jobs from.
    /// @param `q` The queue to add, moved into the pool.
    /// @return A reference to the added queue.
    [[nodiscard]] auto
    add(queue_t&& q, execution policy) -> queue_t&
    {
      job_queue* queue = nullptr;
      {
        auto _ = std::scoped_lock{queueMutex_};
        queue  = queues_.emplace_back(std::make_unique<job_queue>(policy, std::move(q))).get();
      }

      return queue->buffer;
    }

    /// @brief Removes a queue from the pool.
    /// @details Searches for the given queue in the pool's list and removes it if found.
    /// @param `queue` The queue to remove, moved into the function.
    /// @return True if the queue was found and removed, false otherwise.
    [[nodiscard]] auto
    remove(queue_t&& queue) noexcept -> bool // NOLINT
    {
      auto _ = std::scoped_lock{queueMutex_};
      if (auto itr = std::find_if(std::begin(queues_), std::end(queues_),
                                  [&queue](auto const& q)
                                  {
                                    return &q->buffer == &queue;
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

    /// @brief Returns the number of queues that have at least one job.
    /// @details Counts how many queues in the pool currently have jobs (i.e., their size is greater
    /// than zero).
    /// @return The number of non-empty queues.
    [[nodiscard]] auto
    size() const noexcept -> std::size_t
    {
      return std::ranges::count(queues_,
                                [](auto const& queue)
                                {
                                  return !queue.empty();
                                });
    }

    /// @brief Returns the maximum number of jobs that can be held in each queue.
    /// @details Since each queue is a `ring_buffer<max_nr_of_jobs>`, this returns `max_nr_of_jobs`.
    /// @return The maximum size per queue.
    static constexpr auto
    max_size() noexcept -> std::size_t
    {
      return Capacity;
    }

  private:
    alignas(details::cache_line_size) mutable std::mutex queueMutex_;
    alignas(details::cache_line_size) std::atomic_bool stop_{false};
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

  /// @brief Creates a new job queue in the default thread pool with the specified execution policy.
  /// @details This function is a convenience wrapper around `details::pool().create(policy)`.
  /// @param `policy` The execution policy for the new queue. Defaults to `execution::parallel`.
  /// @return A reference to the newly created queue.
  [[nodiscard]] inline auto
  create(execution policy = execution::par) noexcept -> decltype(auto)
  {
    return details::pool().create(policy);
  }

  /// @brief Removes a queue from the default thread pool.
  /// @details This function is a convenience wrapper around
  /// `details::pool().remove(std::move(queue))`.
  /// @param `queue` The queue to remove, moved into the function.
  /// @return True if the queue was found and removed, false otherwise.
  inline auto
  remove(details::queue_t&& queue) noexcept -> decltype(auto)
  {
    return details::pool().remove(std::move(queue));
  }

  /// @brief Pushes a job into a new queue with the specified policy in the default thread pool.
  /// @details Creates a new queue with the given policy if needed and pushes the callable and
  /// arguments into it.
  /// @tparam `Policy` The execution policy for the queue, defaults to `execution::parallel`.
  /// @tparam `Func` The type of the callable, must be copy-constructible and invocable.
  /// @tparam `Args` The types of the arguments.
  /// @param `func` The callable to push.
  /// @param `args` The arguments to pass to the callable.
  /// @return A `slot_token` for the submitted job.
  template<execution Policy = execution::par, std::copy_constructible Func, typename... Args>
  [[nodiscard]] inline auto
  push(Func&& func, Args&&... args) noexcept -> decltype(auto)
    requires requires (details::queue_t q) { q.push(FWD(func), FWD(args)...); }
  {
    static auto& queue = create(Policy);
    return queue.push(FWD(func), FWD(args)...);
  }
}

#undef FWD

#ifdef _WIN32
  #pragma warning(pop)
#endif
