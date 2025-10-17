#pragma once

#include <threadable/execution.hxx>
#include <threadable/function.hxx>
#include <threadable/prng.hxx>
#include <threadable/ring_buffer.hxx>
#include <threadable/scheduler/stealing.hxx>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <ranges>
#include <thread>

#if defined(_MSC_VER) && !defined(__clang__)
  #pragma warning(push)
  #pragma warning(disable : 4324)
#endif

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

namespace fho
{
  /// @brief A thread pool managing work-stealing executors.
  /// @details Manages a set of `executor`s, distributing tasks via work-stealing.
  ///          Supports concurrent task submission, with tasks executed according to their
  ///          specified policy (`seq` for single-edge DAG ordering, `par` for parallel).
  ///          Uses a shared `activity_stats` for efficient executor wake-up.
  class pool
  {
    using queue_t  = ring_buffer<fast_func_t>;
    using queues_t = std::vector<std::shared_ptr<queue_t>>;

  public:
    class queue_view
    {
    public:
      queue_view(pool& pool, queue_t& queue)
        : pool_(&pool)
        , queue_(&queue)
      {}

      queue_view(queue_view&& rhs) noexcept
        : pool_(rhs.pool_)
        , queue_(rhs.queue_)
      {
        rhs.pool_  = nullptr;
        rhs.queue_ = nullptr;
      }

      ~queue_view()
      {
        if (pool_ && queue_)
        {
          (void)pool_->remove(std::move(*queue_));
        }
        pool_  = nullptr;
        queue_ = nullptr;
      }

      auto
      operator=(queue_view&& rhs) noexcept -> queue_view&
      {
        if (this == &rhs) [[unlikely]]
        {
          return *this;
        }
        if (pool_ && queue_)
        {
          (void)pool_->remove(std::move(*queue_));
        }
        pool_      = rhs.pool_;
        queue_     = rhs.queue_;
        rhs.pool_  = nullptr;
        rhs.queue_ = nullptr;
        return *this;
      }

      queue_view()                                     = default;
      queue_view(queue_view const&)                    = delete;
      auto operator=(queue_view const&) -> queue_view& = delete;

      operator bool() const noexcept
      {
        return pool_ && queue_;
      }

      /// @brief Pushes a task with a reusable token and arguments.
      /// @details Emplaces a task constructed from `args...` into the queue, binding it to the
      ///          provided `slot_token` for tracking. Signals pools `notifier` to wake sleeping
      ///          executors. Thread-safe for multiple producers.
      /// @tparam ExPo Type of execution policy.
      /// @tparam U Types of the arguments to construct the task.
      /// @param exPo Execution policy.
      /// @param token Reference to a `slot_token` to track the task's state.
      /// @param args Arguments to construct a `fast_func_t` task.
      /// @return Reference to the provided `slot_token`.
      /// @requires `fast_func_t` must be constructible from `U...`.
      template<exec_policy ExPo, typename... U>
        requires std::constructible_from<fast_func_t, U...>
      auto
      push(ExPo&&, slot_token& token, U&&... args) noexcept -> slot_token&
      {
        if constexpr (std::common_reference_with<ExPo, decltype(execution::seq)>)
        {
          queue_->template emplace_back<slot_state::tag_seq>(token, FWD(args)...);
        }
        else
        {
          queue_->emplace_back(token, FWD(args)...);
        }
        pool_->activity_.notifier.notify_one();
        return token;
      }

      /// @brief Pushes a task with a reusable token and arguments.
      /// @details Emplaces a task constructed from `args...` into the queue, binding it to the
      ///          provided `slot_token` for tracking. Signals pools `notifier` to wake sleeping
      ///          executors. Thread-safe for multiple producers.
      /// @tparam U Types of the arguments to construct the task.
      /// @param token Reference to a `slot_token` to track the task's state.
      /// @param args Arguments to construct a `fast_func_t` task.
      /// @return Reference to the provided `slot_token`.
      /// @requires `fast_func_t` must be constructible from `U...`.
      template<typename... U>
        requires std::constructible_from<fast_func_t, U...>
      auto
      push(slot_token& token, U&&... args) noexcept -> slot_token&
      {
        return push(execution::par, token, FWD(args)...);
      }

