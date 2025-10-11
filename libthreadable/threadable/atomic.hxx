#pragma once

#include <atomic>
#include <cassert>
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
  class atomic_bitfield final : private std::atomic<details::underlying_type_t<T>>
  {
    using underlying_type = details::underlying_type_t<T>;
    using atomic_t        = std::atomic<underlying_type>;

    /// @brief Converts a value to its underlying type.
    /// @details If `T` is an enum, converts it to its underlying integer type; otherwise, returns
    /// `T` unchanged.
    /// @return The underlying type value.
    static constexpr auto
    to_underlying(auto&& t) noexcept
    {
      return static_cast<underlying_type>(t);
    }

    /// @brief Converts a value from its underlying type to the specified type.
    /// @details Casts the underlying type value back to the specified type `T`.
    /// @param u The underlying type value to convert.
    /// @return The value cast to type `T`.
    static constexpr auto
    from_underlying(auto&& u) noexcept -> T
    {
      return static_cast<T>(u);
    }

  public:
    using atomic_t::atomic_t;
    using atomic_t::is_always_lock_free;
    using atomic_t::is_lock_free;
    using atomic_t::operator=;
    using atomic_t::notify_all;
    using atomic_t::notify_one;
    using atomic_t::store;

    explicit constexpr atomic_bitfield(T t)
      : atomic_t(to_underlying(t))
    {}

    /// @brief Compares and exchanges the value, handling enum types.
    /// @details Overloads `compare_exchange_strong` for enum types by converting to underlying
    /// types.
    /// @param expected Reference to the expected value.
    /// @param desired The desired value to set if comparison succeeds.
    /// @param orders Memory orders for the operation (e.g., `std::memory_order_seq_cst`).
    /// @return `true` if the exchange succeeded, `false` otherwise.
    [[nodiscard]] inline auto
    compare_exchange_strong(T& expected, T const desired,
                            std::memory_order success = std::memory_order_seq_cst,
                            std::memory_order failure = std::memory_order_seq_cst) noexcept -> bool
      requires (!std::is_same_v<T, underlying_type>)
    {
      auto curr = to_underlying(expected);
      auto ok   = atomic_t::compare_exchange_strong(curr, to_underlying(desired), success, failure);
      if (!ok)
      {
        expected = from_underlying(curr);
      }
      return ok;
    }

    /// @brief Weak version of compare_exchange_strong for enum types.
    /// @details Similar to `compare_exchange_strong`, but may spuriously fail.
    /// @param expected Reference to the expected value.
    /// @param desired The desired value to set if comparison succeeds.
    /// @param orders Memory orders for the operation (e.g., `std::memory_order_seq_cst`).
    /// @return `true` if the exchange succeeded, `false` otherwise.
    [[nodiscard]] inline auto
    compare_exchange_weak(T& expected, T const desired,
                          std::memory_order success = std::memory_order_seq_cst,
                          std::memory_order failure = std::memory_order_seq_cst) noexcept -> bool
      requires (!std::is_same_v<T, underlying_type>)
    {
      auto curr = to_underlying(expected);
      auto ok   = atomic_t::compare_exchange_weak(curr, to_underlying(desired), success, failure);
      if (!ok)
      {
        expected = from_underlying(curr);
      }
      return ok;
    }

    /// @brief Atomically compares and exchanges bits specified by the mask (strong version).
    /// @details Compares the bits in `Mask` with `expected`. If they match, sets them to the
    /// corresponding bits in `desired`, preserving other bits. Updates `expected` with the current
    /// bits if the operation fails.
    /// @tparam MaskExp Bitmask for expected comparison.
    /// @tparam MaskDes Bitmask for desired store.
    /// @param expected Reference to the expected bit values.
    /// @param desired The desired bit values to set if comparison succeeds.
    /// @param success Memory order for the operation.
    /// @param failure Memory order for the operation.
    /// @return `true` if the exchange succeeded, `false` otherwise.
    template<T MaskExp, T MaskDes = MaskExp>
    [[nodiscard]] inline auto
    compare_exchange_strong(T& expected, T const desired,
                            std::memory_order success = std::memory_order_seq_cst,
                            std::memory_order failure = std::memory_order_seq_cst) noexcept -> bool
    {
      constexpr auto umask_exp = to_underlying(MaskExp);
      constexpr auto umask_des = to_underlying(MaskDes);

      auto const exp  = to_underlying(expected);
      auto const des  = to_underlying(desired);
      auto       curr = to_underlying(atomic_t::load(failure));
      do
      {
        if ((curr & umask_exp) != (exp & umask_exp))
        {
          expected = from_underlying(curr);
          return false;
        }
        auto val = (curr & ~umask_des) | (des & umask_des);
        if (atomic_t::compare_exchange_strong(curr, val, success, failure))
        {
          return true;
        }
      }
      while ((curr & umask_exp) == (exp & umask_exp));

      expected = from_underlying(curr);
      return false;
    }

    /// @brief Atomically compares and exchanges bits specified by the mask (weak version).
    /// @details Similar to `compare_exchange_strong`, but may spuriously fail. Compares the bits in
    /// `Mask` with `expected` and, if they match, sets them to the corresponding bits in `desired`,
    /// preserving other bits. Updates `expected` with the current bits if the operation fails.
    /// @tparam MaskExp Bitmask for expected comparison.
    /// @tparam MaskDes Bitmask for desired store.
    /// @param expected Reference to the expected bit values.
    /// @param desired The desired bit values to set if comparison succeeds.
    /// @param success Memory order for the operation.
    /// @param failure Memory order for the operation.
    /// @return `true` if the exchange succeeded, `false` otherwise.
    template<T MaskExp, T MaskDes = MaskExp>
    inline auto
    compare_exchange_weak(T& expected, T const desired,
                          std::memory_order success = std::memory_order_seq_cst,
                          std::memory_order failure = std::memory_order_seq_cst) noexcept -> bool
    {
      constexpr auto umask_exp = to_underlying(MaskExp);
      constexpr auto umask_des = to_underlying(MaskDes);

      auto const exp  = to_underlying(expected);
      auto const des  = to_underlying(desired);
      auto       curr = atomic_t::load(failure);
      if ((curr & umask_exp) != (exp & umask_exp))
      {
        expected = from_underlying(curr);
        return false;
      }
      auto val = (curr & ~umask_des) | (des & umask_des);
      if (!atomic_t::compare_exchange_weak(curr, val, success, failure))
      {
        expected = from_underlying(curr);
        return false;
      }
      return true;
    }

    [[nodiscard]] inline auto
    load(std::memory_order order = std::memory_order_seq_cst) const noexcept -> T
    {
      return from_underlying(atomic_t::load(order));
    }

    /// @brief Tests if any bits specified by the mask are set.
    /// @details Atomically loads the current value and checks if any bits in `Mask` are set.
    /// @tparam Mask A constant expression representing the bitmask to test.
    /// @param orders Memory orders for the load operation (e.g., `std::memory_order_seq_cst`).
    /// @return True if any bits in `Mask` are set, false otherwise.
    template<T Mask>
    [[nodiscard]] inline auto
    test(std::memory_order order = std::memory_order_seq_cst) const noexcept -> bool
    {
      constexpr auto umask = to_underlying(Mask);
      return (atomic_t::load(order) & umask) != 0;
    }

    /// @brief Atomically tests and sets or clears the bits specified by the mask.
    /// @details Tests the bits in `Mask` and sets them if `Value` is true, or clears them if
    /// `Value` is false.
    /// @tparam Mask A constant expression representing the bitmask to operate on.
    /// @tparam Value If true, sets the bits; if false, clears the bits.
    /// @param orders Memory orders for the operation (e.g., `std::memory_order_seq_cst`).
    /// @return True if any of the bits in `Mask` were set before the operation, false otherwise.
    template<T Mask, bool Value>
    [[nodiscard]] inline auto
    test_and_set(std::memory_order order = std::memory_order_seq_cst) noexcept -> bool
    {
      constexpr auto umask = to_underlying(Mask);
      if constexpr (Value)
      {
        // Set the bits
        return (atomic_t::fetch_or(umask, order) & umask) != 0;
      }
      else
      {
        // Clear the bits
        return (atomic_t::fetch_and(~umask, order) & umask) != 0;
      }
    }

    /// @brief Atomically tests and sets or clears the bits specified by the mask.
    /// @details Tests the bits in `Mask` and sets them if `Value` is true, or clears them if
    /// `Value` is false.
    /// @tparam Mask A constant expression representing the bitmask to operate on.
    /// @param value If true, sets the bits; if false, clears the bits.
    /// @param orders Memory orders for the operation (e.g., `std::memory_order_seq_cst`).
    /// @return True if any of the bits in `Mask` were set before the operation, false otherwise.
    template<T Mask>
    [[nodiscard]] inline auto
    test_and_set(bool value, std::memory_order order = std::memory_order_seq_cst) noexcept -> bool
    {
      constexpr auto umask = to_underlying(Mask);
      if (value)
      {
        // Set the bits
        return (atomic_t::fetch_or(umask, order) & umask) != 0;
      }
      else
      {
        // Clear the bits
        return (atomic_t::fetch_and(~umask, order) & umask) != 0;
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
    set(std::memory_order order = std::memory_order_seq_cst) noexcept
    {
      (void)test_and_set<Mask, Value>(order);
    }

    /// @brief Atomically sets or clears the bits specified by the mask.
    /// @details Sets the bits if `Value` is true, or clears them if `Value` is false, without
    /// returning the previous state.
    /// @tparam Mask A constant expression representing the bitmask to operate on.
    /// @param value If true, sets the bits; if false, clears the bits.
    /// @param orders Memory orders for the operation (e.g., `std::memory_order_seq_cst`).
    template<T Mask>
    inline void
    set(bool value, std::memory_order order = std::memory_order_seq_cst) noexcept
    {
      (void)test_and_set<Mask>(value, order);
    }

    /// @brief Atomically clears the bits specified by the mask and returns the previous state.
    /// @details Clears the bits in `Mask` and returns whether any were set before clearing.
    /// @tparam Mask A constant expression representing the bitmask to clear.
    /// @param orders Memory orders for the operation (e.g., `std::memory_order_seq_cst`).
    /// @return True if any bits in `Mask` were set before clearing, false otherwise.
    template<T Mask>
    inline auto
    reset(std::memory_order order = std::memory_order_seq_cst) noexcept -> bool
    {
      return test_and_set<Mask, false>(order);
    }

    /// @brief Clears all bits in the atomic variable.
    /// @details Sets the entire atomic variable to 0, clearing all bits.
    /// @param orders Memory orders for the store operation (e.g., `std::memory_order_seq_cst`).
    inline void
    clear(std::memory_order order = std::memory_order_seq_cst) noexcept
    {
      atomic_t::store(0, order);
    }

    /// @brief Waits until the bits in the mask change from the specified initial state.
    /// @details Blocks until the bits in `Mask` are all unset if `Old` is true, or all set if `Old`
    /// is false.
    /// @tparam Mask A constant expression representing the bitmask to monitor.
    /// @param old The initial state of the (masked) bits.
    /// @param order Memory order for the load operation (e.g., `std::memory_order_seq_cst`).
    /// @example
    /// ```cpp
    /// auto flag = fho::atomic_bitfield<std::uint8_t>{0b111};
    /// flag.wait<0b111>(0b001); // Waits until 0b110
    /// ```
    template<T Mask>
      requires (!std::same_as<T, bool>)
    inline void
    wait(T old, std::memory_order order = std::memory_order_seq_cst) const noexcept
    {
      constexpr auto umask = to_underlying(Mask);
      auto const     uold  = to_underlying(old);
      auto const     utgt  = (~uold) & umask;

      auto curr = atomic_t::load(order);
      while ((curr & umask) != utgt)
      {
        atomic_t::wait(curr, order);
        curr = atomic_t::load(order);
      }
    }

    /// @brief Waits until the bits in the mask change from the specified initial state.
    /// @details Blocks until the bits in `Mask` are all unset if `Old` is true, or all set if `Old`
    /// is false.
    /// @tparam Mask A constant expression representing the bitmask to monitor.
    /// @param old The initial state of the bits (true for set, false for unset).
    /// @param orders Memory orders for the load operation (e.g., `std::memory_order_seq_cst`).
    /// @example
    /// ```cpp
    /// auto flag = fho::atomic_bitfield<std::uint8_t>{0b111};
    /// flag.wait<0b101>(true); // Waits until bits 0 & 2 are unset (eg. 0b010).
    /// ```
    template<T Mask>
    inline void
    wait(bool old, std::memory_order order = std::memory_order_seq_cst) const noexcept
    {
      constexpr auto umask = to_underlying(Mask);
      auto const     utgt  = to_underlying(old ? T{0} : Mask);

      auto curr = atomic_t::load(order);
      while ((curr & umask) != utgt)
      {
        atomic_t::wait(curr, order);
        curr = atomic_t::load(order);
      }
    }
  };

  static_assert(sizeof(atomic_bitfield<std::uint8_t>) == sizeof(std::atomic<std::uint8_t>));
  static_assert(alignof(atomic_bitfield<std::uint8_t>) == alignof(std::atomic<std::uint8_t>));
}
