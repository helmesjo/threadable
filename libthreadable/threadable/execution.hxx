#pragma once

#include <threadable/ring_buffer.hxx>

#include <atomic>
#include <execution>
#include <ranges>
#include <thread>

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

namespace fho
{
  enum class execution
  {
    seq,
    par
  };

  /// @brief Executes a range of jobs.
  /// @details Invokes the callables in the provided range, either sequentially or in parallel based
  /// on the execution policy.
  /// @tparam `R` The type of the range.
  /// @param `r` The range of jobs to execute.
  /// @param `policy` Execution policy.
  /// @return The number of jobs executed.
  inline auto
  execute(std::ranges::range auto&& r)
    requires std::invocable<std::ranges::range_value_t<decltype(r)>>
  {
    for (auto& e : r)
    {
      e();
    }
    return r.size();
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
      using value_t = std::ranges::range_value_t<decltype(range)>;
      return work_.push(
        [policy](std::ranges::range auto&& r) mutable
        {
          if constexpr (std::same_as<job, value_t>)
          {
            auto const b    = std::begin(r);
            auto const e    = std::end(r);
            auto const prev = b - 1;
            if (policy == execution::seq) [[likely]]
            {
              // Make sure previous slot has been processed.
              if (b != e && prev != e && prev < b) [[unlikely]]
              {
                prev->state.template wait<job_state::active, true>(std::memory_order_acquire);
              }
            }
          }
          for (auto& j : r)
          {
            j();
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
