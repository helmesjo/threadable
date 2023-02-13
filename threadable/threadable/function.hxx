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
#include <vector>
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
        callable_t& func = *static_cast<callable_t*>(addr);
        std::invoke(func);
        if constexpr(std::is_destructible_v<callable_t>)
        {
          func.~callable_t();
        }
      }
      using invoke_func_t = decltype(&invoke_func<void(*)()>);
  }

  template<std::size_t buffer_size = details::cache_line_size - sizeof(details::invoke_func_t)>
  struct function
  {
    using buffer_t = std::array<std::uint8_t, buffer_size>;
    using invoke_func_t = details::invoke_func_t;
    static constexpr std::uint8_t header_size = sizeof(std::uint8_t);
    static constexpr std::uint8_t func_ptr_size = sizeof(invoke_func_t);

    template<typename callable_t>
      requires std::invocable<callable_t>
    void set(callable_t&& callable) noexcept
    {
      using callable_value_t = std::remove_reference_t<callable_t>;
      static constexpr std::uint8_t callable_size = sizeof(callable_value_t);

      static_assert(header_size + func_ptr_size + callable_size <= buffer_size, "callable won't fit in function buffer");

      // header (size)
      // header (invocation pointer)
      // body   (callable)
      size(header_size + func_ptr_size + callable_size);
      invoke_ptr(std::addressof(details::invoke_func<callable_value_t>));
      auto buffPtr = body_ptr();
      if constexpr(std::is_trivially_copyable_v<callable_value_t>)
      {
        std::memcpy(buffPtr, std::addressof(callable), callable_size);
      }
      else
      {
        if(::new (buffPtr) callable_value_t(FWD(callable)) != static_cast<void*>(buffPtr))
        {
          std::terminate();
        }
      }
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
      invoke_ptr(nullptr);
    }

    template<typename callable_t, typename... arg_ts>
      requires std::invocable<callable_t, arg_ts...> 
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

    inline void operator()()
    {
      invoke_ptr()(body_ptr());
    }

    inline operator bool() const noexcept
    {
      return invoke_ptr() != nullptr;
    }

    inline std::uint8_t size() const noexcept
    {
      return static_cast<std::uint8_t>(*buffer.data());
    }

    inline const auto buffer_ptr() const noexcept
    {
      return buffer.data();
    }

  private:
    friend struct function_trimmed;
    inline std::uint8_t& size() noexcept
    {
      return static_cast<std::uint8_t&>(*buffer.data());
    }

    inline void size(std::uint8_t s) noexcept
    {
      size() = s;
    }

    inline const invoke_func_t& invoke_ptr() const noexcept
    {
      return reinterpret_cast<const invoke_func_t&>(*(buffer.data() + header_size));
    }

    inline invoke_func_t& invoke_ptr() noexcept
    {
      return reinterpret_cast<invoke_func_t&>(*(buffer.data() + header_size));
    }

    inline void invoke_ptr(invoke_func_t func) noexcept
    {
      invoke_ptr() = func;
    }

    inline typename buffer_t::pointer body_ptr() noexcept
    {
      return buffer.data() + header_size + func_ptr_size;
    }

    buffer_t buffer;
  };

  struct function_trimmed
  {
    using buffer_t = std::uint8_t*;
    using invoke_func_t = details::invoke_func_t;
    static constexpr std::uint8_t header_size = function<>::header_size;
    static constexpr std::uint8_t func_ptr_size = function<>::func_ptr_size;

    template<typename func_t>
    explicit function_trimmed(func_t&& func)
    {
      assert(func);
      const auto size = func.size() - header_size;
      buffer_ = std::unique_ptr<std::uint8_t[]>(new std::uint8_t[size]);
      std::memcpy(buffer_.get(), func.buffer_ptr() + header_size, size);
    }
    function_trimmed(function_trimmed&&) = default;
    function_trimmed& operator=(function_trimmed&&) = default;

    function_trimmed() = delete;
    function_trimmed(const function_trimmed&) = delete;
    function_trimmed& operator=(const function_trimmed&) = delete;

    inline std::uint8_t size() const noexcept
    {
      return sizeof(buffer_t);
    }

    inline void operator()()
    {
      invoke_ptr()(body_ptr());
    }

  private:
    inline auto buffer() noexcept
    {
      return buffer_.get();
    }
    inline auto buffer() const noexcept
    {
      return buffer_.get();
    }
    inline const invoke_func_t& invoke_ptr() const noexcept
    {
      return reinterpret_cast<const invoke_func_t&>(*(buffer()));
    }

    inline invoke_func_t& invoke_ptr() noexcept
    {
      return reinterpret_cast<invoke_func_t&>(*(buffer()));
    }

    inline std::uint8_t* body_ptr() noexcept
    {
      return buffer() + func_ptr_size;
    }

    std::unique_ptr<std::uint8_t[]> buffer_;
  };

  template<std::size_t T>
  struct detect;

  // detect<sizeof(function_slim)> qdwdqwd;

}

#undef FWD
