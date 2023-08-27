#pragma once

#include <threadable/std_concepts.hxx>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
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
    inline constexpr void invoke_func(void* addr)
    {
      (*static_cast<callable_t*>(addr))();
    }

    template<typename callable_t>
    inline constexpr void invoke_dtor(void* addr)
    {
      if constexpr(std::destructible<callable_t>)
      {
        std::destroy_at(static_cast<callable_t*>(addr));
      }
    }

    using invoke_func_t = decltype(&invoke_func<void(*)()>);
    using invoke_dtor_t = decltype(&invoke_dtor<void(*)()>);
    static_assert(sizeof(invoke_func_t) == sizeof(invoke_dtor_t));
    inline constexpr std::uint8_t header_size = sizeof(std::uint8_t);
    inline constexpr std::uint8_t func_ptr_size = sizeof(invoke_func_t);
    inline constexpr std::size_t function_buffer_meta_size = details::header_size + (details::func_ptr_size * 2);

    inline constexpr std::uint8_t& size(std::uint8_t* buf) noexcept
    {
      return static_cast<std::uint8_t&>(*buf);
    }

    inline constexpr std::uint8_t size(const std::uint8_t* buf) noexcept
    {
      return static_cast<std::uint8_t>(*buf);
    }

    inline constexpr void size(std::uint8_t* buf, std::uint8_t s) noexcept
    {
      size(buf) = s;
    }

    inline constexpr invoke_func_t& invoke_ptr(std::uint8_t* buf) noexcept
    {
      return *static_cast<invoke_func_t*>(static_cast<void*>(buf + header_size));
    }

    inline constexpr void invoke_ptr(std::uint8_t* buf, invoke_func_t func) noexcept
    {
      invoke_ptr(buf) = func;
    }

    inline constexpr invoke_dtor_t& dtor_ptr(std::uint8_t* buf) noexcept
    {
      return *static_cast<invoke_dtor_t*>(static_cast<void*>(buf + header_size + func_ptr_size));
    }

    inline constexpr void dtor_ptr(std::uint8_t* buf, invoke_dtor_t func) noexcept
    {
      dtor_ptr(buf) = func;
    }

    inline constexpr typename std::uint8_t* body_ptr(std::uint8_t* buf) noexcept
    {
      return buf + header_size + func_ptr_size + func_ptr_size;
    }

    inline constexpr void invoke(std::uint8_t* buf) noexcept
    {
      invoke_ptr(buf)(body_ptr(buf));
    }

    inline constexpr void invoke_dtor(std::uint8_t* buf) noexcept
    {
      dtor_ptr(buf)(body_ptr(buf));
    }

    template<typename T>
    struct is_function: std::false_type{};

    template<std::size_t size>
    struct is_function<function<size>>: std::true_type{};

    template<>
    struct is_function<function_dyn>: std::true_type{};

    template<std::invocable callable_t>
    struct required_buffer_size: std::integral_constant<std::size_t, details::function_buffer_meta_size + sizeof(callable_t)>{};

    template<std::size_t size>
    struct required_buffer_size<function<size>>: std::integral_constant<std::size_t, size>{};
  }

  template<typename func_t>
  constexpr bool is_function_v = details::is_function<std::remove_cvref_t<func_t>>::value;

  template<std::invocable func_t>
  constexpr std::size_t required_buffer_size_v = details::required_buffer_size<std::remove_cvref_t<func_t>>::value;

  template<std::size_t buffer_size>
  struct function_buffer
  {
    using buffer_t = std::array<std::uint8_t, buffer_size>;

    function_buffer()
    {
      details::size(buffer_.data(), 0);
    }

    function_buffer(const function_buffer& buffer)
    : buffer_(buffer.buffer_)
    {
    }

    function_buffer(function_buffer&& buffer)
    : buffer_(std::move(buffer.buffer_))
    {
      details::size(buffer.buffer_.data(), 0);
    }

    template<std::size_t size>
    explicit function_buffer(const function<size>& func) noexcept
    : function_buffer<size>(func.buffer())
    {
    }

    explicit function_buffer(std::invocable auto&& callable) noexcept
      requires (!is_function_v<decltype(callable)>)
    {
      details::size(buffer_.data(), 0);
      set(FWD(callable));
    }

    ~function_buffer()
    {
      reset();
    }

    auto& operator=(const function_buffer& buffer)
    {
      buffer_ = buffer.buffer_;
      return *this;
    }

    auto& operator=(function_buffer&& buffer)
    {
      buffer_ = std::move(buffer.buffer_);
      return *this;
    }

    template<std::size_t size>
    void set(const function<size>& func) noexcept
    {
      (*this) = func.buffer();
    }

    template<std::size_t size>
    void set(function<size>&& func) noexcept
    {
      (*this) = std::move(FWD(func).buffer());
    }

    template<typename callable_t>
      requires std::invocable<callable_t>
            && (!is_function_v<callable_t>)
    void set(callable_t&& callable) noexcept
    {
      using callable_value_t = std::remove_reference_t<callable_t>;
      static constexpr std::uint8_t total_size = required_buffer_size_v<callable_t>;

      static_assert(total_size <= buffer_size, "callable won't fit in function buffer");
      reset();

      // header (size)
      // body (invocation pointer)
      // body (destructor pointer)
      // body (callable)
      details::size(buffer_.data(), total_size);
      details::invoke_ptr(buffer_.data(), std::addressof(details::invoke_func<callable_value_t>));
      details::dtor_ptr(buffer_.data(), std::addressof(details::invoke_dtor<callable_value_t>));
      auto bodyPtr = details::body_ptr(buffer_.data());
      std::construct_at(reinterpret_cast<callable_value_t*>(bodyPtr), callable_value_t(FWD(callable)));
    }

    inline void reset() noexcept
    {
      if(size() > 0)
      {
        details::invoke_dtor(data());
        details::size(buffer_.data(), 0);
      }
    }

    inline std::uint8_t size() const noexcept
    {
      return details::size(data());
    }

    inline std::uint8_t* data() noexcept
    {
      return buffer_.data();
    }

    inline const std::uint8_t* data() const noexcept
    {
      return buffer_.data();
    }

  private:
    buffer_t buffer_;
  };

  template<std::invocable callable_t>
  function_buffer(callable_t&&) -> function_buffer<required_buffer_size_v<callable_t>>;

  struct function_dyn
  {
    function_dyn(const function_dyn& that)
    {
      const auto size = that.size();
      buffer_ = std::make_unique<std::uint8_t[]>(size);
      std::memcpy(buffer_.get(), that.buffer_.get(), size);
    }

    function_dyn(function_dyn&& that)
    : buffer_(std::move(that.buffer_))
    {
    }

    template<std::size_t buffer_size>
    explicit function_dyn(const function_buffer<buffer_size>& buffer)
    {
      assert(buffer.size() > 0);
      const auto size = buffer.size();
      // buffer_ = std::make_unique_for_overwrite<std::uint8_t[]>(size);
      buffer_ = std::make_unique<std::uint8_t[]>(size);
      std::memcpy(buffer_.get(), buffer.data(), size);
    }

    template<std::size_t size>
    function_dyn(const function<size>& func) noexcept
    : function_dyn(func.buffer())
    {
    }

    function_dyn(std::invocable auto&& callable) noexcept
      requires (!is_function_v<decltype(callable)>)
    : function_dyn(function_buffer(FWD(callable)))
    {
    }

    inline void operator()()
    {
      details::invoke(buffer_.get());
    }

    inline operator bool() const noexcept
    {
      return size() != 0;
    }

    inline std::uint8_t size() const noexcept
    {
      return details::size(buffer_.get());
    }

  private:
    std::unique_ptr<std::uint8_t[]> buffer_;
  };

  template<std::size_t buffer_size = details::cache_line_size - sizeof(details::invoke_func_t)>
  struct function
  {
    using buffer_t = function_buffer<buffer_size>;

    function() = default;

    function(const function& func):
      buffer_(func.buffer_)
    {
    }

    function(function&& func):
      buffer_(std::move(func.buffer_))
    {
    }

    explicit function(std::invocable auto&& func)
      requires (!is_function_v<decltype(func)>)
    {
      set(FWD(func));
    }

    function& operator=(const function& func) noexcept
    {
      buffer_ = func.buffer_;
      return *this;
    }

    function& operator=(function&& func) noexcept
    {
      buffer_ = std::move(func.buffer_);
      return *this;
    }

    function& operator=(std::invocable auto&& func) noexcept
      requires (!std::same_as<function, std::remove_cvref_t<decltype(func)>>)
    {
      set(FWD(func));
      return *this;
    }

    auto& operator=(std::nullptr_t) noexcept
    {
      reset();
      return *this;
    }

    inline void operator()()
    {
      details::invoke(buffer_.data());
    }

    inline operator bool() const noexcept
    {
      return buffer_.size() != 0;
    }

    const buffer_t& buffer() const noexcept
    {
      return buffer_;
    }

    buffer_t& buffer() noexcept
    {
      return buffer_;
    }

    template<typename callable_t>
      requires std::invocable<callable_t>
    void set(callable_t&& callable) noexcept
    {
      buffer_.set(FWD(callable));
    }

    template<typename callable_t, typename... arg_ts>
      requires std::invocable<callable_t, arg_ts...>
    void set(callable_t&& func, arg_ts&&... args) noexcept
    {
      set([func = FWD(func), ...args = FWD(args)]() mutable {
        std::invoke(FWD(func), FWD(args)...);
      });
    }

    inline void reset() noexcept
    {
      buffer_.reset();
    }

    inline std::uint8_t size() const noexcept
    {
      return buffer_.size();
    }

  private:
    buffer_t buffer_;
  };
}

#undef FWD
