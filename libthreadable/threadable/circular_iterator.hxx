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

    inline static constexpr auto
    mask(size_t index) noexcept
    {
      return index & index_mask;
    }

    circular_iterator() = default;

    explicit circular_iterator(pointer jobs, size_t index) noexcept
      : jobs_(jobs)
      , index_(index)
    {}

    inline auto
    operator*() noexcept -> reference
    {
      return jobs_[mask(index_)];
    }

    inline auto
    operator->() const noexcept -> pointer
    {
      return &jobs_[mask(index_)];
    }

    inline auto
    operator[](difference_type rhs) const noexcept -> reference
    {
      return jobs_[mask(index_ + rhs)];
    }

    friend inline auto
    operator*(circular_iterator const& it) -> reference
    {
      return it.jobs_[mask(it.index_)];
    }

    inline auto
    operator<=>(circular_iterator const& rhs) const noexcept
    {
      if (jobs_ != rhs.jobs_)
      {
        return jobs_ <=> rhs.jobs_;
      }
      return index_ <=> rhs.index_;
    }

    inline auto
    operator==(circular_iterator const& other) const noexcept -> bool
    {
      return jobs_ == other.jobs_ && mask(index_ - other.index_) == 0;
    }

    // Returns logical distance, not circular addition
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
      return circular_iterator(jobs_, mask(index_ + rhs));
    }

    inline auto
    operator-(difference_type rhs) const noexcept -> circular_iterator
    {
      return circular_iterator(jobs_, mask(index_ - rhs));
    }

    friend inline auto
    operator+(difference_type lhs, circular_iterator const& rhs) noexcept -> circular_iterator
    {
      return circular_iterator(rhs.jobs_, mask(lhs + rhs.index_));
    }

    friend inline auto
    operator-(difference_type lhs, circular_iterator const& rhs) noexcept -> circular_iterator
    {
      return circular_iterator(rhs.jobs_, mask(lhs - rhs.index_));
    }

    inline auto
    operator+=(difference_type rhs) noexcept -> circular_iterator&
    {
      index_ += rhs;
      return *this;
    }

    inline auto
    operator-=(difference_type rhs) noexcept -> circular_iterator&
    {
      index_ -= rhs;
      return *this;
    }

    inline auto
    operator++() noexcept -> circular_iterator&
    {
      ++index_;
      return *this;
    }

    inline auto
    operator--() noexcept -> circular_iterator&
    {
      --index_;
      return *this;
    }

    inline auto
    operator++(int) noexcept -> circular_iterator
    {
      return circular_iterator(jobs_, index_++);
    }

    inline auto
    operator--(int) noexcept -> circular_iterator
    {
      return circular_iterator(jobs_, index_--);
    }

    inline auto
    index() const
    {
      return index_;
    }

  private:
    pointer jobs_  = nullptr;
    size_t  index_ = 0;
  };
}
