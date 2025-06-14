#pragma once

#include <atomic>
#include <cstdint>
#include <utility>

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

namespace fho::details
{
  template<typename width_t>
  using atomic_bitfield_t = std::atomic<width_t>;

  template<std::uint8_t bit, typename width_t>
  inline auto
  test(atomic_bitfield_t<width_t> const& field, std::memory_order order = std::memory_order_relaxed)
    -> bool
    requires (bit < sizeof(width_t) * 8)
  {
    static constexpr std::uint8_t mask = 1 << bit;
    return mask & field.load(order);
  }

  template<std::uint8_t bit, bool value, typename width_t>
    requires (bit < sizeof(width_t) * 8)
  inline auto
  test_and_set(atomic_bitfield_t<width_t>& field,
               std::memory_order           order = std::memory_order_relaxed) -> bool
  {
    static constexpr std::uint8_t mask = 1 << bit;
    if constexpr (value)
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

  template<std::uint8_t bit, bool value, typename width_t>
    requires (bit < sizeof(width_t) * 8)
  inline void
  set(atomic_bitfield_t<width_t>& field, std::memory_order order = std::memory_order_relaxed)
  {
    (void)test_and_set<bit, value>(field, order);
  }

  template<std::uint8_t bit, typename width_t>
    requires (bit < sizeof(width_t) * 8)
  inline auto
  reset(atomic_bitfield_t<width_t>& field, std::memory_order order = std::memory_order_relaxed)
    -> bool
  {
    return test_and_set<bit, false>(field, order);
  }

  template<typename width_t>
  inline void
  clear(atomic_bitfield_t<width_t>& field, std::memory_order order = std::memory_order_relaxed)
  {
    field.store(0, order);
  }

  template<std::uint8_t bit, bool old, typename width_t>
    requires (bit < sizeof(width_t) * 8)
  inline void
  wait(atomic_bitfield_t<width_t> const& field, std::memory_order order = std::memory_order_relaxed)
  {
    static constexpr std::uint8_t mask    = 1 << bit;
    auto                          current = field.load(order);
    while (static_cast<bool>(current & mask) == old)
    {
      // Wait for any change in atomicVar
      field.wait(current, order);

      // Reload the current value
      current = field.load(order);
    }
  }
}

#if 0 // __cpp_lib_atomic_flag_test >= 201907 // a bunch of compilers define this without supporting
      // it.
namespace fho::details
{
  using atomic_flag_t = std::atomic_flag;

  inline auto atomic_test(const atomic_flag_t& atomic, std::memory_order order = std::memory_order_relaxed) noexcept
  {
    return atomic.test(order);
  }

  inline auto atomic_test_and_set(atomic_flag_t& atomic, std::memory_order order = std::memory_order_relaxed) noexcept
  {
    return atomic.test_and_set(order);
  }

  inline void atomic_set(atomic_flag_t& atomic, std::memory_order order = std::memory_order_relaxed) noexcept
  {
    (void)atomic_test_and_set(atomic, order);
  }

  inline void atomic_clear(atomic_flag_t& atomic, std::memory_order order = std::memory_order_relaxed) noexcept
  {
    atomic.clear(order);
  }

  inline auto atomic_test_and_clear(atomic_flag_t& atomic, std::memory_order order = std::memory_order_relaxed) noexcept
  {
    auto wasTrue = atomic.test(order);
    atomic.clear(order);
    return wasTrue;
  }
}
#else
namespace fho::details
{
  using atomic_flag_t = std::atomic_bool;

  inline auto
  atomic_test(atomic_flag_t const& atomic,
              std::memory_order    order = std::memory_order_relaxed) noexcept
  {
    return atomic.load(order);
  }

  inline auto
  atomic_test_and_set(atomic_flag_t&    atomic,
                      std::memory_order order = std::memory_order_relaxed) noexcept
  {
    return atomic.exchange(true, order);
  }

  inline void
  atomic_set(atomic_flag_t& atomic, std::memory_order order = std::memory_order_relaxed) noexcept
  {
    atomic.store(true, order);
  }

  inline void
  atomic_clear(atomic_flag_t& atomic, std::memory_order order = std::memory_order_relaxed) noexcept
  {
    atomic.exchange(false, order);
  }

  inline auto
  atomic_test_and_clear(atomic_flag_t&    atomic,
                        std::memory_order order = std::memory_order_relaxed) noexcept
  {
    bool expected = true;
    return atomic.compare_exchange_weak(expected, false, order);
  }
}
#endif

#if __cpp_lib_atomic_wait >= 201907
namespace fho::details
{
  template<typename atomic_t, typename obj_t>
  inline void
  atomic_wait(atomic_t const& atomic, obj_t old,
              std::memory_order order = std::memory_order_relaxed) noexcept
  {
    atomic.wait(FWD(old), order);
  }

  template<typename atomic_t>
  inline void
  atomic_notify_one(atomic_t& atomic) noexcept
  {
    atomic.notify_one();
  }

  template<typename atomic_t>
  inline void
  atomic_notify_all(atomic_t& atomic) noexcept
  {
    atomic.notify_all();
  }
}
#else
  #error Requires __cpp_lib_atomic_wait >= 201907
#endif

#undef FWD
