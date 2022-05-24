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

  template<std::size_t buffer_size = details::cache_line_size>
  struct function
  {
    template<typename callable_t>
      requires std::invocable<callable_t>
    void set(callable_t&& func) noexcept
    {
      using callable_value_t = std::remove_reference_t<callable_t>;
      static_assert(sizeof(callable_value_t) <= buffer_size, "callable won't fit in function buffer");
      unwrap_func = std::addressof(details::invoke_func<callable_value_t>);
      void* buffPtr = buffer.data();
      if constexpr(std::is_trivially_copyable_v<callable_value_t>)
      {
        std::memcpy(buffPtr, std::addressof(func), sizeof(func));
      }
      else
      {
        if(::new (buffPtr) callable_value_t(FWD(func)) != buffPtr)
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

    void reset() noexcept
    {
      unwrap_func = nullptr;
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

    void operator()()
    {
      unwrap_func(buffer.data());
    }

    operator bool() const noexcept
    {
      return unwrap_func;
    }

  private:
    details::invoke_func_t unwrap_func = nullptr;
    std::array<std::uint8_t, buffer_size> buffer;
  };

}

#undef FWD
