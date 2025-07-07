#pragma once

#include <memory>
#include <new>

namespace fho
{
  /// @brief A custom allocator that ensures memory allocations are aligned to a specified
  /// alignment.
  /// @details This allocator inherits from `std::allocator<T>` and overrides the `allocate` and
  /// `deallocate` methods to use aligned memory allocation. The alignment must be a power of 2 and
  /// at least as large as the alignment of `T`, as enforced by a static assertion. This is useful
  /// for performance-critical applications where proper memory alignment can improve efficiency,
  /// such as with SIMD types or cache-line aligned structures.
  /// @tparam T The type of objects being allocated.
  /// @tparam Alignment The desired alignment for memory allocations, must be a power of 2 and >=
  /// alignof(T).
  /// @example
  /// ```cpp
  /// #include <vector>
  /// using aligned_vector_t = std::vector<int, fho::aligned_allocator<int, 64>>;
  /// aligned_vector_t vec(100); // Allocates a vector of 100 ints with 64-byte alignment
  /// ```
  template<typename T, std::size_t Alignment>
  struct aligned_allocator : public std::allocator<T>
  {
    static_assert(Alignment >= alignof(T) && (Alignment & (Alignment - 1)) == 0,
                  "Alignment must be a power of 2 and at least alignof(T)");

    constexpr aligned_allocator() = default;

    using base_t = std::allocator<T>;
    using base_t::base_t;

    using trait_types = std::allocator_traits<base_t>;

    using size_type       = typename trait_types::size_type;
    using difference_type = typename trait_types::difference_type;

    using propagate_on_container_move_assignment = // NOLINT
      typename trait_types::propagate_on_container_move_assignment;

    /// @brief Rebind the allocator to a different type.
    /// @details This is required for the allocator to be used with standard library containers. It
    /// defines how to get an allocator for a different type `U` with the same alignment.
    template<typename U>
    struct rebind
    {
      using other = aligned_allocator<U, Alignment>;
    };

    /// @brief Allocates memory with the specified alignment.
    /// @details Uses `::operator new[](size, std::align_val_t(Alignment))` to allocate memory that
    /// is aligned to `Alignment` bytes.
    /// @param n The number of elements to allocate.
    /// @return A pointer to the allocated memory.
    [[nodiscard]] inline constexpr auto
    allocate(size_type n) -> T*
    {
      return static_cast<T*>(
        ::operator new[](n * sizeof(T), std::align_val_t(Alignment))); // NOLINT
    }

    /// @brief Deallocates the previously allocated memory.
    /// @details Uses `::operator delete[](p, std::align_val_t{Alignment})` to free the memory that
    /// was allocated with the specified alignment.
    /// @param p The pointer to the memory to deallocate.
    /// @param size The number of elements (ignored in this implementation).
    inline constexpr void
    deallocate(T* p, size_type) noexcept
    {
      ::operator delete[](p, std::align_val_t{Alignment}); // NOLINT
    }
  };
}
