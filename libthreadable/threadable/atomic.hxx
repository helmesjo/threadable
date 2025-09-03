#pragma once

#include <atomic>
#include <concepts>
#include <cstdint>
#include <type_traits>

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

namespace fho
{
  namespace details
  {
    template<typename T>
    using underlying_type_t =
      typename std::conditional_t<std::is_enum_v<T>, std::underlying_type<T>,
                                  std::type_identity<T>>::type;
  }

  /// @brief Converts a value to its underlying type.
  /// @details If `T` is an enum, converts it to its underlying integer type; otherwise, returns `T`
  /// unchanged.
  /// @param t The value to convert.
  /// @return The underlying type value.
  inline constexpr auto
  to_underlying(auto&& t) noexcept
  {
    using underlying_t = details::underlying_type_t<std::remove_reference_t<decltype(t)>>;
    return static_cast<underlying_t>(t);
  }

  /// @brief Converts a value from its underlying type to the specified type.
  /// @details Casts the underlying type value back to the specified type `T`.
  /// @tparam T The target type to cast to.
  /// @param u The underlying type value to convert.
  /// @return The value cast to type `T`.
  template<typename T>
  inline constexpr auto
  from_underlying(auto&& u) noexcept
  {
    return static_cast<T>(u);
  }

  /// @brief A template class for atomic bitfield operations.
  /// @details This class extends `std::atomic<T>` to provide atomic operations on individual bits
  /// or groups of bits within an underlying integer type `T`. It supports testing, setting,
  /// resetting, and waiting on specific bits defined by a mask, making it ideal for thread-safe
  /// bitfield manipulation in multi-threaded environments.
  /// @tparam T The underlying type, which must be an integer type or an enum that can be used with
  /// `std::atomic`.
  /// @note For enum types, operations are performed on their underlying integer types.
  /// @example
  /// ```cpp
  /// enum class Flags : std::uint8_t { None = 0, Bit0 = 1, Bit1 = 2 };
  /// auto flag = fho::atomic_bitfield<Flags>{Flags::None};
  /// flag.set<Flags::Bit0, true>(); // Set Bit0
  /// if (flag.test<Flags::Bit0>()) { /* Bit0 is set */ }
  /// ```
  template<typename T>
    requires std::integral<details::underlying_type_t<T>>
  class atomic_bitfield final : public std::atomic<details::underlying_type_t<T>>
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

    /// @brief Compares and exchanges the value, handling enum types.
    /// @details Overloads `compare_exchange_strong` for enum types by converting to underlying
    /// types.
    /// @param expected Reference to the expected value.
    /// @param desired The desired value to set if comparison succeeds.
    /// @param orders Memory orders for the operation (e.g., `std::memory_order_seq_cst`).
    /// @return `true` if the exchange succeeded, `false` otherwise.
    inline auto
    compare_exchange_strong(T& expected, T const desired,
                            std::same_as<std::memory_order> auto... orders) noexcept -> bool
      requires (!std::is_same_v<T, underlying_type>)
    {
      auto exp = to_underlying(expected);
      auto res = this->compare_exchange_strong(exp, to_underlying(desired), orders...);
      expected = from_underlying<T>(exp);
      return res;
    }

