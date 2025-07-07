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
  /// @brief A thread pool class that manages multiple executors and job queues with a fixed
  /// capacity.
  /// @details The `pool` class manages a collection of `executor` instances and multiple job queues
  /// (instances of `ring_buffer<fast_func_t, Capacity>`). It includes a scheduler thread that
  /// distributes jobs from the queues to the executors. The number of worker threads (executors) is
  /// by default set to the number of hardware threads minus one. The pool can create new queues,
  /// add existing queues, and remove queues. Each queue has a fixed capacity specified by the
  /// template parameter `Capacity`, which must be a power of two.
  /// @tparam Capacity The capacity of each job queue in the pool. Must be a power of two.
  /// @example
  /// ```cpp
  /// fho::pool<> pool;
  /// auto& queue = pool.create();
  /// auto token = queue.push([]() { std::cout << "Job executed!\n"; });
  /// token.wait();
  /// ```
  template<std::size_t Capacity = details::default_capacity>
  class pool
  {
  public:
    using queue_t = ring_buffer<fast_func_t, Capacity>;

    /// @brief Structure representing a job queue with an associated execution policy.
    struct job_queue
    {
      alignas(details::cache_line_size) execution policy;
      alignas(details::cache_line_size) queue_t buffer;
    };

    using queues_t = std::vector<std::shared_ptr<job_queue>>;

    /// @brief Initializes the thread pool with a specified number of worker threads.
    /// @details Creates the specified number of `executor` instances, each running in its own
    /// thread, and starts a separate scheduler thread that distributes jobs from the queues to
    /// these executors. If no number is specified, it defaults to the number of hardware threads
    /// minus one to account for the scheduler thread.
    /// @param threads The number of worker threads (executors) to create. Defaults to
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
          auto const mt      = threads > 0;
          auto       rd      = std::random_device{};
          auto       gen     = std::mt19937(rd());
          auto       distr   = std::uniform_int_distribution<std::size_t>(0, mt ? threads - 1 : 0);
          constexpr auto cap = std::size_t{4096};

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

            auto executed = false;
            do
            {
              executed = false;
              for (auto& queue : queues)
              {
                auto& [policy, buffer] = *queue;
                // assign to (random) worker
                // @TODO: Implement a proper load balancer, both
                //        for range size & executor selection.
                // @NOTE: The `cap` is arbitrary and should be
                //        dealt with by a load balancer.
                //        Runs until queues are exhausted.
                for (auto range = buffer.consume(cap); !range.empty(); range = buffer.consume(cap))
                  [[likely]]
                {
                  if (mt) [[likely]]
                  {
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
                  executed = true;
                }
              }
            }
            while (executed);
            std::this_thread::yield();
          }
        });
    }

    pool(pool const&) = delete;
    pool(pool&&)      = delete;

    auto operator=(pool const&) -> pool& = delete;
    auto operator=(pool&&) -> pool&      = delete;

    /// @brief Stops the thread pool and joins all threads.
    /// @details Sets the stop flag to signal the scheduler thread to exit, joins the scheduler
    /// thread, and then stops and joins all executor threads to ensure all jobs are completed
    /// before destruction.
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

    /// @brief Creates and adds a new job queue to the pool with the specified execution policy.
    /// @details Instantiates a new `ring_buffer<fast_func_t, Capacity>` and adds it to the pool's
    /// list of queues. The execution policy determines how jobs in this queue are executed
    /// (sequentially or in parallel).
    /// @param policy The execution policy for the new queue. Defaults to `execution::par`.
    /// @return A reference to the newly created `ring_buffer`.
    [[nodiscard]] auto
    create(execution policy = execution::par) noexcept -> queue_t&
    {
      return add(queue_t{}, policy);
    }

    /// @brief Adds an existing queue to the pool's management.
    /// @details Moves the provided `ring_buffer` into the pool's list of queues, allowing the
    /// scheduler to distribute its jobs to the executors.
    /// @param q The `ring_buffer` to add, moved into the pool.
    /// @param policy The execution policy for the queue.
    /// @return A reference to the added `ring_buffer`.
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

    /// @brief Removes a specified queue from the pool.
    /// @details Searches for the provided `ring_buffer` in the pool's list and removes it if found.
    /// The queue is moved into the function to ensure ownership transfer.
    /// @param queue The `ring_buffer` to remove, moved into the function.
    /// @return True if the queue was successfully removed, false if not found.
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

    /// @brief Counts the number of queues with pending jobs.
    /// @details Iterates through the pool's queues and counts how many have at least one job
    /// (i.e., `queue.size() > 0`).
    /// @return The number of queues containing jobs.
    [[nodiscard]] auto
    size() const noexcept -> std::size_t
    {
      return std::ranges::count(queues_,
                                [](auto const& queue)
                                {
                                  return !queue.empty();
                                });
    }

    /// @brief Returns the maximum capacity of each queue.
    /// @details Each queue is a `ring_buffer<fast_func_t, Capacity>`, so this returns `Capacity`,
    /// the maximum number of jobs each queue can hold.
    /// @return The capacity of each queue.
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
    using pool_t  = fho::pool<>;
    using queue_t = pool_t::queue_t;

    /// @brief External linkage (since C++20) to make sure one and the same
    ///        instance is used regardless if linking static or shared.
    inline std::unique_ptr<pool_t> default_pool = nullptr; // NOLINT
    /// @brief Lazy-initializes `default_pool`.
    extern auto pool() -> pool_t&;
  }

  /// @brief Creates a new job queue in the default thread pool.
  /// @details Uses a lazily-initialized default `pool` instance to create a new queue with the
  /// specified execution policy.
  /// @param policy The execution policy for the new queue. Defaults to `execution::par`.
  /// @return A reference to the newly created `ring_buffer`.
  [[nodiscard]] inline auto
  create(execution policy = execution::par) noexcept -> decltype(auto)
  {
    return details::pool().create(policy);
  }

  /// @brief Removes a queue from the default thread pool.
  /// @details Uses the default `pool` instance to remove the specified queue.
  /// @param queue The `ring_buffer` to remove, moved into the function.
  /// @return True if the queue was successfully removed, false if not found.
  inline auto
  remove(details::queue_t&& queue) noexcept -> decltype(auto)
  {
    return details::pool().remove(std::move(queue));
  }

  /// @brief Submits a job to a queue with the specified policy in the default thread pool.
  /// @details Uses a static `ring_buffer` instance for each unique `Policy`, creating it if
  /// necessary, and pushes the callable with its arguments into that queue.
  /// @tparam Policy The execution policy for the queue, defaults to `execution::par`.
  /// @tparam Func The type of the callable, must be copy-constructible and invocable.
  /// @tparam Args The types of the arguments.
  /// @param func The callable to submit.
  /// @param args The arguments to pass to the callable.
  /// @return A `slot_token` representing the submitted job.
  template<execution Policy = execution::par, std::copy_constructible Func, typename... Args>
  [[nodiscard]] inline auto
  push(Func&& func, Args&&... args) noexcept -> decltype(auto)
    requires requires (details::queue_t q) { q.push(FWD(func), FWD(args)...); }
  {
    static auto& queue = create(Policy);
    return queue.push(FWD(func), FWD(args)...);
  }

  /// @brief Submits a job to a queue with the specified policy, reusing a token.
  /// @details Similar to the other `push` overload but reuses an existing `slot_token`, which is
  /// passed as the first argument to the callable.
  /// @tparam Policy The execution policy for the queue, defaults to `execution::par`.
  /// @tparam Func The type of the callable, must be copy-constructible and invocable.
  /// @tparam Args The types of the arguments.
  /// @param func The callable to submit.
  /// @param token Reference to a reusable `slot_token`.
  /// @param args Additional arguments to pass to the callable.
  /// @return Reference to the reused `token`.
  template<execution Policy = execution::par, std::copy_constructible Func, typename... Args>
  inline auto
  push(Func&& func, slot_token& token, Args&&... args) noexcept -> decltype(auto)
    requires requires (details::queue_t q) { q.push(FWD(func), FWD(args)...); }
  {
    static auto& queue = create(Policy);
    return queue.push(token, FWD(func), FWD(args)...);
  }
}

#undef FWD

#ifdef _WIN32
  #pragma warning(pop)
#endif
