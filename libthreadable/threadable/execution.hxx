#pragma once

#include <threadable/atomic.hxx>
#include <threadable/ring_buffer.hxx>
#include <threadable/schedulers/stealing.hxx>

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

  namespace scheduler = fho::schedulers::stealing;

  class executor
  {
  public:
    executor(scheduler::activity_stats& activity, scheduler::invocable_return auto&& stealer)
    {
      thread_ = std::thread(
        [this, &activity, stealer = FWD(stealer)]()
        {
          (*this)(stop_, activity, stealer);
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
      stop_.store(true, std::memory_order::release);
      if (thread_.joinable())
      {
        thread_.join();
      }
      tasks_.clear();
    }

    [[nodiscard]] auto
    steal(std::ranges::range auto&& r) noexcept -> std::size_t
    {
      if (&r == &tasks_) [[unlikely]]
      {
        return 0;
      }

      if (auto t = tasks_.try_pop_front(); t)
      {
        r.emplace_back(std::move(*t));
        return 1;
      }
      return 0;
    }

    void
    operator()(std::atomic_bool const& t, scheduler::activity_stats& activity,
               scheduler::invocable_return auto& stealer) noexcept
    {
      using claimed_t = claimed_slot<fast_func_t>;

      auto exec   = scheduler::exec_stats{};
      auto order  = std::vector<bool>{};
      auto action = scheduler::action::exploit;
      while (!t.load(std::memory_order_relaxed))
      {
        scheduler::process_action(action, activity, exec, tasks_, stealer);
        action = scheduler::process_state(activity, exec, action);
      }
      tasks_.clear();
    }

  private:
    fho::ring_buffer<ring_buffer<fast_func_t>::claimed_type> tasks_;
    std::atomic_bool                                         stop_{false};
    std::thread                                              thread_;
  };
}

#undef FWD
