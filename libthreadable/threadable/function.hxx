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

    template<typename callable_t>
    inline constexpr void
    invoke_func(void* buffer)
    {
      std::invoke(*static_cast<callable_t*>(buffer));
    }

    enum class method : std::uint8_t
    {
      copy_ctor,
      move_ctor,
      dtor
    };

    template<typename callable_t>
    inline constexpr void
    invoke_special_func(void* self, method m, void* that)
    {
      using callable_value_t = std::remove_cvref_t<callable_t>;
      switch (m)
      {
        case method::copy_ctor:
        {
          static_assert(std::copy_constructible<callable_value_t>);
          std::construct_at(static_cast<callable_value_t*>(self),
                            *static_cast<callable_t const*>(that));
        }
        break;
        case method::move_ctor:
        {
          static_assert(std::move_constructible<callable_value_t>);
          std::construct_at(static_cast<callable_value_t*>(self),
                            std::move(*static_cast<callable_t*>(that)));
        }
        break;
        case method::dtor:
        {
          static_assert(std::destructible<callable_value_t>);
          std::destroy_at(static_cast<callable_value_t*>(self));
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

    inline auto
    size(std::byte* buf) noexcept -> std::uint8_t&
    {
      return reinterpret_cast<std::uint8_t&>(*buf); // NOLINT
    }

    inline auto
    size(std::byte const* buf) noexcept -> std::uint8_t
    {
      return static_cast<std::uint8_t>(*buf);
    }

    inline void
    size(std::byte* buf, std::uint8_t s) noexcept
    {
      size(buf) = s;
    }

    inline auto
    invoke_ptr(std::byte* buf) noexcept -> invoke_func_t&
    {
      return *static_cast<invoke_func_t*>(static_cast<void*>(buf + header_size)); // NOLINT
    }

    inline void
    invoke_ptr(std::byte* buf, invoke_func_t func) noexcept
    {
      invoke_ptr(buf) = func;
    }

    inline auto
    special_func_ptr(std::byte* buf) noexcept -> invoke_special_func_t&
    {
      return *static_cast<invoke_special_func_t*>(
        static_cast<void*>(buf + header_size + func_ptr_size)); // NOLINT
    }

    inline void
    special_func_ptr(std::byte* buf, invoke_special_func_t func) noexcept
    {
      special_func_ptr(buf) = func;
    }

    inline auto
    body_ptr(std::byte* buf) noexcept -> std::byte*
    {
      return buf + header_size + func_ptr_size + func_ptr_size;
    }

    inline void
    invoke(std::byte* buf) noexcept
    {
      invoke_ptr(buf)(body_ptr(buf));
    }

    inline void
    invoke_special_func(std::byte* buf, method m, std::byte* that = nullptr) noexcept
    {
      special_func_ptr(buf)(body_ptr(buf), m, that ? body_ptr(that) : nullptr);
    }

    template<typename callable_t, typename... arg_ts>
    struct deferred_callable final : std::remove_cvref_t<callable_t>
    {
      using base_t = std::remove_cvref_t<callable_t>;

      explicit deferred_callable(callable_t func, arg_ts... args)
        : base_t(FWD(func))
        , args_(FWD(args)...)
      {}

      auto
      operator()() -> decltype(auto)
        requires std::invocable<base_t, arg_ts...>
      {
        return std::apply(
          [this](auto&&... args) mutable
          {
            (void)this; // silence clang 'unused this'-warning
            return base_t::operator()(static_cast<arg_ts>(args)...);
          },
          args_);
      }

    private:
      using tuple_t = decltype(std::tuple(std::declval<arg_ts>()...));
      tuple_t args_;
    };

    template<typename callable_t, typename... arg_ts>
    deferred_callable(callable_t&& callable, arg_ts&&... args)
      -> deferred_callable<decltype(callable), decltype(args)...>;

    template<typename T>
    struct is_function : std::false_type
    {};

    template<std::size_t size>
    struct is_function<function<size>> : std::true_type
    {};

    template<typename T>
    struct is_function_dyn : std::false_type
    {};

    template<>
    struct is_function_dyn<function_dyn> : std::true_type
    {};

    template<typename callable_t, typename... arg_ts>
    struct required_buffer_size :
      std::integral_constant<std::size_t, details::function_buffer_meta_size + sizeof(callable_t) +
                                            (0 + ... + sizeof(arg_ts))>
    {};

    template<std::size_t size>
    struct required_buffer_size<function<size>> : std::integral_constant<std::size_t, size>
    {};
  }

  template<typename callable_t>
  constexpr bool is_function_v = details::is_function<std::remove_cvref_t<callable_t>>::value;

  template<typename callable_t>
  constexpr bool is_function_dyn_v =
    details::is_function_dyn<std::remove_cvref_t<callable_t>>::value;

  template<typename callable_t, typename... arg_ts>
  constexpr std::size_t required_buffer_size_v =
    details::required_buffer_size<std::remove_cvref_t<callable_t>,
                                  std::remove_cvref_t<arg_ts>...>::value;

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
  template<std::size_t buffer_size>
  struct function_buffer
  {
    static_assert(buffer_size <= std::numeric_limits<std::uint8_t>::max(),
                  "Buffer size must be <= 255");
    using buffer_t = std::array<std::byte, buffer_size>;

    /// @brief Assigns a `function<size>` object to the buffer.
    /// @details Copies the provided `function<size>` object into the buffer.
    /// @tparam `size` The size of the function.
    /// @param `func` The `function<size>` object to assign.
    /// @requires `size <= buffer_size`
    template<std::size_t size>
      requires (size <= buffer_size)
    void
    assign(function<size> const& func) noexcept
    {
      (*this) = func.buffer();
    }

    /// @brief Assigns a moved `function<size>` object to the buffer.
    /// @details Moves the provided `function<size>` object into the buffer.
    /// @tparam `size` The size of the function.
    /// @param `func` The `function<size>` object to move.
    /// @requires `size <= buffer_size`
    template<std::size_t size>
      requires (size <= buffer_size)
    void
    assign(function<size>&& func) noexcept
    {
      (*this) = std::move(std::move(func).buffer());
    }

    /// @brief Assigns a callable and its arguments to the buffer.
    /// @details Stores the callable and its arguments, potentially wrapping them in a
    /// `deferred_callable`.
    /// @tparam `func_t` The type of the callable.
    /// @tparam `arg_ts` The types of the arguments.
    /// @param `func` The callable to store.
    /// @param `args` The arguments to bind to the callable.
    /// @requires `std::invocable<func_t&&, arg_ts&&...>`
    template<typename func_t, typename... arg_ts>
    void
    assign(func_t&& func, arg_ts&&... args) noexcept
      requires std::invocable<func_t&&, arg_ts&&...> && (sizeof...(args) > 0)
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
    /// @tparam `callable_t` The type of the callable.
    /// @param `callable` The callable to store.
    /// @requires `required_buffer_size_v<decltype(callable)> > buffer_size`
    template<std::invocable callable_t,
             typename callable_value_t = std::remove_reference_t<callable_t>>
    void
    assign(callable_t&& callable) noexcept // NOLINT
                                           // (bug in clang-tidy doesn't detect FWD in capture)
      requires (!is_function_v<callable_t>) &&
               (required_buffer_size_v<decltype(callable)> > buffer_size)
    {
      assign(
        [func = std::make_shared<std::remove_reference_t<decltype(callable)>>(FWD(callable))]
        {
          std::forward<callable_t> (*func)();
        });
    }

    /// @brief Assigns a callable that fits within the buffer size.
    /// @details Directly constructs the callable in the buffer.
    /// @tparam `callable_t` The type of the callable.
    /// @param `callable` The callable to store.
    /// @requires `required_buffer_size_v<decltype(callable)> <= buffer_size`
    template<std::invocable callable_t,
             typename callable_value_t = std::remove_reference_t<callable_t>>
    void
    assign(callable_t&& callable) noexcept
      requires (!is_function_v<callable_t>) &&
               (required_buffer_size_v<decltype(callable)> <= buffer_size)
    {
      static constexpr std::uint8_t total_size = required_buffer_size_v<decltype(callable)>;

      static_assert(std::copy_constructible<callable_value_t>,
                    "callable must be copy-constructible");
      reset();

      // header (size)
      // body (invocation pointer)
      // body (destructor pointer)
      // body (callable)
      details::size(buffer_.data(), total_size);
      details::invoke_ptr(buffer_.data(), std::addressof(details::invoke_func<callable_value_t>));
      details::special_func_ptr(buffer_.data(),
                                std::addressof(details::invoke_special_func<callable_value_t>));
      std::construct_at(reinterpret_cast<std::remove_const_t<callable_value_t>*>( // NOLINT
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

    /// @brief Constructor that takes a `function<size>` object.
    /// @details Initializes the buffer with the provided `function<size>` object, which must fit
    /// within the buffer size.
    /// @tparam `size` The size of the function.
    /// @param `func` The `function<size>` object to store.
    /// @requires `size` <= buffer_size
    template<std::size_t size>
    explicit function_buffer(function<size> const& func) noexcept
      : function_buffer<size>(func.buffer())
    {}

    /// @brief Constructor that takes a callable and its arguments.
    /// @details Initializes the buffer with a callable and its arguments, potentially wrapping them
    /// in a `deferred_callable`.
    /// @tparam `func_t` The type of the callable.
    /// @tparam `arg_ts` The types of the arguments.
    /// @param `func` The callable to store.
    /// @param `args` The arguments to bind to the callable.
    /// @requires `std::invocable<func_t&&, arg_ts&&...>`
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

  template<typename callable_t, typename... arg_ts>
  function_buffer(callable_t&&, arg_ts&&...)
    -> function_buffer<required_buffer_size_v<callable_t, arg_ts...>>;

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
    /// @tparam `size` The size of the `function_buffer`.
    /// @param `buffer` The `function_buffer` to copy from.
    template<std::size_t size>
    explicit function_dyn(function_buffer<size> const& buffer)
    {
      copy_buffer({buffer.data(), buffer.size()});
    }

    /// @brief Constructor that takes a `function<size>`.
    /// @details Initializes the `function_dyn` with the contents of a `function<size>`, copying the
    /// stored callable.
    /// @tparam `size` The size of the `function`.
    /// @param `func` The `function<size>` to copy from.
    template<std::size_t size>
    function_dyn(function<size> const& func) noexcept
      : function_dyn(func.buffer())
    {}

    /// @brief Constructor that takes a callable.
    /// @details Initializes the `function_dyn` with the provided callable, storing it dynamically.
    /// @tparam `callable_t` The type of the callable.
    /// @param `callable` The callable to store.
    /// @requires `std::invocable<callable_t&&>` and not `fho::is_function<callable_t>` and not
    /// `fho::is_function_dyn<callable_t>`
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
  template<typename buffer_t, typename callable_t, typename... arg_ts>
  concept assignable = requires (buffer_t&& buf, callable_t&& callable, arg_ts&&... args) {
                         buf.assign(FWD(callable), FWD(args)...);
                       };

  template<std::size_t buffer_size = details::cache_line_size - sizeof(details::invoke_func_t)>
  struct function
  {
  private:
    using buffer_t = function_buffer<buffer_size>;
    buffer_t buffer_;

  public:
    function() = default;

    function(buffer_t buffer)
      : buffer_(std::move(buffer))
    {}

    function(function const& func)
      : buffer_(func.buffer_)
    {}

    function(function&& func) noexcept
      : buffer_(std::move(func.buffer_))
    {}

    template<typename... arg_ts>
    explicit function(std::invocable<arg_ts...> auto&& callable, arg_ts&&... args) noexcept
      requires (!is_function_v<decltype(callable)>) &&
               assignable<buffer_t, decltype(callable), decltype(args)...>
    {
      assign(FWD(callable), FWD(args)...);
    }

    ~function()
    {
      reset();
    }

    auto operator=(function const& func) noexcept -> function& = default;

    auto
    operator=(function&& func) noexcept -> function&
    {
      buffer_ = std::move(func.buffer_);
      return *this;
    }

    auto
    operator=(std::invocable auto&& func) noexcept -> function&
      requires std::same_as<function, std::remove_cvref_t<decltype(func)>> || true
    {
      assign(FWD(func));
      return *this;
    }

    auto
    operator=(std::nullptr_t) noexcept -> auto&
    {
      reset();
      return *this;
    }

    inline void
    operator()()
    {
      details::invoke(buffer_.data());
    }

    inline
    operator bool() const noexcept
    {
      return buffer_.size() != 0;
    }

    auto
    buffer() const noexcept -> buffer_t const&
    {
      return buffer_;
    }

    auto
    buffer() noexcept -> buffer_t&
    {
      return buffer_;
    }

    template<typename... arg_ts>
    void
    assign(std::invocable<arg_ts...> auto&& callable, arg_ts&&... args) noexcept
      requires assignable<buffer_t, decltype(callable), decltype(args)...>
    {
      buffer_.assign(FWD(callable), FWD(args)...);
    }

    inline void
    reset() noexcept
    {
      buffer_.reset();
    }

    [[nodiscard]] inline auto
    size() const noexcept -> std::uint8_t
    {
      return buffer_.size();
    }
  };
}

#undef FWD
