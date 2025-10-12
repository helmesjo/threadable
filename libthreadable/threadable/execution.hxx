#pragma once

#include <threadable/atomic.hxx>
#include <threadable/ring_buffer.hxx>
#include <threadable/scheduler/stealing.hxx>

#include <atomic>
#include <thread>

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

namespace fho
{
  enum class execution
  {
    seq,
    par
  };

  namespace sched = fho::scheduler::stealing;

  class executor
  {
  public:
    executor(sched::activity_stats& activity, sched::exec_stats init,
             sched::invocable_return auto&& stealer)
      : stats_(init)
      , notifier_(activity.notifier)
    {
      thread_ = std::thread(
        [this, &activity, stealer = FWD(stealer)]()
        {
          (*this)(activity, stealer);
        });
    }

    executor(executor const&)                    = delete;
    executor(executor&&)                         = delete;
    auto operator=(executor const&) -> executor& = delete;
    auto operator=(executor&&) -> executor&      = delete;

    ~executor()
    {
      stop();
    }

    void
    stop() noexcept
    {
      stats_.abort = true;
      notifier_.notify_all();
      if (thread_.joinable())
      {
        thread_.join();
      }
      tasks_.clear();
    }

    /// @brief Attempts to steal a task from this executor.
    /// @details Tries to pop a task via `task_queue::try_pop_front()`.
    ///          Thread-safe for concurrent stealing.
    /// @param `r` The associated range attempting to steal.
    /// @return Returns `false` if `r` is the executors own queue, `true` otherwise.
    [[nodiscard]] auto
    steal(std::ranges::range auto&& r, auto& cached) noexcept -> bool
    {
      if (&r == &tasks_) [[unlikely]]
      {
        // Returns 'false' if 'r == our queue'
        return false;
      }

      if (auto t = tasks_.try_pop_front(); t)
      {
        cached = std::move(*t);
      }
      return true;
    }

    void
    operator()(sched::activity_stats& activity, sched::invocable_return auto& stealer) noexcept
    {
      sched::worker_loop(activity, stats_, tasks_, stealer);
      tasks_.clear();
    }

  private:
    sched::exec_stats                          stats_;
    decltype(sched::activity_stats::notifier)& notifier_;
    fho::ring_buffer<ring_buffer<fast_func_t>::claimed_type, (details::default_capacity >> 6)>
                tasks_;
    std::thread thread_;
  };
}

#undef FWD
