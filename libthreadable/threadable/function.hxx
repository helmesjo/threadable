#pragma once

#include <threadable/std_concepts.hxx>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <version>

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

namespace fho
{
  template<std::size_t>
  struct function;

  struct function_dyn;

  namespace details
  {
    // NOTE: GCC/Clang incorrectly reports 64 bytes when targeting Apple Silicon.
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
    /// @details This template function casts the buffer to the callable type and invokes it using
    /// std::invoke. Used internally to execute stored callables.
    /// @tparam `Func` The type of the callable object.
    /// @param `buffer` Pointer to the buffer containing the callable object.
    template<typename Func>
    inline constexpr void
    invoke_func(void* buffer)
    {
      std::invoke(*static_cast<Func*>(buffer));
    }

    enum class method : std::uint8_t
    {
      copy_ctor,
      move_ctor,
      dtor
    };

    /// @brief Performs special operations on the callable object in the buffer.
    /// @details Handles copy construction, move construction, or destruction based on the method.
    /// Used internally for callable lifecycle management.
    /// @tparam `Func` The type of the callable object.
    /// @param `self` Pointer to the target buffer.
    /// @param `m` The operation to perform: copy_ctor, move_ctor, or dtor.
    /// @param `that` Pointer to the source buffer for copy/move, optional for destruction.
    template<typename Func>
    inline constexpr void
    invoke_special_func(void* self, method m, void* that)
    {
      using func_t = std::remove_cvref_t<Func>;
      switch (m)
      {
        case method::copy_ctor:
        {
          static_assert(std::copy_constructible<func_t>);
          std::construct_at(static_cast<func_t*>(self), *static_cast<Func const*>(that));
        }
        break;
        case method::move_ctor:
        {
          static_assert(std::move_constructible<func_t>);
          std::construct_at(static_cast<func_t*>(self), std::move(*static_cast<Func*>(that)));
        }
        break;
        case method::dtor:
        {
          static_assert(std::destructible<func_t>);
          std::destroy_at(static_cast<func_t*>(self));
        }
        break;
      }
    }

    using invoke_func_t         = decltype(&invoke_func<void (*)()>);
    using invoke_special_func_t = decltype(&invoke_special_func<void (*)()>);
    static_assert(sizeof(invoke_func_t) == sizeof(invoke_special_func_t));
    inline constexpr std::uint8_t header_size   = sizeof(std::uint8_t);
    inline constexpr std::uint8_t func_ptr_size = sizeof(invoke_func_t);
    inline constexpr std::size_t  function_buffer_meta_size =
      details::header_size + (details::func_ptr_size * 2);

    /// @brief Gets a reference to the buffer's size field.
    /// @details Returns a reference to the first byte, storing the callable's size.
    /// @param `buf` Pointer to the buffer.
    /// @return Reference to the size field (`uint8_t`).
    inline auto
    size(std::byte* buf) noexcept -> std::uint8_t&
    {
      return reinterpret_cast<std::uint8_t&>(*buf); // NOLINT
    }

    /// @brief Gets the buffer's size field value.
    /// @details Returns the first byte's value, representing the callable's size.
    /// @param `buf` Const pointer to the buffer.
    /// @return The size field value (`uint8_t`).
    inline auto
    size(std::byte const* buf) noexcept -> std::uint8_t
    {
      return static_cast<std::uint8_t>(*buf);
    }

    /// @brief Sets the buffer's size field.
    /// @details Updates the first byte with the provided size value.
    /// @param `buf` Pointer to the buffer.
    /// @param `s` Size value to set.
    inline void
    size(std::byte* buf, std::uint8_t s) noexcept
    {
      size(buf) = s;
    }

    /// @brief Gets a reference to the buffer's invoke function pointer.
    /// @details Returns a reference to the invoke function pointer for calling the callable.
    /// @param `buf` Pointer to the buffer.
    /// @return Reference to the invoke function pointer (`invoke_func_t&`).
    inline auto
    invoke_ptr(std::byte* buf) noexcept -> invoke_func_t&
    {
      return *static_cast<invoke_func_t*>(static_cast<void*>(buf + header_size)); // NOLINT
    }

