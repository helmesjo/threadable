#pragma once

#include <threadable/atomic.hxx>
#include <threadable/function.hxx>

#include <algorithm>

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

namespace threadable
{
  namespace details
  {
    struct job_base
    {
      atomic_bitfield_t state;
    };

    static constexpr auto job_buffer_size =
      cache_line_size - sizeof(job_base) - sizeof(function<0>);
  }

  enum job_state : std::uint8_t
  {
    active = 0
  };

  struct alignas(details::cache_line_size) job final : details::job_base
  {
    using function_t = function<details::job_buffer_size>;

    job() = default;

    ~job()
    {
      reset();
    }

    job(job&&)                          = delete;
    job(job const&)                     = delete;
    auto operator=(job&&) -> auto&      = delete;
    auto operator=(job const&) -> auto& = delete;

    template<typename callable_t, typename... arg_ts>
      requires std::invocable<callable_t, arg_ts...>
    auto
    set(callable_t&& func, arg_ts&&... args) noexcept -> decltype(auto)
    {
      func_.set(FWD(func), FWD(args)...);
      details::set<job_state::active, true>(state, std::memory_order_release);
      // NOTE: Intentionally not notifying here since that is redundant (and costly),
      //       it is designed to be waited on (checking state true -> false)
      // details::atomic_notify_all(active);
    }

    template<typename callable_t>
      requires std::invocable<callable_t>
    auto
    operator=(callable_t&& func) noexcept -> auto&
    {
      set(FWD(func));
      return *this;
    }

    auto
    operator=(std::nullptr_t) noexcept -> auto&
    {
      reset();
      return *this;
    }

    void
    reset() noexcept
    {
      func_.reset();
      details::clear(state, std::memory_order_release);
      details::atomic_notify_all(state);
    }

    auto
    done() const noexcept -> bool
    {
      return !details::test<job_state::active>(state, std::memory_order_acquire);
    }

    void
    operator()()
    {
      assert(func_);
      assert(!done());

      func_();
      reset();
    }

    operator bool() const noexcept
    {
      return !done();
    }

    auto
    get() noexcept -> auto&
    {
      return func_;
    }

  private:
    function_t func_;
  };

  static_assert(sizeof(job) == details::cache_line_size, "job size must equal cache line size");

  struct job_token
  {
    job_token()                 = default;
    job_token(job_token const&) = delete;
    ~job_token()                = default;

    job_token(details::atomic_bitfield_t& state)
      : state_(&state)
    {}

    job_token(job_token&& rhs) noexcept
      : cancelled_(rhs.cancelled_.load(std::memory_order_acquire))
      , state_(rhs.state_.load(std::memory_order_acquire))
    {
      rhs.state_.store(nullptr, std::memory_order_release);
    }

    auto operator=(job_token const&) -> job_token& = delete;

    auto
    operator=(job_token&& rhs) noexcept -> auto&
    {
      cancelled_.store(rhs.cancelled_, std::memory_order_release);
      state_.store(rhs.state_, std::memory_order_release);
      rhs.state_.store(nullptr, std::memory_order_release);
      return *this;
    }

    void
    reassign(details::atomic_bitfield_t& state) noexcept
    {
      state_.store(&state, std::memory_order_release);
    }

    auto
    done() const noexcept -> bool
    {
      auto state = state_.load(std::memory_order_acquire);
      return !state || !details::test<job_state::active>(*state, std::memory_order_acquire);
    }

    void
    cancel() noexcept
    {
      details::atomic_set(cancelled_, std::memory_order_release);
    }

    auto
    cancelled() const noexcept -> bool
    {
      return details::atomic_test(cancelled_, std::memory_order_acquire);
    }

    void
    wait() noexcept
    {
      // take into account that the underlying state-ptr might have
      // been re-assigned while waiting (eg. for a recursive/self-queueing job)
      auto state = state_.load(std::memory_order_acquire);
      while (state)
      {
        details::wait<job_state::active, true>(*state, std::memory_order_acquire);

        if (auto next = state_.load(std::memory_order_acquire); next == state) [[likely]]
        {
          break;
        }
        else [[unlikely]]
        {
          state = next;
        }
      }
    }

  private:
    details::atomic_flag_t                   cancelled_ = false;
    std::atomic<details::atomic_bitfield_t*> state_     = nullptr;
  };

  static_assert(std::move_constructible<job_token>);
  static_assert(std::is_move_assignable_v<job_token>);

  struct token_group
  {
    auto
    operator+=(job_token&& token) noexcept -> token_group&
    {
      tokens_.emplace_back(std::move(token));
      return *this;
    }

    [[nodiscard]] auto
    done() const noexcept -> bool
    {
      return std::ranges::all_of(tokens_,
                                 [](auto const& token)
                                 {
                                   return token.done();
                                 });
    }

    void
    cancel() noexcept
    {
      for (auto& token : tokens_)
      {
        token.cancel();
      }
    }

    void
    wait() noexcept
    {
      for (auto& token : tokens_)
      {
        token.wait();
      }
    }

  private:
    std::vector<job_token> tokens_;
  };
}

#undef FWD
