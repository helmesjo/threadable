#pragma once

#include <threadable/atomic.hxx>
#include <threadable/ring_buffer.hxx>
#include <threadable/schedulers/stealing.hxx>

#include <concepts>
#include <ranges>
#include <stop_token>
#include <thread>

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

namespace fho
{
  enum class execution
  {
    seq,
    par
  };

  /// @brief Manages a single-threaded task execution system.
  /// @details The `executor` class runs a single thread that processes tasks submitted via
  /// `submit`.
  ///          tasks are stored in a `ring_buffer` and executed sequentially. Use `stop` to halt the
  ///          executor and clear pending tasks.
  /// @example
  /// ```cpp
  /// fho::executor exec;
  /// auto range = queue.pop range();
  /// auto token = exec.submit(range, fho::execution::seq);
  /// token.wait();
  /// ```
  class executor
  {
  public:
    /// @brief Constructs an executor and starts its thread.
    /// @details Initializes the internal `ring_buffer` and spawns a thread running the `run`
    ///          method to process submitted tasks.
    executor()
      : thread_(
          [this]
          {
            run();
          })
    {}

    /// @brief Destroys the executor, stopping it and joining the thread.
    /// @details Invokes `stop` to terminate the execution loop and joins the thread to ensure
    ///          all tasks complete before cleanup.
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

    /// @brief Submits a range of tasks as a single task with specified execution policy.
    /// @details Wraps the range in a lambda that executes all tasks in the range based on the
    ///          policy (`seq` or `par`). For `seq`, ensures prior `ring_buffer` slots complete.
    /// @tparam T The range type, must contain invocable objects.
    /// @param range The range of tasks to execute as a single task.
    /// @param policy The execution policy (`execution::seq` or `execution::par`).
    /// @return A `slot_token` representing the submitted task in the `ring_buffer`.
    auto
    submit(std::ranges::range auto&& range, execution policy) noexcept
      requires std::invocable<std::ranges::range_value_t<decltype(range)>>
    {
      return work_.emplace_back(
        [policy](std::ranges::range auto r)
        {
          if constexpr (requires { r.begin().base(); })
          {
            using base_t = decltype(*r.begin().base());
            if constexpr (atomic_slot<base_t>)
            {
              if (policy == execution::seq) [[unlikely]]
              {
                auto const b    = r.begin().base();
                auto const e    = r.end().base();
                auto const prev = b - 1;
                // @NOTE
                // This guards seq policy; wait on prev slot completion before exec current range.
                // Each check prevents UB/race:
                // * `b != e`
                //   Skips if range empty (no tasks).
                // * `prev != e`
                //   Skips if prev invalid (e.g., b=0, prev underflow).
                //   Important: Prevents wait on bogus slot (e.g., elems_[huge & mask]
                //   wrong/corrupt); avoids UB from unsigned wrap.
                // * `prev < b`
                //   Ensures `prev` before `b` (logical order).
                //   Important: Validates seq chain (monotonic index); skips if wrap/overflow (rare,
                //   but prevents wait on future/wrong slot, maintaining FIFO).
                if (b != e && prev != e && prev < b) [[unlikely]]
                {
                  prev->template wait<~slot_state::empty, true>(std::memory_order_acquire);
                }
              }
            }
          }
          for (auto& c : r)
          {
            c();
          }
        },
        FWD(range));
    }

    /// @brief Submits a single task to the executor.
    /// @details Adds the callable to the `ring_buffer` for execution by the thread.
    /// @tparam Func The type of the callable, must be invocable.
    /// @param work The callable to execute.
    /// @return A `slot_token` for tracking the task’s state in the `ring_buffer`.
    auto
    submit(std::invocable auto&& work) noexcept
    {
      return work_.emplace_back(FWD(work));
    }

    /// @brief Halts the executor and clears remaining tasks.
    /// @details Sets the `stop_` flag and emplaces a task to clear the `ring_buffer`, ensuring the
    ///          thread exits its loop.
    void
    stop() noexcept
    {
      work_.emplace_back(
        [this]
        {
          stop_ = true;
          work_.clear();
        });
    }

    /// @brief Checks if the executor is currently processing tasks.
    /// @details Returns \`true\` if the executor is in the middle of
    /// processing tasks, else \`false\` if it's waiting for new tasks.
    auto
    busy() const noexcept -> bool
    {
      return executing_.load(std::memory_order_acquire);
    }

  private:
    /// @brief Internal thread loop to process tasks.
    /// @details Continuously consumes and executes tasks from the `ring_buffer` until stopped.
    ///          Avoids sleeping to minimize contention and delays.
    void
    run()
    {
      while (!stop_) [[likely]]
      {
        if (auto r = work_.pop_front_range(); !r.empty()) [[likely]]
        {
          for (auto& j : r)
          {
            j();
          }
        }
        else
        {
          executing_.store(false, std::memory_order_release);
          work_.wait();
          executing_.store(true, std::memory_order_release);
        }
      }
    }

    std::atomic_bool executing_ = true;
    bool             stop_      = false;
    ring_buffer<>    work_;
    std::thread      thread_;
  };

  namespace v2
  {
    namespace sched = fho::schedulers::stealing;

    class executor
    {
    public:
      executor(sched::activity_stats& activity, sched::invocable_return auto&& stealer)
      {
        thread_ = std::jthread(
          [this, &activity, &stealer](std::stop_token const& t)
          {
            (*this)(t, activity, FWD(stealer));
          });
      }

      executor(executor const&)                    = delete;
      executor(executor&&)                         = delete;
      auto operator=(executor const&) -> executor& = delete;
      auto operator=(executor&&) -> executor&      = delete;

      ~executor()
      {
        thread_.request_stop();
      }

      void
      operator()(std::stop_token const& t, sched::activity_stats& activity,
                 sched::invocable_return auto&& stealer) noexcept
      {
        using claimed_t = typename fho::ring_buffer<>::claimed_type;

        auto exec   = sched::exec_stats{};
        auto self   = fho::ring_buffer<claimed_t>{};
        auto order  = std::vector<bool>{};
        auto action = sched::action::exploit;
        while (!t.stop_requested())
        {
          sched::process_action(action, activity, exec, self, stealer);
          action = sched::process_state(activity, exec, action);
        }
      }

    private:
      std::jthread thread_;
    };
  }
}

#undef FWD
