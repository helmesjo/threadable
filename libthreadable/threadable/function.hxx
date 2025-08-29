#pragma once

#include <threadable/std_traits.hxx>

#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception> // std::terminate()
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <version>

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

namespace fho
{
  template<std::size_t>
  class function;

  namespace details
  {
    // @NOTE: GCC/Clang incorrectly reports 64 bytes when targeting Apple Silicon.
#if __cpp_lib_hardware_interference_size >= 201603 && !defined(__APPLE__)
    constexpr auto cache_line_size = std::hardware_destructive_interference_size;
#else
  #if defined(__x86_64__) || defined(_M_X64)
    constexpr auto cache_line_size = std::size_t{64}; // Common for x86-64 (Intel, AMD)
  #elif defined(__aarch64__) || defined(_M_ARM64)
    #if defined(__APPLE__)
    constexpr auto cache_line_size = std::size_t{128}; // Apple Silicon (M1, M2)
    #else
    constexpr auto cache_line_size = std::size_t{64}; // Other ARM64 (e.g., Graviton)
    #endif
  #elif defined(__powerpc64__)
    constexpr auto cache_line_size = std::size_t{128}; // Common for PowerPC64
  #elif defined(__riscv)
    constexpr auto cache_line_size = std::size_t{64}; // Common for RISC-V
  #else
    constexpr auto cache_line_size = std::size_t{64}; // Fallback for unknown architectures
  #endif
#endif
    /// @brief Invokes the callable object stored in the buffer.
    /// @details Casts the buffer to the callable type and invokes it using `std::invoke`. Used
    /// internally to execute stored callables.
    /// @tparam `Func` The type of the callable object.
    /// @param `buffer` Pointer to the buffer containing the callable object.
    template<typename Func>
    inline constexpr void
    invoke_func(void* buffer)
    {
      std::invoke(*static_cast<Func*>(buffer));
    }

    enum class method : std::uint_fast8_t
    {
      copy_ctor,
      move_ctor,
      dtor
    };

    /// @brief Performs special operations on the callable object in the buffer.
    /// @details Handles copy construction, move construction, or destruction based on the method.
    /// Used internally for callable lifecycle management.
    /// @tparam `T` The type of the object.
    /// @param `self` Pointer to the target buffer.
    /// @param `m` The operation to perform: `copy_ctor`, `move_ctor`, or `dtor`.
    /// @param `that` Pointer to the source buffer for copy/move, optional for destruction.
    template<typename T>
    inline constexpr void
    invoke_special_func(void* self, method m, void* that)
    {
      using value_t = std::remove_cvref_t<T>;
      switch (m)
      {
        case method::copy_ctor:
        {
          if constexpr (std::copy_constructible<value_t>)
          {
            std::construct_at(static_cast<value_t*>(self), *static_cast<T const*>(that));
          }
          else
          {
            assert("Type is not copy-constructible" == nullptr and "invoke_special_func()");
            std::terminate();
          }
        }
        break;
        case method::move_ctor:
        {
          if constexpr (std::move_constructible<value_t>)
          {
            std::construct_at(static_cast<value_t*>(self), std::move(*static_cast<T*>(that)));
          }
          else if constexpr (std::copy_constructible<value_t>)
          {
            std::construct_at(static_cast<value_t*>(self), *static_cast<T const*>(that));
          }
          else
          {
            assert("Type is not move- nor copy-constructible" == nullptr and
                   "invoke_special_func()");
            std::terminate();
          }
        }
        break;
        case method::dtor:
        {
          static_assert(std::destructible<value_t>);
          std::destroy_at(static_cast<value_t*>(self));
        }
        break;
      }
    }

    using invoke_func_t         = decltype(&invoke_func<void (*)()>);
    using invoke_special_func_t = decltype(&invoke_special_func<void (*)()>);
    static_assert(sizeof(invoke_func_t) == sizeof(invoke_special_func_t));
    inline constexpr std::uint8_t  header_size   = sizeof(std::uint8_t);
    inline constexpr std::uint8_t  func_ptr_size = sizeof(invoke_func_t);
    inline constexpr std::uint32_t function_buffer_meta_size =
      details::header_size + (details::func_ptr_size * 2);

    /// @brief Gets a reference to the buffer's size field.
    /// @details Returns a reference to the first byte, which stores the callable's size.
    /// @param `buf` Pointer to the buffer.
    /// @return Reference to the size field.
    inline auto
    size(std::byte* buf) noexcept -> std::uint8_t&
    {
      return reinterpret_cast<std::uint8_t&>(*buf); // NOLINT
    }

