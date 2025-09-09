#pragma once

#include <threadable/ring_buffer.hxx>

#include <atomic>
#include <concepts>
#include <thread>

namespace fho::schedulers::stealing
{
  template<typename T>
  concept invocable_return = requires (T t) {
                               { std::invoke(t) } -> std::invocable;
                               { !std::invoke(t) } -> std::same_as<bool>;
                             };

  // Global shared atomics (accessed by all workers).
  struct activity_stats
  {
    std::atomic_bool   ready{false}; // Workers should stop.
    std::atomic_bool   abort{false}; // Workers should stop.
    std::atomic_size_t actives{0};   // Workers currently exploiting/executing tasks.
    std::atomic_size_t thieves{0};   // Workers currently in stealing mode.
  };

  // Per-worker non-atomic counters (thread-local, reset per idle cycle).
  struct exec_stats
  {
    std::size_t steal_bound   = 1024; // Max consecutive failed steals
    std::size_t yield_bound   = 16;   // Max consecutive yields
    std::size_t yields        = 0;    // Count of consecutive yield attempts.
    std::size_t failed_steals = 0;    // Count of consecutive failed steal attempts.
  };

  enum class action
  {
    abort,
    retry,
    yield,
    suspend
  };

  // Attempts to exploit a task from own deque (FIFO pop).
  // Returns true if a task was exploited and executed, false if deque empty.
  auto
  exploit_task(activity_stats& activity, exec_stats& exec, ring_buffer<>& self) noexcept -> bool
  {
    // Increment actives to signal exploiting (acquire-release for visibility).
    auto active = activity.actives.fetch_add(1, std::memory_order_acq_rel);

    // If we are first, wake up next.
    if (active == 1 && activity.thieves.load(std::memory_order_acquire) == 0) [[unlikely]]
    {
      activity.ready.exchange(true, std::memory_order_release);
      activity.ready.notify_one();
    }

    // Try LIFO pop from own deque (maximize locality).
    if (auto t = self.try_pop_back(); t)
    {
      std::invoke(t);

      // Decrement actives post-execution.
      if (auto active = activity.actives.fetch_sub(1, std::memory_order_acq_rel); active == 0)
      {
        activity.ready.exchange(false, std::memory_order_release);
      }

      // Reset per-worker counters on success.
      exec.yields        = 0;
      exec.failed_steals = 0;

      return true;
    }
    else
    {
      // Failed to exploit: Decrement actives immediately.
      activity.actives.fetch_sub(1, std::memory_order_acq_rel);
      return false;
    }
  }

  // Attempts to explore (steal) a task from a random victim (FIFO pop).
  // Returns true if a task was stolen and executed, false if steal failed.
  auto
  explore_task(activity_stats& activity, exec_stats& exec, invocable_return auto&& stealer) noexcept
    -> bool
  {
    // Increment thieves to signal stealing mode.
    activity.thieves.fetch_add(1, std::memory_order_acq_rel);

    // Try FIFO pop from victim's deque (minimize contention).
    if (auto t = stealer(); t)
    {
      // Decrement thieves post-execution.
      activity.thieves.fetch_sub(1, std::memory_order_acq_rel);

      // Stolen: Execute the task (as in exploit).
      std::invoke(t);

      // Reset counters on success.
      exec.yields        = 0;
      exec.failed_steals = 0;

      return true;
    }
    else
    {
      // Failed steal: Increment per-worker counter.
      ++exec.failed_steals;

      // Decrement thieves.
      activity.thieves.fetch_sub(1, std::memory_order_acq_rel);

      return false;
    }
  }

  // Handles adaptive waiting when no task found (after exploit/explore fails).
  // Uses yields and failed steals to decide yield vs. suspend (sleep) vs. retry.
  auto
  wait_for_task(activity_stats& activity, exec_stats& exec) noexcept -> action
  {
    if (activity.abort) [[unlikely]]
    {
      return action::abort;
    }
    // Check failed steals against bound.
    if (exec.failed_steals >= exec.steal_bound) [[unlikely]]
    {
      // Exceeded steal attempts: Reset steals, increment yields.
      exec.failed_steals = 0;
      ++exec.yields;
      return action::yield;
    }

    // Check yields against bound.
    if (exec.yields >= exec.yield_bound) [[unlikely]]
    {
      // Exceeded yields: Reset yields, enter sleep/wait.
      exec.yields = 0;

      // Check global actives: if >0, potential work exists.
      if (activity.actives.load(std::memory_order_acquire) > 0)
      {
        // Brief yield instead of full sleep (to check soon).
        return action::yield;
      }
      else
      {
        // No actives: Safe to sleep.
        return action::suspend;
      }
    }
    else [[likely]]
    {
      // Within bounds: Try again.
      return action::retry;
    }
  }

  void
  executor_loop(activity_stats& activity, auto& self, invocable_return auto&& stealer) noexcept
  {
    exec_stats exec; // Per-worker stats; thread-local, non-atomic, reset on task success in
                     // exploit/explore.
    while (true)     // Infinite loop until abort or external stop; assumes caller handles shutdown
                     // (e.g., via activity flag).
    {
      // First, attempt to exploit (pop and execute) a single task from own ring_buffer (FIFO for
      // owner). This should succeed if local tasks exist (e.g., pushed successors in DAGs),
      // resetting counters on success. Note: Single attempt only — do not drain internally to allow
      // timely stealing (per paper's design for responsiveness).
      if (auto ok = exploit_task(activity, exec, self); !ok)
      { // If exploit fails (empty queue), proceed to explore.

        // Attempt to steal (explore) a single task from a random victim's ring_buffer (LIFO for
        // thief). Resets counters on success; increments failed_steals on failure.
        if (ok = explore_task(activity, exec, stealer);
            ok) // If steal succeeds, continue loop immediately (no wait).
        {
          // Empty block: No-op if success — loop back to exploit next (allows draining after steal
          // if successors pushed).
          continue;
        }
      }

      auto action = wait_for_task(activity, exec); // Decide adaptive action based on
                                                   // counters/actives (retry/yield/suspend).
      switch (action)
      {
        // If abort returned, ensure proper cleanup (e.g., decrement actives/thieves if
        // mid-operation).
        case action::abort:
          return;
        //
        // Within bounds: No yield/sleep — immediately retry exploit/explore for reactivity (no CPU
        // waste).
        case action::retry:
          continue;
        //
        // After steal_bound fails or yield_bound with actives > 0: Brief yield to reduce contention
        // before retry.
        case action::yield:
          std::this_thread::yield();
          break;
        //
        // Yield_bound exceeded and actives == 0: Block until potential work (e.g., new
        // submissions). Wait for tasks to be available.
        case action::suspend:
          activity.ready.wait(false, std::memory_order_acquire);
          break;
      }
      // Post-action: Loop back to exploit — ensures draining if work appeared during yield/suspend.
      // Memory detail: If suspend wakes spuriously, counters unchanged; safe retry without reset
      // (per paper tolerance).
    }
  }
}
