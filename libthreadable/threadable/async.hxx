#pragma once

#include <threadable/execution.hxx>
#include <threadable/pool.hxx>

#include <concepts>
#include <cstddef>

#ifdef _MSC_VER
  #pragma warning(push)
  #pragma warning(disable : 4324)
#endif

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

namespace fho
{
  /// @brief Submits a task to a queue with the specified policy in the default thread pool.
  /// @details Uses a static `ring_buffer` instance for each unique `Policy`, creating it if
  /// necessary, and emplaces the callable with its arguments into that queue.
  /// @tparam Policy The execution policy for the queue, defaults to `execution::par`.
  /// @tparam Func The type of the callable, must be invocable with `args`.
  /// @tparam Args The types of the arguments.
  /// @param func The callable to submit.
  /// @param args The arguments to pass to the callable.
  /// @return A `slot_token` representing the submitted task.
  template<execution Policy = execution::par, typename... Args>
  [[nodiscard]] inline auto
  async(std::invocable<Args...> auto&& func, Args&&... args) noexcept -> decltype(auto)
  {
    static auto& queue = create(Policy);
    return queue.emplace_back(FWD(func), FWD(args)...);
  }

  /// @brief Submits a task to a queue with the specified policy, reusing a token.
  /// @details Similar to the other `async` overload but reuses an existing `slot_token`, which is
  /// (optionally) passed by reference as the first argument to the callable.
  /// @tparam Policy The execution policy for the queue, defaults to `execution::par`.
  /// @tparam Func The type of the callable, must be invocable with `args`.
  /// @tparam Args The types of the arguments.
  /// @param func The callable to submit.
  /// @param token Reference to a reusable `slot_token`.
  /// @param args Additional arguments to pass to the callable.
  /// @return Reference to the reused `token`.
  template<execution Policy = execution::par, typename... Args>
  inline auto
  async(slot_token& token, std::invocable<Args...> auto&& func, Args&&... args) noexcept
    -> decltype(auto)
  {
    static auto& queue = create(Policy);
    return queue.emplace_back(token, FWD(func), FWD(args)...);
  }

  /// @brief Submits a task to a queue with the specified policy, reusing a token, and re-submits it
  /// until cancelled.
  /// @details Submits a callable to a queue with the given execution policy, reusing an existing
  /// `slot_token`. The task is automatically re-submitted after each execution unless the token is
  /// cancelled (e.g., `token.cancelled() == true` once `token.cancel()` has been called by the
  /// user).
  /// @tparam Policy The execution policy for the queue, defaults to `execution::par`.
  /// @tparam Args The types of the arguments.
  /// @param token Reference to a reusable `slot_token` for tracking and cancellation.
  /// @param func The callable to submit, invocable `args`.
  /// @param args Additional arguments to pass to the callable.
  /// @example
  /// ```cpp
  /// auto token = fho::slot_token{};
  /// auto count = 0;
  /// fho::repeat_async(token, [&count, &token]() {
  ///     if (++count >= 5) token.cancel();
  /// });
  /// token.wait(); // Runs 5 times
  /// // count == 5
  /// ```
  template<execution Policy = execution::par, typename... Args>
  inline void
  repeat_async(slot_token& token, std::invocable<Args...> auto&& func, Args&&... args) noexcept
  {
    static auto& queue = create(Policy);

    auto lambda = [](int counter, auto&& self, fho::slot_token& token, decltype(func) func,
                     decltype(args)... args) -> void
    {
      FWD(func)(FWD(args)...);

      if (!token.cancelled())
      {
        queue.emplace_back(token, self, ++counter, self, std::ref(token), FWD(func), FWD(args)...);
      }
    };
    queue.emplace_back(token, lambda, 0, lambda, std::ref(token), FWD(func), FWD(args)...);
  }

  /// @brief Executes a range of callables with specified execution policy.
  /// @details Invokes each callable in the provided range with the given arguments.
  ///          Supports sequential (`seq`) or parallel (`par`) execution.
  /// @tparam R The type of the range containing invocable objects.
  /// @param exPo The execution policy (`fho::execution::seq` or `fho::execution::par`).
  /// @param r The range of callables to execute.
  /// @tparam Args Variadic argument types to pass to each callable.
  /// @param args Arguments forwarded to each callable invocation.
  /// @return The number of callables executed, equivalent to the range's size.
  template<std::ranges::range R, typename... Args>
  inline constexpr auto
  execute(execution exPo, R&& r, Args&&... args)
    requires std::invocable<std::ranges::range_value_t<R>, Args...>
  {
    auto  tokens = token_group{};
    auto& queue  = create(exPo);
    for (auto&& c : FWD(r))
    {
      tokens += queue.emplace_back(FWD(c), FWD(args)...);
    }
    tokens.wait();
    return r.size();
  }

  /// @brief Executes a range of callables with default sequential execution.
  /// @details Invokes each callable in the range sequentially with the provided arguments.
  /// @tparam R The type of the range containing invocable objects.
  /// @param r The range of callables to execute.
  /// @tparam Args Variadic argument types to pass to each callable.
  /// @param args Arguments forwarded to each callable invocation.
  /// @return The number of callables executed, equivalent to the range's size.
  template<std::ranges::range R, typename... Args>
  inline constexpr auto
  execute(R&& r, Args&&... args)
    requires std::invocable<std::ranges::range_value_t<R>, Args...>
  {
    return execute(execution::seq, FWD(r), FWD(args)...);
  }
}

#undef FWD

#ifdef _MSC_VER
  #pragma warning(pop)
#endif