    /// @brief Gets the buffer's size field value.
    /// @details Returns the value of the first byte, representing the callable's size.
    /// @param `buf` Const pointer to the buffer.
    /// @return The size field value.
    inline auto
    size(std::byte const* buf) noexcept -> std::uint8_t
    {
      return static_cast<std::uint8_t>(*buf);
    }

    /// @brief Gets a reference to the buffer's invoke function pointer.
    /// @details Returns a reference to the function pointer used to invoke the callable.
    /// @param `buf` Pointer to the buffer.
    /// @return Reference to the invoke function pointer.
    inline auto
    invoke_ptr(std::byte* buf) noexcept -> invoke_func_t&
    {
      return *static_cast<invoke_func_t*>(static_cast<void*>(buf + header_size)); // NOLINT
    }

    /// @brief Gets a reference to the buffer's special function pointer.
    /// @details Returns a reference to the function pointer for copy/move/destroy operations.
    /// @param `buf` Pointer to the buffer.
    /// @return Reference to the special function pointer.
    inline auto
    special_func_ptr(std::byte* buf) noexcept -> invoke_special_func_t&
    {
      return *static_cast<invoke_special_func_t*>(
        static_cast<void*>(buf + header_size + func_ptr_size)); // NOLINT
    }

    /// @brief Gets a pointer to the callable's storage in the buffer.
    /// @details Returns a pointer to the callable's data, located after the header and function
    /// pointers.
    /// @param `buf` Pointer to the buffer.
    /// @return Pointer to the callable's storage.
    inline auto
    body_ptr(std::byte* buf) noexcept -> std::byte*
    {
      return buf + header_size + func_ptr_size + func_ptr_size;
    }

    /// @brief Invokes the callable stored in the buffer.
    /// @details Calls the callable using its stored invoke function pointer.
    /// @param `buf` Pointer to the buffer containing the callable.
    inline void
    invoke(std::byte* buf) noexcept
    {
      invoke_ptr(buf)(body_ptr(buf));
    }

    /// @brief Performs a special operation on the callable in the buffer.
    /// @details Executes copy construction, move construction, or destruction using the special
    /// function pointer.
    /// @param `buf` Pointer to the buffer.
    /// @param `m` Operation to perform: `copy_ctor`, `move_ctor`, or `dtor`.
    /// @param `that` Pointer to the source buffer for copy/move, optional for destruction.
    inline void
    invoke_special_func(std::byte* buf, method m, std::byte* that = nullptr) noexcept
    {
      special_func_ptr(buf)(body_ptr(buf), m, that ? body_ptr(that) : nullptr);
    }

    template<typename Func, typename... Args>
    struct deferred_callable final : std::remove_cvref_t<Func>
    {
      using base_t = std::remove_cvref_t<Func>;

      explicit deferred_callable(Func func, Args... args)
        : base_t(FWD(func))
        , args_(FWD(args)...)
      {}

      auto
      operator()() -> decltype(auto)
        requires std::invocable<base_t, Args...>
      {
        return std::apply(
          [this](auto&&... args) mutable
          {
            (void)this; // silence clang 'unused this'-warning
            return base_t::operator()(static_cast<Args>(args)...);
          },
          args_);
      }

    private:
      using tuple_t = decltype(std::tuple(std::declval<Args>()...));
      tuple_t args_;
    };

    template<typename Func, typename... Args>
    deferred_callable(Func&& callable, Args&&... args)
      -> deferred_callable<decltype(callable), decltype(args)...>;

    template<typename T>
    struct is_function : std::false_type
    {};

    template<std::size_t Size>
    struct is_function<function<Size>> : std::true_type
    {};

    template<typename Func, typename... Args>
    struct required_buffer_size :
      std::integral_constant<std::size_t, details::function_buffer_meta_size + sizeof(Func) +
                                            (0 + ... + sizeof(Args))>
    {};

    template<std::size_t Size>
    struct required_buffer_size<function<Size>> : std::integral_constant<std::size_t, Size>
    {};
  }

  /// @brief Checks if a type is an instance of `fho::function<Size>` for some `Size`.
  /// @details Evaluates to `true` if the type is a specialization of `fho::function`,
  /// `false` otherwise.
  /// @tparam `Func` The type to check.
  template<typename Func>
  constexpr bool is_function_v = details::is_function<std::remove_cvref_t<Func>>::value;

