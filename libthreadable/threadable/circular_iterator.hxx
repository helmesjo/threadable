#pragma once

#include <cstddef>
#include <iterator>

namespace threadable
{
  template<typename elem_t, size_t index_mask>
  struct circular_iterator
  {
    using iterator_category = std::random_access_iterator_tag;
    using iterator_concept  = std::contiguous_iterator_tag;
    using difference_type   = std::ptrdiff_t;
    using value_type        = elem_t;
    using pointer           = value_type*;
    using reference         = value_type&;
    using element_type      = value_type;

    inline static constexpr size_t buffer_size = index_mask + 1;

    inline static constexpr auto
    mask(size_t index) noexcept
    {
      return index & index_mask;
    }

    circular_iterator() = default;

    explicit circular_iterator(pointer jobs, size_t index) noexcept
      : jobs_(jobs)
      , current_(jobs + mask(index))
      , index_(index)
    {}

    inline auto
    operator[](difference_type rhs) const noexcept -> reference
    {
      return jobs_[mask(index_ + rhs)];
    }

    inline auto
    operator*() noexcept -> reference
    {
      return *current_;
    }

    friend inline auto
    operator*(circular_iterator const& it) -> reference
    {
      return *it.current_;
    }

    inline auto
    operator->() const noexcept -> pointer
    {
      return current_;
    }

    inline auto
    operator<=>(circular_iterator const& rhs) const noexcept
    {
      return index_ <=> rhs.index_;
    }

    inline auto
    operator==(circular_iterator const& rhs) const noexcept -> bool
    {
      return current_ == rhs.current_;
    }

    inline auto
    operator+(circular_iterator const& rhs) const noexcept -> difference_type
    {
      return index_ + rhs.index_;
    }

    inline auto
    operator-(circular_iterator const& rhs) const noexcept -> difference_type
    {
      return index_ - rhs.index_;
    }

    inline auto
    operator+(difference_type rhs) const noexcept -> circular_iterator
    {
      return circular_iterator(jobs_, index_ + rhs);
    }

    inline auto
    operator-(difference_type rhs) const noexcept -> circular_iterator
    {
      return circular_iterator(jobs_, index_ - rhs);
    }

    friend inline auto
    operator+(difference_type lhs, circular_iterator const& rhs) noexcept -> circular_iterator
    {
      return circular_iterator(rhs.jobs_, lhs + rhs.index_);
    }

    friend inline auto
    operator-(difference_type lhs, circular_iterator const& rhs) noexcept -> circular_iterator
    {
      return circular_iterator(rhs.jobs_, lhs - rhs.index_);
    }

    inline auto
    operator+=(difference_type rhs) noexcept -> circular_iterator&
    {
      index_ += rhs;
      current_ = jobs_ + mask(index_);
      return *this;
    }

    inline auto
    operator-=(difference_type rhs) noexcept -> circular_iterator&
    {
      return *this += -rhs;
    }

    inline auto
    operator++() noexcept -> circular_iterator&
    {
      ++index_;
      if (++current_ > (jobs_ + buffer_size - 1)) [[unlikely]]
      {
        current_ = jobs_;
      }
      return *this;
    }

    inline auto
    operator++(int) noexcept -> circular_iterator
    {
      auto prev = *this;
      ++(*this);
      return prev;
    }

    inline auto
    operator--() noexcept -> circular_iterator&
    {
      --index_;
      if (--current_ < jobs_) [[unlikely]]
      {
        current_ = (jobs_ + buffer_size - 1);
      }
      return *this;
    }

    inline auto
    operator--(int) noexcept -> circular_iterator
    {
      auto prev = *this;
      --(*this);
      return prev;
    }

    inline auto
    index() const
    {
      return index_;
    }

  private:
    pointer jobs_    = nullptr;
    pointer current_ = nullptr;
    size_t  index_   = 0;
  };
}
