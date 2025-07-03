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

  /// @brief Executes a range of callables.
  /// @details Invokes the callables in the provided range with the provided arguments.
  /// @tparam `R` The type of the range.
  /// @param `r` The range of callables to execute.
  /// @tparam `Args` The types of the arguments.
  /// @param `args` The arguments forwarded to each invocation.
  /// @return The number of callables executed.
  template<std::ranges::range R, typename... Args>
  inline constexpr auto
  execute(auto&& exPo, R&& r, Args&&... args)
    requires std::invocable<std::ranges::range_value_t<R>, Args...>
  {
    using value_t = std::ranges::range_value_t<decltype(r)>;
    auto const b  = std::begin(r);
    auto const e  = std::end(r);
#if __cpp_lib_execution >= 201603L && __cpp_lib_parallel_algorithm >= 201603L
    std::for_each(exPo, b, e,
                  [&](value_t& elem)
                  {
                    elem(FWD(args)...);
                  });
#else
    for (auto&& c : FWD(r))
    {
      FWD(c)(FWD(args)...);
    }
#endif
    return r.size();
  }

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

  /// @brief A class that manages a single thread for executing jobs.
  /// @details The `executor` class encapsulates a single thread that continuously executes jobs
  /// submitted to it via the `submit` method. It uses a sequential `ring_buffer` to store and
  /// manage the jobs. The executor can be stopped using the `stop` method, which sets a flag to
  /// halt the execution loop and clears any remaining jobs in the buffer.
  /// @example
  /// ```cpp
  /// auto exec = fho::executor();
  /// auto range = queue.consume();
  /// auto t = exec.submit(range);
  /// t.wait();
  /// ```
  class executor
  {
  public:
    /// @brief Default constructor that starts the executor thread.
    /// @details Initializes the executor and starts a new thread that runs the `run` method, which
    /// is responsible for executing jobs from the internal `ring_buffer`.
    executor()
      : thread_(
          [this]
          {
            run();
          })
    {}

    /// @brief Destructor that stops the executor and joins the thread.
    /// @details Calls `stop()` to halt the execution loop and then joins the executor thread to
    /// ensure all jobs are completed before destruction.
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

    /// @brief Submits a range of jobs to be executed as a single job by the executor.
    /// @details This method creates a new job that, when executed, will run all the jobs in the
    /// provided range sequentially. This new job is then pushed into the internal `ring_buffer`.
    /// @tparam `T` The type of the range, must be a subrange of jobs.
    /// @param `range` The range of jobs to be executed together as a single job.
    /// @return A `slot_token` for the submitted job, which represents the entire range.
    auto
    submit(std::ranges::range auto&& range, execution policy) noexcept
      requires std::invocable<std::ranges::range_value_t<decltype(range)>>
    {
      // NOTE: Take range as l-value to claim ownership if it was
      //       passed as r-value reference to make sure it is
      //       correctly released when the lambda completes.
      //       See `active_subrange` destruction for details.
      return work_.push(
        [policy](std::ranges::range auto r /* Must be passed by value */) mutable
        {
          // Special rule for `ring_buffer` ranges:
          // If sequential execution policy, wait for completion of
          // previous slot.
          // NOTE: We call `.base()` since `ring_buffer` internally uses
          //       a transform view adapter, and we look for `ring_slot`.
          if constexpr (requires { range.begin().base()->wait(); })
          {
            if (policy == execution::seq) [[unlikely]]
            {
              auto const b    = r.begin().base();
              auto const e    = r.end().base();
              auto const prev = b - 1;
              // Make sure previous slot has been processed.
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

    /// @brief Submits a single job to be executed by the executor.
    /// @details This method pushes the given callable into the internal `ring_buffer`. The job will
    /// be executed by the executor thread.
    /// @tparam `Func` The type of the callable, must be invocable.
    /// @param `work` The callable to be executed as a job.
    /// @return A `slot_token` for the submitted job, which can be used to monitor its state.
    auto
    submit(std::invocable auto&& work) noexcept
    {
      return work_.push(FWD(work));
    }

    /// @brief Stops the executor from accepting new jobs and consumes any remaining jobs.
    /// @details Sets the stop flag to true and consumes all remaining jobs in the internal
    /// `ring_buffer`, effectively clearing the queue.
    void
    stop() noexcept
    {
      // Release thread if waiting.
      work_.push(
        [this]
        {
          stop_ = true;
          work_.clear();
        });
    }

  private:
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
          // NOTE: Don't sleep here, as even (especially?) small sleeps (nanosecond)
          //       will 1. increase ring buffer contention but also 2. accumulate
          //       in user code with many executors causing unexpected delays.
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
