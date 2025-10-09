#pragma once

#include <bit>
#include <cstddef>
#include <iterator>
#include <ranges>

namespace fho
{
  /// @brief A random access iterator for a ring buffer.
  /// @details This iterator provides random access capabilities for a ring buffer, and efficient
  /// traversal and manipulation of elements in a circular manner. It uses a mask to wrap indices
  /// around the buffer size, optimized for capacities that are powers of two.
  /// @tparam `T` The type of elements stored in the ring buffer.
  /// @tparam `Mask` A mask equal to `Capacity - 1`, where `Capacity` is a power of two, used to
  /// wrap indices efficiently via bitwise operations.
  /// @note Random-access only; subranges may wrap (non-contiguous memory).
  template<typename T, size_t Mask>
  class ring_iterator
  {
  public:
    using iterator_category = std::random_access_iterator_tag;
    using iterator_concept  = std::random_access_iterator_tag;
    using difference_type   = std::ptrdiff_t;
    using value_type        = T;
    using pointer           = value_type*;
    using reference         = value_type&;
    using element_type      = value_type;

    static constexpr size_t buffer_size   = Mask + 1;
    static constexpr auto   capacity_bits = std::countr_zero(buffer_size);

    /// @brief Masks an index to wrap it around the buffer size.
    /// @details Applies a bitwise AND operation with `Mask` to compute the effective position
    /// within the buffer, equivalent to `index % capacity` for power-of-two capacities.
    /// @param `index` The index to mask.
    /// @return The masked index, ensuring it stays within the buffer bounds.
    static constexpr auto
    mask(size_t index) noexcept
    {
      return index & Mask;
    }

    static constexpr auto
    epoch_of(size_t i) noexcept -> bool
    {
      return (i >> capacity_bits) & 1;
    }

    ring_iterator() = default;

    /// @brief Constructs an iterator with a buffer pointer and starting index.
    /// @details Initializes the iterator with a pointer to the ring buffer's start and a logical
    /// index. The current position is set to the masked index offset from the buffer start.
    /// @param `data` Pointer to the beginning of the ring buffer.
    /// @param `index` The starting logical index in the buffer.
    explicit ring_iterator(pointer data, size_t index) noexcept
      : data_(data)
      , current_(data + mask(index))
      , index_(index)
    {}

    /// @brief Provides random access to elements via subscripting.
    /// @details Returns a reference to the element at the specified offset from the current index,
    /// wrapping around the buffer using the mask.
    /// @param `rhs` The offset (positive or negative) from the current position.
    /// @return Reference to the element at the computed position.
    inline auto
    operator[](difference_type i) const noexcept -> reference
    {
      return data_[mask(index_ + i)];
    }

    /// @brief Dereferences the iterator to access the current element.
    /// @details Returns a reference to the element at the iterator's current position.
    /// @return Reference to the current element.
    inline auto
    operator*() noexcept -> reference
    {
      return *current_;
    }

    /// @brief Friend function for dereferencing.
    /// @details Enables dereferencing the iterator from outside the class, mirroring the behavior
    /// of `operator*`.
    /// @param `it` The iterator to dereference.
    /// @return Reference to the current element.
    friend inline auto
    operator*(ring_iterator const& it) -> reference
    {
      return *it.current_;
    }

    /// @brief Provides pointer-like access to the current element.
    /// @details Returns a pointer to the current element for member access (e.g., `it->member`).
    /// @return Pointer to the current element.
    inline auto
    operator->() const noexcept -> pointer
    {
      return current_;
    }

    /// @brief Compares two iterators using three-way comparison.
    /// @details Uses the spaceship operator to compare iterators based on their logical indices,
    /// enabling ordering.
    /// @param `rhs` The iterator to compare with.
    /// @return A `std::strong_ordering` result indicating relative position.
    inline auto
    operator<=>(ring_iterator const& rhs) const noexcept
    {
      return index_ <=> rhs.index_;
    }

