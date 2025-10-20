#pragma once

#include <threadable/function.hxx>
#include <threadable/ring_buffer.hxx>

#include <atomic>
#include <concepts>
#include <cstdint>
#include <type_traits>

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

  /// @concept `invocable_opt`
  /// @brief Concept for types that are invocable (callable like a function) and convertible to bool
  /// (for optional-like success/failure checks).
  ///
  /// This concept combines std::invocable with implicit bool conversion, making it suitable for
  /// task handles or optional functors that can be executed and queried for validity (e.g., a
  /// stolen task that is null/opt-in on failure).
  ///
  /// @tparam T The type to check.
  /// @requires
  /// - T must be invocable with no arguments (`std::invocable<T>`), i.e., `operator()` exists and
  ///     can be called.
  /// - T must be convertible to bool (e.g., for truthy/falsy checks like
  ///     `if (t) { t(); })`.
  ///
  /// @see `std::invocable`, `std::convertible_to`
  ///
  /// @example
  /// ```cpp
  /// struct function {
  ///   bool operator()() { /* execute */ }
  ///   operator bool() const { return valid; }
  /// };
  /// static_assert(invocable_opt<function>);
  /// ```
  template<typename T>
  concept invocable_opt = std::convertible_to<T, bool> && std::invocable<T>;

  /// @concept `cas_deque`
  /// @brief Concept for double-ended queues (deques) that support Compare-And-Swap (CAS) operations
  /// and lock-free guarantees.
  ///
  /// This models a concurrent deque for work-stealing patterns, requiring thread-safe `try_pop`
  /// from both ends (front for consumers, back for owners) and a static lock-free property.
  ///
  /// @tparam T The type to check (the deque class).
  /// @requires
  /// - `try_pop_front()`: Must return an invocable (e.g., lambda or function that pops and returns
  /// optional-like result).
  /// - `try_pop_back()`: Similar to front, for owner-side pops.
  /// - `T::is_always_lock_free`: Must be comparable to bool, indicating if atomics in the impl are
  /// lock-free (`std::atomic`-like flag).
  ///
  /// @see `std::invocable` (for pop return types)
  ///
  /// @example
  /// ```cpp
  /// struct mpmc_deque {
  ///   static constexpr bool is_always_lock_free = true;
  ///   auto try_pop_front() { /* pop logic */ return opt_value; }
  ///   auto try_pop_back() { /* pop logic */ return opt_value; }
  /// };
  /// static_assert(cas_deque<mpmc_deque>);
  /// ```
  template<typename T>
  concept cas_deque = requires (T t) {
                        { t.try_pop_front() } -> std::invocable;
                        { t.try_pop_back() } -> std::invocable;
                        { T::is_always_lock_free } -> std::equality_comparable_with<bool>;
                      };

  /// @concept `master_queue`
  /// @brief Concept for a master (global/shared) queue in work-stealing schedulers,
  ///        supporting theft into a target receivable and emptiness checks.
  ///
  /// This concept models a central, shared queue that holds ownerless or overflow tasks
  /// in a multi-worker scheduler. Thieves can steal items directly into a local
  /// structure (e.g., a worker's deque) when local steals fail. The design ensures
  /// low-contention access and quick idleness detection to prevent under-subscription
  /// (e.g., idle workers despite pending tasks).
  ///
  /// @tparam T The master queue type itself.
  /// @tparam R The target receivable type (e.g., a reference to a local deque or buffer
  ///           where stolen items are transferred). Must be a reference-like type;
  ///           `std::remove_reference_t<R>` is used to normalize for requires-expression.
  ///
  /// @requires
  /// - `t.steal(r, bool masterOnly)`: An invocable method that transfers items
  ///   from the master queue into the target `r`, excluding the first stolen item which
  ///   should be returned directly (this is the `cached` item).
  ///   The `bool masterOnly` parameter specifies the steal mode: when `true`, steals only from the
  ///   master queue (ignoring other executors or victims); when `false`, may extend to broader
  ///   theft (e.g., from peers). Returns an invocable (e.g., a lambda or closure)
  ///   that performs or finalizes the steal operation when called. This allows
  ///   deferred execution and integration with concepts like `invocable_opt`.
  /// - `t.empty()`: A const method returning a value convertible to `bool`,
  ///   indicating whether the queue has no items available for steal. Used for
  ///   gating sleep transitions in worker loops (e.g., before committing to wait).
  ///
  /// @see cas_deque (for local worker queues that can serve as theft targets),
  ///      invocable_opt (for handling steal results).
  ///
  /// @example
  /// ```cpp
  /// // Compliant master queue implementation
  /// struct global_master {
  ///   // Steal into target (e.g., a local deque), masterOnly mode ignores victims
  ///   auto steal(auto& target, bool masterOnly) {
  ///     // Logic: if (masterOnly) { steal from self; } else { try victims; }
  ///     return cached; };
  ///   }
  ///
  ///   bool empty() const { return items_.load() == 0; }
  /// };
  ///
  /// static_assert(master_queue<global_master, worker_deque&>);
  /// ```
  template<typename T, typename R>
  concept master_queue = requires (T t, std::remove_reference_t<R>& r) {
                           { t.steal(r, true) } -> std::invocable;
                           { t.empty() } -> std::convertible_to<bool>;
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
      bell.wait(epoch, std::memory_order_acquire);
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
               master_queue<decltype(self)> auto& master) noexcept -> bool
  {
    exec.failed_steals = 0;
    exec.yields        = 0;
    for (;;) [[likely]]
    {
      // steal from random victim, or master if victim == self.
      if (cached = master.steal(self, false); cached) [[likely]]
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
                cas_deque auto& self, master_queue<decltype(self)> auto& master) noexcept -> bool
  {
    activity.thieves.fetch_add(1, std::memory_order_acq_rel);
    for (;;) [[likely]]
    {
      assert(!stolen);
      // Alg. 4
      if (explore_task(stolen, exec, self, master); stolen)
      { // t != NIL
        if (activity.thieves.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
          activity.notifier.notify_one();
        }
        return true;
      }
      auto epoch = activity.notifier.prepare();
      // @NOTE: Exact match ('empty(true)') because tasks stolen from master
      //        queue are specifically 'locked|ready' until processed by
      //        owning executor.
      if (!master.empty())
      {
        if (stolen = master.steal(self, true); stolen) //< masterOnly = true
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
              master_queue<decltype(self)> auto& master)
  {
    using task_t = decltype(master.steal(self, false));
    auto stolen  = task_t{nullptr}; // default-constructible empty task
    for (;;)
    {
      if (!wait_for_task(stolen, activity, exec, self, master))
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
