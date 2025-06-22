#pragma once

#include <threadable/atomic.hxx>
#include <threadable/function.hxx>

#include <algorithm>
#include <atomic>

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

namespace fho
{

  /// @brief Defines possible states for a job in the threading library.
  /// @details Represents job lifecycle states: empty (no callable), active (ready or running), and
  /// claimed (reserved or in progress). States may combine via bitwise operations.
  enum job_state : std::uint8_t
  {
    /// @brief Job is empty, with no callable assigned.
    empty = 0,
    /// @brief Job is active, with a callable ready or executing.
    active = 1 << 0,
    /// @brief Job is claimed, reserved or being processed.
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

  /// @brief A token representing a claim on a job's state.
  /// @details The `job_token` class allows for monitoring and controlling the state of a job,
  /// including waiting for completion, cancelling, and checking if it's done or cancelled. It is
  /// move-only to ensure exclusive ownership.
  /// @example
  /// ```cpp
  /// auto j = fho::job{};
  /// auto t = fho::job_token(my_job.state);
  /// j.assign([]() { /* job work */ });
  /// t.wait(); // Waits for the job to complete
  /// ```
  struct job_token
  {
    /// @brief Default constructor.
    /// @details Initializes the token with no associated job.
    job_token() = default;

    /// @brief Deleted copy constructor.
    /// @details Tokens cannot be copied.
    job_token(job_token const&) = delete;

    /// @brief Destructor.
    /// @details Releases any resources held by the token.
    ~job_token() = default;

    /// @brief Constructor from atomic state.
    /// @details Initializes the token with the given job state.
    /// @param `state` The atomic state of the job.
    job_token(atomic_state_t& state)
      : state_(&state)
    {}

    /// @brief Move constructor.
    /// @details Transfers ownership of the token's state.
    /// @param `rhs` The token to move from.
    job_token(job_token&& rhs) noexcept
      : cancelled_(rhs.cancelled_.load(std::memory_order_relaxed))
      , state_(rhs.state_.load(std::memory_order_relaxed))
    {
      rhs.state_.store(nullptr, std::memory_order_relaxed);
    }

    /// @brief Deleted copy assignment.
    /// @details Tokens cannot be copied.
    auto operator=(job_token const&) -> job_token& = delete;

    /// @brief Move assignment.
    /// @details Transfers ownership of the token's state.
    /// @param `rhs` The token to move from.
    /// @return A reference to this token.
    auto
    operator=(job_token&& rhs) noexcept -> auto&
    {
      cancelled_.store(rhs.cancelled_, std::memory_order_relaxed);
      state_.store(rhs.state_, std::memory_order_relaxed);
      rhs.state_.store(nullptr, std::memory_order_relaxed);
      return *this;
    }

    /// @brief Reassigns the token to a different job state.
    /// @details Updates the internal state pointer.
    /// @param `state` The new atomic state of the job.
    void
    reassign(atomic_state_t& state) noexcept
    {
      state_.store(&state, std::memory_order_release);
    }

    /// @brief Checks if the associated job is done.
    /// @details Returns true if the job state is not active or if no job is associated.
    auto
    done() const noexcept -> bool
    {
      auto state = state_.load(std::memory_order_acquire);
      return !state || state->load(std::memory_order_acquire) != 1;
    }

    /// @brief Cancels the associated job.
    /// @details Sets the cancellation flag.
    void
    cancel() noexcept
    {
      details::atomic_set(cancelled_, std::memory_order_release);
    }

    /// @brief Checks if the job has been cancelled.
    /// @details Returns the cancellation flag status.
    auto
    cancelled() const noexcept -> bool
    {
      return details::atomic_test(cancelled_, std::memory_order_acquire);
    }

    /// @brief Waits for the associated job to complete.
    /// @details Blocks until the job state changes from active.
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

  /// @brief A group of job tokens, allowing collective operations on multiple jobs.
  /// @details The `token_group` class manages a collection of `job_token` objects, providing
  /// methods to check if all jobs are done, cancel all jobs, or wait for all jobs to complete.
  /// @example
  /// ```cpp
  /// auto group = fho::token_group{};
  /// group += std::move(token1);
  /// group += std::move(token2);
  /// group.wait(); // Waits for both jobs to complete
  /// ```
  struct token_group
  {
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

    /// @brief Adds a job token to the group.
    /// @details Appends the token to the internal vector.
    /// @param `token` The job token to add.
    /// @return A reference to this token group.
    auto
    operator+=(job_token&& token) noexcept -> token_group&
    {
      tokens_.emplace_back(std::move(token));
      return *this;
    }

    /// @brief Checks if all jobs in the group are done.
    /// @details Returns true if every token in the group reports that its job is done.
    [[nodiscard]] auto
    done() const noexcept -> bool
    {
      return std::ranges::all_of(tokens_,
                                 [](auto const& token)
                                 {
                                   return token.done();
                                 });
    }

    /// @brief Cancels all jobs in the group.
    /// @details Calls `cancel` on each token in the group.
    void
    cancel() noexcept
    {
      for (auto& token : tokens_)
      {
        token.cancel();
      }
    }

    /// @brief Waits for all jobs in the group to complete.
    /// @details Calls `wait` on each token in the group.
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
