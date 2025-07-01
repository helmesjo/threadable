#pragma once

#include <cstddef>
#include <iterator>

namespace fho
{
  /// @brief A random access iterator for a ring buffer.
  /// @details This iterator provides random access and contiguous iteration capabilities for a ring
  /// buffer, allowing efficient traversal and manipulation of elements in a circular fashion. It is
  /// designed to work with the `ring_buffer` class.
  /// @tparam `T` The type of elements stored in the ring buffer.
  /// @tparam `Mask` A (capacity) mask used to wrap indices around the buffer size.
  template<typename T, size_t Mask>
  struct ring_iterator
  {
    using iterator_category = std::random_access_iterator_tag;
    using iterator_concept  = std::contiguous_iterator_tag;
    using difference_type   = std::ptrdiff_t;
    using value_type        = T;
    using pointer           = value_type*;
    using reference         = value_type&;
    using element_type      = value_type;

    inline static constexpr size_t buffer_size = Mask + 1;

    /// @brief Masks an index to wrap around the buffer size.
    /// @param `index` The index to mask.
    /// @return The masked index.
    inline static constexpr auto
    mask(size_t index) noexcept
    {
      return index & Mask;
    }

    ring_iterator() = default;

    /// @brief Constructor with buffer pointer and index.
    /// @details Initializes the iterator with the given buffer pointer and starting index.
    /// @param `jobs` Pointer to the beginning of the ring buffer.
    /// @param `index` The starting index in the buffer.
    explicit ring_iterator(pointer jobs, size_t index) noexcept
      : jobs_(jobs)
      , current_(jobs + mask(index))
      , index_(index)
    {}

    /// @brief Subscript operator for random access.
    /// @details Returns a reference to the element at the specified offset from the current
    /// position.
    /// @param `rhs` The offset from the current position.
    /// @return Reference to the element at the offset position.
    inline auto
    operator[](difference_type rhs) const noexcept -> reference
    {
      return jobs_[mask(index_ + rhs)];
    }

    /// @brief Dereference operator.
    /// @details Returns a reference to the current element.
    /// @return Reference to the current element.
    inline auto
    operator*() noexcept -> reference
    {
      return *current_;
    }

    /// @brief Friend function for dereferencing.
    /// @details Allows dereferencing the iterator from outside the class.
    friend inline auto
    operator*(ring_iterator const& it) -> reference
    {
      return *it.current_;
    }

    /// @brief Member access operator.
    /// @details Returns a pointer to the current element.
    /// @return Pointer to the current element.
    inline auto
    operator->() const noexcept -> pointer
    {
      return current_;
    }

    /// @brief Three-way comparison operator.
    /// @details Compares this iterator with another based on their indices.
    /// @param `rhs` The iterator to compare with.
    /// @return The result of the comparison.
    inline auto
    operator<=>(ring_iterator const& rhs) const noexcept
    {
      return index_ <=> rhs.index_;
    }

    /// @brief Equality operator.
    /// @details Checks if this iterator points to the same element as another.
    /// @param `rhs` The iterator to compare with.
    /// @return True if both iterators point to the same element, false otherwise.
    inline auto
    operator==(ring_iterator const& rhs) const noexcept -> bool
    {
      return current_ == rhs.current_;
    }

    /// @brief Addition operator with another iterator.
    /// @details Adds the indices of two iterators.
    /// @param `rhs` The iterator to add.
    /// @return The sum of the indices.
    inline auto
    operator+(ring_iterator const& rhs) const noexcept -> difference_type
    {
      return index_ + rhs.index_;
    }

    /// @brief Subtraction operator with another iterator.
    /// @details Subtracts the index of one iterator from another.
    /// @param `rhs` The iterator to subtract.
    /// @return The difference of the indices.
    inline auto
    operator-(ring_iterator const& rhs) const noexcept -> difference_type
    {
      return index_ - rhs.index_;
    }

    /// @brief Addition with a difference type.
    /// @details Returns a new iterator offset by the given difference.
    /// @param `rhs` The offset.
    /// @return A new iterator at the offset position.
    inline auto
    operator+(difference_type rhs) const noexcept -> ring_iterator
    {
      return ring_iterator(jobs_, index_ + rhs);
    }

