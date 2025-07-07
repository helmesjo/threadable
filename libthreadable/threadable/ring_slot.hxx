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

#ifdef _WIN32
  #pragma warning(push)
  #pragma warning(disable : 4324)
#endif

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

namespace fho
{
  template<typename T>
  class alignas(details::cache_line_size) ring_slot
  {
  public:
    ring_slot() = default;

    ring_slot(fho::atomic_state_t state, T value)
      : state_(state.load(std::memory_order_relaxed))
      , value_(std::move(value))
    {}

    ring_slot(ring_slot&& that) noexcept
      : state_(that.state_.load(std::memory_order_relaxed))
      , value_(std::move(that.value_))
    {}

    ring_slot(ring_slot const&) = delete;
    ~ring_slot()                = default;

    auto operator=(ring_slot const&) -> ring_slot& = delete;

    inline auto
    operator=(ring_slot&& that) noexcept -> ring_slot&
    {
      state_ = that.state_.load(std::memory_order_relaxed);
      value_ = std::move(that.value_);
      return *this;
    }

    inline
    operator T&() noexcept
    {
      return value();
    }

    inline
    operator T const&() const noexcept
    {
      return value();
    }

    inline void
    acquire() noexcept
    {
      auto exp = slot_state::empty;
      while (!state_.compare_exchange_weak(exp, slot_state::claimed, std::memory_order_release,
                                           std::memory_order_relaxed)) [[likely]]
      {
        exp = slot_state::empty;
      }
    }

    inline auto
    assign(auto&&... args) noexcept -> decltype(auto)
    {
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
      // Must be claimed
      assert(state_.load(std::memory_order_acquire) == slot_state::claimed);

      std::construct_at<T>(data(), FWD(args)...);
      state_.store(slot_state::active, std::memory_order_release);
      // NOTE: Intentionally not notifying here since that is redundant (and costly),
      //       it is designed to be waited on by checking state active -> inactive.
      // state.notify_all();
    }

    inline void
    wait() const noexcept
    {
      state_.template wait<slot_state::active, true>(std::memory_order_acquire);
    }

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
      state_.store(slot_state::empty, std::memory_order_release);
      state_.notify_all();
    }

    inline void
    token(slot_token& t) noexcept
    {
      t.assign(state_);
    }

    inline constexpr auto
    data() noexcept -> T*
    {
      return reinterpret_cast<T*>(value_.data()); // NOLINT
    }

    inline constexpr auto
    data() const noexcept -> T const*
    {
      return reinterpret_cast<T const*>(value_.data()); // NOLINT
    }

    inline constexpr auto
    value() noexcept -> T&
    {
      return *data(); // NOLINT
    }

    inline constexpr auto
    value() const noexcept -> T const&
    {
      return *data(); // NOLINT
    }

  private:
    fho::atomic_state_t state_;
    alignas(T) std::array<std::byte, sizeof(T)> value_;
  };
}

#undef FWD

#ifdef _WIN32
  #pragma warning(pop)
#endif
