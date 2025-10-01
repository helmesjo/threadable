#pragma once

#include <threadable/function.hxx>
#include <threadable/ring_buffer.hxx>

#include <atomic>
#include <concepts>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64)
  #include <immintrin.h> // _mm_pause
#endif

namespace fho::schedulers::stealing
{
  namespace detail
  {
    inline void
    cpu_relax() noexcept
    {
#if defined(__x86_64__) || defined(_M_X64)
      _mm_pause();
#else
      std::this_thread::yield(); // fallback
#endif
    }
  }

  template<typename T>
  concept invocable_return = requires (T t) {
                               {
                                 std::invoke(t, ring_buffer<fast_func_t>{})
                               } -> std::same_as<std::size_t>;
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
    // Tasks ready to process.
    alignas(details::cache_line_size) std::atomic_size_t ready{0};
    // Bell counter to wake up workers.
    alignas(details::cache_line_size) std::atomic_uint64_t bell{0};
    // Workers should stop.
    alignas(details::cache_line_size) std::atomic_bool abort{false};
    // Workers currently exploiting/executing tasks.
    alignas(details::cache_line_size) std::atomic_size_t actives{0};
    // Workers currently in stealing mode.
    alignas(details::cache_line_size) std::atomic_size_t thieves{0};
  };

  // Per-worker non-atomic counters (thread-local, reset per idle cycle).
  struct exec_stats
  {
    std::size_t steal_bound   = 16; // Max consecutive failed steals
    std::size_t yield_bound   = 16; // Max consecutive yields
    std::size_t failed_steals = 0;  // Count of consecutive failed steal attempts.
    std::size_t yields        = 0;  // Count of consecutive yield attempts.
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
                        // suspends (e.g., activity.ready.wait(0, std::memory_order_acquire)),
                        // resets yields to 0, then proceeds to next state (exploit).
  };

  inline auto
  process_state(activity_stats const& activity, exec_stats const& exec, action prev) -> action
  {
    switch (prev)
    {
      case action::abort:
        return action::abort;
      case action::exploit:
      { // Maps to paper's worker loop (Figure 2): after failed owner_pop (exploit fail),
        // immediately attempt steal_random (explore). Correct because caller only invokes
        // process_state on exploit failure; success cases are handled by caller retrying exploit
        // directly, preserving single-attempt responsiveness without state machine involvement.
        return action::explore;
      }
      case action::explore:
      { // Maps to paper's wait_for_task entry (Algorithm 1, after inc failed_steals_ which caller
        // performs before this call). Correct because exec.failed_steals reflects post-increment
        // value; function decides first if steal_bound exceeded (trigger special yield with
        // updates), then always evaluates yield_bound (sequential ifs), or falls to default yield
        // (else branch).

        if (exec.yields >= exec.yield_bound)
        {
          return action::suspend; // CHANGED: park after enough yields regardless of actives
        }
        else if (exec.failed_steals >= exec.steal_bound)
        {
          return action::yield_steal_exceed;
        }
        return action::yield;
      }
      case action::retry:
      {
        // Retry immediately
        return action::retry;
      }
      case action::yield_steal_exceed:
      { // Maps to paper's state after first if block (yield executed, failed reset, yields
        // incremented by caller): now evaluate second if with updated yields. Correct because
        // re-call with this previous allows seeing post-increment yields without function
        // modification, ensuring sequential logic fidelity.
        return (exec.yields >= exec.yield_bound) &&
                   (activity.ready.load(std::memory_order_acquire) == 0) ?
                 action::suspend :
                 action::exploit;
      }
      case action::yield:
        [[fallthrough]];
      case action::yield_reset_yields:
        [[fallthrough]];
      case action::suspend:
      { // Maps to paper's loop continuation after wait_for_task: always return to owner_pop
        // (exploit) post-yield/sleep. Correct because all wait paths (simple or with resets) lead
        // back to exploit for checking local deque first, exploiting locality without memory
        // barriers needed here (caller ensures visibility via release on pushes/notifies).
        return action::exploit;
      }
      default:
        // Unhandled previous: assume abort or error handling in caller; not in paper.
        return action::abort;
    }
  }

