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

    /// @brief Converts a value to its underlying type.
    /// @details If `T` is an enum, converts it to its underlying integer type; otherwise, returns
    /// `T` unchanged.
    /// @return The underlying type value.
    inline constexpr auto
    to_underlying(auto&& t) noexcept
    {
      return static_cast<underlying_type>(t);
    }

    /// @brief Converts a value from its underlying type to the specified type.
    /// @details Casts the underlying type value back to the specified type `T`.
    /// @param u The underlying type value to convert.
    /// @return The value cast to type `T`.
    inline static constexpr auto
    from_underlying(auto&& u) noexcept -> T
    {
      return static_cast<T>(u);
    }

  public:
    using atomic_t::atomic_t;
    using atomic_t::is_lock_free;
    using atomic_t::operator=;
    using atomic_t::compare_exchange_strong;
    using atomic_t::compare_exchange_weak;
    using atomic_t::exchange;
    using atomic_t::fetch_and;
    using atomic_t::fetch_or;
    // using atomic_t::load;
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
    [[nodiscard]] inline auto
    compare_exchange_strong(T& expected, T const desired,
                            std::same_as<std::memory_order> auto... orders) noexcept -> bool
      requires (!std::is_same_v<T, underlying_type>)
    {
      return atomic_t::compare_exchange_strong(expected, desired, orders...);
    }

    /// @brief Weak version of compare_exchange_strong for enum types.
    /// @details Similar to `compare_exchange_strong`, but may spuriously fail.
    /// @param expected Reference to the expected value.
    /// @param desired The desired value to set if comparison succeeds.
    /// @param orders Memory orders for the operation (e.g., `std::memory_order_seq_cst`).
    /// @return `true` if the exchange succeeded, `false` otherwise.
    [[nodiscard]] inline auto
    compare_exchange_weak(T& expected, T const desired,
                          std::same_as<std::memory_order> auto... orders) noexcept -> bool
      requires (!std::is_same_v<T, underlying_type>)
    {
      return atomic_t::compare_exchange_weak(expected, desired, orders...);
    }

    /// @brief Atomically compares and exchanges bits specified by the mask (strong version).
    /// @details Compares the bits in `Mask` with `expected`. If they match, sets them to the
    /// corresponding bits in `desired`, preserving other bits. Updates `expected` with the current
    /// bits if the operation fails.
    /// @tparam MaskDes A constant expression representing the bitmask to operate on.
    /// @tparam MaskExp A constant expression representing the bitmask to operate on.
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
      auto curr = from_underlying(atomic_t::load(failure));
      for (;;)
      {
        if ((curr & MaskExp) != (expected & MaskExp))
        {
          expected = curr;
          return false;
        }
        if (compare_exchange_weak<MaskExp, MaskDes>(curr, desired, success, failure))
        {
          return true;
        }
      }
    }

    /// @brief Atomically compares and exchanges bits specified by the mask (weak version).
    /// @details Similar to `compare_exchange_strong`, but may spuriously fail. Compares the bits in
    /// `Mask` with `expected` and, if they match, sets them to the corresponding bits in `desired`,
    /// preserving other bits. Updates `expected` with the current bits if the operation fails.
    /// @tparam Mask A constant expression representing the bitmask to operate on.
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
      auto curr = atomic_t::load(failure);
      if ((curr & MaskExp) != (expected & MaskExp))
      {
        expected = from_underlying(curr);
        return false;
      }
      auto val = (curr & ~MaskDes) | (desired & MaskDes);
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
    test(std::same_as<std::memory_order> auto... orders) const noexcept -> bool
    {
      return (atomic_t::load(orders...) & Mask) != 0;
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
    test_and_set(std::same_as<std::memory_order> auto... orders) noexcept -> bool
    {
      if constexpr (Value)
      {
        // Set the bits
        return (atomic_t::fetch_or(to_underlying(Mask), orders...) & Mask) != 0;
      }
      else
      {
        // Clear the bits
        return (atomic_t::fetch_and(to_underlying(~Mask), orders...) & Mask) != 0;
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
    test_and_set(bool value, std::same_as<std::memory_order> auto... orders) noexcept -> bool
    {
      if (value)
      {
        // Set the bits
        return (atomic_t::fetch_or(to_underlying(Mask), orders...) & Mask) != 0;
      }
      else
      {
        // Clear the bits
        return (atomic_t::fetch_and(to_underlying(~Mask), orders...) & Mask) != 0;
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

    /// @brief Atomically sets or clears the bits specified by the mask.
    /// @details Sets the bits if `Value` is true, or clears them if `Value` is false, without
    /// returning the previous state.
    /// @tparam Mask A constant expression representing the bitmask to operate on.
    /// @param value If true, sets the bits; if false, clears the bits.
    /// @param orders Memory orders for the operation (e.g., `std::memory_order_seq_cst`).
    template<T Mask>
    inline void
    set(bool value, std::same_as<std::memory_order> auto... orders) noexcept
    {
      (void)test_and_set<Mask>(value, orders...);
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
      atomic_t::store(0, orders...);
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
      auto current = atomic_t::load(orders...);
      if constexpr (Old)
      {
        while ((current & Mask) != 0)
        {
          atomic_t::wait(current, orders...);
          current = atomic_t::load(orders...);
        }
      }
      else
      {
        while ((current & Mask) != Mask)
        {
          atomic_t::wait(current, orders...);
          current = atomic_t::load(orders...);
        }
      }
    }
  };

  static_assert(sizeof(atomic_bitfield<std::uint8_t>) == sizeof(std::atomic<std::uint8_t>));
  static_assert(alignof(atomic_bitfield<std::uint8_t>) == alignof(std::atomic<std::uint8_t>));
}

#undef FWD
