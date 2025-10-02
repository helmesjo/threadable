#pragma once

#include <threadable/execution.hxx>
#include <threadable/function.hxx>
#include <threadable/ring_buffer.hxx>
#include <threadable/schedulers/stealing.hxx>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <format>
#include <iostream>
#include <mutex>
#include <random>
#include <ranges>
#include <syncstream>

#ifdef _MSC_VER
  #pragma warning(push)
  #pragma warning(disable : 4324)
#endif

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

namespace fho
{
  /// @brief A task queue for managing tasks in a thread pool with work-stealing support.
  /// @details Extends `ring_buffer<fast_func_t>` to store tasks with an associated execution policy
  ///          and activity stats for work-stealing coordination. Supports pushing tasks with or
  ///          without a reusable `slot_token`, and popping tasks with sequential dependency checks
  ///          for `execution::seq` to enforce single-edge DAG ordering. Thread-safe for multiple
  ///          producers and consumers.
  class alignas(details::cache_line_size) task_queue : private ring_buffer<fast_func_t>
  {
    using base_t = ring_buffer<fast_func_t>;

  public:
    /// @brief Constructs a task queue from an existing ring buffer with specified policy and stats.
    /// @param base The `ring_buffer<fast_func_t>` to move into the queue.
    /// @param policy The execution policy (`seq` for sequential, `par` for parallel).
    /// @param activity Reference to shared `activity_stats` for work-stealing notifications.
    explicit task_queue(base_t&& base, execution policy,
                        schedulers::stealing::activity_stats& activity)
      : base_t(std::move(base))
      , activity_(activity)
    {}

    /// @brief Constructs an empty task queue with specified policy and stats.
    /// @param policy The execution policy (`seq` for sequential, `par` for parallel).
    /// @param activity Reference to shared `activity_stats` for work-stealing notifications.
    explicit task_queue(schedulers::stealing::activity_stats& activity)
      : activity_(activity)
    {}

    /// @brief Attempts to claim the first task from the queue.
    /// @details For `execution::seq`, checks if the previous task is completed (`empty` state)
    ///          before claiming, enforcing single-edge DAG ordering. For `execution::par`, claims
    ///          without checking dependencies. Returns a `claimed_slot<fast_func_t>` that
    ///          auto-releases on destruction. Thread-safe for multiple consumers.
    /// @return A `claimed_slot<fast_func_t>` if a task is claimed, or `nullptr` if the queue is
    ///         empty, contention occurs, or the previous task is not completed (for `seq`).
    [[nodiscard]] auto
    try_pop() noexcept -> decltype(auto)
    {
      return base_t::try_pop_front();
    }

    /// @brief Returns the maximum capacity of the queue.
    /// @details Returns the capacity of the underlying `ring_buffer<fast_func_t>`.
    /// @return The maximum number of tasks the queue can hold.
    [[nodiscard]] static constexpr auto
    max_size() noexcept
    {
      return base_t::max_size();
    }

  private:
    alignas(details::cache_line_size) schedulers::stealing::activity_stats& activity_; // NOLINT
  };

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
    explicit pool(unsigned int threads = std::thread::hardware_concurrency())
    {
      executors_.reserve(threads);
      for (unsigned int i = 0; i < threads; ++i)
      {
        executors_.emplace_back(std::make_unique<executor>(activity_,
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
    /// @details Emplaces a task constructed from `val...` into the queue, binding it to the
    ///          provided `slot_token` for tracking. Signals `activity_.ready` to wake sleeping
    ///          executors. Thread-safe for multiple producers.
    /// @tparam U Types of the arguments to construct the task.
    /// @param token Reference to a `slot_token` to track the task's state.
    /// @param val Arguments to construct a `fast_func_t` task.
    /// @return Reference to the provided `slot_token`.
    /// @requires `fast_func_t` must be constructible from `U...`.
    template<execution Policy = execution::par, typename... U>
      requires std::constructible_from<fast_func_t, U...>
    auto
    push(slot_token& token, U&&... val) noexcept -> decltype(auto)
    {
      static constexpr auto seq =
        Policy == execution::seq ? slot_state::tag_seq : slot_state::invalid;
      activity_.master.template emplace_back<seq>(token, FWD(val)...);
      activity_.notifier.notify_one();
      return token;
    }

    /// @brief Pushes a task with arguments and returns a new token.
    /// @details Emplaces a task constructed from `val...` into the queue, creating a new
    ///          `slot_token` for tracking. Signals `activity_.ready` to wake sleeping executors.
    ///          Thread-safe for multiple producers.
    /// @tparam U Types of the arguments to construct the task.
    /// @param val Arguments to construct a `fast_func_t` task.
    /// @return A `slot_token` for tracking the task's state.
    /// @requires `fast_func_t` must be constructible from `U...`.
    template<execution Policy = execution::par, typename... U>
      requires std::constructible_from<fast_func_t, U...>
    auto
    push(U&&... val) noexcept -> decltype(auto)
    {
      static constexpr auto seq =
        Policy == execution::seq ? slot_state::tag_seq : slot_state::invalid;
      auto token = activity_.master.template emplace_back<seq>(FWD(val)...);
      activity_.notifier.notify_one();
      return token;
    }

    /// @brief Attempts to steal a task from a random queue.
    /// @details Selects a random queue using a thread-local random number generator and tries to
    ///          pop a task via `task_queue::try_pop()`. Returns `nullptr` if no task is available
    ///          or the queue is empty. Thread-safe for concurrent stealing.
    /// @return A `claimed_slot<fast_func_t>` if a task is stolen, or `nullptr` if none available.
    [[nodiscard]] auto
    steal(std::ranges::range auto&& r) noexcept
    {
      thread_local auto rng = std::mt19937{std::random_device{}()};
      using cached_t        = std::ranges::range_value_t<decltype(r)>;

      auto  dist   = std::uniform_int_distribution<std::size_t>(0, executors_.size() - 1);
      auto  idx    = dist(rng);
      auto& victim = executors_[idx];

      auto cached = cached_t{nullptr};

      auto s = victim->steal(FWD(r), cached);
      if (!cached)
      {
        constexpr auto cap = std::size_t{4};
        for (auto t : activity_.master.try_pop_front(cap))
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
      return std::move(cached);
    }

    /// @brief Returns the number of pending tasks.
    /// @details Counts the current number of tasks in the pool.
    /// @return Number of tasks.
    [[nodiscard]] auto
    size() const noexcept -> std::size_t
    {
      return activity_.master.size();
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
    alignas(details::cache_line_size) schedulers::stealing::activity_stats activity_;
    alignas(details::cache_line_size) std::vector<std::unique_ptr<executor>> executors_;
  };

  namespace details
  {
    using pool_t = fho::pool;

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
}

#undef FWD

#ifdef _MSC_VER
  #pragma warning(pop)
#endif
