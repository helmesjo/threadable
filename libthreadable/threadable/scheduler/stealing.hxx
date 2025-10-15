#pragma once

#include <threadable/function.hxx>
#include <threadable/ring_buffer.hxx>

#include <atomic>
#include <concepts>
#include <cstdint>

#if 0                    // defined(__x86_64__) || defined(_M_X64)
  #include <immintrin.h> // _mm_pause
#else
  #include <thread>
#endif

#if defined(_MSC_VER) && !defined(__clang__)
  #pragma warning(push)
  #pragma warning(disable : 4324)
#endif

namespace fho::scheduler::stealing
{

  namespace detail
  {
    inline void
    cpu_relax() noexcept
    {
      // @NOTE: Disable '_mm_pause()' for now, maybe it's
      //        too agressive.
#if 0                            // defined(__x86_64__) || defined(_M_X64)
      _mm_pause();
#else
      std::this_thread::yield(); // fallback
#endif
    }
  }

  template<typename T>
  concept invocable_return = requires (T t) {
                               { std::invoke(t, ring_buffer<fast_func_t>{}) };
                             };

  template<typename T>
  concept invocable_opt = std::convertible_to<T, bool> && std::invocable<T>;

  template<typename T>
  concept cas_deque = requires (T t) {
                        { t.try_pop_front() } -> std::invocable;
                        { t.try_pop_back() } -> std::invocable;
                        { T::is_always_lock_free } -> std::equality_comparable_with<bool>;
                      };

  struct alignas(details::cache_line_size) event_count
  {
    std::atomic<std::uint64_t> bell{0};

    inline auto
    prepare() const noexcept -> std::uint64_t
    {
      return bell.load(std::memory_order_acquire);
    }

    inline void
    commit_wait(std::uint64_t epoch) const noexcept
    {
      bell.wait(epoch, std::memory_order_relaxed);
    }

    inline void
    notify_one() noexcept
    {
      bell.fetch_add(1, std::memory_order_release);
      bell.notify_one();
    }

    inline void
    notify_all() noexcept
    {
      bell.fetch_add(1, std::memory_order_release);
      bell.notify_all();
    }
  };

  // Global shared atomics (accessed by all workers).
  struct activity_stats
  {
    alignas(details::cache_line_size) event_count notifier;
    alignas(details::cache_line_size) ring_buffer<fast_func_t> master;
    alignas(details::cache_line_size) std::atomic<std::size_t> actives{0};
    alignas(details::cache_line_size) std::atomic<std::size_t> thieves{0};
    alignas(details::cache_line_size) std::atomic<bool> stops{false};
  };

  // Per-worker non-atomic stats (thread-local).
  struct exec_stats
  {
    std::size_t steal_bound   = 2; // @NOTE: Set externally to 2 * (W + 1)
    std::size_t yield_bound   = 100;
    std::size_t failed_steals = 0;
    std::size_t yields        = 0;
    bool        abort         = false;
  };

  /// @brief Mirrors Algorithm 3. Preconditions: may enter with t != NIL when a steal succeeded.
  /// @details
  /// 1) If t == NIL: return immediately (nothing to exploit).
  /// 2) Atomically increment num_actives.
  ///    2.1) If (previous value was 0) AND (num_thieves == 0): notifier.notify_one().
  /// 3) do {
  ///      3.1) execute(t).
  ///      3.2) If w.cache != NIL: t <- w.cache; else t <- pop(w.queue).
  ///    } while (t != NIL);
  /// 4) Atomically decrement num_actives; return.
  inline void
  exploit_task(invocable_opt auto& stolen, activity_stats& activity, cas_deque auto& self)
  {
    if (stolen)
    { // t ≠ NIL; assume !t falsy for invalid/empty
      if (activity.actives.fetch_add(1, std::memory_order_acq_rel) == 0 &&
          activity.thieves.load(std::memory_order_acquire) == 0)
      {
        activity.notifier.notify_one();
      }
      std::invoke(stolen);
      stolen = nullptr;
      while (auto t = self.try_pop_back())
      {
        std::invoke(t);
      }
      activity.actives.fetch_sub(1, std::memory_order_acq_rel);
    }
  }

