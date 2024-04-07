#pragma once

#include <memory>
#include <new>

namespace threadable
{
  template<typename T, std::size_t alignment>
  struct aligned_allocator : public std::allocator<T>
  {
    aligned_allocator() = default;

    using base_t = std::allocator<T>;
    using base_t::base_t;

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
      return static_cast<pointer>(
        ::operator new[](n * sizeof(T), std::align_val_t(alignment))); // NOLINT
    }

    inline void
    deallocate(pointer p, size_type) noexcept
    {
      ::operator delete[](p, std::align_val_t{alignment}); // NOLINT
    }
  };
}
