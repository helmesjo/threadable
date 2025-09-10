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

  template<typename T>
  concept cas_deque = requires (T t) {
                        { t.try_pop_front() } -> std::invocable;
                        { t.try_pop_back() } -> std::invocable;
                        { T::is_always_lock_free } -> std::equality_comparable_with<bool>;
                      };

  // Global shared atomics (accessed by all workers).
  struct activity_stats
  {
    std::atomic_bool   ready{false}; // Workers should process tasks.
    std::atomic_bool   abort{false}; // Workers should stop.
    std::atomic_size_t actives{0};   // Workers currently exploiting/executing tasks.
    std::atomic_size_t thieves{0};   // Workers currently in stealing mode.
  };

  // Per-worker non-atomic counters (thread-local, reset per idle cycle).
  struct exec_stats
  {
    std::size_t steal_bound    = 1024; // Max consecutive failed steals
    std::size_t yield_bound    = 16;   // Max consecutive yields
    std::size_t yields         = 0;    // Count of consecutive yield attempts.
    std::size_t failed_exploit = 0;    // Count of consecutive yield attempts.
    std::size_t failed_steals  = 0;    // Count of consecutive failed steal attempts.
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
  exploit_task(ring_buffer<>& self) noexcept -> bool
  {
    // Try LIFO pop from own deque (maximize locality).
    if (auto t = self.try_pop_back(); t)
    {
      std::invoke(t);
      return true;
    }
    else
    {
      // Failed to exploit: Decrement actives immediately.
      // activity.actives.fetch_sub(1, std::memory_order_acq_rel);
      return false;
    }
  }

  // Attempts to explore (steal) a task from a random victim (FIFO pop).
  // Returns true if a task was stolen and executed, false if steal failed.
  auto
  explore_task(invocable_return auto&& stealer) noexcept -> bool
  {
    // Try FIFO pop from victim's deque (minimize contention).
    if (auto t = stealer(); t)
    {
      // Stolen: Execute the task (as in exploit).
      std::invoke(t);

      return true;
    }
    else
    {
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
    activity.actives.fetch_add(1, std::memory_order_release);
    while (true)     // Infinite loop until abort or external stop; assumes caller handles shutdown
                     // (e.g., via activity flag).
    {
      // First, attempt to exploit (pop and execute) a single task from own ring_buffer (FIFO for
      // owner). This should succeed if local tasks exist (e.g., pushed successors in DAGs),
      // resetting counters on success. Note: Single attempt only — do not drain internally to allow
      // timely stealing (per paper's design for responsiveness).
      if (exploit_task(self))
      {
        // Reset per-worker counters on success.
        exec.yields         = 0;
        exec.failed_exploit = 0;
        exec.failed_steals  = 0;
        continue;
      }

      // If exploit fails (empty queue), proceed to explore.
      if (++exec.failed_exploit == 1)
      {
        // Increment thieves to signal stealing mode.
        activity.thieves.fetch_add(1, std::memory_order_acq_rel);
      }
      // Attempt to steal (explore) a single task from a random victim's ring_buffer (LIFO for
      // thief). Resets counters on success; increments failed_steals on failure.
      if (explore_task(stealer))
      // If steal succeeds, continue loop immediately (no wait).
      {
        // Reset counters on success.
        exec.yields        = 0;
        exec.failed_steals = 0;

        // Empty block: No-op if success — loop back to exploit next (allows draining after steal
        // if successors pushed).
        continue;
      }
      else
      {
        // Failed steal: Increment per-worker counter.
        ++exec.failed_steals;
      }

      // If explore fails (nothing to steal), evaluate next action.
      ++exec.failed_exploit;

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
          ++exec.yields;
          std::this_thread::yield();
          break;
        //
        // Yield_bound exceeded and actives == 0: Block until potential work (e.g., new
        // submissions). Wait for tasks to be available.
        case action::suspend:
          // Decrement actives post-execution.
          if (auto active = activity.actives.fetch_sub(1, std::memory_order_acq_rel); active == 1)
          {
            auto exp = true;
            activity.ready.compare_exchange_strong(exp, false, std::memory_order_release);
          }

          activity.ready.wait(false, std::memory_order_acquire);
          exec = {}; // Clear workers state, start from clean slate.
          // Increment actives to signal exploiting.
          auto active = activity.actives.fetch_add(1, std::memory_order_acq_rel);
          // If we are first, wake up next.
          if (active == 1 && activity.thieves.load(std::memory_order_acquire) == 0) [[unlikely]]
          {
            activity.ready.exchange(true, std::memory_order_release);
            activity.ready.notify_one();
          }
          break;
      }
      // Post-action: Loop back to exploit — ensures draining if work appeared during yield/suspend.
      // Memory detail: If suspend wakes spuriously, counters unchanged; safe retry without reset
      // (per paper tolerance).
    }
  }

  namespace v2
  {
    // Global shared atomics (accessed by all workers).
    struct activity_stats
    {
      std::atomic_bool   ready{false}; // Workers should process tasks.
      std::atomic_bool   abort{false}; // Workers should stop.
      std::atomic_size_t actives{0};   // Workers currently exploiting/executing tasks.
      std::atomic_size_t thieves{0};   // Workers currently in stealing mode.
    };

    // Per-worker non-atomic counters (thread-local, reset per idle cycle).
    struct exec_stats
    {
      std::size_t steal_bound   = 1024; // Max consecutive failed steals
      std::size_t yield_bound   = 16;   // Max consecutive yields
      std::size_t failed_steals = 0;    // Count of consecutive failed steal attempts.
      std::size_t yields        = 0;    // Count of consecutive yield attempts.
    };
    enum class action
    {
      exploit,
      explore,
      abort,
      retry,
      yield, // Simple yield with no counter updates; caller yields then proceeds to next state
             // (exploit).
      yield_steal_exceed, // Yield due to exceeding steal_bound; caller yields, resets failed_steals
                          // to 0, increments yields, then calls process_state again with this as
                          // previous to continue the sequential logic.
      yield_reset_yields, // Yield due to exceeding yield_bound with actives > 0; caller yields,
                          // resets yields to 0, then proceeds to next state (exploit).
      suspend             // Suspend due to exceeding yield_bound with actives == 0; caller
                          // suspends (e.g., activity.ready.wait(false,
                          // std::memory_order_acquire)), resets yields to 0, then proceeds to next
                          // state (exploit).
    };

    auto
    process_state(activity_stats const& activity, exec_stats const& exec, action previous) -> action
    {
      if (previous == action::abort)
      {
        return action::abort;
      }

      if (previous == action::exploit)
      {
        // Maps to paper's worker loop (Figure 2): after failed owner_pop (exploit fail),
        // immediately attempt steal_random (explore). Correct because caller only invokes
        // process_state on exploit failure; success cases are handled by caller retrying exploit
        // directly, preserving single-attempt responsiveness without state machine involvement.
        return action::explore;
      }

      if (previous == action::explore)
      {
        // Maps to paper's wait_for_task entry (Algorithm 1, after inc failed_steals_ which caller
        // performs before this call). Correct because exec.failed_steals reflects post-increment
        // value; function decides first if steal_bound exceeded (trigger special yield with
        // updates), then always evaluates yield_bound (sequential ifs), or falls to default yield
        // (else branch).
        if (exec.failed_steals > exec.steal_bound)
        {
          // Maps to paper's first if (failed_steals_ > STEAL_BOUND): yield, but defers updates to
          // caller; returns special action to trigger continuation for second if. Correct as it
          // isolates the first yield and updates, allowing caller to update exec before re-calling
          // for the remaining logic, replicating the sequential execution without modifying state
          // here.
          return action::yield_steal_exceed;
        }
        // Sequential: always check second if, even if steal_bound not exceeded.
        if (exec.yields > exec.yield_bound)
        {
          // Maps to paper's second if (yields_ > YIELD_BOUND): reset yields (deferred to caller),
          // then conditional yield or wait. Correct as actives read with acquire ensures visibility
          // of concurrent executions; no modification here, caller handles reset post-action.
          if (activity.actives.load(std::memory_order_acquire) > 0)
          {
            return action::yield_reset_yields;
          }
          else
          {
            return action::suspend;
          }
        }
        // Maps to paper's else branch: simple yield when neither bound exceeded (or after checks).
        // Correct as this catches low-contention idleness without updates, maintaining minimal
        // overhead per C++20 yield semantics.
        return action::yield;
      }

      if (previous == action::yield_steal_exceed)
      {
        // Maps to paper's state after first if block (yield executed, failed reset, yields
        // incremented by caller): now evaluate second if with updated yields. Correct because
        // re-call with this previous allows seeing post-increment yields without function
        // modification, ensuring sequential logic fidelity.
        if (exec.yields > exec.yield_bound)
        {
          // Same as above: maps to second if, with conditional based on actives.
          // Correct for handling the rare case where inc pushes yields over bound, triggering reset
          // and yield/suspend.
          if (activity.actives.load(std::memory_order_acquire) > 0)
          {
            return action::yield_reset_yields;
          }
          else
          {
            return action::suspend;
          }
        }
        // Maps to else after first if (when new yields <= bound): another yield.
        // Correct as this replicates the double-yield path for non-threshold cases, preserving
        // adaptive gradual backoff.
        return action::yield;
      }

      if (previous == action::yield || previous == action::yield_reset_yields ||
          previous == action::suspend)
      {
        // Maps to paper's loop continuation after wait_for_task: always return to owner_pop
        // (exploit) post-yield/sleep. Correct because all wait paths (simple or with resets) lead
        // back to exploit for checking local deque first, exploiting locality without memory
        // barriers needed here (caller ensures visibility via release on pushes/notifies).
        return action::exploit;
      }

      // Unhandled previous: assume abort or error handling in caller; not in paper.
      return action::abort;
    }

    void
    process_action(action a, activity_stats& activity, exec_stats& exec, cas_deque auto& self,
                   invocable_return auto&& stealer)
    {
      switch (a)
      {
        /// @brief Exhaust own queue.
        /// @details This action is reached after a successful steal or post-idleness wake-up. It
        /// increments num_actives atomically, notifies one if first active with no thieves, drains
        /// all tasks from deque via repeated LIFO pops and executions until empty, resets counters
        /// if any executed, and decrements num_actives. Drains empty queue without execution or
        /// resets. Drains non-empty queue, executes all tasks, resets counters.
        case action::exploit:
        {
          if (activity.actives.fetch_add(1, std::memory_order_acq_rel) == 0 &&
              activity.thieves.load(std::memory_order_acquire) == 0)
          {
            activity.ready.notify_one();
          }
          auto executed = false;
          while (auto t = self.try_pop_back())
          {
            (*t)();
            executed = true;
          }
          if (executed)
          {
            exec.yields        = 0;
            exec.failed_steals = 0;
          }
          activity.actives.fetch_sub(1, std::memory_order_acq_rel);
        }
        break;
        /// @brief Perform bounded steals.
        /// @details This action is reached after exploit fails. It resets failed_steals and yields,
        /// increments thieves, loops up to steal_bound attempting steals from random victims
        /// (master if self), moves stolen task to own queue and resets on success breaking early,
        /// increments failed_steals on fail, decrements thieves and notifies one if zero, yields
        /// and increments yields if bound exceeded. Fails all steals, yields post-bound, increments
        /// yields. Succeeds a steal, moves task to own queue (self={}, victim={?} becomes
        /// self={stolen}, victim={}), resets counters, notifies if thieves zero.
        case action::explore:
        {
          exec.failed_steals = 0;
          exec.yields        = 0;
          activity.thieves.fetch_add(1, std::memory_order_acq_rel);
          for (; exec.failed_steals < exec.steal_bound; ++exec.failed_steals)
          {
            if (auto t = stealer(); t)
            {
              (void)self.push_back(std::move(t));
              exec.yields        = 0;
              exec.failed_steals = 0;
              break;
            }
          }
          if (activity.thieves.fetch_sub(1, std::memory_order_acq_rel) == 1)
          {
            activity.ready.notify_one();
          }
          if (exec.failed_steals >= exec.steal_bound)
          {
            std::this_thread::yield();
            ++exec.yields;
          }
          break;
        }
        /// @brief Abort scheduler.
        /// @details This action is reached on external stop signal. It returns immediately without
        /// changes. Returns without affecting states or queues.
        case action::abort:
        {
          return;
        }
        break;
        /// @brief Retry immediately.
        /// @details This action is reached within bounds post-fail. It does nothing, signaling
        /// caller to retry exploit/explore. No changes occur.
        case action::retry:
        {
        }
        break;
        /// @brief Yield briefly.
        /// @details This action is reached on non-exceeded bounds in idleness. It yields without
        /// updates. Yields, no state changes.
        case action::yield:
        {
          std::this_thread::yield();
        }
        break;
        /// @brief Yield on steal exceed.
        /// @details This action is reached after steal_bound fails in explore. It yields, resets
        /// failed_steals, increments yields. Yields, updates counters, no global changes.
        case action::yield_steal_exceed:
        {
          std::this_thread::yield();
          exec.failed_steals = 0;
          ++exec.yields;
        }
        break;
        /// @brief Yield on yield exceed with actives.
        /// @details This action is reached after yield_bound exceeded with actives >0. It yields,
        /// resets yields. Yields, resets yields, no other changes.
        case action::yield_reset_yields:
        {
          std::this_thread::yield();
          exec.yields = 0;
        }
        break;
        /// @brief Suspend on yield exceed without actives.
        /// @details This action is reached after yield_bound exceeded with actives == 0. It checks
        /// abort and notifies all if true, else waits with re-check loop until ready, resets
        /// counters post-wake. Aborts if signalled, notifies all. Waits, resets counters post-wake.
        case action::suspend:
        {
          if (activity.abort.load(std::memory_order_acquire))
          {
            activity.ready.store(true, std::memory_order_release);
            activity.ready.notify_all();
            break;
          }
          activity.ready.wait(false, std::memory_order_acquire);
          exec.yields        = 0;
          exec.failed_steals = 0;
        }
        break;
      }
    }
  }
}