  /// @brief Mirrors Algorithm 4. Populates 't' by random stealing or gives up after bounded
  /// backoff.
  /// @details
  /// 1) num_failed_steals <- 0; num_yields <- 0.
  /// 2) while scheduler_not_stops:
  ///      2.1) victim <- random();
  ///      2.2) if victim == self:
  ///             t <- steal(master_queue);
  ///           else:
  ///             t <- steal_from(victim);
  ///      2.3) if t != NIL: break;
  ///      2.4) num_failed_steals++;
  ///           if num_failed_steals >= STEAL_BOUND:
  ///              yield();
  ///              num_yields++;
  ///              if num_yields == YIELD_BOUND: break;
  inline auto
  explore_task(invocable_opt auto& cached, exec_stats& exec, cas_deque auto& self,
               invocable_return auto&& stealer) noexcept -> bool
  {
    exec.failed_steals = 0;
    exec.yields        = 0;
    for (;;) [[likely]]
    {
      // steal from random victim, or master if victim == self.
      if (cached = stealer(self); cached) [[likely]] // t ≠ NIL; assume truthy for valid task
      {
        break;
      }
      else [[unlikely]]
      {
        ++exec.failed_steals;
        if (exec.failed_steals >= exec.steal_bound)
        {
          detail::cpu_relax();
          ++exec.yields;
          if (exec.yields == exec.yield_bound)
          {
            break;
          }
        }
      }
    }
    return cached;
  }

  /// @brief Mirrors Algorithm 5. Ensures Lemma 1: with any active and any inactive worker, a thief
  /// exists.
  /// @details
  /// 1) Atomically increment num_thieves.
  /// 2) explore_task(t, w).
  ///    2.1) If t != NIL:
  ///          2.1.1) If AtomDec(num_thieves) == 0: notifier.notify_one();
  ///          2.1.2) return true.   // go to exploit phase
  /// 3) u <- notifier.prepare_wait();   // EventCount prepare
  /// 4) If master_queue is not empty:
  ///      4.1) notifier.cancel_wait(u);
  ///      4.2) t <- steal(master_queue);
  ///      4.3) if t != NIL:
  ///             4.3.1) If AtomDec(num_thieves) == 0: notifier.notify_one();
  ///             4.3.2) return true.
  ///           else:
  ///             4.3.3) goto step 2)  // re-run explore_task
  /// 5) If scheduler_stops:
  ///      5.1) notifier.cancel_wait(u);
  ///      5.2) notifier.notify_all();
  ///      5.3) AtomDec(num_thieves);
  ///      5.4) return false.  // exit worker loop
  /// 6) If AtomDec(num_thieves) == 0 AND num_actives > 0:
  ///      6.1) notifier.cancel_wait(u);
  ///      6.2) goto step 1)  // reiterate wait_for_task to keep one thief alive
  /// 7) notifier.commit_wait(u);   // sleep until notified
  /// 8) return true.  // woken up; loop will retry
  inline auto
  wait_for_task(invocable_opt auto& stolen, activity_stats& activity, exec_stats& exec,
                cas_deque auto& self, invocable_return auto&& stealer) noexcept -> bool
  {
    activity.thieves.fetch_add(1, std::memory_order_acq_rel);
    for (;;) [[likely]]
    {
      assert(!stolen);
      // Alg. 4
      if (explore_task(stolen, exec, self, stealer); stolen)
      { // t != NIL
        if (activity.thieves.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
          activity.notifier.notify_one();
        }
        return true;
      }
      auto epoch = activity.notifier.prepare();
      if (!activity.master.empty())
      {
        if (stolen = activity.master.try_pop_front();
            stolen) // t ≠ NIL; assume truthy for valid task
        {
          if (activity.thieves.fetch_sub(1, std::memory_order_acq_rel) == 1)
          {
            activity.notifier.notify_one();
          }
          return true;
        }
        continue;
      }
      // still failed; proceed to dec check
      if (activity.stops.load(std::memory_order_acquire)) [[unlikely]]
      {
        activity.notifier.notify_all();
        activity.thieves.fetch_sub(1, std::memory_order_acq_rel);
        return false;
      }
      // Tentative dec for scarcity check
      if (activity.thieves.fetch_sub(1, std::memory_order_acq_rel) == 1 &&
          activity.actives.load(std::memory_order_acquire) > 0)
      {
        // Undo dec: persist as last thief (thieves back to 1)
        activity.thieves.fetch_add(1, std::memory_order_acq_rel);
        // Discard epoch (no commit); re-loop to explore
        continue;
      }
      // Permanent dec: sleep (thieves adjusted correctly)
      activity.notifier.commit_wait(epoch);
      return true;
    }
  }

  inline void
  worker_loop(activity_stats& activity, exec_stats& exec, cas_deque auto& self,
              invocable_return auto&& stealer)
  {
    using task_t = decltype(activity.master.try_pop_front());
    auto stolen  = task_t{nullptr}; // default-constructible empty task
    while (!activity.stops.load(std::memory_order_acquire))
    {
      if (!wait_for_task(stolen, activity, exec, self, stealer))
      { // Alg. 5: Steal or sleep; returns false on stop
        break;
      }
      // Alg. 3: Execute local tasks, update actives
      exploit_task(stolen, activity, self);
    }
  }
}

#if defined(_MSC_VER) && !defined(__clang__)
  #pragma warning(pop)
#endif
