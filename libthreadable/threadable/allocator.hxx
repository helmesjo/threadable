#pragma once

#include <memory>
#include <new>

namespace fho
{
  /// @brief A custom allocator that ensures memory allocations are aligned to a specified
  /// Alignment.
  /// @details This allocator inherits from `std::allocator<T>` and overrides the `allocate` and
  /// `deallocate` methods to use aligned memory allocation. The Alignment must be a power of 2 and
  /// at least as large as the Alignment of `T`, as enforced by a static assertion. This is useful
  /// for performance-critical applications where proper memory Alignment can improve efficiency,
  /// such as with SIMD types or cache-line aligned structures.
  /// @tparam `T` The type of objects being allocated.
  /// @tparam `Alignment` The desired Alignment for memory allocations, must be a power of 2 and >=
  /// alignof(T).
  /// @example
  /// ```cpp
  /// using aligned_vector_t = vector<int, fho::aligned_allocator<int, 64>>;
  /// auto vec = aligned_vector_t(100); // Allocates a vector of 100 ints with 64-byte Alignment
  /// ```
  template<typename T, std::size_t Alignment>
  struct aligned_allocator : public std::allocator<T>
  {
    static_assert(Alignment >= alignof(T) && (Alignment & (Alignment - 1)) == 0,
                  "Alignment must be a power of 2 and at least alignof(T)");

    aligned_allocator() = default;

    using base_t = std::allocator<T>;
    using base_t::base_t;

    using pointer   = typename std::allocator_traits<base_t>::pointer;
    using size_type = typename std::allocator_traits<base_t>::size_type;

    /// @brief Rebind the allocator to a different type.
    /// @details This is required for the allocator to be used with standard library containers. It
    /// defines how to get an allocator for a different type `U` with the same Alignment.
    template<typename U>
    struct rebind
    {
      using other = aligned_allocator<U, Alignment>;
    };

    /// @brief Allocates memory with the specified Alignment.
    /// @details Uses `::operator new[](size, std::align_val_t(Alignment))` to allocate memory that
    /// is aligned to `Alignment` bytes.
    /// @param `n` The number of elements to allocate.
    /// @return A pointer to the allocated memory.
    inline auto
    allocate(size_type n) -> pointer
    {
      return static_cast<pointer>(
        ::operator new[](n * sizeof(T), std::align_val_t(Alignment))); // NOLINT
    }

    /// @brief Deallocates the previously allocated memory.
    /// @details Uses `::operator delete[](p, std::align_val_t{Alignment})` to free the memory that
    /// was allocated with the specified Alignment.
    /// @param `p` The pointer to the memory to deallocate.
    /// @param `size` The number of elements (ignored in this implementation).
    inline void
    deallocate(pointer p, size_type) noexcept
    {
      ::operator delete[](p, std::align_val_t{Alignment}); // NOLINT
    }
  };
}