  /// @brief Computes an estimated buffer size required to store a callable and its arguments.
  /// @details For a callable `Func` with arguments `Args...`, estimates the size as the sum of
  /// metadata size, `sizeof(Func)`, and the sizes of all arguments. For `fho::function<Size>`,
  /// returns `Size`.
  /// @tparam `Func` The type of the callable.
  /// @tparam `Args` The types of the arguments.
  template<typename Func, typename... Args>
  constexpr std::size_t required_buffer_size_v =
    details::required_buffer_size<std::remove_cvref_t<Func>, std::remove_cvref_t<Args>...>::value;

  /// @brief A fixed-size buffer for storing callable objects.
  /// @details The `function_buffer` class provides a way to store callable objects (such as
  /// functions, lambdas, or functors) within a fixed-size buffer. It is designed to be used as a
  /// building block for higher-level function objects like `fho::function`. The buffer size is
  /// specified at compile time, and the class handles
  /// the storage, copying, moving, and destruction of the callable objects. If the callable and
  /// its bound arguments fit within the buffer size, they are stored directly in the buffer.
  /// Otherwise, a small lambda that captures a `std::shared_ptr` to the callable is stored in
  /// the buffer, allowing the callable to be managed on the heap.
  /// @example
  /// ```cpp
  /// auto buffer = fho::function_buffer<128>{};
  /// auto value = 10;
  /// buffer.emplace([](int val) { cout << format("val: {}\n", val); }, value);
  /// // Typically used through `fho::function`
  /// details::invoke(buffer.data());
  /// ```
  template<std::size_t Size>
  class function_buffer
  {
    static_assert(Size - 1 <= std::numeric_limits<std::uint8_t>::max(),
                  "Buffer size must be within index range (0-255)");

  public:
    /// @brief Constructs a callable and its arguments to in buffer.
    /// @details Stores the callable and its arguments, potentially wrapping them in a
    /// `deferred_callable`.
    /// @tparam `Func` The type of the callable.
    /// @tparam `Args` The types of the arguments.
    /// @param `func` The callable to store.
    /// @param `args` The arguments to bind to the callable.
    /// @requires `std::invocable<Func&&, Args&&...>`
    template<typename Func, typename... Args>
    void
    emplace(Func&& func, Args&&... args) noexcept
      requires std::invocable<Func&&, Args&&...> && (sizeof...(args) > 0)
    {
      using value_t = std::remove_reference_t<decltype(func)>;
      if constexpr (std::is_function_v<value_t> || std::is_member_function_pointer_v<value_t>)
      {
        emplace(
          [func, ... args = FWD(args)]() mutable
          {
            std::invoke(func, FWD(args)...);
          });
      }
      else
      {
        emplace(details::deferred_callable(FWD(func), FWD(args)...));
      }
    }

    /// @brief Constructs a callable that fits within the buffer size.
    /// @details Directly constructs the callable in the buffer if it fits.
    /// @tparam `Func` The type of the callable.
    /// @param `func` The callable to store.
    /// @requires `required_buffer_size_v<Func> <= Size`
    template<std::invocable Func>
    void
    emplace(Func&& func) noexcept
      requires (!is_function_v<Func>) && (required_buffer_size_v<Func> <= Size)
    {
      using value_t                            = std::remove_reference_t<Func>;
      static constexpr std::uint8_t total_size = required_buffer_size_v<Func>;
      static_assert(std::copy_constructible<Func> || std::move_constructible<Func>,
                    "Callable must be copy- or move-constructible");
      reset();
      // header (size)
      // body (invocation pointer)
      // body (destructor pointer)
      // body (callable)
      details::size(buffer_.data())       = total_size;
      details::invoke_ptr(buffer_.data()) = std::addressof(details::invoke_func<value_t>);
      details::special_func_ptr(buffer_.data()) =
        std::addressof(details::invoke_special_func<value_t>);

      std::construct_at(reinterpret_cast<std::remove_const_t<value_t>*>( // NOLINT
                          details::body_ptr(buffer_.data())),
                        FWD(func));
    }

