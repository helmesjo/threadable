#pragma once

#include <type_traits>
#include <utility>

namespace fho::stdext
{

  /// @brief See: https://en.cppreference.com/w/cpp/utility/forward_like.html
  template<class T, class U>
  constexpr auto&&
  forward_like(U&& x) noexcept // NOLINT
  {
#if __cpp_lib_forward_like >= 202207L
    return std::forward_like<T>(x);
#else
    constexpr bool is_adding_const = std::is_const_v<std::remove_reference_t<T>>;
    if constexpr (std::is_lvalue_reference_v<T&&>)
    {
      if constexpr (is_adding_const)
      {
        return std::as_const(x);
      }
      else
      {
        return static_cast<U&>(x);
      }
    }
    else
    {
      if constexpr (is_adding_const)
      {
        return std::move(std::as_const(x));
      }
      else
      {
        return std::move(x); // NOLINT
      }
    }
#endif
  }
}
