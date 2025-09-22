#pragma once

#include <threadable/atomic.hxx>
#include <threadable/ring_buffer.hxx>
#include <threadable/schedulers/stealing.hxx>

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

  namespace scheduler = fho::schedulers::stealing;

  class executor
  {
  public:
    executor(scheduler::activity_stats& activity, scheduler::invocable_return auto&& stealer)
    {
      thread_ = std::jthread(
        [this, &activity, stealer = FWD(stealer)](std::stop_token const& t)
        {
          (*this)(t, activity, stealer);
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
    operator()(std::stop_token const& t, scheduler::activity_stats& activity,
               scheduler::invocable_return auto& stealer) noexcept
    {
      using claimed_t = claimed_slot<fast_func_t>;

      auto exec   = scheduler::exec_stats{};
      auto self   = fho::ring_buffer<claimed_t>{};
      auto order  = std::vector<bool>{};
      auto action = scheduler::action::exploit;
      while (!t.stop_requested())
      {
        scheduler::process_action(action, activity, exec, self, stealer);
        action = scheduler::process_state(activity, exec, action);
      }
    }

  private:
    std::jthread thread_;
  };
}

#undef FWD
