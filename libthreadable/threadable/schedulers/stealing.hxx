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
    std::size_t steal_bound   = 0; // Set externally to 2 * (W + 1)
    std::size_t yield_bound   = 100;
    std::size_t failed_steals = 0;
    std::size_t yields        = 0;
    bool        abort         = false;
  };

  inline void
  exploit_task(invocable_opt auto& stolen, activity_stats& activity, cas_deque auto& self)
  {
    if (stolen)
    { // t ≠ NIL; assume !t falsy for invalid/empty
      if (activity.actives.fetch_add(1, std::memory_order_seq_cst) == 0 &&
          activity.thieves.load(std::memory_order_seq_cst) == 0)
      {
        activity.notifier.notify_one();
      }
      std::invoke(stolen);
      stolen = nullptr;
      while (auto t = self.try_pop_back())
      {
        std::invoke(t);
      }
      activity.actives.fetch_sub(1, std::memory_order_seq_cst);
    }
  }

  inline void
  explore_task(invocable_opt auto& cached, activity_stats& activity, exec_stats& exec,
               cas_deque auto& self, invocable_return auto&& stealer)
  {
    exec.failed_steals = 0;
    exec.yields        = 0;
    while (!activity.stops.load(std::memory_order_acquire))
    {
      // steal from random victim, or master if victim == self.
      if (cached = stealer(self); cached) // t ≠ NIL; assume truthy for valid task
      {
        break;
      }
      else
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
  }

  inline auto
  wait_for_task(invocable_opt auto& stolen, activity_stats& activity, exec_stats& exec,
                cas_deque auto& self, invocable_return auto&& stealer) -> bool
  {
    activity.thieves.fetch_add(1, std::memory_order_acq_rel);
    while (true)
    {
      // Alg. 4
      if (explore_task(stolen, activity, exec, self, stealer); stolen)
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
      if (activity.stops.load(std::memory_order_acquire))
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
      // Alg. 3: Execute local tasks, update actives
      exploit_task(stolen, activity, self);
      if (!wait_for_task(stolen, activity, exec, self, stealer))
      { // Alg. 5: Steal or sleep; returns false on stop
        break;
      }
    }
  }
}