    /// @brief Sets the buffer's invoke function pointer.
    /// @details Updates the invoke function pointer with the provided function.
    /// @param `buf` Pointer to the buffer.
    /// @param `func` Invoke function to set.
    inline void
    invoke_ptr(std::byte* buf, invoke_func_t func) noexcept
    {
      invoke_ptr(buf) = func;
    }

    /// @brief Gets a reference to the buffer's special function pointer.
    /// @details Returns a reference to the special function pointer for copy/move/destroy
    /// operations.
    /// @param `buf` Pointer to the buffer.
    /// @return Reference to the special function pointer (`invoke_special_func_t&`).
    inline auto
    special_func_ptr(std::byte* buf) noexcept -> invoke_special_func_t&
    {
      return *static_cast<invoke_special_func_t*>(
        static_cast<void*>(buf + header_size + func_ptr_size)); // NOLINT
    }

    /// @brief Sets the buffer's special function pointer.
    /// @details Updates the special function pointer with the provided function.
    /// @param `buf` Pointer to the buffer.
    /// @param `func` Special function to set.
    inline void
    special_func_ptr(std::byte* buf, invoke_special_func_t func) noexcept
    {
      special_func_ptr(buf) = func;
    }

    /// @brief Gets a pointer to the buffer's callable body.
    /// @details Returns a pointer to the callable's storage, after header and function pointers.
    /// @param `buf` Pointer to the buffer.
    /// @return Pointer to the callable body (`byte*`).
    inline auto
    body_ptr(std::byte* buf) noexcept -> std::byte*
    {
      return buf + header_size + func_ptr_size + func_ptr_size;
    }

    /// @brief Invokes the callable stored in the buffer.
    /// @details Calls the callable using the buffer's invoke function pointer.
    /// @param `buf` Pointer to the buffer containing the callable.
    inline void
    invoke(std::byte* buf) noexcept
    {
      invoke_ptr(buf)(body_ptr(buf));
    }

    /// @brief Invokes the buffer's special function.
    /// @details Performs the specified operation (copy, move, destroy) using the special function
    /// pointer.
    /// @param `buf` Pointer to the buffer.
    /// @param `m` Operation to perform: copy_ctor, move_ctor, or dtor.
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

    template<typename T>
    struct is_function_dyn : std::false_type
    {};

    template<>
    struct is_function_dyn<function_dyn> : std::true_type
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

  template<typename Func>
  constexpr bool is_function_v = details::is_function<std::remove_cvref_t<Func>>::value;

  template<typename Func>
  constexpr bool is_function_dyn_v = details::is_function_dyn<std::remove_cvref_t<Func>>::value;

  template<typename Func, typename... Args>
  constexpr std::size_t required_buffer_size_v =
    details::required_buffer_size<std::remove_cvref_t<Func>, std::remove_cvref_t<Args>...>::value;

  /// @brief A fixed-size buffer for storing callable objects.
  /// @details The `function_buffer` class provides a way to store callable objects (such as
  /// functions, lambdas, or functors) within a fixed-size buffer. It is designed to be used as a
  /// building block for higher-level function objects like `fho::function` and `fho::function_dyn`.
  /// The buffer size is specified at compile time, and the class handles the storage, copying,
  /// moving, and destruction of the callable objects. If the callable object exceeds the buffer
  /// size, it is stored on the heap using a `std::shared_ptr`.
  /// @example
  /// ```cpp
  /// auto buffer = fho::function_buffer<128>{};
  /// auto value = 10;
  /// buffer.assign([](int val) { cout << format("val: {}\n", val); }, value);
  /// details::invoke(buffer.data()); // but typically through fho::function or fho::function_dyn
  /// ```
  template<std::size_t Size>
  struct function_buffer
  {
    static_assert(Size <= std::numeric_limits<std::uint8_t>::max(), "Buffer size must be <= 255");
    using buffer_t = std::array<std::byte, Size>;

