#pragma once

#include <memory>

namespace threadable
{
  template<typename T>
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
      using other = aligned_allocator<U>;
    };

    inline auto
    allocate(size_type n) -> pointer
    {
      if (auto p = static_cast<pointer>(std::aligned_alloc(sizeof(T), n * sizeof(T)))) // NOLINT
      {
        return p;
      }
      throw std::bad_alloc();
    }

    inline void
    deallocate(pointer p, size_type) noexcept
    {
      std::free(p); // NOLINT
    }
  };
}
