#pragma once

#include <threadable/function.hxx>
#include <threadable/ring_buffer.hxx>

#include <atomic>
#include <concepts>
#include <cstdint>
#include <format>
#include <iostream>
#include <syncstream>
#include <thread>
#include <utility>

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

    // Generic wait on an atomic counter using C++20 wait/notify.
    template<class A>
    inline void
    wait_until_changed(A& a, typename A::value_type old) noexcept
    {
      // C++20-style; harmless if spurious-woken.
      a.wait(old, std::memory_order_acquire); ///< @EXPLAIN suspend without spinning; paper uses
                                              ///< EventCount prepare/commit. (Alg.5 L10,33)
                                              ///< :contentReference[oaicite:1]{index=1}
    }

    template<class A>
    inline void
    notify_one(A& a) noexcept
    {
      a.fetch_add(1, std::memory_order_release); ///< @EXPLAIN advance epoch; sleepers waiting on
                                                 ///< old value will wake. (Alg.3 L3; Alg.5 L6,16)
                                                 ///< :contentReference[oaicite:2]{index=2}
      a.notify_one();
    }

    template<class A>
    inline void
    notify_all(A& a) noexcept
    {
      a.fetch_add(1, std::memory_order_release); ///< @EXPLAIN wake all (termination). (Alg.5 L25)
                                                 ///< :contentReference[oaicite:3]{index=3}
      a.notify_all();
    }
  } // namespace detail

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
    // Ready tasks globally visible (master + workers); used as the "condition" to wake thieves.
    alignas(64) std::atomic_size_t ready{0};
    // Bell/epoch for wakeups (used as EventCount analog).
    alignas(64) std::atomic_uint64_t bell{0};
    // Global stop flag.
    alignas(64) std::atomic_bool abort{false};
    // Workers currently exploiting/executing tasks. (Definition 1: active). (Alg.3 L2,13)
    // :contentReference[oaicite:4]{index=4}
    alignas(64) std::atomic_size_t actives{0};
    // Workers currently thieves (awake & not sleeping & not exploiting). (Definition 1: thief).
    // (Alg.5 scope) :contentReference[oaicite:5]{index=5}
    alignas(64) std::atomic_size_t thieves{0};
    // Total amount of available workers
    alignas(64) std::atomic_size_t workers{0};
  };

  // Per-worker non-atomic counters (thread-local, reset per idle cycle).
  struct exec_stats
  {
    std::size_t steal_bound =
      16; // Max consecutive failed steals. (Alg.4 L14) :contentReference[oaicite:6]{index=6}
    std::size_t yield_bound =
      16; // Max consecutive yields. (Alg.4 L17-18)     :contentReference[oaicite:7]{index=7}
    std::size_t failed_steals = 0; // Count of consecutive failed steal attempts.
    std::size_t yields        = 0; // Count of consecutive yield attempts.

    // Sticky outcome from the most recent explore step:
    bool got_task_last = false; // Set true when explore acquired a task (Alg.5 L4-8,14-18).
                                // :contentReference[oaicite:8]{index=8}
    bool abort = false;
  };

  enum class action
  {
    exploit,
    explore,
    abort,
    retry,
    yield,              // plain yield between attempts
    yield_steal_exceed, // failed_steals >= steal_bound -> yield (Alg.4 L14-16)
                        // :contentReference[oaicite:9]{index=9}
    yield_reset_yields, // yields == yield_bound but actives>0 -> yield & reset (keeps one thief)
                        // (Alg.5 L29-31 + Lemma 1) :contentReference[oaicite:10]{index=10}
    suspend             // yields == yield_bound and actives==0 -> sleep (Alg.5 L33) + Lemma 2.
                        // :contentReference[oaicite:11]{index=11}
  };

  // ================
  // State evaluation
  // ================

  inline auto
  process_state(activity_stats const& activity, exec_stats const& exec, action prev) -> action
  {
    if (exec.abort || activity.abort.load(std::memory_order_relaxed))
    {
      /// @brief: Global abort → Terminate. Mirrors Alg.5 L23–27 (notify_all + return false).
      return action::abort;
    }

    switch (prev)
    {
      case action::exploit:
        /// @brief: Finished exploitation → attempt exploration next (worker drained queue). Alg.3
        /// L13 → Alg.5. :contentReference[oaicite:12]{index=12}
        return action::explore;

      case action::retry:
        [[fallthrough]];
      case action::yield:
        [[fallthrough]];
      case action::yield_steal_exceed:
        [[fallthrough]];
      case action::yield_reset_yields:
        [[fallthrough]];
      case action::suspend:
        /// @brief: After transient step (yield/suspend/retry), resume exploration. Alg.4 loop body.
        /// :contentReference[oaicite:13]{index=13}
        return action::explore;

      case action::explore:
      {
        /// @brief: Exploration outcome decides: exploit on success; otherwise control backing-off &
        /// sleep. Alg.4 + Alg.5. :contentReference[oaicite:14]{index=14}
        if (exec.got_task_last)
        {
          // Successful steal -> go execute. (Alg.5 L4-8,15-18)
          return action::exploit;
        }
        // Not yet successful: check backoff thresholds (Alg.4 L14-18).
        if (exec.failed_steals >= exec.steal_bound)
        {
          if (exec.yields < exec.yield_bound)
          {
            return action::yield_steal_exceed; // Start yielding after many failed steals.
          }
          // Exceeded yields: decide between staying awake (if some active exists) vs sleeping.
          if (activity.actives.load(std::memory_order_relaxed) > 0)
          {
            /// @brief: Ensure ≥1 thief while any active exists (Lemma 1); bounded thinning (Lemma
            /// 2). Alg.5 L29-31. :contentReference[oaicite:15]{index=15}
            return action::yield_reset_yields;
          }
          else
          {
            /// @brief: No actives → safe to sleep to mitigate over-subscription (Lemma 2). Alg.5
            /// L33. :contentReference[oaicite:16]{index=16}
            return action::suspend;
          }
        }
        // Below steal_bound → keep trying.
        return action::retry;
      }

      case action::abort:
        [[fallthrough]];
      default:
        /// @brief: Already aborted → remain abort.
        return action::abort;
    }
  }

  // =========
  // Actions
  // =========

  inline void
  process_action(action act, activity_stats& activity, exec_stats& exec, cas_deque auto& self,
                 invocable_return auto&& stealer)
  {
    switch (act)
    {
      case action::exploit:
      {
        /// @brief: Exploit tasks; if first active & no thieves, wake a thief. Alg.3 L2–3,5–13.
        /// (Def.1 active; Lemma 1) :contentReference[oaicite:17]{index=17}
        auto const a0 = activity.actives.fetch_add(1, std::memory_order_acq_rel);
        if (a0 == 0 && activity.thieves.load(std::memory_order_relaxed) == 0)
        {
          // Eagerly wake multiple thieves proportional to backlog.
          auto const ready   = activity.ready.load(std::memory_order_relaxed);
          auto const max     = activity.workers.load(std::memory_order_relaxed) - 1;
          auto const maxWake = std::min(ready, max); // pool_size known in pool
          // cap to avoid stampede; e.g., 4 or 8
          auto const k = std::min(maxWake, 4ull);
          for (std::size_t i = 0; i < k; ++i)
          {
            detail::notify_one(activity.bell); ///< ramp quickly from 1 active to many thieves
          }
          // detail::notify_one(
          //   activity.bell); ///< @EXPLAIN first active & no thief → ring bell to create a thief;
          //                   ///< prevents under-subscription. (Alg.3 L2–3; Lemma 1)
          //                   ///< :contentReference[oaicite:18]{index=18}
          // std::osyncstream(std::cout)
          //   << std::format("AFTER_ACTIVATE actives:{} thieves:{} ready:{}\n",
          //                  activity.actives.load(std::memory_order_relaxed),
          //                  activity.thieves.load(std::memory_order_relaxed),
          //                  activity.ready.load(std::memory_order_relaxed));
        }

        // Execute available work until local depletion.
        // We don’t assume a specific deque API; instead, we model "doing work"
        // by consuming global 'ready' opportunistically. Your actual executor
        // would pop & run tasks here and update 'ready' accordingly.
        while (auto t = self.try_pop_back())
        {
          auto prev = activity.ready.fetch_sub(1,
                                               std::memory_order_acq_rel); ///< @EXPLAIN consume one
                                                                           ///< unit of ready work.
          (*t)();
          if (prev == 1)
          {
            break; ///< @EXPLAIN both cache & queue empty → exit exploitation (Alg.3 L12–13).
                   ///< :contentReference[oaicite:19]{index=19}
          }
          // activity.ready.compare_exchange_weak(before, before - 1,
          //                                      std::memory_order_acq_rel); ///< @EXPLAIN consume
          //                                      one
          //                                                                  ///< unit of ready
          //                                                                  work.
          // detail::cpu_relax(); ///< @EXPLAIN placeholder for execute(t). (Alg.3 L6)
          //                      ///< :contentReference[oaicite:20]{index=20}
        }

        activity.actives.fetch_sub(
          1, std::memory_order_acq_rel); ///< @EXPLAIN leave active set. (Alg.3 L13)
                                         ///< :contentReference[oaicite:21]{index=21}
        exec.got_task_last = false;      ///< @EXPLAIN next state will go explore.
        exec.failed_steals = 0;          ///< @EXPLAIN exploitation reset.
        return;
      }

      case action::explore:
      {
        /// @brief: Become/act as a thief; try stealing (random victims + master). Alg.5 L2–9 +
        /// Alg.4. (Def.1 thief) :contentReference[oaicite:22]{index=22}
        // On the *first* entry into an exploration episode (i.e., when failed_steals==0 and
        // yields==0), we count ourselves as a thief (Alg.5 L2). We keep the counter until
        // success/sleep.
        if (exec.failed_steals == 0 && exec.yields == 0 && !exec.got_task_last)
        {
          activity.thieves.fetch_add(
            1, std::memory_order_acq_rel); ///< @EXPLAIN enter thief set. (Alg.5 L2)
                                           ///< :contentReference[oaicite:23]{index=23}
        }

        // Try to obtain work. The 'stealer' is expected to return #tasks acquired (0 if none).
        std::size_t acquired = 0;
        // Example policy: let 'stealer' encapsulate Alg.4 loop (random victims/master).
        // It should update global 'ready' accordingly (decrement when moving to local).
        acquired = std::invoke(stealer, self); ///< @EXPLAIN externalized Alg.4; 0 means fail this
                                               ///< round. :contentReference[oaicite:24]{index=24}

        if (acquired > 0)
        {
          // Success path (Alg.5 L4–8; L15–18).
          exec.got_task_last = true;
          exec.failed_steals = 0;
          exec.yields        = 0;

          // Leaving thief set; if we were the last thief, wake one sleeper (Alg.5 L5–7).
          if (activity.thieves.fetch_sub(1, std::memory_order_acq_rel) == 1)
          {
            detail::notify_one(
              activity.bell); ///< @EXPLAIN last-thief leaves → ring bell to spawn next thief if
                              ///< needed. (Alg.5 L5–7) :contentReference[oaicite:25]{index=25}
          }
        }
        else
        {
          // Failure path: account for backoff (Alg.4 L13–18).
          exec.got_task_last = false;
          ++exec.failed_steals; ///< @EXPLAIN increase consecutive failed steals. (Alg.4 L13)
                                ///< :contentReference[oaicite:26]{index=26}
        }
        return;
      }

      case action::yield:
      {
        /// @brief: Opportunistic yield between exploration attempts. (Alg.4 intends to let others
        /// run) :contentReference[oaicite:27]{index=27}
        std::this_thread::yield(); ///< @EXPLAIN let producers make progress; reduces contention.
                                   ///< (Alg.4 L15) :contentReference[oaicite:28]{index=28}
        return;
      }

      case action::yield_steal_exceed:
      {
        /// @brief: After ≥ steal_bound failures, yield and start yield counting. Alg.4 L14–17.
        /// :contentReference[oaicite:29]{index=29}
        std::this_thread::yield(); ///< @EXPLAIN bounded-yield between steal attempts.
                                   ///< :contentReference[oaicite:30]{index=30}
        exec.failed_steals =
          0; ///< @EXPLAIN reset steal counter once we enter yielding phase. (mirrors Alg.4
             ///< transition) :contentReference[oaicite:31]{index=31}
        ++exec.yields; ///< @EXPLAIN bounded yields before sleeping. (Alg.4 L17–18)
                       ///< :contentReference[oaicite:32]{index=32}
        return;
      }

      case action::yield_reset_yields:
      {
        /// @brief: Hit yield_bound, but actives>0 → keep one thief awake (Lemma 1). Cancel sleeping
        /// & retry. Alg.5 L29–31. :contentReference[oaicite:33]{index=33}
        std::this_thread::yield(); ///< @EXPLAIN short delay; avoids hot spinning while keeping a
                                   ///< thief. :contentReference[oaicite:34]{index=34}
        exec.yields = 0; ///< @EXPLAIN re-arm yielding window. (bounded convergence per Lemma 2)
                         ///< :contentReference[oaicite:35]{index=35}
        return;
      }

      case action::suspend:
      {
        /// @brief: Hit yield_bound and no actives → commit to sleep. Alg.5 L33; mitigates
        /// over-subscription (Lemma 2). :contentReference[oaicite:36]{index=36}
        // Leaving thief set before sleeping (Alg.5 L29 checked earlier in state-eval logic).

        // PREPARE
        auto const epoch = activity.bell.load(std::memory_order_acquire);

        // NOW COMMIT to sleeping: *after* confirming no immediate work.
        if (activity.thieves.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
          // If this were the last thief and actives>0, we should *not* sleep (paper’s last-thief
          // loop).
          if (activity.actives.load(std::memory_order_relaxed) > 0)
          {
            activity.thieves.fetch_add(1, std::memory_order_acq_rel); ///< @EXPLAIN preserve ≥1
                                                                      ///< thief while actives exist
                                                                      ///< (Lemma 1).
            std::this_thread::yield();
            return;
          }
        }

        detail::wait_until_changed(activity.bell,
                                   epoch); ///< @EXPLAIN commit_wait; will wake on bell bump.
        exec.failed_steals = exec.yields = 0;
        exec.got_task_last               = false;
        return;
      }

      case action::retry:
      {
        /// @brief: Continue exploration without yield; nothing to do here.
        detail::cpu_relax(); ///< @EXPLAIN tiny pause to reduce contention.
        return;
      }

      case action::abort:
      {
        /// @brief: Termination path. Alg.5 L23–27 (notify_all already done by controller).
        /// :contentReference[oaicite:38]{index=38}
        // Optionally, help wake others if we see abort.
        detail::notify_all(activity.bell); ///< @EXPLAIN ensure any sleepers observe abort.
        return;
      }
    }
  }
}
