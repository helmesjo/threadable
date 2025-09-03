#pragma once

#include <threadable/atomic.hxx>
#include <threadable/function.hxx>
#include <threadable/token.hxx>

#include <atomic>
#include <memory>
#include <type_traits>

#ifndef NDEBUG
  #include <algorithm>
  #include <ranges>
#endif

#ifdef _MSC_VER
  #pragma warning(push)
  #pragma warning(disable : 4324)
#endif

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

namespace fho
{
  /// @brief A slot in a ring buffer that holds a value of type `T` and manages its state
  /// atomically.
  /// @details The `ring_slot` class is designed for use in concurrent environments, such as thread
  /// pools or task queues, where slots must be acquired, assigned values, processed, and released
  /// efficiently. It uses atomic operations to manage state transitions between `empty`, `claimed`,
  /// and `active`. The class is aligned to the cache line size to prevent false sharing in
  /// multithreaded scenarios. Thread safety is ensured through atomic state management, but users
  /// must follow the correct sequence of operations.
  /// @tparam `T` The type of the value stored in the slot. Must be movable and, if not trivially
  /// destructible, must be destructible.
  /// @note The state must transition as follows: `empty` -> `claimed` -> `active` -> `empty`.
  /// Deviating from this sequence may result in undefined behavior.
  template<typename T>
  class alignas(details::cache_line_size) ring_slot
  {
  public:
    /// @brief Default constructor.
    /// @details Initializes the slot with an `empty` state and no value.
    ring_slot() = default;

    /// @brief Constructor from atomic state and value.
    /// @details Initializes the slot with the specified state and moves the provided value into it.
    /// @param `state` The initial atomic state of the slot.
    /// @param `value` The value to store in the slot.
    ring_slot(fho::atomic_state_t state, T value)
      : state_(state.load(std::memory_order_relaxed))
      , value_(std::move(value))
    {}

    /// @brief Move constructor.
    /// @details Transfers the state and value from another slot.
    /// @param `that` The slot to move from.
    ring_slot(ring_slot&& that) noexcept
      : state_(that.state_.load(std::memory_order_relaxed))
    {
      if constexpr (!std::is_trivially_move_constructible_v<T>)
      {
        // Placement new with move to properly invoke
        // T's move ctor; avoids UB for non-trivial T.
        new (data()) T(std::move(*that.data()));
        std::destroy_at(that.data()); // Explicitly destroy moved-from object to end its
                                      // lifetime per [basic.life]/8.
      }
      else
      {
        value_ = std::move(that.value_); // For trivial T, bitwise move (memcpy equivalent).
      }
    }

    /// @brief Deleted copy constructor.
    /// @details Copying slots is not allowed to prevent unintended duplication.
    ring_slot(ring_slot const&) = delete;

    /// @brief Destructor.
    /// @details Releases any resources held by the slot. No special cleanup is performed.
    ~ring_slot() = default;

    /// @brief Deleted copy assignment.
    /// @details Copying slots is not allowed to prevent unintended duplication.
    auto operator=(ring_slot const&) -> ring_slot& = delete;

    /// @brief Move assignment.
    /// @details Transfers the state and value from another slot.
    /// @param `that` The slot to move from.
    /// @return A reference to this slot.
    inline auto
    operator=(ring_slot&& that) noexcept -> ring_slot&
    {
      state_ = that.state_.load(std::memory_order_relaxed);
      if constexpr (!std::is_trivially_move_constructible_v<T>)
      {
        // Placement new with move to properly invoke
        // T's move ctor; avoids UB for non-trivial T.
        new (data()) T(std::move(*that.data()));
        std::destroy_at(that.data()); // Explicitly destroy moved-from object to end its
                                      // lifetime per [basic.life]/8.
      }
      else
      {
        value_ = std::move(that.value_); // For trivial T, bitwise move (memcpy equivalent).
      }
      return *this;
    }

    /// @brief Conversion to reference to `T`.
    /// @details Allows the slot to be used as if it were the stored value. Ensure the slot is in
    /// an appropriate state (e.g., `active`) before accessing the value to avoid undefined
    /// behavior.
    inline
    operator T&() noexcept
    {
      return value();
    }

    /// @brief Conversion to const reference to `T`.
    /// @details Allows the slot to be used as if it were the stored value. Ensure the slot is in
    /// an appropriate state (e.g., `active`) before accessing the value to avoid undefined
    /// behavior.
    inline
    operator T const&() const noexcept
    {
      return value();
    }

