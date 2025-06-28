#pragma once

#include <atomic>
#include <concepts>
#include <type_traits>

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

namespace fho
{
  namespace details
  {
    template<typename T>
    using underlying_type_t =
      std::conditional_t<std::is_enum_v<T>, std::underlying_type<T>, std::type_identity<T>>::type;
  }

  inline constexpr auto
  to_underlying(auto&& t) noexcept
  {
    using underlying_t = details::underlying_type_t<std::remove_reference_t<decltype(t)>>;
    return static_cast<underlying_t>(t);
  }

  template<typename T>
  inline constexpr auto
  from_underlying(auto&& u) noexcept
  {
    return static_cast<T>(u);
  }

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
    requires std::integral<details::underlying_type_t<T>>
  class atomic_bitfield final : std::atomic<details::underlying_type_t<T>>
  {
    using underlying_type = details::underlying_type_t<T>;
    using atomic_t        = std::atomic<underlying_type>;

  public:
    using atomic_t::atomic_t;
    using atomic_t::is_lock_free;
    using atomic_t::operator=;
    using atomic_t::compare_exchange_strong;
    using atomic_t::compare_exchange_weak;
    using atomic_t::exchange;
    using atomic_t::fetch_and;
    using atomic_t::fetch_or;
    using atomic_t::load;
    using atomic_t::notify_all;
    using atomic_t::notify_one;
    using atomic_t::store;
    using atomic_t::wait;

    /// @brief Overload for when `T` & `underlying_type` differs.
    /// @tparam `Mask` Eg. the enum value.
    /// @param `order` The memory order for the load operation.
    /// @return Result of of the same operation but with `underlying_type`.
    auto
    compare_exchange_strong(T& expected, T const desired,
                            std::same_as<std::memory_order> auto... orders) noexcept -> bool
      requires (!std::is_same_v<T, underlying_type>)
    {
      auto exp = to_underlying(expected);
      auto res = this->compare_exchange_strong(exp, to_underlying(desired), orders...);
      expected = from_underlying<T>(exp);
      return res;
    }

    auto
    compare_exchange_weak(T& expected, T const desired,
                          std::same_as<std::memory_order> auto... orders) noexcept -> bool
      requires (!std::is_same_v<T, underlying_type>)
    {
      auto exp = to_underlying(expected);
      auto res = this->compare_exchange_weak(exp, to_underlying(desired), orders...);
      expected = from_underlying<T>(exp);
      return res;
    }

    /// @brief Tests if the bits specified by the mask are set.
    /// @details This method checks if any of the bits specified by the template parameter `Mask`
    /// are set in the atomic variable.
    /// @tparam `Mask` A constant expression representing the bitmask to test.
    /// @param `order` The memory order for the load operation. Defaults to
    /// `std::memory_order_seq_cst`.
    /// @return True if any of the bits specified by `Mask` are set, false otherwise.
    template<T Mask>
    [[nodiscard]] inline auto
    test(std::same_as<std::memory_order> auto... orders) const noexcept -> bool
    {
      return Mask & this->load(orders...);
    }

    /// @brief Atomically tests and sets or clears the bits specified by the mask.
    /// @details This method atomically tests the bits specified by `Mask` and sets or clears them
    /// based on the `Value` template parameter.
    /// @tparam `Mask` A constant expression representing the bitmask to operate on.
    /// @tparam `Value` If true, sets the bits; if false, clears the bits.
    /// @param `order` The memory order for the operation. Defaults to `std::memory_order_seq_cst`.
    /// @return The previous state of the bits specified by `Mask` (true if any were set, false if
    /// none were set).
    template<T Mask, bool Value>
    inline auto
    test_and_set(std::same_as<std::memory_order> auto... orders) noexcept -> bool
    {
      if constexpr (Value)
      {
        // Set the bit
        return Mask & this->fetch_or(Mask, orders...);
      }
      else
      {
        // Clear the bit
        return Mask & this->fetch_and(to_underlying(~Mask), orders...);
      }
    }

    /// @brief Atomically sets or clears the bits specified by the mask.
    /// @details This method atomically sets or clears the bits specified by `Mask` based on the
    /// `Value` template parameter. It does not return the previous state.
    /// @tparam `Mask` A constant expression representing the bitmask to operate on.
    /// @tparam `Value` If true, sets the bits; if false, clears the bits.
    /// @param `order` The memory order for the operation. Defaults to `std::memory_order_seq_cst`.
    template<T Mask, bool Value>
    inline void
    set(std::same_as<std::memory_order> auto... orders) noexcept
    {
      (void)test_and_set<Mask, Value>(orders...);
    }

    /// @brief Atomically clears the bits specified by the mask.
    /// @details This method is a convenience for `set<mask, false>`, which clears the bits
    /// specified by `Mask`. It returns the previous state of the bits.
    /// @tparam `Mask` A constant expression representing the bitmask to clear.
    /// @param `order` The memory order for the operation. Defaults to `std::memory_order_seq_cst`.
    /// @return The previous state of the bits specified by `Mask` (true if any were set, false if
    /// none were set).
    template<T Mask>
    inline auto
    reset(std::same_as<std::memory_order> auto... orders) noexcept -> bool
    {
      return test_and_set<Mask, false>(orders...);
    }

    /// @brief Clears all bits in the atomic variable.
    /// @details This method sets the entire atomic variable to 0, effectively clearing all bits.
    /// @param `order` The memory order for the store operation. Defaults to
    /// `std::memory_order_seq_cst`.
    inline void
    clear(std::same_as<std::memory_order> auto... orders) noexcept
    {
      this->store(0, orders...);
    }

    /// @brief Waits until all bits specified by the mask change from the specified initial state.
    /// @details This method blocks until all bits in `Mask` are opposite to the `old` state. If
    /// `old` is true, it waits until all bits in `Mask` are unset (i.e., `(current & mask) == 0`).
    /// If `old` is false, it waits until all bits in `Mask` are set (i.e., `(current & mask) ==
    /// mask`).
    /// @tparam `Mask` A constant expression representing the bitmask to monitor.
    /// @tparam `Old` The initial state of the bits (true for set, false for unset).
    /// @param `order` The memory order for the load operation. Defaults to
    /// `std::memory_order_seq_cst`.
    /// @example
    /// ```cpp
    /// auto flag = fho::atomic_bitfield<std::uint8_t>{0b111};
    /// flag.wait<0b111, true>(); // Waits until bits 0,1,2 are unset (`0b000`)
    /// ```
    template<T Mask, bool Old>
    inline void
    wait(std::same_as<std::memory_order> auto... orders) const noexcept
    {
      auto current = this->load(orders...);
      if constexpr (Old)
      {
        while ((current & Mask) != 0)
        {
          this->wait(current, orders...);
          current = this->load(orders...);
        }
      }
      else
      {
        while ((current & Mask) != Mask)
        {
          this->wait(current, orders...);
          current = this->load(orders...);
        }
      }
    }
  };

  static_assert(sizeof(atomic_bitfield<std::uint8_t>) == sizeof(std::atomic<std::uint8_t>));
  static_assert(alignof(atomic_bitfield<std::uint8_t>) == alignof(std::atomic<std::uint8_t>));
}

#undef FWD
