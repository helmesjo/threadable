#pragma once

#include <threadable/atomic.hxx>
#include <threadable/ring_buffer.hxx>

#include <ranges>
#include <thread>

#if __cpp_lib_execution >= 201603L && __cpp_lib_parallel_algorithm >= 201603L
  #include <algorithm>
  #include <execution>
#endif

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

namespace fho
{
  enum class execution
  {
    seq,
    par
  };

  /// @brief Executes a range of callables with specified execution policy.
  /// @details Invokes each callable in the provided range with the given arguments.
  ///          Supports sequential (`seq`) or parallel (`par`) execution when the C++17 execution
  ///          policies are available; otherwise, defaults to a sequential loop.
  /// @tparam R The type of the range containing invocable objects.
  /// @param exPo The execution policy (e.g., `fho::execution::seq`, `fho::execution::par`, or a
  /// standard policy).
  /// @param r The range of callables to execute.
  /// @tparam Args Variadic argument types to pass to each callable.
  /// @param args Arguments forwarded to each callable invocation.
  /// @return The number of callables executed, equivalent to the range's size.
  template<std::ranges::range R, typename... Args>
  inline constexpr auto
  execute([[maybe_unused]] auto&& exPo, R&& r, Args&&... args)
    requires std::invocable<std::ranges::range_value_t<R>, Args...>
  {
#if __cpp_lib_execution >= 201603L && __cpp_lib_parallel_algorithm >= 201603L
    using value_t = std::ranges::range_value_t<decltype(r)>;
    auto const b  = std::begin(r);
    auto const e  = std::end(r);
    if constexpr (std::same_as<std::remove_cvref_t<decltype(exPo)>, fho::execution>)
    {
      if (exPo == execution::seq)
      {
        std::for_each(std::execution::seq, b, e,
                      [&](value_t& elem)
                      {
                        elem(FWD(args)...);
                      });
      }
      else if (exPo == execution::par)
      {
        std::for_each(std::execution::par, b, e,
                      [&](value_t& elem)
                      {
                        elem(FWD(args)...);
                      });
      }
    }
    else
    {
      std::for_each(exPo, b, e,
                    [&](value_t& elem)
                    {
                      elem(FWD(args)...);
                    });
    }
#else
    for (auto&& c : FWD(r))
    {
      FWD(c)(FWD(args)...);
    }
#endif
    return r.size();
  }

  /// @brief Executes a range of callables with default sequential execution.
  /// @details Invokes each callable in the range sequentially with the provided arguments.
  ///          Uses `std::execution::seq` if available; otherwise, falls back to a simple loop.
  /// @tparam R The type of the range containing invocable objects.
  /// @param r The range of callables to execute.
  /// @tparam Args Variadic argument types to pass to each callable.
  /// @param args Arguments forwarded to each callable invocation.
  /// @return The number of callables executed, equivalent to the range's size.
  template<std::ranges::range R, typename... Args>
  inline constexpr auto
  execute(R&& r, Args&&... args)
    requires std::invocable<std::ranges::range_value_t<R>, Args...>
  {
#if __cpp_lib_execution >= 201603L && __cpp_lib_parallel_algorithm >= 201603L
    return execute(std::execution::seq, FWD(r), FWD(args)...);
#else
    return execute(0, FWD(r), FWD(args)...);
#endif
  }

  /// @brief Manages a single-threaded job execution system.
  /// @details The `executor` class runs a single thread that processes jobs submitted via `submit`.
  ///          Jobs are stored in a `ring_buffer` and executed sequentially. Use `stop` to halt the
  ///          executor and clear pending jobs.
  /// @example
  /// ```cpp
  /// fho::executor exec;
  /// auto range = queue.consume();
  /// auto token = exec.submit(range, fho::execution::seq);
  /// token.wait();
  /// ```
  class executor
  {
  public:
    /// @brief Constructs an executor and starts its thread.
    /// @details Initializes the internal `ring_buffer` and spawns a thread running the `run`
    ///          method to process submitted jobs.
    executor()
      : thread_(
          [this]
          {
            run();
          })
    {}

    /// @brief Destroys the executor, stopping it and joining the thread.
    /// @details Invokes `stop` to terminate the execution loop and joins the thread to ensure
    ///          all jobs complete before cleanup.
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

    /// @brief Submits a range of jobs as a single task with specified execution policy.
    /// @details Wraps the range in a lambda that executes all jobs in the range based on the
    ///          policy (`seq` or `par`). For `seq`, ensures prior `ring_buffer` slots complete.
    /// @tparam T The range type, must contain invocable objects.
    /// @param range The range of jobs to execute as a single task.
    /// @param policy The execution policy (`execution::seq` or `execution::par`).
    /// @return A `slot_token` representing the submitted task in the `ring_buffer`.
    auto
    submit(std::ranges::range auto&& range, execution policy) noexcept
      requires std::invocable<std::ranges::range_value_t<decltype(range)>>
    {
      return work_.push(
        [policy](std::ranges::range auto r) mutable
        {
          if constexpr (requires { r.begin().base()->wait(); })
          {
            if (policy == execution::seq) [[unlikely]]
            {
              auto const b    = r.begin().base();
              auto const e    = r.end().base();
              auto const prev = b - 1;
              if (b != e && prev != e && prev < b) [[unlikely]]
              {
                prev->wait();
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

    /// @brief Submits a single job to the executor.
    /// @details Adds the callable to the `ring_buffer` for execution by the thread.
    /// @tparam Func The type of the callable, must be invocable.
    /// @param work The callable to execute.
    /// @return A `slot_token` for tracking the job’s state in the `ring_buffer`.
    auto
    submit(std::invocable auto&& work) noexcept
    {
      return work_.push(FWD(work));
    }

    /// @brief Halts the executor and clears remaining jobs.
    /// @details Sets the `stop_` flag and pushes a job to clear the `ring_buffer`, ensuring the
    ///          thread exits its loop.
    void
    stop() noexcept
    {
      work_.push(
        [this]
        {
          stop_ = true;
          work_.clear();
        });
    }

  private:
    /// @brief Internal thread loop to process jobs.
    /// @details Continuously consumes and executes jobs from the `ring_buffer` until stopped.
    ///          Avoids sleeping to minimize contention and delays.
    void
    run()
    {
      while (!stop_) [[likely]]
      {
        if (auto r = work_.consume(); !r.empty()) [[likely]]
        {
          for (auto& j : r)
          {
            j();
          }
        }
        else
        {
          work_.wait();
        }
      }
    }

    bool          stop_ = false;
    ring_buffer<> work_;
    std::thread   thread_;
  };
}

#undef FWD