    /// @brief Acquires the slot by claiming it.
    /// @details Atomically attempts to change the state from `empty` to `claimed`. Spins until
    /// successful, making it suitable for low-contention scenarios.
    inline void
    acquire() noexcept
    {
      auto exp = slot_state::free;
      while (!state_.compare_exchange_weak(exp, slot_state::claimed, std::memory_order_release,
                                           std::memory_order_relaxed)) [[likely]]
      {
        exp = slot_state::free;
      }
    }

    /// @brief Assigns a value to the slot.
    /// @details Requires the slot to be in the `claimed` state. Constructs a new value of type `T`
    /// in place using the provided arguments and sets the state to `active`. In debug mode, asserts
    /// that the current value is all zeros if convertible to `bool`.
    /// @param `args` Arguments to forward to the constructor of `T`.
    inline void
    emplace(auto&&... args) noexcept
    {
#ifndef NDEBUG
      if constexpr (requires {
                      { value() } -> std::convertible_to<bool>;
                    })
      {
        assert(std::ranges::all_of(value_,
                                   [](auto b)
                                   {
                                     return b == std::byte{0};
                                   }));
      }
#endif
      // Must be claimed
      assert(state_.load(std::memory_order_acquire) == slot_state::claimed);

      std::construct_at<T>(data(), FWD(args)...);
      state_.store(slot_state::active, std::memory_order_release);
      // @NOTE: Intentionally not notifying here since that is redundant (and costly),
      //        it is designed to be waited on by checking state active -> inactive.
    }

    /// @brief Waits for the slot to become inactive.
    /// @details Blocks until the slot's state is no longer `active`, typically indicating that the
    /// stored value (e.g., a task) has been processed and released.
    inline void
    wait() const noexcept
    {
      state_.template wait<slot_state::active, true>(std::memory_order_acquire);
    }

    /// @brief Releases the slot.
    /// @details Requires the slot to be in the `active` state. Destroys the stored value if it is
    /// not trivially destructible, sets the state to `empty`, and notifies any waiters. In debug
    /// mode, resets the value to zero to aid in detecting use-after-free errors.
    inline void
    release() noexcept
    {
      // Must be active
      assert(state_.test<slot_state::active>());
      // Free up slot for re-use.
      if constexpr (!std::is_trivially_destructible_v<T>)
      {
        std::destroy_at<T>(data());
      }
#ifndef NDEBUG
      value_ = {};
#endif
      state_.store(slot_state::free, std::memory_order_release);
      state_.notify_all();
    }

    /// @brief Binds the slot's state to a `slot_token`.
    /// @details Enables external monitoring or control of the slot's state through the provided
    /// `slot_token`.
    /// @param `t` The `slot_token` to bind the state to.
    inline void
    bind(slot_token& t) noexcept
    {
      t.rebind(state_);
    }

    /// @brief Gets a reference to the stored value.
    /// @details Returns a reference to the stored value. Ensure the slot is in an appropriate
    /// state (e.g., `active`) before accessing to avoid undefined behavior.
    inline constexpr auto
    value() noexcept -> T&
    {
      assert(state_.load(std::memory_order_acquire) == slot_state::active and "ring_slot::value()");
      return *data(); // NOLINT
    }

    /// @brief Gets a const reference to the stored value.
    /// @details Returns a const reference to the stored value. Ensure the slot is in an
    /// appropriate state (e.g., `active`) before accessing to avoid undefined behavior.
    inline constexpr auto
    value() const noexcept -> T const&
    {
      assert(state_.load(std::memory_order_acquire) == slot_state::active and "ring_slot::value()");
      return *data(); // NOLINT
    }

  private:
    /// @brief Gets a pointer to the stored value.
    /// @details Returns a pointer to the memory where the value is stored. Ensure the slot is in
    /// an appropriate state (e.g., `ready`) before accessing to avoid undefined behavior.
    inline constexpr auto
    data() noexcept -> T*
    {
      // Launder to ensure aliasing compliance post-construction.
      return details::launder_as<T>(value_.data());
    }

    /// @brief Gets a const pointer to the stored value.
    /// @details Returns a const pointer to the memory where the value is stored. Ensure the slot
    /// is in an appropriate state (e.g., `ready`) before accessing to avoid undefined behavior.
    inline constexpr auto
    data() const noexcept -> T const*
    {
      // Launder to ensure aliasing compliance post-construction.
      return details::launder_as<T const>(value_.data());
    }

    /// Atomic state of the slot.
    fho::atomic_state_t state_{slot_state::free};
    /// Aligned storage for the value of type `T`.
    alignas(T) std::array<std::byte, sizeof(T)> value_;
  };

  template<typename T>
  inline constexpr auto slot_value_accessor = [](auto&& a) -> std::add_lvalue_reference_t<T>
  {
    return FWD(a).value();
  };
}

#undef FWD

#ifdef _MSC_VER
  #pragma warning(pop)
#endif