    /// @brief Performs a strong compare-and-exchange on masked bits.
    /// @details Atomically loads the current value using the `failure` memory order.
    /// Checks if the bits in `ExpMask` match the expected state: all set if `Expected` is true,
    /// all unset if false. If the condition holds, computes a new value by setting the bits in
    /// `DesMask` to all set if `Desired` is true or all unset if false, while leaving bits
    /// outside `DesMask` unchanged. Attempts to replace the current value with this new value
    /// using compare-exchange. Retries with weak compare-exchange until success or the bit
    /// condition no longer holds (due to concurrent modification).
    /// @tparam `ExpMask` A constant expression representing the bitmask for the expected condition.
    /// @tparam `Expected` The expected state of the `ExpMask` bits: true for all set, false for all
    /// unset.
    /// @tparam `DesMask` A constant expression representing the bitmask for the desired update.
    /// @tparam `Desired` The desired state to set the `DesMask` bits to: true for all set, false
    /// for all unset.
    /// @param `success` Memory order on success (defaults to `seq_cst`).
    /// @param `failure` Memory order on failure (defaults to `seq_cst`; must not be stronger than
    /// success per `std::atomic` rules).
    /// @return `true` if the exchange succeeded (condition held and CAS won), `false` otherwise.
    template<T ExpMask, bool Expected, T DesMask, bool Desired>
    inline auto
    compare_exchange_strong(std::memory_order success = std::memory_order_seq_cst,
                            std::memory_order failure = std::memory_order_seq_cst) noexcept -> bool
      requires (!std::is_same_v<T, underlying_type>)
    {
      auto           cur      = this->load(failure);
      constexpr auto exp_bits = Expected ? ExpMask : underlying_type{0u};
      for (;;)
      {
        if ((cur & ExpMask) != exp_bits)
        {
          return false;
        }
        auto const des = (cur & ~DesMask) | (Desired ? DesMask : underlying_type{0u});
        if (this->compare_exchange_weak(cur, des, success, failure))
        {
          return true;
        }
      }
    }

    template<T ExpMask, bool Expected>
    inline auto
    compare_exchange_strong(T desired, std::memory_order success = std::memory_order_seq_cst,
                            std::memory_order failure = std::memory_order_seq_cst) noexcept -> bool
      requires (!std::is_same_v<T, underlying_type>)
    {
      auto           cur      = this->load(failure);
      constexpr auto exp_bits = Expected ? ExpMask : underlying_type{0u};
      for (;;)
      {
        if ((cur & ExpMask) != exp_bits)
        {
          return false;
        }
        auto const des = (cur & ~desired) | (desired ? desired : underlying_type{0u});
        if (this->compare_exchange_weak(cur, des, success, failure))
        {
          return true;
        }
      }
    }

    /// @brief Weak version of compare_exchange_strong for enum types.
    /// @details Similar to `compare_exchange_strong`, but may spuriously fail.
    /// @param expected Reference to the expected value.
    /// @param desired The desired value to set if comparison succeeds.
    /// @param orders Memory orders for the operation (e.g., `std::memory_order_seq_cst`).
    /// @return `true` if the exchange succeeded, `false` otherwise.
    inline auto
    compare_exchange_weak(T& expected, T const desired,
                          std::same_as<std::memory_order> auto... orders) noexcept -> bool
      requires (!std::is_same_v<T, underlying_type>)
    {
      auto exp = to_underlying(expected);
      auto res = this->compare_exchange_weak(exp, to_underlying(desired), orders...);
      expected = from_underlying<T>(exp);
      return res;
    }

    /// @brief Performs a weak compare-and-exchange on masked bits.
    /// @details Atomically loads the current value using the `failure` memory order.
    /// Checks if the bits in `ExpMask` match the expected state: all set if `Expected` is true,
    /// all unset if false. If the condition holds, computes a new value by setting the bits in
    /// `DesMask` to all set if `Desired` is true or all unset if false, while leaving bits
    /// outside `DesMask` unchanged. Attempts a single weak replacement with this new value; may
    /// fail spuriously even if condition holds and value matches. No retries; returns false on
    /// condition failure or CAS failure (including spurious).
    /// @tparam `ExpMask` A constant expression representing the bitmask for the expected condition.
    /// @tparam `Expected` The expected state of the `ExpMask` bits: true for all set, false for all
    /// unset.
    /// @tparam `DesMask` A constant expression representing the bitmask for the desired update.
    /// @tparam `Desired` The desired state to set the `DesMask` bits to: true for all set, false
    /// for all unset.
    /// @param `success` Memory order on success (defaults to `seq_cst`).
    /// @param `failure` Memory order on failure (defaults to `seq_cst`; must not be stronger than
    /// success per `std::atomic` rules).
    /// @return `true` if the exchange succeeded (condition held and CAS won), `false` otherwise.
    template<T ExpMask, bool Expected, T DesMask, bool Desired>
    inline auto
    compare_exchange_weak(std::memory_order success = std::memory_order_seq_cst,
                          std::memory_order failure = std::memory_order_seq_cst) noexcept -> bool
      requires (!std::is_same_v<T, underlying_type>)
    {
      auto           cur      = this->load(failure);
      constexpr auto exp_bits = Expected ? ExpMask : underlying_type{0u};
      constexpr auto des_bits = Desired ? DesMask : underlying_type{0u};
      if ((cur & ExpMask) != exp_bits)
      {
        return false;
      }
      auto const des = (cur & ~DesMask) | des_bits;
      return this->compare_exchange_weak(cur, des, success, failure);
    }

