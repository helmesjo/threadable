#pragma once

#include <threadable/execution.hxx>
#include <threadable/function.hxx>
#include <threadable/ring_buffer.hxx>
#include <threadable/schedulers/stealing.hxx>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <random>
#include <thread>

#ifdef _MSC_VER
  #pragma warning(push)
  #pragma warning(disable : 4324)
#endif

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

namespace fho
{
  namespace v2
  {

    class alignas(details::cache_line_size) task_queue : private ring_buffer<fast_func_t>
    {
      using base_t = ring_buffer<fast_func_t>;

    public:
      explicit task_queue(execution policy, schedulers::stealing::activity_stats& activity)
        : policy_(policy)
        , activity_(activity)
      {}

      template<typename U>
        requires std::constructible_from<fast_func_t, U>
      auto
      push(slot_token& token, U&& val) noexcept -> decltype(auto)
      {
        emplace_back(token, FWD(val));
        activity_.ready.fetch_add(1, std::memory_order_release);
        activity_.ready.notify_one();
      }

      template<typename U>
        requires std::constructible_from<fast_func_t, U>
      auto
      push(U&& val) noexcept -> decltype(auto)
      {
        auto token = emplace_back(FWD(val));
        activity_.ready.fetch_add(1, std::memory_order_release);
        activity_.ready.notify_one();
        return token;
      }

      [[nodiscard]] auto
      try_pop() noexcept -> decltype(auto)
      {
        if (policy_ != execution::seq) [[likely]]
        {
          return base_t::try_pop_front(false);
        }
        else
        {
          return base_t::try_pop_front(true);
        }
      }

      [[nodiscard]] static constexpr auto
      max_size() noexcept
      {
        return base_t::max_size();
      }

    private:
      alignas(details::cache_line_size) execution policy_;
      alignas(details::cache_line_size) schedulers::stealing::activity_stats& activity_; // NOLINT
    };

    class pool
    {
    public:
      using queue_t  = task_queue;
      using queues_t = std::vector<std::shared_ptr<queue_t>>;

      explicit pool(unsigned int threads = std::thread::hardware_concurrency())
      {
        executors_.reserve(threads);
        for (unsigned int i = 0; i < threads; ++i)
        {
          executors_.emplace_back(std::make_unique<executor>(activity_,
                                                             [this]
                                                             {
                                                               return steal();
                                                             }));
        }
      }

      pool(pool const&)                    = delete;
      pool(pool&&)                         = delete;
      auto operator=(pool const&) -> pool& = delete;
      auto operator=(pool&&) -> pool&      = delete;

      ~pool()
      {
        activity_.abort.store(true, std::memory_order_release);
        activity_.ready.store(1, std::memory_order_release);
        activity_.ready.notify_all();
      }

      [[nodiscard]] auto
      create(execution policy = execution::par) noexcept -> queue_t&
      {
        auto queue = std::make_shared<queue_t>(policy, activity_);
        {
          auto _ = std::scoped_lock{queueMutex_};
          queues_.push_back(queue);
        }
        return *queue;
      }

      [[nodiscard]] auto
      remove(queue_t&& queue) noexcept -> bool // NOLINT
      {
        auto _ = std::scoped_lock{queueMutex_};
        if (auto itr = std::ranges::find_if(queues_,
                                            [&queue](auto const& q)
                                            {
                                              return q.get() == &queue;
                                            });
            itr != std::end(queues_))
        {
          // TODO: Clear queue, it can't be used after user explicitly "removed" it.
          queues_.erase(itr);
          return true;
        }
        return false;
      }

      [[nodiscard]] auto
      size() const noexcept -> std::size_t
      {
        auto _ = std::scoped_lock{queueMutex_};
        return queues_.size();
      }

      static constexpr auto
      max_size() noexcept -> std::size_t
      {
        return queue_t::max_size();
      }