    /// @brief Checks equality between two iterators.
    /// @details Compares logical (unbounded) indices. Handles wraparound.
    /// @param `rhs` The iterator to compare with.
    /// @return `true` if logical indices are equal, `false` otherwise.
    inline auto
    operator==(ring_iterator const& rhs) const noexcept -> bool
    {
      return index_ == rhs.index_;
    }

    /// @brief Computes the distance between two iterators.
    /// @details Subtracts the index of one iterator from another, yielding the number of positions
    /// between them, as required for random access iterators.
    /// @param `rhs` The iterator to subtract.
    /// @return The difference in logical indices.
    inline auto
    operator-(ring_iterator const& rhs) const noexcept -> difference_type
    {
      return index_ - rhs.index_;
    }

    /// @brief Advances the iterator by an offset.
    /// @details Creates a new iterator positioned `rhs` elements after the current one, with
    /// wrapping handled by the constructor.
    /// @param `rhs` The number of positions to advance.
    /// @return A new iterator at the offset position.
    inline auto
    operator+(difference_type rhs) const noexcept -> ring_iterator
    {
      return ring_iterator(data_, index_ + rhs);
    }

    /// @brief Retreats the iterator by an offset.
    /// @details Creates a new iterator positioned `rhs` elements before the current one, with
    /// wrapping handled by the constructor.
    /// @param `rhs` The number of positions to retreat.
    /// @return A new iterator at the offset position.
    inline auto
    operator-(difference_type rhs) const noexcept -> ring_iterator
    {
      return ring_iterator(data_, index_ - rhs);
    }

    /// @brief Allows adding an offset to an iterator from the left.
    /// @details Enables expressions like `5 + it`, creating a new iterator advanced by `lhs`.
    /// @param `lhs` The offset to add.
    /// @param `rhs` The original iterator.
    /// @return A new iterator at the offset position.
    friend inline auto
    operator+(difference_type lhs, ring_iterator const& rhs) noexcept -> ring_iterator
    {
      return ring_iterator(rhs.data_, lhs + rhs.index_);
    }

    /// @brief Allows subtracting an offset from an iterator from the left.
    /// @details Enables expressions like `5 - it`, creating a new iterator retreated by `lhs`.
    /// @param `lhs` The offset to subtract.
    /// @param `rhs` The original iterator.
    /// @return A new iterator at the offset position.
    friend inline auto
    operator-(difference_type lhs, ring_iterator const& rhs) noexcept -> ring_iterator
    {
      return ring_iterator(rhs.data_, lhs - rhs.index_);
    }

    /// @brief Advances the iterator in place by an offset.
    /// @details Updates the iterator's index and current position, wrapping around the buffer as
    /// needed.
    /// @param `rhs` The number of positions to advance.
    /// @return Reference to this modified iterator.
    inline auto
    operator+=(difference_type rhs) noexcept -> ring_iterator&
    {
      index_ += rhs;
      current_ = data_ + mask(index_);
      return *this;
    }

    /// @brief Retreats the iterator in place by an offset.
    /// @details Updates the iterator's index and current position by moving backward, wrapping as
    /// needed.
    /// @param `rhs` The number of positions to retreat.
    /// @return Reference to this modified iterator.
    inline auto
    operator-=(difference_type rhs) noexcept -> ring_iterator&
    {
      return *this += -rhs;
    }

    /// @brief Advances the iterator to the next element (pre-increment).
    /// @details Increments the index and updates the current pointer, wrapping to the buffer's
    /// start if the end is exceeded.
    /// @return Reference to this modified iterator.
    inline auto
    operator++() noexcept -> ring_iterator&
    {
      current_ = data_ + mask(++index_);
      return *this;
    }

    /// @brief Advances the iterator to the next element (post-increment).
    /// @details Increments the iterator and returns its previous state.
    /// @return A copy of the iterator before incrementing.
    inline auto
    operator++(int) noexcept -> ring_iterator
    {
      auto prev = *this;
      ++(*this);
      return prev;
    }

    /// @brief Moves the iterator to the previous element (pre-decrement).
    /// @details Decrements the index and updates the current pointer, wrapping to the buffer's end
    /// if below the start.
    /// @return Reference to this modified iterator.
    inline auto
    operator--() noexcept -> ring_iterator&
    {
      current_ = data_ + mask(--index_);
      return *this;
    }

