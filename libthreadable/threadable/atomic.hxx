#pragma once

#include <atomic>
#include <concepts>
#include <type_traits>

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

namespace fho
{
  /// @brief A template class for atomic bitfield operations.
  /// @details This class provides atomic operations on individual bits or groups of bits within an
  /// underlying integer type `T`. It inherits from `std::atomic<T>` and adds methods to test, set,
  /// reset, and wait on specific bits defined by a mask. This is useful for thread-safe
  /// manipulation of bitfields in multi-threaded environments.
  /// @tparam `T` The underlying integer type, must be an integer type that can be used with
  /// `std::atomic`.
  /// @example
  /// ```cpp
  /// auto flags = fho::atomic_bitfield<std::uint8_t>{0};
  /// flag.template set<1, true>(); // Set bit 0 to 1
  /// if (flag.template test<1>()) { /* bit is set */ }
  /// ```
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

    /// @brief Tests if the bits specified by the mask are set.
    /// @details This method checks if any of the bits specified by the template parameter `mask`
    /// are set in the atomic variable.
    /// @tparam `mask` A constant expression representing the bitmask to test.
    /// @param `order` The memory order for the load operation. Defaults to
    /// `std::memory_order_relaxed`.
    /// @return True if any of the bits specified by `mask` are set, false otherwise.
    template<T mask>
    [[nodiscard]] inline auto
    test(std::memory_order order = std::memory_order_relaxed) const noexcept -> bool
    {
      return mask & this->load(order);
    }

    /// @brief Atomically tests and sets or clears the bits specified by the mask.
    /// @details This method atomically tests the bits specified by `mask` and sets or clears them
    /// based on the `value` template parameter.
    /// @tparam `mask` A constant expression representing the bitmask to operate on.
    /// @tparam `value` If true, sets the bits; if false, clears the bits.
    /// @param `order` The memory order for the operation. Defaults to `std::memory_order_relaxed`.
    /// @return The previous state of the bits specified by `mask` (true if any were set, false if
    /// none were set).
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

    /// @brief Atomically sets or clears the bits specified by the mask.
    /// @details This method atomically sets or clears the bits specified by `mask` based on the
    /// `value` template parameter. It does not return the previous state.
    /// @tparam `mask` A constant expression representing the bitmask to operate on.
    /// @tparam `value` If true, sets the bits; if false, clears the bits.
    /// @param `order` The memory order for the operation. Defaults to `std::memory_order_relaxed`.
    template<T mask, bool value>
    inline void
    set(std::memory_order order = std::memory_order_relaxed) noexcept
    {
      (void)test_and_set<mask, value>(order);
    }

    /// @brief Atomically clears the bits specified by the mask.
    /// @details This method is a convenience for `set<mask, false>`, which clears the bits
    /// specified by `mask`. It returns the previous state of the bits.
    /// @tparam `mask` A constant expression representing the bitmask to clear.
    /// @param `order` The memory order for the operation. Defaults to `std::memory_order_relaxed`.
    /// @return The previous state of the bits specified by `mask` (true if any were set, false if
    /// none were set).
    template<T mask>
    inline auto
    reset(std::memory_order order = std::memory_order_relaxed) noexcept -> bool
    {
      return test_and_set<mask, false>(order);
    }

    /// @brief Clears all bits in the atomic variable.
    /// @details This method sets the entire atomic variable to 0, effectively clearing all bits.
    /// @param `order` The memory order for the store operation. Defaults to
    /// `std::memory_order_relaxed`.
    inline void
    clear(std::memory_order order = std::memory_order_relaxed) noexcept
    {
      this->store(0, order);
    }

    /// @brief Waits until all bits specified by the mask change from the specified initial state.
    /// @details This method blocks until all bits in `mask` are opposite to the `old` state. If
    /// `old` is true, it waits until all bits in `mask` are unset (i.e., `(current & mask) == 0`).
    /// If `old` is false, it waits until all bits in `mask` are set (i.e., `(current & mask) ==
    /// mask`).
    /// @tparam `mask` A constant expression representing the bitmask to monitor.
    /// @tparam `old` The initial state of the bits (true for set, false for unset).
    /// @param `order` The memory order for the load operation. Defaults to
    /// `std::memory_order_relaxed`.
    /// @example
    /// ```cpp
    /// auto flag = fho::atomic_bitfield<std::uint8_t>{0b111};
    /// flag.wait<0b111, true>(); // Waits until bits 0,1,2 are unset (`0b000`)
    /// ```
    template<T mask, bool old>
    inline void
    wait(std::memory_order order = std::memory_order_relaxed) const noexcept
    {
      auto current = this->load(order);
      if constexpr (old)
      {
        while ((current & mask) != 0)
        {
          this->wait(current, order);
          current = this->load(order);
        }
      }
      else
      {
        while ((current & mask) != mask)
        {
          this->wait(current, order);
          current = this->load(order);
        }
      }
    }
  };

  static_assert(sizeof(atomic_bitfield<std::uint8_t>) == sizeof(std::atomic<std::uint8_t>));
  static_assert(alignof(atomic_bitfield<std::uint8_t>) == alignof(std::atomic<std::uint8_t>));
}

#undef FWD