    private:
      [[nodiscard]] auto
      steal() noexcept -> ring_buffer<>::claimed_type
      {
        thread_local static auto gen = std::mt19937{std::random_device{}()};

        queues_t queues;
        {
          auto _ = std::scoped_lock{queueMutex_};
          queues = queues_;
        }

        if (queues.empty()) [[unlikely]]
        {
          return nullptr;
        }
        auto distr = std::uniform_int_distribution<std::size_t>{0, queues.size() - 1};
        auto idx   = distr(gen);
        return queues[idx]->try_pop();
      }

      alignas(details::cache_line_size) sched::activity_stats activity_;
      alignas(details::cache_line_size) mutable std::mutex queueMutex_;
      alignas(details::cache_line_size) queues_t queues_;
      alignas(details::cache_line_size) std::vector<std::unique_ptr<executor>> executors_;
    };
  }

  /// @brief A thread pool class that manages multiple executors and task queues with a fixed
  /// capacity.
  /// @details The `pool` class manages a collection of `executor` instances and multiple task
  /// queues (instances of `ring_buffer<fast_func_t, Capacity>`). It includes a scheduler thread
  /// that distributes tasks from the queues to the executors. The number of worker threads
  /// (executors) is by default set to the number of hardware threads minus one. The pool can create
  /// new queues, add existing queues, and remove queues. Each queue has a fixed capacity specified
  /// by the template parameter `Capacity`, which must be a power of two.
  /// @tparam Capacity The capacity of each task queue in the pool. Must be a power of two.
  /// @example
  /// ```cpp
  /// fho::pool<> pool;
  /// auto& queue = pool.create();
  /// auto token = queue.emplace_back([]() { std::cout << "task executed!\n"; });
  /// token.wait();
  /// ```
  template<std::size_t Capacity = details::default_capacity>
  class pool
  {
  public:
    using queue_t = ring_buffer<fast_func_t, Capacity>;

    /// @brief Structure representing a task queue with an associated execution policy.
    struct task_queue
    {
      task_queue(task_queue const&)                    = delete;
      task_queue(task_queue&&)                         = default;
      auto operator=(task_queue const&) -> task_queue& = delete;
      auto operator=(task_queue&&) -> task_queue&      = default;

      task_queue(execution policy, queue_t buffer)
        : policy(policy)
        , buffer(std::move(buffer))
      {}

      ~task_queue() = default;

      alignas(details::cache_line_size) execution policy;
      alignas(details::cache_line_size) queue_t buffer;
    };

    using queues_t = std::vector<std::shared_ptr<task_queue>>;

    /// @brief Initializes the thread pool with a specified number of worker threads.
    /// @details Creates the specified number of `executor` instances, each running in its own
    /// thread, and starts a separate scheduler thread that distributes tasks from the queues to
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
          using namespace std::chrono_literals;
          using hpclock_t = std::chrono::high_resolution_clock;

          auto const mt       = threads > 0;
          auto       rd       = std::random_device{};
          auto       gen      = std::mt19937(rd());
          auto       distr    = std::uniform_int_distribution<std::size_t>(0, mt ? threads - 1 : 0);
          constexpr auto cap  = std::size_t{4096};
          auto           last = hpclock_t::time_point{};

