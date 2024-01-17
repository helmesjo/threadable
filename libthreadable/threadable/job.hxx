#pragma once

#include <threadable/atomic.hxx>
#include <threadable/function.hxx>

#include <algorithm>

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

#if __has_cpp_attribute(likely) && __has_cpp_attribute(unlikely)
#define LIKELY [[likely]]
#define UNLIKELY [[unlikely]]
#else
#define LIKELY
#define UNLIKELY
#endif

namespace threadable
{
  namespace details
  {
    struct job_base
    {
      atomic_bitfield states;
      std::atomic<atomic_bitfield*> child_states = nullptr;
    };
    static constexpr auto job_buffer_size = cache_line_size - sizeof(job_base) - sizeof(function<0>);
  }

  enum job_state: std::uint8_t
  {
    active = 0
  };

  struct alignas(details::cache_line_size) job final: details::job_base
  {
    using function_t = function<details::job_buffer_size>;

    job() = default;
    job(job&&) = delete;
    job(const job&) = delete;
    auto operator=(job&&) = delete;
    auto operator=(const job&) = delete;

    template<typename callable_t, typename... arg_ts>
      requires std::invocable<callable_t, arg_ts...>
    decltype(auto) set(callable_t&& func, arg_ts&&... args) noexcept
    {
      func_.set(FWD(func), FWD(args)...);
      details::set<job_state::active, true>(states, std::memory_order_release);
      // NOTE: Intentionally not notifying here since that is redundant (and costly),
      //       it is designed to be waited on (checking state true -> false)
      // details::atomic_notify_all(active);
    }

    inline void wait_for(details::atomic_bitfield& child)
    {
      child_states.store(&child, std::memory_order_release);
    }

    template<typename callable_t>
      requires std::invocable<callable_t>
    auto& operator=(callable_t&& func) noexcept
    {
      set(FWD(func));
      return *this;
    }

    auto& operator=(std::nullptr_t) noexcept
    {
      reset();
      return *this;
    }

    void reset() noexcept
    {
      func_.reset();
      child_states.store(nullptr, std::memory_order_release);
      details::clear(states, std::memory_order_release);
      details::atomic_notify_all(states);
    }

    bool done() const noexcept
    {
      return !details::test<job_state::active>(states, std::memory_order_acquire);
    }

    void operator()()
    {
      assert(func_);
      assert(!done());

      if(auto flag = child_states.load(std::memory_order_acquire))
      UNLIKELY
      {
        details::wait<job_state::active, true>(*flag, std::memory_order_acquire);
      }
      func_();
      reset();
    }

    operator bool() const noexcept
    {
      return !done();
    }

    auto& get() noexcept
    {
      return func_;
    }

  private:
    function_t func_;
  };
  static_assert(sizeof(job) == details::cache_line_size, "job size must equal cache line size");

  struct job_token
  {
    job_token() = default;

    job_token(details::atomic_bitfield& states):
      states(&states)
    {
    }

    job_token(job_token&& rhs) noexcept:
      cancelled_(rhs.cancelled_.load(std::memory_order_acquire)),
      states(rhs.states.load(std::memory_order_acquire))
    {
      rhs.states.store(nullptr, std::memory_order_release);
    }

    auto& operator=(job_token&& rhs) noexcept
    {
      cancelled_.store(rhs.cancelled_, std::memory_order_release);
      states.store(rhs.states, std::memory_order_release);
      rhs.states.store(nullptr, std::memory_order_release);
      return *this;
    }

    void reassign(details::atomic_bitfield& newStates) noexcept
    {
      states.store(&newStates, std::memory_order_release);
    }

    bool done() const noexcept
    {
      auto statesPtr = states.load(std::memory_order_acquire);
      return !statesPtr || !details::test<job_state::active>(*statesPtr, std::memory_order_acquire);
    }

    void cancel() noexcept
    {
      details::atomic_set(cancelled_, std::memory_order_release);
    }

    bool cancelled() const noexcept
    {
      return details::atomic_test(cancelled_, std::memory_order_acquire);
    }

    void wait() const noexcept
    {
      // take into account that the underlying states-ptr might have
      // been re-assigned while waiting (eg. for a recursive/self-queueing job)
      auto statesPtr = states.load(std::memory_order_acquire);
      while(statesPtr)
      {
        details::wait<job_state::active, true>(*statesPtr, std::memory_order_acquire);

        if(auto next = states.load(std::memory_order_acquire); next == statesPtr)
        LIKELY
        {
          break;
        }
        else
        UNLIKELY
        {
          statesPtr = next;
        }
      }
    }

  private:
    details::atomic_flag cancelled_ = false;
    std::atomic<details::atomic_bitfield*> states = nullptr;
    inline static details::atomic_bitfield null_flag;
  };
  static_assert(std::move_constructible<job_token>);
  static_assert(std::is_move_assignable_v<job_token>);

  struct token_group
  {
    token_group& operator+=(job_token&& token) noexcept
    {
      tokens_.emplace_back(std::move(token));
      return *this;
    }

    bool done() const noexcept
    {
      return std::ranges::all_of(tokens_, [](const auto& token){
        return token.done();
      });
    }

    void cancel() noexcept
    {
      for(auto& token : tokens_)
      {
        token.cancel();
      }
    }

    void wait() const noexcept
    {
      for(const auto& token : tokens_)
      {
        token.wait();
      }
    }

  private:
    std::vector<job_token> tokens_;
  };
}

#undef FWD
#undef LIKELY
#undef UNLIKELY
