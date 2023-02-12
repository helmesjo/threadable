#pragma once

#include <threadable/std_concepts.hxx>

#include <array>
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
    static constexpr auto func_ptr_size = sizeof(invoke_func_t);

    template<typename callable_t>
      requires std::invocable<callable_t>
    void set(callable_t&& callable) noexcept
    {
      using callable_value_t = std::remove_reference_t<callable_t>;
      static constexpr auto callable_size = sizeof(callable_value_t);

      static_assert(func_ptr_size + callable_size <= buffer_size, "callable won't fit in function buffer");

      // store invoke-pointer in header, and actual
      // callable in the remaining buffer
      invoke_ptr(std::addressof(details::invoke_func<callable_value_t>));
      auto* buffPtr = buffer.data() + func_ptr_size;
      if constexpr(std::is_trivially_copyable_v<callable_value_t>)
      {
        std::memcpy(buffPtr, std::addressof(callable), callable_size);
      }
      else
      {
        if(::new (buffPtr) callable_value_t(FWD(callable)) != buffPtr)
        {
          std::terminate();
        }
      }
    }

    template<typename callable_t, typename... arg_ts>
      requires std::invocable<callable_t, arg_ts...>
    void set(callable_t&& func, arg_ts&&... args) noexcept
    {
      const auto tmp = [smerg = [func = FWD(func), ...args = FWD(args)]{}]{};
      static_assert(buffer_size >= sizeof(tmp));

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
      invoke_ptr()(buffer.data() + func_ptr_size);
    }

    inline operator bool() const noexcept
    {
      return invoke_ptr() != nullptr;
    }

  private:
    inline invoke_func_t& invoke_ptr() noexcept
    {
      return reinterpret_cast<invoke_func_t&>(*buffer.data());
    }

    inline void invoke_ptr(invoke_func_t func) noexcept
    {
      invoke_ptr() = func;
    }

    buffer_t buffer;
  };
}

#undef FWD