    /// @brief Assigns a `function<Size>` object to the buffer.
    /// @details Copies the provided `function<Size>` object into the buffer.
    /// @tparam `Size_` The size of the function.
    /// @param `func` The `function<Size>` object to assign.
    /// @requires `size <= buffer_size`
    template<std::size_t S>
      requires (S <= Size)
    void
    assign(function<S> const& func) noexcept
    {
      (*this) = func.buffer();
    }

    /// @brief Assigns a moved `function<Size>` object to the buffer.
    /// @details Moves the provided `function<Size>` object into the buffer.
    /// @tparam `Size_` The size of the function.
    /// @param `func` The `function<Size>` object to move.
    /// @requires `size <= buffer_size`
    template<std::size_t Size_>
      requires (Size_ <= Size)
    void
    assign(function<Size>&& func) noexcept
    {
      (*this) = std::move(std::move(func).buffer());
    }

    /// @brief Assigns a callable and its arguments to the buffer.
    /// @details Stores the callable and its arguments, potentially wrapping them in a
    /// `deferred_callable`.
    /// @tparam `Func` The type of the callable.
    /// @tparam `Args` The types of the arguments.
    /// @param `func` The callable to store.
    /// @param `args` The arguments to bind to the callable.
    /// @requires `std::invocable<Func&&, Args&&...>`
    template<typename Func, typename... Args>
    void
    assign(Func&& func, Args&&... args) noexcept
      requires std::invocable<Func&&, Args&&...> && (sizeof...(args) > 0)
    {
      using func_value_t = std::remove_reference_t<decltype(func)>;
      if constexpr (std::is_function_v<func_value_t> ||
                    std::is_member_function_pointer_v<func_value_t>)
      {
        assign(
          [func, ... args = FWD(args)]() mutable
          {
            std::invoke(func, FWD(args)...);
          });
      }
      else
      {
        assign(details::deferred_callable(FWD(func), FWD(args)...));
      }
    }

    /// @brief Assigns a callable that does not fit within the buffer size.
    /// @details Stores the callable on the heap using a `std::shared_ptr` and creates a lambda to
    /// invoke it.
    /// @tparam `Func` The type of the callable.
    /// @param `callable` The callable to store.
    /// @requires `required_buffer_size_v<decltype(callable)> > buffer_size`
    template<std::invocable Func>
    void
    assign(Func&& callable) noexcept // NOLINT
                                     // (bug in clang-tidy doesn't detect FWD in capture)
      requires (!is_function_v<Func>) && (required_buffer_size_v<decltype(callable)> > Size)
    {
      assign(
        [func = std::make_shared<std::remove_reference_t<decltype(callable)>>(FWD(callable))]
        {
          std::forward<Func> (*func)();
        });
    }

    /// @brief Assigns a callable that fits within the buffer size.
    /// @details Directly constructs the callable in the buffer.
    /// @tparam `Func` The type of the callable.
    /// @param `callable` The callable to store.
    /// @requires `required_buffer_size_v<decltype(callable)> <= buffer_size`
    template<std::invocable Func>
    void
    assign(Func&& callable) noexcept
      requires (!is_function_v<Func>) && (required_buffer_size_v<Func> <= Size)
    {
      using func_t = std::remove_reference_t<Func>;

      static constexpr std::uint8_t total_size = required_buffer_size_v<Func>;

      static_assert(std::copy_constructible<Func>, "Callable must be copy-constructible");
      reset();

      // header (size)
      // body (invocation pointer)
      // body (destructor pointer)
      // body (callable)
      details::size(buffer_.data(), total_size);
      details::invoke_ptr(buffer_.data(), std::addressof(details::invoke_func<func_t>));
      details::special_func_ptr(buffer_.data(),
                                std::addressof(details::invoke_special_func<func_t>));
      std::construct_at(reinterpret_cast<std::remove_const_t<func_t>*>( // NOLINT
                          details::body_ptr(buffer_.data())),
                        FWD(callable));
    }

    /// @brief Default constructor.
    /// @details Initializes the buffer with a size of 0, indicating no callable is stored.
    function_buffer()
    {
      details::size(buffer_.data(), 0);
    }

