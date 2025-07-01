#pragma once

#include <threadable/atomic.hxx>
#include <threadable/function.hxx>
#include <threadable/token.hxx>

#include <memory>
#include <type_traits>

#ifdef _WIN32
  #pragma warning(push)
  #pragma warning(disable : 4324)
#endif

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

namespace fho
{
  template<typename T>
  struct alignas(details::cache_line_size) ring_slot
  {
    fho::atomic_state_t state;
    T                   value;

    ring_slot() = default;

    ring_slot(fho::atomic_state_t state, T value)
      : state(state.load(std::memory_order_relaxed))
      , value(std::move(value))
    {}

    ring_slot(ring_slot&& that) noexcept
      : state(that.state.load(std::memory_order_relaxed))
      , value(std::move(that.value))
    {}

    ring_slot(ring_slot const&) = delete;
    ~ring_slot()                = default;

    auto operator=(ring_slot const&) -> ring_slot& = delete;

    inline auto
    operator=(ring_slot&& that) noexcept -> ring_slot&
    {
      state = std::move(that.state.load(std::memory_order_relaxed));
      value = std::move(that.value);
    }

    inline
    operator T&() noexcept
    {
      return value;
    }

    inline
    operator T const&() const noexcept
    {
      return value;
    }

    inline void
    acquire() noexcept
    {
      auto exp = slot_state::empty;
      while (!state.compare_exchange_weak(exp, slot_state::claimed, std::memory_order_release,
                                          std::memory_order_relaxed)) [[likely]]
      {
        exp = slot_state::empty;
      }
    }

    inline auto
    assign(auto&&... args) noexcept -> decltype(auto)
    {
      assert(state.load(std::memory_order_acquire) == slot_state::claimed);
      assert(!value);
      std::construct_at(&value, FWD(args)...);
      state.store(slot_state::active, std::memory_order_release);
      // NOTE: Intentionally not notifying here since that is redundant (and costly),
      //       it is designed to be waited on by checking state active -> inactive.
      // state.notify_all();
    }

    inline void
    release() noexcept
    {
      // must be active
      assert(state.test<slot_state::active>());
      // Free up slot for re-use.
      if constexpr (!std::is_trivially_destructible_v<T>)
      {
        std::destroy_at(&value);
      }
      state.store(slot_state::empty, std::memory_order_release);
      state.notify_all();
    }

    inline void
    token(slot_token& t) noexcept
    {
      t.assign(state);
    }
  };
}

#undef FWD

#ifdef _WIN32
  #pragma warning(pop)
#endif
