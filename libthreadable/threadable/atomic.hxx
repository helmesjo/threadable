#pragma once

#include <atomic>
#include <concepts>
#include <type_traits>

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

namespace fho
{
  template<typename T>
    requires (std::integral<T> || std::integral<std::underlying_type_t<T>>)
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

#undef FWD