    /// @brief Copy constructor.
    /// @details Creates a new `function_buffer` by copying the contents of another
    /// `function_buffer`. This involves copying the stored callable if one exists.
    /// @param `buffer` The `function_buffer` to copy from.
    function_buffer(function_buffer const& buffer)
    {
      *this = buffer;
    }

    /// @brief Move constructor.
    /// @details Creates a new `function_buffer` by moving the contents from another
    /// `function_buffer`. This transfers ownership of the stored callable.
    /// @param `buffer` The `function_buffer` to move from.
    function_buffer(function_buffer&& buffer) noexcept
    {
      *this = std::move(buffer);
    }

    /// @brief Constructor that takes a `function<Size>` object.
    /// @details Initializes the buffer with the provided `function<Size>` object, which must fit
    /// within the buffer size.
    /// @tparam `Size_` The size of the function.
    /// @param `func` The `function<Size>` object to store.
    /// @requires `Size_` <= buffer_size
    template<std::size_t Size_>
    explicit function_buffer(function<Size> const& func) noexcept
      : function_buffer<Size>(func.buffer())
    {}

    /// @brief Constructor that takes a callable and its arguments.
    /// @details Initializes the buffer with a callable and its arguments, potentially wrapping them
    /// in a `deferred_callable`.
    /// @tparam `Func` The type of the callable.
    /// @tparam `Args` The types of the arguments.
    /// @param `func` The callable to store.
    /// @param `args` The arguments to bind to the callable.
    /// @requires `std::invocable<Func&&, Args&&...>`
    function_buffer(auto&& callable, auto&&... args) noexcept
      requires requires { this->assign(FWD(callable), FWD(args)...); }
    {
      details::size(buffer_.data(), 0);
      assign(FWD(callable), FWD(args)...);
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
    operator=(function_buffer const& buffer) -> auto&
    {
      std::memcpy(data(), buffer.data(), details::function_buffer_meta_size);
      details::invoke_special_func(data(), details::method::copy_ctor,
                                   const_cast<std::byte*>(buffer.data())); // NOLINT
      return *this;
    }

    /// @brief Move assignment operator.
    /// @details Moves the contents from another `function_buffer` to this one, transferring
    /// ownership of the stored callable.
    /// @param `buffer` The `function_buffer` to move from.
    /// @return A reference to this `function_buffer`.
    auto
    operator=(function_buffer&& buffer) noexcept -> auto&
    {
      std::memcpy(data(), buffer.data(), details::function_buffer_meta_size);
      details::invoke_special_func(data(), details::method::move_ctor, buffer.data());
      details::size(buffer.data(), 0);
      return *this;
    }

    /// @brief Resets the buffer.
    /// @details Destroys the stored callable if one exists and sets the buffer size to 0.
    inline void
    reset() noexcept
    {
      if (size() > 0)
      {
        details::invoke_special_func(data(), details::method::dtor);
        details::size(buffer_.data(), 0);
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
    buffer_t buffer_;
  };

  template<typename Func, typename... Args>
  function_buffer(Func&&, Args&&...) -> function_buffer<required_buffer_size_v<Func, Args...>>;

  /// @brief A dynamic function wrapper that can store callables of any size.
  /// @details The `function_dyn` class is designed to store and invoke callable objects of
  /// arbitrary size by dynamically allocating memory on the heap. It is suitable for scenarios
  /// where the size of the callable is unknown or exceeds the fixed buffer size of `fho::function`.
  /// It provides similar functionality to `std::function` but with custom optimizations and
  /// constraints.
  /// @example
  /// ```cpp
  /// auto lambda = []() { std::cout << "Hello, World!\n"; };
  /// auto dyn_func = fho::function_dyn(lambda);
  /// dyn_func(); // Invokes the lambda
  /// ```
  struct function_dyn
  {
    auto operator=(function_dyn const&) -> function_dyn& = delete;
    auto operator=(function_dyn&&) -> function_dyn&      = delete;

    /// @brief Copy constructor.
    /// @details Creates a new `function_dyn` by copying the contents of another `function_dyn`.
    /// This involves dynamically allocating new memory and copying the stored callable.
    /// @param `that` The `function_dyn` to copy from.
    function_dyn(function_dyn const& that)
    {
      copy_buffer({that.buffer_, that.size()});
    }

    /// @brief Move constructor.
    /// @details Creates a new `function_dyn` by moving the contents from another `function_dyn`.
    /// This transfers ownership of the dynamically allocated buffer.
    /// @param `that` The `function_dyn` to move from.
    function_dyn(function_dyn&& that) noexcept
      : buffer_(that.buffer_)
    {
      that.buffer_ = nullptr;
    }

    /// @brief Constructor that takes a `function_buffer`.
    /// @details Initializes the `function_dyn` with the contents of a `function_buffer`, copying
    /// the stored callable.
    /// @tparam `Size` The size of the `function_buffer`.
    /// @param `buffer` The `function_buffer` to copy from.
    template<std::size_t Size>
    explicit function_dyn(function_buffer<Size> const& buffer)
    {
      copy_buffer({buffer.data(), buffer.size()});
    }

    /// @brief Constructor that takes a `function<Size>`.
    /// @details Initializes the `function_dyn` with the contents of a `function<Size>`, copying the
    /// stored callable.
    /// @tparam `Size` The size of the `function`.
    /// @param `func` The `function<Size>` to copy from.
    template<std::size_t Size>
    function_dyn(function<Size> const& func) noexcept
      : function_dyn(func.buffer())
    {}

    /// @brief Constructor that takes a callable.
    /// @details Initializes the `function_dyn` with the provided callable, storing it dynamically.
    /// @tparam `Func` The type of the callable.
    /// @param `callable` The callable to store.
    /// @requires `std::invocable<Func&&>` and not `fho::is_function<Func>` and not
    /// `fho::is_function_dyn<Func>`
    function_dyn(std::invocable auto&& callable) noexcept
      requires (!is_function_v<decltype(callable)> && !is_function_dyn_v<decltype(callable)>)
      : function_dyn(function_buffer(FWD(callable)))
    {}

    /// @brief Destructor.
    /// @details Frees the dynamically allocated memory and destroys the stored callable if one
    /// exists.
    ~function_dyn()
    {
      reset();
    }

    /// @brief Invokes the stored callable.
    /// @details Calls the stored callable with no arguments.
    inline void
    operator()()
    {
      details::invoke(buffer_);
    }

    /// @brief Conversion to bool.
    /// @details Returns true if the `function_dyn` contains a valid callable, false otherwise.
    inline
    operator bool() const noexcept
    {
      return size() != 0;
    }

    /// @brief Gets the size of the stored callable.
    /// @details Returns the number of bytes used by the stored callable, or 0 if no callable is
    /// stored.
    /// @return The size of the stored callable in bytes.
    [[nodiscard]] inline auto
    size() const noexcept -> std::uint8_t
    {
      return buffer_ ? details::size(buffer_) : 0;
    }

    /// @brief Resets the `function_dyn`.
    /// @details Frees the dynamically allocated memory and sets the `function_dyn` to an empty
    /// state.
    inline void
    reset() noexcept
    {
      if (buffer_)
      {
        details::invoke_special_func(buffer_, details::method::dtor);
      }
      delete[] buffer_;
      buffer_ = nullptr;
    }

  private:
    void
    copy_buffer(std::span<std::byte const> src)
    {
      assert(src.size() > details::function_buffer_meta_size);
      buffer_ = new std::byte[src.size()];                              // NOLINT
      std::memcpy(buffer_, src.data(), details::function_buffer_meta_size);
      details::invoke_special_func(buffer_, details::method::copy_ctor,
                                   const_cast<std::byte*>(src.data())); // NOLINT
    }

    std::byte* buffer_ = nullptr;
  };

  // NOTE: On GCC (13.2.0) doing this inline with 'requires requires'
  //       triggers ICE (Internal Compiler Error).
  template<typename Buffer, typename Func, typename... Args>
  concept assignable = requires (Buffer&& buf, Func&& callable, Args&&... args) {
                         buf.assign(FWD(callable), FWD(args)...);
                       };

  /// @brief A type-erased function wrapper with a fixed-size buffer.
  /// @details The `function` class template provides a way to store and invoke callable objects
  /// (such as functions, lambdas, or functors) within a fixed-size buffer. It is designed to be
  /// efficient for small callables by storing them on the stack, while larger callables are handled
  /// dynamically. The buffer size can be specified at compile time, defaulting to the CPU cache
  /// line size minus the size of a function pointer.
  /// @example
  /// ```cpp
  /// fho::function<> f = []() { std::cout << "Hello, World!\n"; };
  /// f();
  /// ```
  template<std::size_t Size = details::cache_line_size - sizeof(details::invoke_func_t)>
  struct function
  {
  private:
    using buffer_t = function_buffer<Size>;
    buffer_t buffer_;

  public:
    /// @brief Default constructor.
    /// @details Initializes an empty `function` object with no stored callable.
    function() = default;

    /// @brief Constructor that takes a `buffer_t`.
    /// @details Initializes the `function` with the provided `buffer_t`, which is a
    /// `function_buffer`.
    /// @param `buffer` The `buffer_t` to initialize with.
    function(buffer_t buffer)
      : buffer_(std::move(buffer))
    {}

    /// @brief Copy constructor.
    /// @details Creates a new `function` by copying the contents of another `function`.
    /// @param `func` The `function` to copy from.
    function(function const& func)
      : buffer_(func.buffer_)
    {}

    /// @brief Move constructor.
    /// @details Creates a new `function` by moving the contents from another `function`.
    /// @param func The `function` to move from.
    function(function&& func) noexcept
      : buffer_(std::move(func.buffer_))
    {}

    /// @brief Constructor that takes a callable and its arguments.
    /// @details Initializes the `function` with the provided callable and its arguments, storing
    /// them for later invocation.
    /// @tparam `Args` The types of the arguments.
    /// @param `callable` The callable to store.
    /// @param `args` The arguments to bind to the callable.
    /// @requires `std::invocable<Func&&, Args&&...>`
    template<typename... Args>
    explicit function(std::invocable<Args...> auto&& callable, Args&&... args) noexcept
      requires (!is_function_v<decltype(callable)>) &&
               assignable<buffer_t, decltype(callable), decltype(args)...>
    {
      assign(FWD(callable), FWD(args)...);
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
    auto operator=(function const& func) noexcept -> function& = default;

    /// @brief Move assignment operator.
    /// @details Moves the contents from another `function` to this one.
    /// @param `func` The `function` to move from.
    /// @return A reference to this `function`.
    auto
    operator=(function&& func) noexcept -> function&
    {
      buffer_ = std::move(func.buffer_);
      return *this;
    }

    /// @brief Assignment operator for invocable objects.
    /// @details Assigns a new callable to this `function`.
    /// @param `func` The callable to assign.
    /// @return A reference to this `function`.
    auto
    operator=(std::invocable auto&& func) noexcept -> function&
      requires std::same_as<function, std::remove_cvref_t<decltype(func)>> || true
    {
      assign(FWD(func));
      return *this;
    }

    /// @brief Assignment operator for nullptr.
    /// @details Resets the `function` to an empty state.
    /// @param `nullptr_t` The nullptr value.
    /// @return A reference to this `function`.
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
    /// @details Returns true if the `function` contains a valid callable, false otherwise.
    inline
    operator bool() const noexcept
    {
      return buffer_.size() != 0;
    }

    /// @brief Gets the internal buffer.
    /// @details Returns a const reference to the internal `buffer_t`.
    /// @return A const reference to the internal buffer.
    auto
    buffer() const noexcept -> buffer_t const&
    {
      return buffer_;
    }

    /// @brief Gets the internal buffer.
    /// @details Returns a reference to the internal `buffer_t`.
    /// @return A reference to the internal buffer.
    auto
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
    assign(std::invocable<Args...> auto&& callable, Args&&... args) noexcept
      requires assignable<buffer_t, decltype(callable), decltype(args)...>
    {
      buffer_.assign(FWD(callable), FWD(args)...);
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
  };
}

#undef FWD