    /// @brief Tests if any bits specified by the mask are set.
    /// @details Atomically loads the current value and checks if any bits in `Mask` are set.
    /// @tparam Mask A constant expression representing the bitmask to test.
    /// @param orders Memory orders for the load operation (e.g., `std::memory_order_seq_cst`).
    /// @return True if any bits in `Mask` are set, false otherwise.
    template<T Mask>
    [[nodiscard]] inline auto
    test(std::same_as<std::memory_order> auto... orders) const noexcept -> bool
    {
      return (this->load(orders...) & Mask) != 0;
    }

    /// @brief Atomically tests and sets or clears the bits specified by the mask.
    /// @details Tests the bits in `Mask` and sets them if `Value` is true, or clears them if
    /// `Value` is false.
    /// @tparam Mask A constant expression representing the bitmask to operate on.
    /// @tparam Value If true, sets the bits; if false, clears the bits.
    /// @param orders Memory orders for the operation (e.g., `std::memory_order_seq_cst`).
    /// @return True if any of the bits in `Mask` were set before the operation, false otherwise.
    template<T Mask, bool Value>
    inline auto
    test_and_set(std::same_as<std::memory_order> auto... orders) noexcept -> bool
    {
      if constexpr (Value)
      {
        // Set the bits
        return (this->fetch_or(Mask, orders...) & Mask) != 0;
      }
      else
      {
        // Clear the bits
        return (this->fetch_and(from_underlying<T>(~Mask), orders...) & Mask) != 0;
      }
    }

    /// @brief Atomically sets or clears the bits specified by the mask.
    /// @details Sets the bits if `Value` is true, or clears them if `Value` is false, without
    /// returning the previous state.
    /// @tparam Mask A constant expression representing the bitmask to operate on.
    /// @tparam Value If true, sets the bits; if false, clears the bits.
    /// @param orders Memory orders for the operation (e.g., `std::memory_order_seq_cst`).
    template<T Mask, bool Value>
    inline void
    set(std::same_as<std::memory_order> auto... orders) noexcept
    {
      (void)test_and_set<Mask, Value>(orders...);
    }

    /// @brief Atomically clears the bits specified by the mask and returns the previous state.
    /// @details Clears the bits in `Mask` and returns whether any were set before clearing.
    /// @tparam Mask A constant expression representing the bitmask to clear.
    /// @param orders Memory orders for the operation (e.g., `std::memory_order_seq_cst`).
    /// @return True if any bits in `Mask` were set before clearing, false otherwise.
    template<T Mask>
    inline auto
    reset(std::same_as<std::memory_order> auto... orders) noexcept -> bool
    {
      return test_and_set<Mask, false>(orders...);
    }

    /// @brief Clears all bits in the atomic variable.
    /// @details Sets the entire atomic variable to 0, clearing all bits.
    /// @param orders Memory orders for the store operation (e.g., `std::memory_order_seq_cst`).
    inline void
    clear(std::same_as<std::memory_order> auto... orders) noexcept
    {
      this->store(0, orders...);
    }

    /// @brief Waits until the bits in the mask change from the specified initial state.
    /// @details Blocks until the bits in `Mask` are all unset if `Old` is true, or all set if `Old`
    /// is false.
    /// @tparam Mask A constant expression representing the bitmask to monitor.
    /// @tparam Old The initial state of the bits (true for set, false for unset).
    /// @param orders Memory orders for the load operation (e.g., `std::memory_order_seq_cst`).
    /// @example
    /// ```cpp
    /// auto flag = fho::atomic_bitfield<std::uint8_t>{0b111};
    /// flag.wait<0b111, true>(); // Waits until all bits are unset (0b000)
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
