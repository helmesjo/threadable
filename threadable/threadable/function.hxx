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
    static inline void invoke_func(void* addr)
    {
      std::invoke(*static_cast<callable_t*>(addr));
    }

    template<typename callable_t>
    static inline void invoke_dtor(void* addr)
    {
      if constexpr(!std::is_trivially_copyable_v<callable_t> && std::is_destructible_v<callable_t>)
      {
        static_cast<callable_t*>(addr)->~callable_t();
      }
    }

    using invoke_func_t = decltype(&invoke_func<void(*)()>);
    using invoke_dtor_t = decltype(&invoke_dtor<void(*)()>);
    static_assert(sizeof(invoke_func_t) == sizeof(invoke_dtor_t));
    static constexpr std::uint8_t header_size = sizeof(std::uint8_t);
    static constexpr std::uint8_t func_ptr_size = sizeof(invoke_func_t);

    inline std::uint8_t& size(std::uint8_t* buf) noexcept
    {
      return static_cast<std::uint8_t&>(*buf);
    }

    inline std::uint8_t size(const std::uint8_t* buf) noexcept
    {
      return static_cast<std::uint8_t>(*buf);
    }

    inline void size(std::uint8_t* buf, std::uint8_t s) noexcept
    {
      size(buf) = s;
    }

    inline invoke_func_t& invoke_ptr(std::uint8_t* buf) noexcept
    {
      return reinterpret_cast<invoke_func_t&>(*(buf + header_size));
    }

    inline void invoke_ptr(std::uint8_t* buf, invoke_func_t func) noexcept
    {
      invoke_ptr(buf) = func;
    }

    inline invoke_dtor_t& dtor_ptr(std::uint8_t* buf) noexcept
    {
      return reinterpret_cast<invoke_dtor_t&>(*(buf + header_size + func_ptr_size));
    }

    inline void dtor_ptr(std::uint8_t* buf, invoke_dtor_t func) noexcept
    {
      dtor_ptr(buf) = func;
    }

    inline typename std::uint8_t* body_ptr(std::uint8_t* buf) noexcept
    {
      return buf + header_size + func_ptr_size + func_ptr_size;
    }

    inline void invoke(std::uint8_t* buf) noexcept
    {
      invoke_ptr(buf)(body_ptr(buf));
    }

    inline void invoke_dtor(std::uint8_t* buf) noexcept
    {
      dtor_ptr(buf)(body_ptr(buf));
    }
  }

  template<std::size_t buffer_size = details::cache_line_size - details::header_size - (details::func_ptr_size * 2)>
  struct function_buffer
  {
    using buffer_t = std::array<std::uint8_t, buffer_size>;

    function_buffer()
    {
      details::size(buffer_.data(), 0);
    }

    ~function_buffer()
    {
      reset();
    }

    template<typename callable_t>
      requires std::invocable<callable_t>
    void set(callable_t&& callable) noexcept
    {
      using callable_value_t = std::remove_reference_t<callable_t>;
      static constexpr std::uint8_t callable_size = sizeof(callable_value_t);
      static constexpr std::uint8_t total_size = details::header_size + (details::func_ptr_size * 2) + callable_size;

      static_assert(total_size <= buffer_size, "callable won't fit in function buffer");

      // header (size)
      // body (invocation pointer)
      // body (destructor pointer)
      // body (callable)
      details::size(buffer_.data(), total_size);
      details::invoke_ptr(buffer_.data(), std::addressof(details::invoke_func<callable_value_t>));
      details::dtor_ptr(buffer_.data(), std::addressof(details::invoke_dtor<callable_value_t>));
      auto bodyPtr = details::body_ptr(buffer_.data());
      if constexpr(std::is_trivially_copyable_v<callable_value_t>)
      {
        std::memcpy(bodyPtr, std::addressof(callable), callable_size);
      }
      else
      {
        if(::new (bodyPtr) callable_value_t(FWD(callable)) != static_cast<void*>(bodyPtr))
        {
          std::terminate();
        }
      }
    }

    inline void reset() noexcept
    {
      if(size() > 0)
      {
        details::invoke_dtor(data());
      }
      details::size(buffer_.data(), 0);
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

  struct function_dyn
  {
    template<std::size_t buffer_size>
    explicit function_dyn(const function_buffer<buffer_size>& buffer)
    {
      assert(buffer.size() > 0);
      const auto size = buffer.size();
      buffer_ = std::make_unique_for_overwrite<std::uint8_t[]>(size);
      std::memcpy(buffer_.get(), buffer.data(), size);
    }

    function_dyn(function_dyn&& func):
      buffer_(std::move(func.buffer_))
    {
    }

    inline void operator()()
    {
      details::invoke(buffer_.get());
    }

    inline operator bool() const noexcept
    {
      return details::size(buffer_.get()) != 0;
    }

  private:
    std::unique_ptr<std::uint8_t[]> buffer_;
  };

  template<std::size_t buffer_size = details::cache_line_size - sizeof(details::invoke_func_t)>
  struct function
  {
    function() = default;

    function(const function& func):
      buffer_(func.buffer_)
    {
    }

    function(function&& func):
      buffer_(std::move(func.buffer_))
    {
    }

    function(std::invocable auto&& func)
      requires (!std::same_as<function, std::remove_cvref_t<decltype(func)>>)
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

    operator std::function<void()>() const noexcept
    {
      return [func = *this]() mutable { func(); };
    }

    operator function_dyn() const noexcept
    {
      return function_dyn(buffer_);
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
    function_buffer<buffer_size> buffer_;
  };
}

#undef FWD
