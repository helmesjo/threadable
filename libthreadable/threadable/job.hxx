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
      atomic_bitfield_t states;
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

    job()                               = default;
    ~job()                              = default;
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
      details::set<job_state::active, true>(states, std::memory_order_release);
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
      details::clear(states, std::memory_order_release);
      details::atomic_notify_all(states);
    }

    auto
    done() const noexcept -> bool
    {
      return !details::test<job_state::active>(states, std::memory_order_acquire);
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

    job_token(details::atomic_bitfield_t& states)
      : states_(&states)
    {}

    job_token(job_token&& rhs) noexcept
      : cancelled_(rhs.cancelled_.load(std::memory_order_acquire))
      , states_(rhs.states_.load(std::memory_order_acquire))
    {
      rhs.states_.store(nullptr, std::memory_order_release);
    }

    auto operator=(job_token const&) -> job_token& = delete;

    auto
    operator=(job_token&& rhs) noexcept -> auto&
    {
      cancelled_.store(rhs.cancelled_, std::memory_order_release);
      states_.store(rhs.states_, std::memory_order_release);
      rhs.states_.store(nullptr, std::memory_order_release);
      return *this;
    }

    void
    reassign(details::atomic_bitfield_t& newStates) noexcept
    {
      states_.store(&newStates, std::memory_order_release);
    }

    auto
    done() const noexcept -> bool
    {
      auto statesPtr = states_.load(std::memory_order_acquire);
      return !statesPtr || !details::test<job_state::active>(*statesPtr, std::memory_order_acquire);
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
    wait() const noexcept
    {
      // take into account that the underlying states-ptr might have
      // been re-assigned while waiting (eg. for a recursive/self-queueing job)
      auto statesPtr = states_.load(std::memory_order_acquire);
      while (statesPtr)
      {
        details::wait<job_state::active, true>(*statesPtr, std::memory_order_acquire);

        if (auto next = states_.load(std::memory_order_acquire); next == statesPtr) [[likely]]
        {
          break;
        }
        else [[unlikely]]
        {
          statesPtr = next;
        }
      }
    }

  private:
    details::atomic_flag_t                   cancelled_ = false;
    std::atomic<details::atomic_bitfield_t*> states_    = nullptr;
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
    wait() const noexcept
    {
      for (auto const& token : tokens_)
      {
        token.wait();
      }
    }

  private:
    std::vector<job_token> tokens_;
  };
}

#undef FWD
