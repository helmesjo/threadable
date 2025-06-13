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
#include <span>
#include <type_traits>
#include <version>

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

namespace threadable
{
  template<std::size_t>
  struct function;

  struct function_dyn;

  namespace details
  {
#if __cpp_lib_hardware_interference_size >= 201603
    // Pretty much no compiler implements this yet
    constexpr auto cache_line_size = std::hardware_destructive_interference_size;
#else
    // TODO: Make portable
    constexpr auto cache_line_size = std::size_t{64};
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

  template<std::size_t buffer_size>
  struct function_buffer
  {
    static_assert(buffer_size <= std::numeric_limits<std::uint8_t>::max(),
                  "Buffer size must be <= 255");
    using buffer_t = std::array<std::byte, buffer_size>;

    template<std::size_t size>
      requires (size <= buffer_size)
    void
    set(function<size> const& func) noexcept
    {
      (*this) = func.buffer();
    }

    template<std::size_t size>
      requires (size <= buffer_size)
    void
    set(function<size>&& func) noexcept
    {
      (*this) = std::move(std::move(func).buffer());
    }

    template<typename func_t, typename... arg_ts>
    void
    set(func_t&& func, arg_ts&&... args) noexcept
      requires std::invocable<func_t&&, arg_ts&&...> && (sizeof...(args) > 0)
    {
      using func_value_t = std::remove_reference_t<decltype(func)>;
      if constexpr (std::is_function_v<func_value_t> ||
                    std::is_member_function_pointer_v<func_value_t>)
      {
        set(
          [func, ... args = FWD(args)]() mutable
          {
            std::invoke(func, FWD(args)...);
          });
      }
      else
      {
        set(details::deferred_callable(FWD(func), FWD(args)...));
      }
    }

    template<std::invocable callable_t,
             typename callable_value_t = std::remove_reference_t<callable_t>>
    void
    set(callable_t&& callable) noexcept // NOLINT
                                        // (bug in clang-tidy doesn't detect FWD incapture)
      requires (!is_function_v<callable_t>) &&
               (required_buffer_size_v<decltype(callable)> > buffer_size)
    {
      set(
        [func = std::make_shared<std::remove_reference_t<decltype(callable)>>(FWD(callable))]
        {
          std::forward<callable_t> (*func)();
        });
    }

    template<std::invocable callable_t,
             typename callable_value_t = std::remove_reference_t<callable_t>>
    void
    set(callable_t&& callable) noexcept
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

    function_buffer()
    {
      details::size(buffer_.data(), 0);
    }

    function_buffer(function_buffer const& buffer)
    {
      *this = buffer;
    }

    function_buffer(function_buffer&& buffer) noexcept
    {
      *this = std::move(buffer);
    }

    template<std::size_t size>
    explicit function_buffer(function<size> const& func) noexcept
      : function_buffer<size>(func.buffer())
    {}

    function_buffer(auto&& callable, auto&&... args) noexcept
      requires requires { this->set(FWD(callable), FWD(args)...); }
    {
      details::size(buffer_.data(), 0);
      set(FWD(callable), FWD(args)...);
    }

    ~function_buffer()
    {
      reset();
    }

    auto
    operator=(function_buffer const& buffer) -> auto&
    {
      std::memcpy(data(), buffer.data(), details::function_buffer_meta_size);
      details::invoke_special_func(data(), details::method::copy_ctor,
                                   const_cast<std::byte*>(buffer.data())); // NOLINT
      return *this;
    }

    auto
    operator=(function_buffer&& buffer) noexcept -> auto&
    {
      std::memcpy(data(), buffer.data(), details::function_buffer_meta_size);
      details::invoke_special_func(data(), details::method::move_ctor, buffer.data());
      details::size(buffer.data(), 0);
      return *this;
    }

    inline void
    reset() noexcept
    {
      if (size() > 0)
      {
        details::invoke_special_func(data(), details::method::dtor);
        details::size(buffer_.data(), 0);
      }
    }

    [[nodiscard]] inline auto
    size() const noexcept -> std::uint8_t
    {
      return details::size(data());
    }

    inline auto
    data() noexcept -> std::byte*
    {
      return buffer_.data();
    }

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

  struct function_dyn
  {
    auto operator=(function_dyn const&) -> function_dyn& = delete;
    auto operator=(function_dyn&&) -> function_dyn&      = delete;

    function_dyn(function_dyn const& that)
    {
      copy_buffer({that.buffer_, that.size()});
    }

    function_dyn(function_dyn&& that) noexcept
      : buffer_(that.buffer_)
    {
      that.buffer_ = nullptr;
    }

    template<std::size_t buffer_size>
    explicit function_dyn(function_buffer<buffer_size> const& buffer)
    {
      copy_buffer({buffer.data(), buffer.size()});
    }

    template<std::size_t size>
    function_dyn(function<size> const& func) noexcept
      : function_dyn(func.buffer())
    {}

    function_dyn(std::invocable auto&& callable) noexcept
      requires (!is_function_v<decltype(callable)> && !is_function_dyn_v<decltype(callable)>)
      : function_dyn(function_buffer(FWD(callable)))
    {}

    ~function_dyn()
    {
      reset();
    }

    inline void
    operator()()
    {
      details::invoke(buffer_);
    }

    inline
    operator bool() const noexcept
    {
      return size() != 0;
    }

    [[nodiscard]] inline auto
    size() const noexcept -> std::uint8_t
    {
      return buffer_ ? details::size(buffer_) : 0;
    }

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
  concept settable = requires (buffer_t&& buf, callable_t&& callable, arg_ts&&... args) {
                       buf.set(FWD(callable), FWD(args)...);
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
               settable<buffer_t, decltype(callable), decltype(args)...>
    {
      set(FWD(callable), FWD(args)...);
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
      set(FWD(func));
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
    set(std::invocable<arg_ts...> auto&& callable, arg_ts&&... args) noexcept
      requires settable<buffer_t, decltype(callable), decltype(args)...>
    {
      buffer_.set(FWD(callable), FWD(args)...);
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
