#pragma once

#include <threadable/atomic.hxx>
#include <threadable/debug.hxx>
#include <threadable/function.hxx>
#include <threadable/token.hxx>

#include <atomic>
#include <concepts>
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
  /// efficiently. It uses atomic operations to manage state transitions between `empty`, `ready`
  /// and inbetween. The class is aligned to the cache line size to prevent false sharing in
  /// multithreaded scenarios. Thread safety is ensured through atomic state management, but users
  /// must follow the correct sequence of operations.
  /// @tparam `T` The type of the value stored in the slot. Must be movable and, if not trivially
  /// destructible, must be destructible.
  /// @note The state must transition as follows:
  //  `empty` -> `locked_empty` -> `ready` -> `locked_ready` -> `empty`.
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
        dbg::verify_bitwise(that.state_, slot_state::ready | slot_state::locked_ready);
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
    inline constexpr auto
    operator=(ring_slot&& that) noexcept -> ring_slot&
    {
      state_ = that.state_.load(std::memory_order_relaxed);
      if constexpr (!std::is_trivially_move_constructible_v<T>)
      {
        // Placement new with move to properly invoke
        // T's move ctor; avoids UB for non-trivial T.
        dbg::verify_bitwise(that.state_, slot_state::ready | slot_state::locked_ready);
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

    [[nodiscard]] inline constexpr auto
    operator==(ring_slot const& rhs) const noexcept -> bool
    {
      return data() == rhs.data();
    }

    /// @brief Conversion to reference to `T`.
    /// @details Allows the slot to be used as if it were the stored value. Ensure the slot is in
    /// an appropriate state (e.g., `ready`) before accessing the value to avoid undefined
    /// behavior.
    inline
    operator T&() noexcept
    {
      return value();
    }

    /// @brief Conversion to const reference to `T`.
    /// @details Allows the slot to be used as if it were the stored value. Ensure the slot is in
    /// an appropriate state (e.g., `ready`) before accessing the value to avoid undefined
    /// behavior.
    inline
    operator T const&() const noexcept
    {
      return value();
    }

    template<slot_state State>
      requires (State == slot_state::empty || State == slot_state::ready)
    inline void
    lock(std::memory_order success = std::memory_order_acq_rel,
         std::memory_order failure = std::memory_order_acquire) noexcept
    {
      constexpr auto desired =
        State == slot_state::empty ? slot_state::locked_empty : slot_state::locked_ready;

      auto exp = State;
      while (!state_.compare_exchange_weak<slot_state::state_mask>(exp, desired, success, failure))
      {
        exp = State;
      }
    }

    template<slot_state State>
      requires (State == slot_state::empty || State == slot_state::ready)
    inline constexpr auto
    try_lock(std::memory_order success = std::memory_order_acq_rel,
             std::memory_order failure = std::memory_order_acquire) noexcept -> bool
    {
      constexpr auto desired =
        State == slot_state::empty ? slot_state::locked_empty : slot_state::locked_ready;

      auto exp = State;
      if (state_.compare_exchange_weak<slot_state::state_mask>(exp, desired, success, failure))
      {
        return true;
      }
      return false;
    }

    template<slot_state State>
      requires (State == slot_state::locked_empty)
    inline void
    commit(std::memory_order success = std::memory_order_acq_rel,
           std::memory_order failure = std::memory_order_acquire) noexcept
    {
      constexpr auto desired = slot_state::ready;

      dbg::verify<slot_state::state_mask>(state_, State);
      auto exp = State;
      if (!state_.compare_exchange_strong<slot_state::state_mask>(exp, desired, success, failure))
      {
        dbg::log("Prerequisite violation: ", exp, slot_state::locked_empty);
        std::abort();
      }
    }

    template<slot_state State>
      requires (State == slot_state::locked_empty || State == slot_state::locked_ready)
    void
    unlock(std::memory_order success = std::memory_order_acq_rel,
           std::memory_order failure = std::memory_order_acquire) noexcept
    {
      constexpr auto desired =
        State == slot_state::locked_empty ? slot_state::empty : slot_state::ready;

      dbg::verify<slot_state::state_mask>(state_, State);
      auto exp = State;
      while (!state_.compare_exchange_weak<slot_state::state_mask>(exp, desired, success, failure))
      {
        exp = State;
      }
    }

    /// @brief Assigns a value to the slot.
    /// @details Requires the slot to be in the `locked_empty` state. Constructs a new value of type
    /// `T` in place using the provided arguments and sets the state to `ready`. In debug mode,
    /// asserts that the current value is all zeros if convertible to `bool`.
    /// @param `args` Arguments to forward to the constructor of `T`.
    inline void
    emplace(auto&&... args) noexcept
    {
      dbg::verify<slot_state::state_mask>(state_, slot_state::locked_empty);
#ifndef NDEBUG
      if constexpr (requires {
                      { value() } -> std::convertible_to<bool>;
                    })
      {
        assert(std::ranges::all_of(value_,
                                   [](auto b)
                                   {
                                     return b == std::byte{0};
                                   }) and
               "ring_slot::emplace()");
      }
#endif

      std::construct_at<T>(data(), FWD(args)...);
      // @NOTE: Intentionally not notifying here since it's redundant (and costly),
      //        it is designed to be waited on by checking state `ready | ready_locked` -> `empty`.
    }

    /// @brief Releases the slot.
    /// @details Requires the slot to be in the `ready` state. Destroys the stored value if it is
    /// not trivially destructible, sets the state to `empty`, and notifies any waiters. In debug
    /// mode, resets the value to zero to aid in detecting use-after-free errors.
    /// @tparam `State` State to transition.
    template<slot_state State>
      requires (State == slot_state::locked_ready)
    inline void
    release(std::memory_order order = std::memory_order_release) noexcept
    {
      dbg::verify<slot_state::state_mask>(state_, State);
      // Free up slot for re-use.
      if constexpr (!std::is_trivially_destructible_v<T>)
      {
        std::destroy_at<T>(data());
      }
#ifndef NDEBUG
      value_ = {};
#endif
      // @NOTE: Ownership is dropped, so we reset all bits.
      // Expected = current {state,epoch} (and we assert state is locked_ready)
      auto curr = state_.load(std::memory_order_relaxed);
      // Desired = {state=empty, epoch = flipped}
      auto val = static_cast<slot_state>(slot_state::empty |
                                         ((curr ^ slot_state::epoch) & slot_state::epoch));
      state_.store(val, order);
      state_.notify_all();
    }

    /// @brief Waits for the slot to leave a state.
    /// @details Blocks until the slot's state changes to/from `State`, typically indicating that
    /// the stored value (e.g., a task) has been processed and released.
    /// @tparam `State` State to transition.
    /// @tparam `Old` Value to transition from.
    ///               Defaults to `true`.
    template<slot_state State = slot_state::ready, bool Old = true>
    inline void
    wait(std::memory_order order = std::memory_order_acquire) const noexcept
    {
      state_.template wait<State, Old>(order);
    }

    /// @brief Tests if any states specified by the mask are set.
    /// @details Atomically loads the current value and checks if any bits in `Mask` are set.
    /// @tparam `State` A constant expression representing the bitmask to test.
    /// @param `orders` Memory orders for the load operation (e.g., `std::memory_order_seq_cst`).
    /// @return True if any bits in `Mask` are set, false otherwise.
    template<slot_state State>
    [[nodiscard]] inline constexpr auto
    test(std::same_as<std::memory_order> auto... orders) const noexcept -> bool
    {
      return state_.test<State>(orders...);
    }

    /// @brief Atomically sets or clears the bits specified by the mask.
    /// @details Sets the bits if `Value` is true, or clears them if `Value` is false, without
    /// returning the previous state.
    /// @tparam Mask A constant expression representing the bitmask to operate on.
    /// @tparam Value If true, sets the bits; if false, clears the bits.
    /// @param orders Memory orders for the operation (e.g., `std::memory_order_seq_cst`).
    template<slot_state Mask, bool Value>
      requires ((Mask & (slot_state::state_mask)) == 0)
    inline constexpr void
    set(std::same_as<std::memory_order> auto... orders) noexcept
    {
      state_.set<Mask, Value>(orders...);
    }

    template<slot_state Mask>
      requires ((Mask & (slot_state::state_mask)) == 0)
    inline constexpr void
    set(bool value, std::same_as<std::memory_order> auto... orders) noexcept
    {
      state_.set<Mask>(value, orders...);
    }

    [[nodiscard]] inline constexpr auto
    load(std::memory_order order) const noexcept -> slot_state
    {
      return state_.load(order);
    }

    /// @brief Binds the slot's state to a `slot_token`.
    /// @details Enables external monitoring or control of the slot's state through the provided
    /// `slot_token`.
    /// @param `t` The `slot_token` to bind the state to.
    inline constexpr void
    bind(slot_token& t) noexcept
    {
      t.rebind(state_);
    }

    /// @brief Gets a reference to the stored value.
    /// @details Returns a reference to the stored value. Ensure the slot is in an appropriate
    /// state (e.g., `ready`) before accessing to avoid undefined behavior.
    inline constexpr auto
    value() noexcept -> T&
    {
      dbg::verify<slot_state::state_mask>(state_.load(std::memory_order_acquire) |
                                            slot_state::locked,
                                          slot_state::locked_ready);
      return *data(); // NOLINT
    }

    /// @brief Gets a const reference to the stored value.
    /// @details Returns a const reference to the stored value. Ensure the slot is in an
    /// appropriate state (e.g., `ready`) before accessing to avoid undefined behavior.
    inline constexpr auto
    value() const noexcept -> T const&
    {
      dbg::verify<slot_state::state_mask>(state_.load(std::memory_order_acquire) |
                                            slot_state::locked,
                                          slot_state::locked_ready);
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
    fho::atomic_state_t state_{slot_state::empty};
    /// Aligned storage for the value of type `T`.
    alignas(T) std::array<std::byte, sizeof(T)> value_;
  };

  template<typename T>
  inline constexpr auto slot_value_accessor = [](auto&& a) -> std::add_lvalue_reference_t<T>
  {
    dbg::verify<slot_state::state_mask>(a.load(std::memory_order_acquire) | slot_state::locked,
                                        slot_state::locked_ready);
    return FWD(a).value();
  };
}

#undef FWD

#ifdef _MSC_VER
  #pragma warning(pop)
#endif