  inline void
  process_action(action act, activity_stats& activity, exec_stats& exec, cas_deque auto& self,
                 invocable_return auto&& stealer)
  {
    switch (act)
    {
      case action::exploit:
      { /// @brief Exhaust own queue.
        /// @details This action is reached after a successful steal or post-idleness wake-up. It
        /// increments num_actives atomically, notifies one if first active with no thieves, drains
        /// all tasks from deque via repeated LIFO pops and executions until empty, resets counters
        /// if any executed, and decrements num_actives. Drains empty queue without execution or
        /// resets. Drains non-empty queue, executes all tasks, resets counters.
        auto executed = false;
        while (auto t = self.try_pop_back())
        {
          if (!executed) [[likely]]
          {
            executed = true;
            if (activity.actives.fetch_add(1, std::memory_order_acq_rel) == 0 &&
                activity.thieves.load(std::memory_order_acquire) == 0) [[unlikely]]
            {
              activity.bell.fetch_add(1, std::memory_order_release);
              activity.bell.notify_one();
            }
          }

          auto derp = activity.ready.fetch_sub(1, std::memory_order_release);
          assert(derp != 0 and "stealer::process_action(exploit)");
          (*t)();
        }
        if (executed)
        {
          activity.actives.fetch_sub(1, std::memory_order_acq_rel);
          exec.yields        = 0;
          exec.failed_steals = 0;
        }
      }
      break;
      case action::explore:
      { /// @brief Perform bounded steals.
        /// @details This action is reached after exploit fails. It resets failed_steals and yields,
        /// increments thieves, loops up to steal_bound attempting steals from random victims
        /// (master if self), moves stolen task to own queue and resets on success breaking early,
        /// increments failed_steals on fail, decrements thieves and notifies one if zero, yields
        /// and increments yields if bound exceeded. Fails all steals, yields post-bound, increments
        /// yields. Succeeds a steal, moves task to own queue (self={}, victim={?} becomes
        /// self={stolen}, victim={}), resets counters, notifies if thieves zero.
        activity.thieves.fetch_add(1, std::memory_order_acq_rel);
        for (; exec.failed_steals < exec.steal_bound; ++exec.failed_steals)
        {
          if (stealer(self))
          {
            exec.yields        = 0;
            exec.failed_steals = 0;
            break;
          }
          detail::cpu_relax(); // CHANGED: tiny intra-loop backoff to reduce interconnect thrash
        }
        bool const last = (activity.thieves.fetch_sub(1, std::memory_order_acq_rel) == 1);
        if (last &&
            (exec.failed_steals == 0 || activity.actives.load(std::memory_order_acquire) > 0 ||
             activity.ready.load(std::memory_order_acquire) > 0)) [[unlikely]]
        {
          activity.bell.fetch_add(1, std::memory_order_release); // CHANGED: also wake when last
                                                                 // thief failed but work exists
          activity.bell.notify_one();
        }

        break;
      }
      case action::abort:
      { /// @brief Abort scheduler.
        /// @details This action is reached on external stop signal. It returns immediately without
        /// changes. Returns without affecting states or queues.
        activity.bell.fetch_add(1, std::memory_order_release);
        activity.bell.notify_all();
        return;
      }
      break;
      case action::retry:
      { /// @brief Retry immediately.
        /// @details This action is reached within bounds post-fail. It does nothing, signaling
        /// caller to retry exploit/explore. No changes occur.
      }
      break;
      case action::yield:
      { /// @brief Yield briefly.
        /// @details This action is reached on non-exceeded bounds in idleness. It yields without
        /// updates. Yields, no state changes.
        // CHANGED: short timed wait on bell instead of raw yield (reduces CPU pegging)
        // auto tk = activity.bell.load(std::memory_order_acquire);
        // activity.bell.wait(tk, std::memory_order_acquire);
        detail::cpu_relax();
        ++exec.yields;
      }
      break;
      case action::yield_steal_exceed:
      { /// @brief Yield on steal exceed.
        /// @details This action is reached after steal_bound fails in explore. It yields, resets
        /// failed_steals, increments yields. Yields, updates counters, no global changes.
        std::this_thread::yield();
        exec.failed_steals = 0;
        ++exec.yields;
      }
      break;
      case action::yield_reset_yields:
      { /// @brief Yield on yield exceed with actives.
        /// @details This action is reached after yield_bound exceeded with actives >0. It yields,
        /// resets yields. Yields, resets yields, no other changes.
        std::this_thread::yield();
        exec.yields = 0;
      }
      break;
      case action::suspend:
      { /// @brief Suspend on yield exceed without actives.
        /// @details This action is reached after yield_bound exceeded with actives == 0. It checks
        /// abort and notifies all if true, else waits with re-check loop until ready, resets
        /// counters post-wake. Aborts if signalled, notifies all. Waits, resets counters post-wake.
        if (activity.abort.load(std::memory_order_acquire))
        {
          activity.bell.fetch_add(1, std::memory_order_release); // CHANGED: bump bell on abort
          activity.bell.notify_all();                            // CHANGED: wake all waiters
          break;
        }
        auto tk = activity.bell.load(std::memory_order_acquire);
        if (activity.ready.load(std::memory_order_acquire) == 0)
        {
          activity.bell.wait(tk, std::memory_order_acquire);
          tk = activity.bell.load(std::memory_order_acquire);
        }

        exec.yields        = 0;
        exec.failed_steals = 0;
      }
      break;
    }
  }
}