    /// @brief Constructs a callable that does not fit within the buffer size.
    /// @details Stores a lambda that captures a `std::shared_ptr` to the callable.
    /// @tparam `Func` The type of the callable.
    /// @param `func` The callable to store.
    /// @requires `required_buffer_size_v<Func> > Size`
    template<std::invocable Func>
    void
    emplace(Func&& func) noexcept // NOLINT
      requires (!is_function_v<Func>) && (required_buffer_size_v<Func> > Size)
    {
      emplace(
        [func = std::make_shared<std::remove_reference_t<Func>>(FWD(func))]() mutable
        {
          (*func)();
        });
    }

    /// @brief Default constructor.
    /// @details Initializes the buffer with a size of `0`, indicating no callable is stored.
    function_buffer() noexcept
    {
      details::size(buffer_.data()) = 0;
    }

    /// @brief Copy constructor.
    /// @details Creates a new `function_buffer` by copying the contents of another
    /// `function_buffer`. This involves copying the stored callable if one exists.
    /// @param `buffer` The `function_buffer` to copy from.
    function_buffer(function_buffer const& buffer)
    {
      details::size(buffer_.data()) = 0;
      *this                         = buffer;
    }

    /// @brief Move constructor.
    /// @details Creates a new `function_buffer` by moving the contents from another
    /// `function_buffer`. This transfers ownership of the stored callable.
    /// @param `buffer` The `function_buffer` to move from.
    function_buffer(function_buffer&& buffer) noexcept
    {
      details::size(buffer_.data()) = 0;
      *this                         = std::move(buffer);
    }

    /// @brief Constructor that takes a `function<Size>` object.
    /// @details Initializes the buffer with the provided `function<Size>` object, which must fit
    /// within the buffer size.
    /// @tparam `Func` The `function<S>` type.
    /// @param `func` The `function<S>` object to store.
    /// @requires `S <= Size`
    template<std::invocable Func>
      requires (is_function_v<Func> && required_buffer_size_v<Func> <= Size)
    explicit function_buffer(Func&& func) noexcept
      : function_buffer(stdext::forward_like<Func>(FWD(func).buffer()))
    {}

    /// @brief Constructor that takes a callable.
    /// @details Initializes the buffer with a callable.
    /// @tparam `Func` The type of the callable.
    /// @param `callable` The callable to store.
    template<std::invocable Func>
    explicit function_buffer(Func&& callable) noexcept
      requires (!is_function_v<Func>)
    {
      details::size(buffer_.data()) = 0;
      *this                         = FWD(callable);
    }

    /// @brief Constructor that takes a callable and its arguments.
    /// @details Initializes the buffer with a callable and its arguments, potentially wrapping them
    /// in a `deferred_callable`.
    /// @tparam `Func` The type of the callable.
    /// @tparam `Args` The types of the arguments.
    /// @param `callable` The callable to store.
    /// @param `args` The arguments to bind to the callable.
    /// @requires `std::invocable<Func&&, Args&&...>`
    explicit function_buffer(auto&& callable, auto&&... args) noexcept
      requires (sizeof...(args) > 0)
    {
      details::size(buffer_.data()) = 0;
      emplace(FWD(callable), FWD(args)...);
    }

    /// @brief Destructor.
    /// @details Calls `reset()` to properly destroy the stored callable if one exists.
    ~function_buffer()
    {
      reset();
    }

    /// @brief Copy assignment operator.
    /// @details Assigns the contents of another `function_buffer` to this one, copying the stored
    /// callable if one exists.
    /// @param `buffer` The `function_buffer` to copy from.
    /// @return A reference to this `function_buffer`.
    auto
    operator=(function_buffer const& buffer) -> function_buffer&
    {
      if (buffer.size()) [[likely]]
      {
        std::memcpy(data(), buffer.data(), details::function_buffer_meta_size);
        details::invoke_special_func(data(), details::method::copy_ctor,
                                     const_cast<std::byte*>(buffer.data())); // NOLINT
      }
      else [[unlikely]]
      {
        reset();
      }
      return *this;
    }

    /// @brief Move assignment operator.
    /// @details Moves the contents from another `function_buffer` to this one, transferring
    /// ownership of the stored callable.
    /// @param `buffer` The `function_buffer` to move from.
    /// @return A reference to this `function_buffer`
    auto
    operator=(function_buffer&& buffer) noexcept -> function_buffer&
    {
      if (buffer.size()) [[likely]]
      {
        std::memcpy(data(), buffer.data(), details::function_buffer_meta_size);
        details::invoke_special_func(data(), details::method::move_ctor, buffer.data());
        buffer.reset();
      }
      else [[unlikely]]
      {
        reset();
      }
      return *this;
    }

