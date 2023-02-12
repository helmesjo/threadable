#pragma once

#include <atomic>
#include <utility>
#include <version>

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

#if 0// __cpp_lib_atomic_flag_test >= 201907 // a bunch of compilers define this without supporting it.
namespace threadable::details
{
  using atomic_flag = std::atomic_flag;

  inline auto atomic_test(const atomic_flag& atomic, std::memory_order order = std::memory_order_seq_cst) noexcept
  {
    return atomic.test(order);
  }

  inline auto atomic_test_and_set(atomic_flag& atomic, std::memory_order order = std::memory_order_seq_cst) noexcept
  {
    return atomic.test_and_set(order);
  }

  inline auto atomic_clear(atomic_flag& atomic, std::memory_order order = std::memory_order_seq_cst) noexcept
  {
    return atomic.clear(order);
  }
}
#else
namespace threadable::details
{
  using atomic_flag = std::atomic_bool;

  inline auto atomic_test(const atomic_flag& atomic, std::memory_order order = std::memory_order_seq_cst) noexcept
  {
    return atomic.load(order);
  }

  inline auto atomic_test_and_set(atomic_flag& atomic, std::memory_order order = std::memory_order_seq_cst) noexcept
  {
    return atomic.store(true, order);
  }

  inline auto atomic_clear(atomic_flag& atomic, std::memory_order order = std::memory_order_seq_cst) noexcept
  {
    return atomic.store(false, order);
  }
}
#endif

#if __cpp_lib_atomic_wait >= 201907 && (!defined(__APPLE__) || (__clang_major__ > 13 || __clang_major__ == 13 && __clang_minor__ == 1 && __clang_patchlevel__ == 6)) // apple-clang defines it without supplying the functions
namespace threadable::details
{
  template<typename atomic_t, typename obj_t>
  inline void atomic_wait(const atomic_t& atomic, obj_t&& old, std::memory_order order = std::memory_order_seq_cst) noexcept
  {
    atomic.wait(FWD(old), order);
  }

  template<typename atomic_t>
  inline void atomic_notify_one(atomic_t& atomic) noexcept
  {
    atomic.notify_one();
  }

  template<typename atomic_t>
  inline void atomic_notify_all(atomic_t& atomic) noexcept
  {
    atomic.notify_all();
  }
}
#else
#include <thread>

namespace threadable::details
{
  template<typename obj_t>
  inline void atomic_wait(const atomic_flag& atomic, obj_t old, std::memory_order order = std::memory_order_seq_cst) noexcept
  {
    while(atomic_test(atomic, order) == old){ std::this_thread::yield(); }
  }

  template<typename T, typename obj_t>
  inline void atomic_wait(const std::atomic<T>& atomic, obj_t old, std::memory_order order = std::memory_order_seq_cst) noexcept
  {
    while(atomic.load(order) == old){ std::this_thread::yield(); }
  }

  template<typename atomic_t>
  inline void atomic_notify_one(atomic_t&) noexcept
  {
  }

  template<typename atomic_t>
  inline void atomic_notify_all(atomic_t&) noexcept
  {
  }
}
#endif

#undef FWD
