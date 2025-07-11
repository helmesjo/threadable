#pragma once

#include <threadable/atomic.hxx>
#include <threadable/function.hxx>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace fho
{
  enum slot_state : std::uint_fast8_t
  {
    /// @brief Empty, with no value assigned.
    empty = 0,
    /// @brief Claimed, reserved or being processed.
    claimed = 1 << 0,
    /// @brief Active, with a value ready or being processed.
    active = 1 << 1
  };

  using atomic_state_t = fho::atomic_bitfield<slot_state>;

  /// @brief A token representing a claim on a `ring_slot` state.
  /// @details The `slot_token` class allows for monitoring and controlling the state of a
  /// `ring_slot`, including waiting for completion, cancelling, and checking if it's done or
  /// cancelled. It is move-only to ensure exclusive ownership.
  /// @example
  /// ```cpp
  /// auto s = fho::ring_slot<fho::function<64>>{};
  /// auto t = fho::token{};
  /// s.token(t);
  /// s.emplace([]() { /* task */ });
  /// t.wait(); // Waits for the `ring_slot` to be processed by another thread
  /// ```
  class slot_token
  {
  public:
    /// @brief Default constructor.
    /// @details Initializes the token with no associated `ring_slot`.
    slot_token() = default;

    /// @brief Deleted copy constructor.
    /// @details Tokens cannot be copied.
    slot_token(slot_token const&) = delete;

    /// @brief Destructor.
    /// @details Releases any resources held by the token.
    ~slot_token() = default;

    /// @brief Constructor from atomic state.
    /// @details Initializes the token with the given `ring_slot` state.
    /// @param `state` A reference to the atomic state of the `ring_slot`.
    slot_token(atomic_state_t& state)
      : state_(&state)
    {}

    /// @brief Move constructor.
    /// @details Transfers ownership of the token's state.
    /// @param `rhs` The `slot_token` to move from.
    slot_token(slot_token&& rhs) noexcept
      : cancelled_(rhs.cancelled_.load(std::memory_order_relaxed))
      , state_(rhs.state_.load(std::memory_order_relaxed))
    {
      rhs.state_.store(nullptr, std::memory_order_relaxed);
    }

    /// @brief Deleted copy assignment.
    /// @details Tokens cannot be copied.
    auto operator=(slot_token const&) -> slot_token& = delete;

    /// @brief Move assignment.
    /// @details Transfers ownership of the token's state.
    /// @param `rhs` The `slot_token` to move from.
    /// @return A reference to this token.
    auto
    operator=(slot_token&& rhs) noexcept -> slot_token&
    {
      cancelled_.store(rhs.cancelled_, std::memory_order_relaxed);
      state_.store(rhs.state_, std::memory_order_relaxed);
      rhs.state_.store(nullptr, std::memory_order_relaxed);
      return *this;
    }

    /// @brief Rebinds the token to a different `ring_slot` state.
    /// @details Updates the internal state pointer.
    /// @param `state` A const reference to the new atomic state of the `ring_slot`.
    void
    rebind(atomic_state_t const& state) noexcept
    {
      state_.store(&state, std::memory_order_release);
    }

    /// @brief Checks if the associated `ring_slot` is done.
    /// @details Returns `true` if the `ring_slot` state is not `active` or if no `ring_slot` is
    /// associated.
    [[nodiscard]] auto
    done() const noexcept -> bool
    {
      auto state = state_.load(std::memory_order_acquire);
      return !state || !state->test<slot_state::active>();
    }

    /// @brief Cancels the associated `ring_slot`.
    /// @details Sets the cancellation flag.
    void
    cancel() noexcept
    {
      cancelled_.store(true, std::memory_order_release);
    }

    /// @brief Checks if the `ring_slot` has been cancelled.
    /// @details Returns the cancellation flag status.
    [[nodiscard]] auto
    cancelled() const noexcept -> bool
    {
      return cancelled_.load(std::memory_order_acquire);
    }

    /// @brief Waits for the associated `ring_slot` to be processed.
    /// @details Blocks until the `ring_slot` state changes from `active`.
    /// NOTE: The underlying state pointer might be rebound during waiting, for example, in
    /// recursive or self-queueing tasks. This function will wait until it's fully processed.
    void
    wait() const noexcept
    {
      auto state = state_.load(std::memory_order_acquire);
      state->wait<slot_state::active, true>(std::memory_order_acquire);
    }

  private:
    std::atomic_bool                   cancelled_ = false;
    std::atomic<atomic_state_t const*> state_     = nullptr;
  };

  static_assert(std::move_constructible<slot_token>);
  static_assert(std::is_move_assignable_v<slot_token>);

  /// @brief A group of `ring_slot` tokens, allowing collective operations on multiple tasks.
  /// @details The `token_group` class manages a collection of `slot_token` objects, providing
  /// methods to check if all tasks are done, cancel all tasks, or wait for all tasks to complete.
  /// @example
  /// ```cpp
  /// auto group = fho::token_group{};
  /// group += std::move(token1);
  /// group += std::move(token2);
  /// group.wait(); // Waits for both tasks to complete
  /// ```
  class token_group
  {
  public:
    /// @brief Default constructor.
    /// @details Initializes an empty token group.
    token_group() = default;

    /// @brief Destructor.
    /// @details Releases any resources held by the group.
    ~token_group() = default;

    /// @brief Copy constructor.
    /// @details Creates a copy of the token group.
    token_group(token_group const&) = default;

    /// @brief Move constructor.
    /// @details Moves the contents of another token group into this one.
    token_group(token_group&&) = default;

    /// @brief Copy assignment.
    /// @details Assigns the contents of another token group to this one.
    auto operator=(token_group const&) -> token_group& = default;

    /// @brief Move assignment.
    /// @details Moves the contents of another token group into this one.
    auto operator=(token_group&&) -> token_group& = default;

    /// @brief Constructor with capacity.
    /// @details Initializes the token group with the specified capacity.
    /// @param `capacity` The initial capacity of the group.
    token_group(std::size_t capacity)
      : tokens_(capacity)
    {}

    /// @brief Adds a `token` to the group.
    /// @details Appends the token to the internal vector.
    /// @param `token` The `slot_token` to add.
    /// @return A reference to this token group.
    auto
    operator+=(slot_token&& token) noexcept -> token_group&
    {
      tokens_.emplace_back(std::move(token));
      return *this;
    }

    /// @brief Returns the number of tokens in the group.
    [[nodiscard]] auto
    size() const noexcept -> std::size_t
    {
      return tokens_.size();
    }

    /// @brief Checks if all tasks in the group are done.
    /// @details Returns `true` if every token in the group reports that its state is done.
    [[nodiscard]] auto
    done() const noexcept -> bool
    {
      return std::ranges::all_of(tokens_,
                                 [](auto const& token)
                                 {
                                   return token.done();
                                 });
    }

    /// @brief Cancels all tasks in the group.
    /// @details Calls `cancel` on each token in the group.
    void
    cancel() noexcept
    {
      for (auto& token : tokens_)
      {
        token.cancel();
      }
    }

    /// @brief Waits for all tasks in the group to complete.
    /// @details Calls `wait` on each token in the group.
    void
    wait() const noexcept
    {
      for (auto& token : tokens_)
      {
        token.wait();
      }
    }

  private:
    std::vector<slot_token> tokens_;
  };
}
