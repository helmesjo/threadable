#pragma once

#if !defined(_WIN32)
  #include <cstdlib>
#endif
#include <memory>

namespace threadable
{
  template<typename T, std::size_t alignment>
  struct aligned_allocator : public std::allocator<T>
  {
    aligned_allocator() = default;

    using base_t = std::allocator<T>;
    using base_t::base_t;

    // Type definitions
    using pointer   = typename std::allocator_traits<base_t>::pointer;
    using size_type = typename std::allocator_traits<base_t>::size_type;

    template<typename U>
    struct rebind
    {
      using other = aligned_allocator<U, alignment>;
    };

    inline auto
    allocate(size_type n) -> pointer
    {
#if defined(_WIN32)
      auto p = static_cast<pointer>(_aligned_malloc(n * sizeof(T), alignment));
#else
      auto p = static_cast<pointer>(std::aligned_alloc(alignment, n * sizeof(T)));
#endif
      if (p) // NOLINT
      {
        return p;
      }
      throw std::bad_alloc();
    }

    inline void
    deallocate(pointer p, size_type) noexcept
    {
#if defined(_WIN32)
      _aligned_free(p); // NOLINT
#else
      std::free(p); // NOLINT
#endif
    }
  };
}