      /// @brief Pushes a task with arguments and returns a new token.
      /// @details Emplaces a task constructed from `args...` into the queue, creating a new
      ///          `slot_token` for tracking. Signals pools `notifier` to wake sleeping
      ///          executors. Thread-safe for multiple producers.
      /// @tparam ExPo Type of execution policy.
      /// @tparam U Types of the arguments to construct the task.
      /// @param exPo Execution policy.
      /// @param args Arguments to construct a `fast_func_t` task.
      /// @return A `slot_token` for tracking the task's state.
      /// @requires `fast_func_t` must be constructible from `U...`.
      template<exec_policy ExPo, typename... U>
        requires std::constructible_from<fast_func_t, U...>
      auto
      push(ExPo&& exPo, U&&... args) noexcept -> slot_token
      {
        auto token = slot_token{};
        push(FWD(exPo), token, FWD(args)...);
        return token;
      }

      /// @brief Pushes a task with arguments and returns a new token.
      /// @details Emplaces a task constructed from `args...` into the queue, creating a new
      ///          `slot_token` for tracking. Signals pools `notifier` to wake sleeping
      ///          executors. Thread-safe for multiple producers.
      /// @tparam U Types of the arguments to construct the task.
      /// @param args Arguments to construct a `fast_func_t` task.
      /// @return A `slot_token` for tracking the task's state.
      /// @requires `fast_func_t` must be constructible from `U...`.
      template<typename... U>
        requires std::constructible_from<fast_func_t, U...>
      auto
      push(U&&... args) noexcept -> slot_token
      {
        return push(execution::par, FWD(args)...);
      }

      /// @brief Waits until the master queue is empty.
      /// @details Blocks until there are no slots in state `ready`.
      void
      wait() const noexcept
      {
        if (*this)
        {
          queue_->wait();
        }
      }

      /// @brief Returns the maximum task capacity.
      /// @details Returns the capacity of the underlying queue.
      /// @return Maximum number of pending tasks.
      static constexpr auto
      max_size() noexcept -> std::size_t
      {
        return queue_t::max_size();
      }

    private:
      pool*    pool_  = nullptr;
      queue_t* queue_ = nullptr;
    };

    /// @brief Constructs a thread pool with a specified number of worker threads.
    /// @details Initializes the pool with `threads` executors, each running a work-stealing loop.
    ///          Defaults to `std::thread::hardware_concurrency()` for optimal parallelism.
    ///          Creates a shared `activity_stats` for task notifications.
    /// @param threads Number of worker threads (executors).
    explicit pool(std::size_t threads = std::thread::hardware_concurrency())
    {
      executors_.reserve(threads);
      auto stats = scheduler::stealing::exec_stats{
        .steal_bound = (threads + 1) * 2, // 2*(W+1) as paper notes (W=workers)
        .yield_bound = 64,
      };
      for (unsigned int i = 0; i < threads; ++i)
      {
        executors_.emplace_back(std::make_unique<executor>(activity_, stats,
                                                           [this](std::ranges::range auto&& r)
                                                             -> claimed_slot<fast_func_t>
                                                           {
                                                             return steal(FWD(r));
                                                           }));
      }
    }

    pool(pool const&)                    = delete;
    pool(pool&&)                         = delete;
    auto operator=(pool const&) -> pool& = delete;
    auto operator=(pool&&) -> pool&      = delete;

    /// @brief Destroys the pool, stopping all executors.
    /// @details Sets the `activity_.abort` flag and signals `activity_.ready` to wake all
    /// executors,
    ///          ensuring clean shutdown. Executors are joined automatically via their `jthread`
    ///          destructors.
    ~pool()
    {
      activity_.stops.store(true, std::memory_order_release);
      activity_.notifier.notify_all();
      for (auto& e : executors_)
      {
        e->stop();
      }
    }

    /// @brief Pushes a task with a reusable token and arguments.
    /// @details Emplaces a task constructed from `args...` into the queue, binding it to the
    ///          provided `slot_token` for tracking. Signals `activity_.notifier` to wake sleeping
    ///          executors. Thread-safe for multiple producers.
    /// @tparam U Types of the arguments to construct the task.
    /// @param token Reference to a `slot_token` to track the task's state.
    /// @param args Arguments to construct a `fast_func_t` task.
    /// @return Reference to the provided `slot_token`.
    /// @requires `fast_func_t` must be constructible from `U...`.
    template<typename... U>
      requires std::constructible_from<fast_func_t, U...>
    auto
    push(slot_token& token, U&&... args) noexcept -> slot_token&
    {
      activity_.master.emplace_back(token, FWD(args)...);
      activity_.notifier.notify_one();
      return token;
    }

