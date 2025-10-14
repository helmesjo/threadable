#pragma once

#include <threadable/execution.hxx>
#include <threadable/function.hxx>
#include <threadable/ring_buffer.hxx>
#include <threadable/scheduler/stealing.hxx>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <random>
#include <ranges>

#ifdef _MSC_VER
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
  public:
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
    template<execution Policy = execution::par, typename... U>
      requires std::constructible_from<fast_func_t, U...>
    auto
    push(slot_token& token, U&&... args) noexcept -> slot_token&
    {
      static constexpr auto tags =
        Policy == execution::seq ? slot_state::tag_seq : slot_state::invalid;
      activity_.master.template emplace_back<tags>(token, FWD(args)...);
      activity_.notifier.notify_one();
      return token;
    }

    /// @brief Pushes a task with arguments and returns a new token.
    /// @details Emplaces a task constructed from `args...` into the queue, creating a new
    ///          `slot_token` for tracking. Signals `activity_.notifier` to wake sleeping executors.
    ///          Thread-safe for multiple producers.
    /// @tparam U Types of the arguments to construct the task.
    /// @param args Arguments to construct a `fast_func_t` task.
    /// @return A `slot_token` for tracking the task's state.
    /// @requires `fast_func_t` must be constructible from `U...`.
    template<execution Policy = execution::par, typename... U>
      requires std::constructible_from<fast_func_t, U...>
    auto
    push(U&&... args) noexcept -> slot_token
    {
      static constexpr auto tags =
        Policy == execution::seq ? slot_state::tag_seq : slot_state::invalid;
      auto token = activity_.master.template emplace_back<tags>(FWD(args)...);
      activity_.notifier.notify_one();
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
      thread_local auto rng = std::mt19937{std::random_device{}()};
      using cached_t        = std::ranges::range_value_t<decltype(r)>;

      auto  dist   = std::uniform_int_distribution<std::size_t>(0, executors_.size() - 1);
      auto  idx    = dist(rng);
      auto& victim = executors_[idx];

      auto cached = cached_t{nullptr};

      // Returns 'false' if 'r == victim queue'
      if (!victim->steal(r, cached))
      {
        // NOTE: Since no DAG (Direct Acyclic Graph) is (currently) supported
        //       by ring_buffer, we "bulk-steal" from master. This is not
        //       ideal (loop-try) and should instead be handled by the buffer
        //       directly (eg. through a 'range claim' FAA).
        constexpr auto cap = std::size_t{128};
        for (auto i = 0; i < cap; ++i)
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
      }
      return std::move(cached);
    }

    /// @brief Returns the number of threads.
    /// @return Number of threads.
    [[nodiscard]] auto
    thread_count() const noexcept -> unsigned int
    {
      return executors_.size();
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
}

#undef FWD

#ifdef _MSC_VER
  #pragma warning(pop)
#endif
