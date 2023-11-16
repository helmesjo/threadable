#pragma once

#include <atomic>
#include <cstdint>

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

namespace threadable::details
{
  using atomic_bitfield = std::atomic<std::uint8_t>;

  template<std::uint8_t bit>
  inline bool test(const atomic_bitfield& field, std::memory_order order = std::memory_order_seq_cst)
    requires (bit < sizeof(bit) * 8)
  {
    static constexpr std::uint8_t mask = 1 << bit;
    return mask & field.load(order);
  }

  template<std::uint8_t bit, bool value>
    requires (bit < sizeof(bit) * 8)
  inline bool test_and_set(atomic_bitfield& field, std::memory_order order = std::memory_order_seq_cst)
  {
    static constexpr std::uint8_t mask = 1 << bit;
    if constexpr(value)
    {
      // Set the bit
      return mask & field.fetch_or(mask, order);
    }
    else
    {
      // Clear the bit
      return mask & field.fetch_and(static_cast<std::uint8_t>(~mask), order);
    }
  }

  template<std::uint8_t bit, bool value>
    requires (bit < sizeof(bit) * 8)
  inline void set(atomic_bitfield& field, std::memory_order order = std::memory_order_seq_cst)
  {
    (void)test_and_set<bit, value>(field, order);
  }

  inline void clear(atomic_bitfield& field, std::memory_order order = std::memory_order_seq_cst)
  {
    field.exchange(0, order);
  }

  template<std::uint8_t bit, bool old>
    requires (bit < sizeof(bit) * 8)
  inline void wait(const atomic_bitfield& field, std::memory_order order = std::memory_order_seq_cst)
  {
    static constexpr std::uint8_t mask = 1 << bit;
    auto current = field.load(order);
    while (static_cast<bool>(current & mask) == old)
    {
      // Wait for any change in atomicVar
      field.wait(current, order);

      // Reload the current value
      current = field.load(order);
    }
  }
}

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

  inline void atomic_set(atomic_flag& atomic, std::memory_order order = std::memory_order_seq_cst) noexcept
  {
    (void)atomic_test_and_set(atomic, order);
  }

  inline void atomic_clear(atomic_flag& atomic, std::memory_order order = std::memory_order_seq_cst) noexcept
  {
    atomic.clear(order);
  }

  inline auto atomic_test_and_clear(atomic_flag& atomic, std::memory_order order = std::memory_order_seq_cst) noexcept
  {
    auto wasTrue = atomic.test(order);
    atomic.clear(order);
    return wasTrue;
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
    return atomic.exchange(true, order);
  }

  inline void atomic_set(atomic_flag& atomic, std::memory_order order = std::memory_order_seq_cst) noexcept
  {
    atomic.store(true, order);
  }

  inline void atomic_clear(atomic_flag& atomic, std::memory_order order = std::memory_order_seq_cst) noexcept
  {
    atomic.exchange(false, order);
  }

  inline auto atomic_test_and_clear(atomic_flag& atomic, std::memory_order order = std::memory_order_seq_cst) noexcept
  {
    bool expected = true;
    return atomic.compare_exchange_weak(expected, false, order);
  }
}
#endif

#if __cpp_lib_atomic_wait >= 201907 && (!defined(__APPLE__) || (__clang_major__ > 13 || __clang_major__ == 13 && __clang_minor__ == 1 && __clang_patchlevel__ == 6)) // apple-clang defines it without supplying the functions
namespace threadable::details
{
  template<typename atomic_t, typename obj_t>
  inline void atomic_wait(const atomic_t& atomic, obj_t old, std::memory_order order = std::memory_order_seq_cst) noexcept
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