    /// @brief Assigns a `function<Size>` object to the buffer.
    /// @details Copies the provided `function<Size>` object into the buffer.
    /// @tparam `Func` The `function<S>` type.
    /// @param `func` The `function<S>` object to assign.
    /// @requires `S <= Size`
    template<std::invocable Func>
      requires (is_function_v<Func> && required_buffer_size_v<Func> <= Size)
    auto
    operator=(Func&& func) -> function_buffer&
    {
      *this = stdext::forward_like<Func>(FWD(func).buffer());
      return *this;
    }

    /// @brief Assigns a callable to the buffer.
    /// @details Stores the callable directly if it fits, otherwise stores a lambda with a
    /// `std::shared_ptr`.
    /// @tparam `Func` The type of the callable.
    /// @param `func` The callable to store.
    template<std::invocable Func>
      requires (!is_function_v<Func>)
    auto
    operator=(Func&& func) noexcept -> function_buffer&
    {
      emplace(FWD(func));
      return *this;
    }

    /// @brief Conversion to bool.
    /// @details Returns `true` if a callable is stored, `false` otherwise.
    inline
    operator bool() const noexcept
    {
      static_assert(sizeof(size()) == 1,
                    "Implicit boolean conversion assumes size-field to be 1 byte");
      return static_cast<bool>(*data());
    }

    /// @brief Resets the buffer.
    /// @details Destroys the stored callable if one exists and sets the buffer size to `0`.
    inline void
    reset() noexcept
    {
      if (*this) [[unlikely]]
      {
        details::invoke_special_func(data(), details::method::dtor);
        details::size(buffer_.data()) = 0;
      }
    }

    /// @brief Gets the current size of the buffer.
    /// @details Returns the number of bytes currently used by the stored callable.
    /// @return The size of the stored callable in bytes.
    [[nodiscard]] inline auto
    size() const noexcept -> std::uint8_t
    {
      return details::size(data());
    }

    /// @brief Gets a pointer to the buffer data.
    /// @details Returns a pointer to the internal buffer where the callable is stored.
    /// @return A pointer to the buffer data.
    inline auto
    data() noexcept -> std::byte*
    {
      return buffer_.data();
    }

    /// @brief Gets a const pointer to the buffer data.
    /// @details Returns a const pointer to the internal buffer.
    /// @return A const pointer to the buffer data.
    [[nodiscard]] inline auto
    data() const noexcept -> std::byte const*
    {
      return buffer_.data();
    }

  private:
    std::array<std::byte, Size> buffer_;
  };

  template<typename Func, typename... Args>
  function_buffer(Func&&, Args&&...) -> function_buffer<required_buffer_size_v<Func, Args...>>;

  function_buffer() -> function_buffer<details::cache_line_size>;

  static_assert(sizeof(function_buffer<details::cache_line_size>) == details::cache_line_size);

  /// @brief Concept to check if `Args...` can be constructed in the buffer.
  /// @details Checks if there exists a function `Buffer::emplace(Args...)`.
  template<typename Buffer, typename... Args>
  concept constructible_at = requires (Buffer&& buf, Args&&... args) { buf.emplace(FWD(args)...); };

  /// @brief A type-erased function wrapper with a fixed-size buffer.
  /// @details The `function` class template provides a way to store and invoke callable objects
  /// (such as functions, lambdas, or functors) within a fixed-size buffer. It is designed to be
  /// efficient for small callables by storing them on the stack, while larger callables are
  /// handled dynamically. The buffer size can be specified at compile time, defaulting to the CPU
  /// cache line size.
  /// @example
  /// ```cpp
  /// auto func = fho::function([]() { cout << "Hello, World!\n"; });
  /// func();
  /// ```
  template<std::size_t Size = details::cache_line_size>
  class function
  {
    using buffer_t = function_buffer<Size>;

  public:
    /// @brief Default constructor.
    /// @details Initializes an empty `function` object with no stored callable.
    explicit function() noexcept = default;

    /// @brief Constructor that takes a `function_buffer<Size>`.
    /// @details Initializes the `function` by copying or moving from the provided
    /// `function_buffer<Size>`.
    /// @param `buffer` The `function_buffer<Size>` to initialize with.
    explicit function(std::common_reference_with<function_buffer<Size>> auto&& buffer)
      : buffer_(FWD(buffer))
    {}

    /// @brief Copy constructor.
    /// @details Creates a new `function` by copying the contents of another `function`.
    /// @param `func` The `function` to copy from.
    function(function const& func)
      : buffer_(func.buffer_)
    {}

