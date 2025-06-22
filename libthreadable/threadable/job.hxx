#pragma once

#include <threadable/atomic.hxx>
#include <threadable/function.hxx>

#include <algorithm>
#include <atomic>

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

namespace fho
{

  enum job_state : std::uint8_t
  {
    empty   = 0,
    active  = 1 << 0,
    claimed = 1 << 1
  };

  static_assert(sizeof(job_state) == 1);

  using atomic_state_t = std::atomic<job_state>;

  namespace details
  {
    struct job_base
    {
      fho::atomic_state_t state;
    };

    static constexpr auto job_buffer_size =
      cache_line_size - sizeof(job_base) - sizeof(function<0>);
  }

  /// @brief A class representing a job that can be executed, with state management.
  /// @details The `job` class encapsulates a callable object and its state, allowing for execution,
  /// state checking, and reset. It is designed to be used in a threading context, where jobs can be
  /// queued and executed by threads. The class ensures thread-safe operations and is aligned to
  /// cache line boundaries for performance.
  /// @example
  /// ```cpp
  /// auto j = fho::job{};
  /// j.assign([]() { cout << "Job executed!\n"; });
  /// j();  // Executes the job
  /// ```
  struct alignas(details::cache_line_size) job final : details::job_base
  {
    using function_t = function<details::job_buffer_size>;

    /// @brief Default constructor.
    /// @details Initializes the job with an empty state.
    job() = default;

    /// @brief Destructor.
    /// @details Resets the job to ensure proper cleanup.
    ~job()
    {
      reset();
    }

    job(job&&)                          = delete;
    job(job const&)                     = delete;
    auto operator=(job&&) -> auto&      = delete;
    auto operator=(job const&) -> auto& = delete;

    /// @brief Assigns a callable to the job.
    /// @details Stores the callable and its arguments, setting the job state to active.
    /// @tparam `callable_t` The type of the callable.
    /// @tparam `arg_ts` The types of the arguments.
    /// @param `func` The callable to store.
    /// @param `args` The arguments to bind to the callable.
    /// @return A reference to this job.
    template<typename callable_t, typename... arg_ts>
      requires std::invocable<callable_t, arg_ts...>
    auto
    assign(callable_t&& func, arg_ts&&... args) noexcept -> decltype(auto)
    {
      func_.assign(FWD(func), FWD(args)...);
      state.store(job_state::active, std::memory_order_relaxed);
      // NOTE: Intentionally not notifying here since that is redundant (and costly),
      //       it is designed to be waited on by checking state active -> inactive.
      // details::atomic_notify_all(active);
    }

    /// @brief Assigns a callable to the job.
    /// @details Stores the callable, setting the job state to active.
    /// @tparam `callable_t` The type of the callable.
    /// @param `func` The callable to store.
    /// @return A reference to this job.
    template<typename callable_t>
      requires std::invocable<callable_t>
    auto
    operator=(callable_t&& func) noexcept -> auto&
    {
      assign(FWD(func));
      return *this;
    }

    /// @brief Resets the job to an empty state.
    /// @details Clears the stored callable and sets the state to empty.
    /// @return A reference to this job.
    auto
    operator=(std::nullptr_t) noexcept -> auto&
    {
      reset();
      return *this;
    }

    /// @brief Executes the stored callable.
    /// @details Invokes the callable, resets the job & notifies waiting threads if successful.
    void
    operator()() noexcept
    {
      assert(func_);
      assert(!done());

      func_();
      if (reset() != job_state::empty) [[likely]]
      {
        state.notify_all();
      }
    }

    /// @brief Checks if the job is active.
    /// @details Returns true if the job state is active, false otherwise.
    operator bool() const noexcept
    {
      return !done();
    }

    /// @brief Resets the job to an empty state.
    /// @details Clears the stored callable and sets the state to empty.
    /// @return The previous state of the job.
    auto
    reset() noexcept -> job_state
    {
      func_.reset();
      return state.exchange(job_state::empty, std::memory_order_release);
    }

    /// @brief Checks if the job is done.
    /// @details Returns true if the job state is not active.
    auto
    done() const noexcept -> bool
    {
      return state.load(std::memory_order_acquire) != job_state::active;
    }

    /// @brief Gets the internal function object.
    /// @details Provides access to the stored callable for inspection or modification.
    /// @return A reference to the internal function object.
    auto
    get() noexcept -> auto&
    {
      return func_;
    }

  private:
    function_t func_;
  };

  static_assert(sizeof(job) == details::cache_line_size, "job size must equal cache line size");
  static_assert(alignof(job) == details::cache_line_size,
                "job must be aligned to cache line boundaries");

  struct job_token
  {
    job_token()                 = default;
    job_token(job_token const&) = delete;
    ~job_token()                = default;

    job_token(atomic_state_t& state)
      : state_(&state)
    {}

    job_token(job_token&& rhs) noexcept
      : cancelled_(rhs.cancelled_.load(std::memory_order_relaxed))
      , state_(rhs.state_.load(std::memory_order_relaxed))
    {
      rhs.state_.store(nullptr, std::memory_order_relaxed);
    }

    auto operator=(job_token const&) -> job_token& = delete;

    auto
    operator=(job_token&& rhs) noexcept -> auto&
    {
      cancelled_.store(rhs.cancelled_, std::memory_order_relaxed);
      state_.store(rhs.state_, std::memory_order_relaxed);
      rhs.state_.store(nullptr, std::memory_order_relaxed);
      return *this;
    }

    void
    reassign(atomic_state_t& state) noexcept
    {
      state_.store(&state, std::memory_order_release);
    }

    auto
    done() const noexcept -> bool
    {
      auto state = state_.load(std::memory_order_acquire);
      return !state || state->load(std::memory_order_acquire) != 1;
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
        state->wait(job_state::active, std::memory_order_acquire);

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
    details::atomic_flag_t       cancelled_ = false;
    std::atomic<atomic_state_t*> state_     = nullptr;
  };

  static_assert(std::move_constructible<job_token>);
  static_assert(std::is_move_assignable_v<job_token>);

  struct token_group
  {
    token_group()  = default;
    ~token_group() = default;

    token_group(token_group const&)                    = default;
    token_group(token_group&&)                         = default;
    auto operator=(token_group const&) -> token_group& = default;
    auto operator=(token_group&&) -> token_group&      = default;

    token_group(std::size_t capacity)
      : tokens_(capacity)
    {}

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