    /// @brief Pushes a task with a reusable token and arguments without notifying executors.
    /// @details Emplaces a task constructed from `args...` into the queue, binding it to the
    ///          provided `slot_token` for tracking. Does not signal `activity_.notifier`.
    ///          Thread-safe for multiple producers.
    /// @tparam U Types of the arguments to construct the task.
    /// @param token Reference to a `slot_token` to track the task's state.
    /// @param args Arguments to construct a `fast_func_t` task.
    /// @return Reference to the provided `slot_token`.
    /// @requires `fast_func_t` must be constructible from `U...`.
    template<typename... U>
      requires std::constructible_from<fast_func_t, U...>
    auto
    push_quiet(slot_token& token, U&&... args) noexcept -> slot_token&
    {
      return activity_.master.emplace_back(token, FWD(args)...);
    }

    /// @brief Pushes a task with arguments and returns a new token.
    /// @details Emplaces a task constructed from `args...` into the queue, creating a new
    ///          `slot_token` for tracking. Signals `activity_.notifier` to wake sleeping executors.
    ///          Thread-safe for multiple producers.
    /// @tparam U Types of the arguments to construct the task.
    /// @param args Arguments to construct a `fast_func_t` task.
    /// @return A `slot_token` for tracking the task's state.
    /// @requires `fast_func_t` must be constructible from `U...`.
    template<typename... U>
      requires std::constructible_from<fast_func_t, U...>
    auto
    push(U&&... args) noexcept -> slot_token
    {
      auto token = slot_token{};
      push(token, FWD(args)...);
      return token;
    }

    /// @brief Attempts to steal a task from a random queue.
    /// @details Selects a random victim using a thread-local random number generator and tries to
    ///          pop a task via `task_queue::try_pop_front()`. Returns empty claim if no task is
    ///          available from either steal-attempt or master queue.
    ///          Thread-safe for concurrent stealing.
    /// @return A `claimed_slot<fast_func_t>` if a task is stolen, else empty claim.
    [[nodiscard]] auto
    steal(std::ranges::range auto& r) noexcept -> claimed_slot<fast_func_t>
    {
      thread_local auto rng = fho::prng_engine{simple_seed()};
      using cached_t        = std::ranges::range_value_t<decltype(r)>;

      auto       dist   = fho::prng_dist<std::size_t>(0, executors_.size() - 1);
      auto const idx    = dist(rng);
      auto&      victim = executors_[idx];

      auto cached = cached_t{nullptr};

      // Returns 'false' if 'r == victim queue'
      if (!victim->steal(r, cached))
      {
        // NOTE: Since no DAG (Direct Acyclic Graph) is (currently) supported
        //       by ring_buffer, we "bulk-steal" from master. This is not
        //       ideal (loop-try) and should instead be handled by the buffer
        //       directly (eg. through a 'range claim' FAA).
        constexpr auto cap = 128u;
        for (auto i = 0u; i < cap; ++i)
        {
          if (auto t = activity_.master.try_pop_front(); t)
          {
            if (!cached)
            {
              cached = std::move(t);
            }
            else
            {
              r.emplace_back(std::move(t));
            }
          }
          else
          {
            break;
          }
        }
        // Second fallback to user-created queues.
        if (!cached) [[unlikely]]
        {
          auto queues = queues_.load(std::memory_order_acquire);
          if (queues->size() > 0) [[unlikely]]
          {
            auto       dist2 = fho::prng_dist<std::size_t>(0, queues->size() - 1);
            auto const cap2  = queues->size() * 2;
            for (auto i = 0u; i < cap2; ++i)
            {
              auto const idx2  = dist2(rng);
              auto&      queue = (*queues)[idx2];
              if (auto t = queue->try_pop_front(); t)
              {
                if (!cached)
                {
                  cached = std::move(t);
                }
                else
                {
                  r.emplace_back(std::move(t));
                }
              }
            }
          }
        }
      }
      return cached;
    }

    /// @brief Makes a new task queue and adds it to the pool.
    /// @details Instantiates a new `ring_buffer<fast_func_t>` and adds it to the pool's
    /// list of available queues.
    /// @return A reference to the newly created `ring_buffer`.
    [[nodiscard]] auto
    make() noexcept -> queue_view
    {
      auto queue = std::make_shared<queue_t>();
      for (;;)
      {
        // 1. Load old pointer to list.
        auto old = queues_.load(std::memory_order_acquire);
        // 2. Copy over items to a new list.
        auto cpy = std::make_shared<queues_t>(*old);
        // 3. Insert new item.
        cpy->emplace_back(queue);
        // 4. CAS the pointer.
        if (queues_.compare_exchange_strong(old, cpy))
        {
          break;
        }
      }

      return {*this, *queue};
    }