    /// @brief Move constructor.
    /// @details Creates a new `function` by moving the contents from another `function`.
    /// @param `func` The `function` to move from.
    function(function&& func) noexcept
      : buffer_(std::move(func.buffer_))
    {}

    /// @brief Constructor that takes a callable and its arguments.
    /// @details Initializes the `function` with the provided callable and its arguments.
    /// @tparam `Args` The types of the arguments.
    /// @param `callable` The callable to store.
    /// @param `args` The arguments to bind to the callable.
    /// @requires `std::invocable<Func&&, Args&&...>`
    template<typename... Args>
    explicit function(std::invocable<Args...> auto&& callable, Args&&... args) noexcept
      requires (!is_function_v<decltype(callable)>) &&
               constructible_at<buffer_t, decltype(callable), decltype(args)...>
    {
      emplace(FWD(callable), FWD(args)...);
    }

    /// @brief Destructor.
    /// @details Calls `reset()` to properly destroy the stored callable if one exists.
    ~function()
    {
      reset();
    }

    /// @brief Copy assignment operator.
    /// @details Assigns the contents of another `function` to this one.
    /// @param `func` The `function` to copy from.
    /// @return A reference to this `function`.
    auto operator=(function const& func) -> function& = default;

    /// @brief Move assignment operator.
    /// @details Moves the contents from another `function` to this one.
    /// @param `func` The `function` to move from.
    /// @return A reference to this `function`
    auto operator=(function&& func) noexcept -> function& = default;

    /// @brief Assignment operator for invocable objects.
    /// @details Assigns a new callable to this `function`.
    /// @param `func` The callable to assign.
    /// @return A reference to this `function`
    template<std::invocable Func>
      requires (!is_function_v<Func>)
    auto
    operator=(Func&& func) noexcept -> function&
    {
      emplace(FWD(func));
      return *this;
    }

    /// @brief Assignment operator for nullptr.
    /// @details Resets the `function` to an empty state.
    /// @param `nullptr_t` The nullptr value.
    /// @return A reference to this `function`
    auto
    operator=(std::nullptr_t) noexcept -> auto&
    {
      reset();
      return *this;
    }

    /// @brief Invokes the stored callable.
    /// @details Calls the stored callable with no arguments.
    inline void
    operator()()
    {
      details::invoke(buffer_.data());
    }

    /// @brief Conversion to bool.
    /// @details Returns `true` if a callable is stored, `false` otherwise.
    inline
    operator bool() const noexcept
    {
      return buffer_.size() != 0;
    }

    /// @brief Gets the internal buffer.
    /// @details Returns a const reference to the internal `function_buffer<Size>`.
    /// @return A const reference to the internal buffer.
    [[nodiscard]] auto
    buffer() const noexcept -> buffer_t const&
    {
      return buffer_;
    }

    /// @brief Gets the internal buffer.
    /// @details Returns a reference to the internal `function_buffer<Size>`.
    /// @return A reference to the internal buffer.
    [[nodiscard]] auto
    buffer() noexcept -> buffer_t&
    {
      return buffer_;
    }

    /// @brief Assigns a new callable to the function.
    /// @details Stores the provided callable and its arguments in the function.
    /// @tparam `Args` The types of the arguments.
    /// @param `callable` The callable to store.
    /// @param `args` The arguments to bind to the callable.
    /// @requires `std::invocable<Func&&, Args&&...>`
    template<typename... Args>
    void
    emplace(std::invocable<Args...> auto&& callable, Args&&... args) noexcept
      requires constructible_at<buffer_t, decltype(callable), decltype(args)...>
    {
      buffer_.emplace(FWD(callable), FWD(args)...);
    }

    /// @brief Resets the function.
    /// @details Clears the stored callable, setting the function to an empty state.
    inline void
    reset() noexcept
    {
      buffer_.reset();
    }

    /// @brief Gets the size of the stored callable.
    /// @details Returns the number of bytes used by the stored callable.
    /// @return The size of the stored callable in bytes.
    [[nodiscard]] inline auto
    size() const noexcept -> std::uint8_t
    {
      return buffer_.size();
    }

  private:
    buffer_t buffer_;
  };

  template<typename... Args>
  function(std::invocable<Args...> auto&&, Args&&...) -> function<>;

  static_assert(sizeof(function<details::cache_line_size>) == details::cache_line_size);
}

#undef FWD