          while (true)
          {
            // 1. Check if quit = true. If so, bail.
            // 2. Distribute queues' tasks to executors.
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
                for (auto range = buffer.pop_front_range(cap); !range.empty();
                     range      = buffer.pop_front_range(cap)) [[likely]]
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
                      assert(j and "pool::schedule()");
                      j();
                    }
                  }
                  executed = true;
                  last     = hpclock_t::now();
                }
              }
            }
            while (executed);

            // We want to save CPU time while still being reactive, but
            // `sleep()` is not precise enough (eg. min ~1ms on Windows)
            // and `yield()` is too aggressive.
            // Make an educated guess if it's unlikely there will be
            // tasks to process immediately, and only sleep if so.
            // Prefer `yield()` during semi-high load.
            if (hpclock_t::now() - last > 5ms) [[unlikely]]
            {
              if (!std::ranges::any_of(executors_,
                                       [](auto const& e) -> bool
                                       {
                                         return e->busy();
                                       }) &&
                  !std::ranges::any_of(queues,
                                       [](auto const& q) -> bool
                                       {
                                         return q->buffer.size() > 0;
                                       })) [[unlikely]]
              {
                if (hpclock_t::now() - last < 10ms) [[likely]]
                {
                  std::this_thread::yield();
                }
                else [[unlikely]]
                {
                  std::this_thread::sleep_for(1us);
                  last = hpclock_t::now();
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

    /// @brief Stops the thread pool and joins all threads.
    /// @details Sets the stop flag to signal the scheduler thread to exit, joins the scheduler
    /// thread, and then stops and joins all executor threads to ensure all tasks are completed
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

    /// @brief Creates and adds a new task queue to the pool with the specified execution policy.
    /// @details Instantiates a new `ring_buffer<fast_func_t, Capacity>` and adds it to the pool's
    /// list of queues. The execution policy determines how tasks in this queue are executed
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
    /// scheduler to distribute its tasks to the executors.
    /// @param q The `ring_buffer` to add, moved into the pool.
    /// @param policy The execution policy for the queue.
    /// @return A reference to the added `ring_buffer`.
    [[nodiscard]] auto
    add(queue_t&& q, execution policy) -> queue_t&
    {
      task_queue* queue = nullptr;
      {
        auto _ = std::scoped_lock{queueMutex_};
        queue  = queues_.emplace_back(std::make_unique<task_queue>(policy, std::move(q))).get();
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

    /// @brief Counts the number of queues with pending tasks.
    /// @details Iterates through the pool's queues and counts how many have at least one task
    /// (i.e., `queue.size() > 0`).
    /// @return The number of queues containing tasks.
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
    /// the maximum number of tasks each queue can hold.
    /// @return The capacity of each queue.
    static constexpr auto
    max_size() noexcept -> std::size_t
    {
      return queue_t::max_size();
    }

  private:
    alignas(details::cache_line_size) schedulers::stealing::activity_stats activity_;
    alignas(details::cache_line_size) mutable std::mutex queueMutex_;
    alignas(details::cache_line_size) std::atomic_bool stop_{false};
    alignas(details::cache_line_size) queues_t queues_;
    alignas(details::cache_line_size) std::thread scheduler_;
    alignas(details::cache_line_size) std::vector<std::unique_ptr<executor>> executors_;
  };

  namespace details
  {
    using pool_t  = fho::pool<>;
    using queue_t = typename pool_t::queue_t;

    /// @brief Returns a reference to the default thread pool instance.
    /// @details Provides a thread pool initialized lazily on first call using `std::call_once` for
    /// thread safety. The pool is constructed with default values and cleaned up via `std::atexit`,
    /// resetting the pointer at program exit.
    /// @return Reference to the default `pool<>` instance.
    inline static auto
    default_pool() -> pool_t&
    {
      static auto default_pool = std::unique_ptr<pool_t>{}; // NOLINT
      static auto once         = std::once_flag{};
      std::call_once(once,
                     []
                     {
                       default_pool = std::make_unique<fho::details::pool_t>();
                       std::atexit(
                         []
                         {
                           default_pool = nullptr;
                         });
                     });
      return *default_pool;
    }
  }

  /// @brief Creates a new task queue in the default thread pool.
  /// @details Uses a lazily-initialized default `pool` instance to create a new queue with the
  /// specified execution policy.
  /// @param policy The execution policy for the new queue. Defaults to `execution::par`.
  /// @return A reference to the newly created `ring_buffer`.
  [[nodiscard]] inline auto
  create(execution policy = execution::par) noexcept -> decltype(auto)
  {
    return details::default_pool().create(policy);
  }

  /// @brief Removes a queue from the default thread pool.
  /// @details Uses the default `pool` instance to remove the specified queue.
  /// @param queue The `ring_buffer` to remove, moved into the function.
  /// @return True if the queue was successfully removed, false if not found.
  inline auto
  remove(details::queue_t&& queue) noexcept -> decltype(auto)
  {
    return details::default_pool().remove(std::move(queue));
  }
}

#undef FWD

#ifdef _MSC_VER
  #pragma warning(pop)
#endif