    /// @brief Removes a specified queue from the pool.
    /// @details Searches for the provided `ring_buffer` in the pool's list and removes it if found.
    /// The queue is moved into the function to ensure ownership transfer.
    /// @param queue The `ring_buffer` to remove, moved into the function.
    /// @return True if the queue was successfully removed, false if not found.
    [[nodiscard]] auto
    remove(queue_t&& queue) noexcept -> bool // NOLINT
    {
      for (;;)
      {
        // 1. Load old pointer to list.
        auto old = queues_.load(std::memory_order_acquire);
        // 4. Copy over items to a new list.
        auto cpy = std::make_shared<queues_t>(*old);
        // 2. Find queue (if it exists).
        auto itr = std::ranges::find_if(*cpy,
                                        [&queue](auto const& q)
                                        {
                                          return q.get() == &queue;
                                        });
        // 3. Bail if it doesn't exist.
        if (itr == std::end(*cpy)) [[unlikely]]
        {
          break;
        }

        auto removed = *itr; //< Keep one ref alive.
        // 5. Remove queue from copy-list.
        cpy->erase(itr);
        // 6. CAS the pointer.
        if (queues_.compare_exchange_strong(old, cpy))
        {
          old = nullptr;
          cpy = nullptr;
          // @NOTE: Wait for any active tasks in queue to finish processing.
          //        We are the last strong ref when 'use_count()' reaches 1.
          //        Once 1, no one else can regain a ref (queue has already
          //        been removed).
          while (removed.use_count() > 1)
          {
            std::this_thread::yield();
          }
          return true;
        }
      }
      return false;
    }

    /// @brief Returns the number of threads.
    /// @return Number of threads.
    [[nodiscard]] auto
    thread_count() const noexcept -> unsigned int
    {
      return static_cast<unsigned int>(executors_.size());
    }

    /// @brief Returns the number of pending tasks.
    /// @details Counts the current number of tasks in the pool.
    /// @return Number of tasks.
    [[nodiscard]] auto
    size() const noexcept -> std::size_t
    {
      return activity_.master.size();
    }

    /// @brief Returns true if there are no pending tasks.
    /// @details Scans for any slot in the `ready` state.
    /// @return True if none pending, false otherwise.
    [[nodiscard]] auto
    empty() const noexcept -> bool
    {
      return activity_.master.empty();
    }

    /// @brief Waits until the master queue is empty.
    /// @details Blocks until there are no slots in state `ready`.
    void
    wait() const noexcept
    {
      activity_.master.wait();
    }

    /// @brief Returns the maximum task capacity.
    /// @details Returns the capacity of the underlying master queue.
    /// @return Maximum number of pending tasks.
    static constexpr auto
    max_size() noexcept -> std::size_t
    {
      return decltype(activity_.master)::max_size();
    }

  private:
    alignas(details::cache_line_size) scheduler::stealing::activity_stats activity_;
    alignas(details::cache_line_size) std::vector<std::unique_ptr<executor>> executors_;
    alignas(details::cache_line_size) std::atomic<std::shared_ptr<queues_t>> queues_ =
      std::make_shared<queues_t>();
  };

  namespace details
  {
    using pool_t = fho::pool;

    /// @brief Returns a reference to the default thread pool instance.
    /// @details Provides a thread pool initialized lazily on first call using `std::call_once` for
    /// thread safety. The pool is constructed with default values and cleaned up via `std::atexit`,
    /// resetting the pointer at program exit.
    /// @return Reference to the default `pool` instance.
    inline auto
    default_pool(unsigned int threads = std::thread::hardware_concurrency()) -> pool_t&
    {
      static auto default_pool = std::unique_ptr<pool_t>{}; // NOLINT
      static auto once         = std::once_flag{};
      std::call_once(once,
                     [threads]
                     {
                       default_pool = std::make_unique<fho::details::pool_t>(threads);
                       std::atexit(
                         []
                         {
                           default_pool = nullptr;
                         });
                     });
      return *default_pool;
    }
  }

  /// @brief Attempts to set the number of threads for the default thread pool.
  /// @details If no default thread pool has been instantiated, this function
  ///          will instantiate it with the specified number of threads.
  ///          Otherwise, no new instance is created.
  /// @return True if the number of pool threads matches the specified number.
  inline auto
  thread_count(unsigned int threads) -> bool
  {
    return details::default_pool(threads).thread_count() == threads;
  }

  inline auto
  make_queue() noexcept
  {
    return details::default_pool().make();
  }
}

#undef FWD

#if defined(_MSC_VER) && !defined(__clang__)
  #pragma warning(pop)
#endif