    /// @brief Moves the iterator to the previous element (post-decrement).
    /// @details Decrements the iterator and returns its previous state.
    /// @return A copy of the iterator before decrementing.
    inline auto
    operator--(int) noexcept -> ring_iterator
    {
      auto prev = *this;
      --(*this);
      return prev;
    }

    /// @brief Retrieves the current logical index.
    /// @details Returns the iterator's index, which may exceed the buffer size due to wrapping.
    /// @return The current logical index.
    [[nodiscard]] inline auto
    index() const noexcept
    {
      return index_;
    }

    /// @brief Provides access to the buffer's starting pointer (non-const).
    /// @details Returns a pointer to the beginning of the ring buffer for direct manipulation.
    /// @return Pointer to the buffer's start.
    [[nodiscard]] inline auto
    data() noexcept
    {
      return data_;
    }

    /// @brief Provides access to the buffer's starting pointer (const).
    /// @details Returns a const pointer to the beginning of the ring buffer.
    /// @return Const pointer to the buffer's start.
    [[nodiscard]] inline auto
    data() const noexcept
    {
      return data_;
    }

  private:
    pointer data_    = nullptr; ///< Pointer to the start of the ring buffer.
    pointer current_ = nullptr; ///< Pointer to the current element.
    size_t  index_   = 0;       ///< Logical index, may exceed buffer size.
  };

  // Iterator & Ranges compatibility with standard algorithms
  //
  // Verifies basic iterator validity for input/output operations.
  static_assert(std::input_or_output_iterator<ring_iterator<int, 1024>>);
  // Increment without equality preservation, base for traversal.
  static_assert(std::weakly_incrementable<ring_iterator<int, 1024>>);
  // Increment preserves equality, enabling reliable looping.
  static_assert(std::incrementable<ring_iterator<int, 1024>>);
  // Read access and single-pass traversal, core for input ranges.
  static_assert(std::input_iterator<ring_iterator<int, 1024>>);
  // Multi-pass traversal without modifying the iterated sequence.
  static_assert(std::forward_iterator<ring_iterator<int, 1024>>);
  // Forward/backward traversal, useful for algorithms like reverse.
  static_assert(std::bidirectional_iterator<ring_iterator<int, 1024>>);
  // Constant-time jumps and arithmetic, optimizing stdlib algos.
  static_assert(std::random_access_iterator<ring_iterator<int, 1024>>);
  // Efficient size computation via operator-, aiding range-based ops.
  static_assert(std::sized_sentinel_for<ring_iterator<int, 1024>, ring_iterator<int, 1024>>);

  /// @brief A type alias for a transformed view over a ring buffer's subrange.
  /// @details The `ring_transform_view` alias defines a `std::ranges::transform_view` that applies
  /// a transformation (via `Accessor`) to a subrange of elements in a ring buffer, specified by
  /// `Iterator`. This enables convenient access to the values stored in the buffer's slots without
  /// directly exposing the `ring_slot` objects. It is typically used with `ring_buffer` to provide
  /// a view of the slot values (e.g., `T`) rather than the slots themselves.
  /// @tparam `Iterator` The iterator type, typically `ring_iterator<T, Mask>` or
  ///                   `ring_iterator<const T, Mask>`, defining the range of slots.
  /// @tparam `Accessor` A callable type (e.g., lambda or function pointer) that transforms a slot
  ///                    into a reference to its stored value (typically `T&` or `const T&`).
  /// @example
  /// ```cpp
  /// auto buffer = fho::ring_buffer<>{};
  /// buffer.emplace_back([]() { std::cout << "Task\n"; });
  /// auto range = buffer.pop range();
  /// for (auto& func : range) { // range is a ring_transform_view
  ///     func();
  /// }
  /// ```
  template<typename Iterator, typename Accessor>
  using ring_transform_view = // NOLINT
    std::ranges::transform_view<std::ranges::subrange<Iterator>, Accessor>;
}
