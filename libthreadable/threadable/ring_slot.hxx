#pragma once

#include <threadable/atomic.hxx>
#include <threadable/function.hxx>
#include <threadable/token.hxx>

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
      , value_(std::move(that.value_))
    {}

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
      value_ = std::move(that.value_);
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
    /// @details Atomically attempts to change the state from `exp` to `claimed`. Spins until
    /// successful, making it suitable for low-contention scenarios.
    /// @param `exp` Expected state.
    inline void
    claim(slot_state const exp) noexcept
    {
      auto expected = exp;
      while (!state_.compare_exchange_weak(expected, slot_state::claimed, std::memory_order_acq_rel,
                                           std::memory_order_acquire)) [[likely]]
      {
        expected = exp;
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
                                   }) and
               "ring_slot::emplace()");
      }
#endif
      // Must be claimed
      assert(state_.load(std::memory_order_acquire) == slot_state::claimed and
             "ring_slot::emplace()");

      std::construct_at<T>(data(), FWD(args)...);
      state_.store(slot_state::active, std::memory_order_release);
      // @NOTE: Intentionally not notifying here since that is redundant (and costly),
      //        it is designed to be waited on by checking state active -> inactive.
    }

    /// @brief Try to acquire the slot by claiming it.
    /// @details Atomically attempts to change the state from `active` to `claimed`. Spins until
    /// successful unless `exp` doesn't match current value.
    /// @param `exp` Expected state.
    /// @return `true` if exchanged `active` -> `lent` (`claimed`), else `false`.
    inline auto
    try_claim() noexcept -> bool
    {
      auto expected = slot_state::active;
      return state_.compare_exchange_strong(expected, slot_state::claimed,
                                            std::memory_order_acq_rel, std::memory_order_acquire);
    }

    inline void
    undo_claim() noexcept
    {
      assert(state_.template test<slot_state::claimed>(std::memory_order_acquire) and
             "ring_slot::undo_claim()");
      state_.store(slot_state::active, std::memory_order_release);
    }

    /// @brief Waits for the slot to leave a state.
    /// @details Blocks until the slot's state changes to/from `State`, typically indicating that
    /// the stored value (e.g., a task) has been processed and released.
    /// @tparam `State` State to transition.
    /// @tparam `Old` Value to transition from.
    ///               Defaults to `true`.
    template<slot_state State, bool Old = true>
    inline void
    wait(std::same_as<std::memory_order> auto order) const noexcept
    {
      state_.template wait<State, Old>(order);
    }

    /// @brief Tests if any states specified by the mask are set.
    /// @details Atomically loads the current value and checks if any bits in `Mask` are set.
    /// @tparam `State` A constant expression representing the bitmask to test.
    /// @param `orders` Memory orders for the load operation (e.g., `std::memory_order_seq_cst`).
    /// @return True if any bits in `Mask` are set, false otherwise.
    template<slot_state State>
    [[nodiscard]] inline auto
    test(std::same_as<std::memory_order> auto... orders) const noexcept -> bool
    {
      return state_.test<State>(orders...);
    }

    /// @brief Releases the slot.
    /// @details Requires the slot to be in the `active` state. Destroys the stored value if it is
    /// not trivially destructible, sets the state to `empty`, and notifies any waiters. In debug
    /// mode, resets the value to zero to aid in detecting use-after-free errors.
    /// @tparam `State` State to transition.
    template<slot_state State>
    inline void
    release() noexcept
    {
      // Must be active
      assert(state_.template test<State>(std::memory_order_acquire) and "ring_slot::release()");
      // Free up slot for re-use.
      if constexpr (!std::is_trivially_destructible_v<T>)
      {
        std::destroy_at<T>(data());
      }
#ifndef NDEBUG
      value_ = {};
#endif
      auto expected = State;
      while (!state_.compare_exchange_weak(expected, slot_state::free, std::memory_order_acq_rel,
                                           std::memory_order_acquire)) [[likely]]
      {
        expected = State;
      }
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

    /// @brief Gets a pointer to the stored value.
    /// @details Returns a pointer to the memory where the value is stored. Ensure the slot is in
    /// an appropriate state (e.g., `active`) before accessing to avoid undefined behavior.
    inline constexpr auto
    data() noexcept -> T*
    {
      return reinterpret_cast<T*>(value_.data()); // NOLINT
    }

    /// @brief Gets a const pointer to the stored value.
    /// @details Returns a const pointer to the memory where the value is stored. Ensure the slot
    /// is in an appropriate state (e.g., `active`) before accessing to avoid undefined behavior.
    inline constexpr auto
    data() const noexcept -> T const*
    {
      return reinterpret_cast<T const*>(value_.data()); // NOLINT
    }

    /// @brief Gets a reference to the stored value.
    /// @details Returns a reference to the stored value. Ensure the slot is in an appropriate
    /// state (e.g., `active`) before accessing to avoid undefined behavior.
    inline constexpr auto
    value() noexcept -> T&
    {
      return *data(); // NOLINT
    }

    /// @brief Gets a const reference to the stored value.
    /// @details Returns a const reference to the stored value. Ensure the slot is in an
    /// appropriate state (e.g., `active`) before accessing to avoid undefined behavior.
    inline constexpr auto
    value() const noexcept -> T const&
    {
      return *data(); // NOLINT
    }

  private:
    /// Atomic state of the slot (e.g., `empty`, `free`, `claimed`, `active`).
    fho::atomic_state_t state_{slot_state::free};
    /// Aligned storage for the value of type `T`.
    alignas(T) std::array<std::byte, sizeof(T)> value_;
  };

  template<typename T>
  inline constexpr auto slot_value_accessor = [](auto&& a) -> std::add_lvalue_reference_t<T>
  {
    return FWD(a).value();
  };

  /// @brief A scoped RAII wrapper for a borrowed `ring_slot` pointer, ensuring release on
  /// destruction.
  /// @details The `claimed_slot` class provides temporary access to a `ring_slot`'s value,
  /// automatically releasing the slot when it goes out of scope. It supports dereferencing, arrow
  /// access, and invocation if the value is callable. Move-only to transfer ownership safely.
  /// @tparam `Slot` The slot type, typically `ring_slot<T>`.
  /// @note Assumes the slot is in `active` state when lent; undefined otherwise.
  /// @example
  /// ```cpp
  /// auto slot = queue.try_pop();
  /// if (slot) {
  ///   slot();  // If callable
  /// }  // Auto-releases
  /// ```
  template<typename T>
  class alignas(details::cache_line_size) claimed_slot
  {
    using Slot  = ring_slot<T>;
    Slot* slot_ = nullptr;

  public:
    claimed_slot(Slot* slot)
      : slot_(slot)
    {
      assert((!slot_ || slot_->template test<slot_state::claimed>(std::memory_order_acquire)) and
             "claimed_slot::claimed_slot()");
    }

    claimed_slot(claimed_slot&& other) noexcept
      : slot_(std::exchange(other.slot_, nullptr))
    {
      assert((!slot_ || slot_->template test<slot_state::claimed>(std::memory_order_acquire)) and
             "claimed_slot::claimed_slot()");
    }

    auto
    operator=(claimed_slot&& other) noexcept -> claimed_slot&
    {
      if (this != &other) [[likely]]
      {
        if (slot_) [[unlikely]]
        {
          slot_->template release<slot_state::claimed>();
        }
        slot_ = std::exchange(other.slot_, nullptr);
      }
      return *this;
    }

    claimed_slot(claimed_slot const&)                    = delete;
    auto operator=(claimed_slot const&) -> claimed_slot& = delete;

    ~claimed_slot()
    {
      if (slot_)
      {
        slot_->template release<slot_state::claimed>();
      }
    }

    [[nodiscard]] inline constexpr explicit
    operator bool() const noexcept
    {
      return slot_ != nullptr;
    }

    auto
    operator*() noexcept -> decltype(auto)
    {
      return slot_->value();
    }

    auto
    operator*() const noexcept -> decltype(auto)
    {
      return slot_->value();
    }

    auto
    operator->() noexcept -> decltype(auto)
    {
      return &slot_->value();
    }

    auto
    operator->() const noexcept -> decltype(auto)
    {
      return &slot_->value();
    }

    template<typename... Args>
    auto
    operator()(Args&&... args) noexcept -> decltype(auto)
      requires std::invocable<decltype(**this), Args...>
    {
      return std::invoke(**this, std::forward<Args>(args)...);
    }

    template<typename... Args>
    auto
    operator()(Args&&... args) const noexcept -> decltype(auto)
      requires std::invocable<decltype(**this) const, Args...>
    {
      return std::invoke(**this, std::forward<Args>(args)...);
    }
  };
}

#undef FWD

#ifdef _MSC_VER
  #pragma warning(pop)
#endif
