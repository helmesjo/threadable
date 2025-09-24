#pragma once

#include <threadable/execution.hxx>
#include <threadable/function.hxx>
#include <threadable/ring_buffer.hxx>
#include <threadable/schedulers/stealing.hxx>

#include <algorithm>
#include <atomic>
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
      , policy_(policy)
      , activity_(activity)
    {}

    /// @brief Constructs an empty task queue with specified policy and stats.
    /// @param policy The execution policy (`seq` for sequential, `par` for parallel).
    /// @param activity Reference to shared `activity_stats` for work-stealing notifications.
    explicit task_queue(execution policy, schedulers::stealing::activity_stats& activity)
      : policy_(policy)
      , activity_(activity)
    {}

    /// @brief Pushes a task with a reusable token and arguments.
    /// @details Emplaces a task constructed from `val...` into the queue, binding it to the
    ///          provided `slot_token` for tracking. Signals `activity_.ready` to wake sleeping
    ///          executors. Thread-safe for multiple producers.
    /// @tparam U Types of the arguments to construct the task.
    /// @param token Reference to a `slot_token` to track the task's state.
    /// @param val Arguments to construct a `fast_func_t` task.
    /// @return Reference to the provided `slot_token`.
    /// @requires `fast_func_t` must be constructible from `U...`.
    template<typename... U>
      requires std::constructible_from<fast_func_t, U...>
    auto
    push(slot_token& token, U&&... val) noexcept -> decltype(auto)
    {
      emplace_back(token, FWD(val)...);
      activity_.ready.fetch_add(1, std::memory_order_release);
      activity_.ready.notify_one();
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
    template<typename... U>
      requires std::constructible_from<fast_func_t, U...>
    auto
    push(U&&... val) noexcept -> decltype(auto)
    {
      auto token = emplace_back(FWD(val)...);
      activity_.ready.fetch_add(1, std::memory_order_release);
      activity_.ready.notify_one();
      return token;
    }

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
      if (policy_ != execution::seq) [[likely]]
      {
        return base_t::try_pop_front(false);
      }
      else
      {
        return base_t::try_pop_front(true);
      }
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
    alignas(details::cache_line_size) execution policy_;
    alignas(details::cache_line_size) schedulers::stealing::activity_stats& activity_; // NOLINT
  };

  /// @brief A thread pool managing task queues with work-stealing executors.
  /// @details Manages a set of `task_queue`s and `executor`s, distributing tasks via work-stealing.
  ///          Supports dynamic queue creation and removal, with tasks executed according to their
  ///          queue’s policy (`seq` for single-edge DAG ordering, `par` for parallel). Uses a
  ///          shared `activity_stats` for efficient executor wake-up. Thread-safe for queue
  ///          management.
  class pool
  {
  public:
    using queue_t  = task_queue;
    using queues_t = std::vector<std::shared_ptr<queue_t>>;

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

    /// @brief Destroys the pool, stopping all executors.
    /// @details Sets the `activity_.abort` flag and signals `activity_.ready` to wake all
    /// executors,
    ///          ensuring clean shutdown. Executors are joined automatically via their `jthread`
    ///          destructors.
    ~pool()
    {
      activity_.abort.store(true, std::memory_order_release);
      activity_.ready.store(1, std::memory_order_release);
      activity_.ready.notify_all();
      executors_.clear();
    }

    /// @brief Creates a new task queue with the specified execution policy.
    /// @details Adds a new `task_queue` to the pool’s managed queues, with the given policy
    ///          (`seq` for sequential, `par` for parallel). Thread-safe for concurrent creation.
    /// @param policy Execution policy for the new queue (defaults to `execution::par`).
    /// @return Reference to the created `task_queue`.
    [[nodiscard]] auto
    create(execution policy = execution::par) noexcept -> queue_t&
    {
      using q_t = decltype(queues_.load());

      auto queue    = std::make_shared<queue_t>(policy, activity_);
      auto expected = queues_.load();

      auto local   = q_t{};
      auto desired = q_t{};
      do
      {
        local = std::make_shared<std::vector<std::shared_ptr<queue_t>>>(*expected);
        local->push_back(queue);
        desired = std::move(local);
      }
      while (!queues_.compare_exchange_weak(expected, desired));

      return *queue;
    }

    /// @brief Adds an existing ring buffer as a task queue to the pool.
    /// @details Moves the provided `ring_buffer<fast_func_t>` into a new `task_queue` with the
    ///          specified policy, adding it to the pool’s managed queues. Thread-safe for
    ///          concurrent addition.
    /// @param q The `ring_buffer<fast_func_t>` to add, moved into the queue.
    /// @param policy Execution policy for the queue (`seq` or `par`).
    /// @return Reference to the created `task_queue`.
    [[nodiscard]] auto
    add(ring_buffer<fast_func_t>&& q, execution policy) -> queue_t&
    {
      using q_t = decltype(queues_.load());

      auto queue    = std::make_shared<task_queue>(std::move(q), policy, activity_);
      auto expected = queues_.load();

      auto local   = q_t{};
      auto desired = q_t{};

      do
      {
        local = std::make_shared<std::vector<std::shared_ptr<queue_t>>>(*expected);
        local->push_back(queue);
        desired = std::move(local);
      }
      while (!queues_.compare_exchange_weak(expected, desired));

      return *queue;
    }

    /// @brief Removes a task queue from the pool.
    /// @details Removes the specified `task_queue` from the pool’s managed queues if found.
    ///          Thread-safe for concurrent removal. The queue is not cleared; users must ensure
    ///          no further use after removal.
    /// @param queue The `task_queue` to remove.
    /// @return `true` if the queue was removed, `false` if not found.
    [[nodiscard]] auto
    remove(queue_t&& queue) noexcept -> bool // NOLINT
    {
      using q_t = decltype(queues_.load());

      auto expected = queues_.load();

      auto local   = q_t{};
      auto desired = q_t{};

      do
      {
        local = std::make_shared<std::vector<std::shared_ptr<queue_t>>>(*expected);
        if (auto itr = std::ranges::find_if(*local,
                                            [&queue](auto const& q)
                                            {
                                              return q.get() == &queue;
                                            });
            itr != std::end(*local))
        {
          // TODO: Clear queue, it can't be used after user explicitly "removed" it.
          local->erase(itr);
          desired = std::move(local);
        }
        else
        {
          return false;
        }
      }
      while (!queues_.compare_exchange_weak(expected, desired));

      return true;
    }

    /// @brief Returns the number of managed task queues.
    /// @details Counts the current number of queues in the pool. Thread-safe.
    /// @return Number of task queues.
    [[nodiscard]] auto
    size() const noexcept -> std::size_t
    {
      auto queues = queues_.load();
      return queues->size();
    }

    /// @brief Returns the maximum capacity of each task queue.
    /// @details Returns the capacity of the underlying `task_queue` (same as
    /// `ring_buffer::max_size()`).
    /// @return Maximum number of tasks per queue.
    static constexpr auto
    max_size() noexcept -> std::size_t
    {
      return queue_t::max_size();
    }

  private:
    /// @brief Attempts to steal a task from a random queue.
    /// @details Selects a random queue using a thread-local random number generator and tries to
    ///          pop a task via `task_queue::try_pop()`. Returns `nullptr` if no task is available
    ///          or the queue is empty. Thread-safe for concurrent stealing.
    /// @return A `claimed_slot<fast_func_t>` if a task is stolen, or `nullptr` if none available.
    [[nodiscard]] auto
    steal() noexcept -> ring_buffer<>::claimed_type
    {
      thread_local static auto gen = std::mt19937{std::random_device{}()};

      auto queues = queues_.load();

      if (queues->empty()) [[unlikely]]
      {
        return nullptr;
      }
      auto distr = std::uniform_int_distribution<std::size_t>{0, queues->size() - 1};
      auto idx   = distr(gen);
      return (*queues)[idx]->try_pop();
    }

    alignas(details::cache_line_size) schedulers::stealing::activity_stats activity_;
    alignas(details::cache_line_size) std::atomic<std::shared_ptr<queues_t>> queues_ =
      std::make_shared<queues_t>();
    alignas(details::cache_line_size) std::vector<std::unique_ptr<executor>> executors_;
  };

  namespace details
  {
    using pool_t  = fho::pool;
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
