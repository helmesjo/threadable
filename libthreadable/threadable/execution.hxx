#pragma once

#include <threadable/atomic.hxx>
#include <threadable/ring_buffer.hxx>
#include <threadable/scheduler/stealing.hxx>

#include <concepts>

#if __cpp_lib_execution >= 201603L && __cpp_lib_parallel_algorithm >= 201603L
  #include <execution>
#endif

#include <thread>

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

namespace fho
{
  namespace execution
  {
#if __cpp_lib_execution >= 201603L && __cpp_lib_parallel_algorithm >= 201603L
    inline constexpr auto seq = std::execution::seq;
    inline constexpr auto par = std::execution::par;
#else
    struct parallel_policy
    {
    } const par;

    struct sequential_policy
    {
    } const seq;
#endif
  }

  template<typename ExPo>
  concept exec_policy = std::common_reference_with<ExPo, decltype(execution::par)> ||
                        std::common_reference_with<ExPo, decltype(execution::seq)>;

  namespace sched = fho::scheduler::stealing;

  class executor
  {
    using queue_t =
      fho::ring_buffer<ring_buffer<fast_func_t>::claimed_type, (details::default_capacity >> 6)>;

  public:
    executor(sched::event_count& notifier)
      : notifier_(notifier)
    {}

    executor(executor const&)                    = delete;
    executor(executor&&)                         = delete;
    auto operator=(executor const&) -> executor& = delete;
    auto operator=(executor&&) -> executor&      = delete;

    ~executor()
    {
      stop();
    }

    auto
    start(sched::activity_stats& activity, sched::exec_stats init, auto&& master) -> executor&
    {
      stats_  = init;
      thread_ = std::thread(
        [this, &activity, &master]()
        {
          (*this)(activity, master);
        });

      return *this;
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
    operator()(sched::activity_stats& activity, sched::master_queue<queue_t> auto& master) noexcept
    {
      sched::worker_loop(activity, stats_, tasks_, master);
      tasks_.clear();
    }

  private:
    sched::exec_stats                          stats_;
    decltype(sched::activity_stats::notifier)& notifier_;
    queue_t                                    tasks_;
    std::thread                                thread_;
  };
}

#undef FWD
