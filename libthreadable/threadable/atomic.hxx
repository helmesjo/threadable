#pragma once

#include <atomic>
#include <utility>

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

namespace fho
{
  template<typename T>
  class atomic_bitfield final : std::atomic<T>
  {
  public:
    using std::atomic<T>::atomic;
    using std::atomic<T>::operator=;
    using std::atomic<T>::load;
    using std::atomic<T>::store;
    using std::atomic<T>::exchange;
    using std::atomic<T>::compare_exchange_weak;
    using std::atomic<T>::compare_exchange_strong;
    using std::atomic<T>::wait;
    using std::atomic<T>::notify_one;
    using std::atomic<T>::notify_all;

    template<T mask>
    [[nodiscard]] inline auto
    test(std::memory_order order = std::memory_order_relaxed) const noexcept -> bool
    {
      return mask & this->load(order);
    }

    template<T mask, bool value>
    inline auto
    test_and_set(std::memory_order order = std::memory_order_relaxed) noexcept -> bool
    {
      if constexpr (value)
      {
        // Set the bit
        return mask & this->fetch_or(mask, order);
      }
      else
      {
        // Clear the bit
        return mask & this->fetch_and(static_cast<T>(~mask), order);
      }
    }

    template<T mask, bool value>
    inline void
    set(std::memory_order order = std::memory_order_relaxed) noexcept
    {
      (void)test_and_set<mask, value>(order);
    }

    template<T mask>
    inline auto
    reset(std::memory_order order = std::memory_order_relaxed) noexcept -> bool
    {
      return test_and_set<mask, false>(order);
    }

    inline void
    clear(std::memory_order order = std::memory_order_relaxed) noexcept
    {
      this->store(0, order);
    }

    template<T mask, bool old>
    inline void
    wait(std::memory_order order = std::memory_order_relaxed) const noexcept
    {
      auto current = this->load(order);
      while (static_cast<bool>(current & mask) == old)
      {
        // Wait for any change in atomicVar
        this->wait(current, order);

        // Reload the current value
        current = this->load(order);
      }
    }
  };

  static_assert(sizeof(atomic_bitfield<std::uint8_t>) == sizeof(std::atomic<std::uint8_t>));
  static_assert(alignof(atomic_bitfield<std::uint8_t>) == alignof(std::atomic<std::uint8_t>));
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