    /// @brief Subtraction with a difference type.
    /// @details Returns a new iterator offset by the negative of the given difference.
    /// @param `rhs` The offset.
    /// @return A new iterator at the offset position.
    inline auto
    operator-(difference_type rhs) const noexcept -> ring_iterator
    {
      return ring_iterator(jobs_, index_ - rhs);
    }

    /// @brief Friend function for adding a difference type to an iterator.
    /// @details Adds a difference to the iterator.
    /// @param `lhs` The difference to add.
    /// @param `rhs` The iterator.
    /// @return A new iterator at the sum position.
    friend inline auto
    operator+(difference_type lhs, ring_iterator const& rhs) noexcept -> ring_iterator
    {
      return ring_iterator(rhs.jobs_, lhs + rhs.index_);
    }

    /// @brief Friend function for subtracting a difference type from an iterator.
    /// @details Subtracts a difference from the iterator.
    /// @param `lhs` The difference to subtract.
    /// @param `rhs` The iterator.
    /// @return A new iterator at the difference position.
    friend inline auto
    operator-(difference_type lhs, ring_iterator const& rhs) noexcept -> ring_iterator
    {
      return ring_iterator(rhs.jobs_, lhs - rhs.index_);
    }

    /// @brief Addition assignment with a difference type.
    /// @details Offsets the iterator by the given difference.
    /// @param `rhs` The offset.
    /// @return Reference to this iterator.
    inline auto
    operator+=(difference_type rhs) noexcept -> ring_iterator&
    {
      index_ += rhs;
      current_ = jobs_ + mask(index_);
      return *this;
    }

    /// @brief Subtraction assignment with a difference type.
    /// @details Offsets the iterator by the negative of the given difference.
    /// @param `rhs` The offset.
    /// @return Reference to this iterator.
    inline auto
    operator-=(difference_type rhs) noexcept -> ring_iterator&
    {
      return *this += -rhs;
    }

    /// @brief Pre-increment operator.
    /// @details Advances the iterator to the next element, wrapping around if necessary.
    /// @return Reference to this iterator.
    inline auto
    operator++() noexcept -> ring_iterator&
    {
      ++index_;
      if (++current_ > (jobs_ + buffer_size - 1)) [[unlikely]]
      {
        current_ = jobs_;
      }
      return *this;
    }

    /// @brief Post-increment operator.
    /// @details Advances the iterator to the next element and returns the previous state.
    /// @return The iterator before incrementing.
    inline auto
    operator++(int) noexcept -> ring_iterator
    {
      auto prev = *this;
      ++(*this);
      return prev;
    }

    /// @brief Pre-decrement operator.
    /// @details Moves the iterator to the previous element, wrapping around if necessary.
    /// @return Reference to this iterator.
    inline auto
    operator--() noexcept -> ring_iterator&
    {
      --index_;
      if (--current_ < jobs_) [[unlikely]]
      {
        current_ = (jobs_ + buffer_size - 1);
      }
      return *this;
    }

    /// @brief Post-decrement operator.
    /// @details Moves the iterator to the previous element and returns the previous state.
    /// @return The iterator before decrementing.
    inline auto
    operator--(int) noexcept -> ring_iterator
    {
      auto prev = *this;
      --(*this);
      return prev;
    }

    /// @brief Returns the current index of the iterator.
    /// @return The current index.
    [[nodiscard]] inline auto
    index() const noexcept
    {
      return index_;
    }

    [[nodiscard]] inline auto
    data() noexcept
    {
      return jobs_;
    }

    [[nodiscard]] inline auto
    data() const noexcept
    {
      return jobs_;
    }

  private:
    pointer jobs_    = nullptr;
    pointer current_ = nullptr;
    size_t  index_   = 0;
  };

  // Make sure iterator is valid for parallelization with the standard algorithms
  static_assert(std::random_access_iterator<ring_iterator<int, 1024>>);
  static_assert(std::contiguous_iterator<ring_iterator<int, 1024>>);
}
