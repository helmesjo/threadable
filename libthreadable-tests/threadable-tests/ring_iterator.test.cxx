#include <threadable-tests/doctest_include.hxx>
#include <threadable/ring_iterator.hxx>

SCENARIO("ring_iterator")
{
  static constexpr auto buf_size   = std::size_t{16};
  static constexpr auto max_size   = buf_size - 1u;
  static constexpr auto index_mask = buf_size - 1u;
  using buffer_t                   = std::array<int, buf_size>;
  using iter_t                     = fho::ring_iterator<int, index_mask>;

  static_assert((buf_size & index_mask) == 0, "number of tasks must be a power of 2");
  static_assert(buf_size == 16); // for comments to be accurate

  auto v   = buffer_t{};
  auto buf = v.data();

  auto const begin = iter_t(buf, 0);
  auto const end   = iter_t(buf, buf_size);

  WHEN("iterator wraps around at buffer end")
  {
    auto it = iter_t(buf, 0); // index_ = 0
    it += buf_size;           // Wraps to 0

    THEN("it points to the same position as begin")
    {
      REQUIRE(it != begin);   // Logically different positions - index(16) != index(0)
      REQUIRE(*it == *begin); // But physically the same task  -  mask(16) == mask(0)
    }
  }

  WHEN("tail is before head across wraparound")
  {
    auto tailIt = iter_t(buf, 5);            // index_ = 5
    auto headIt = iter_t(buf, buf_size + 4); // index_ = 20

    THEN("tail is less than head")
    {
      REQUIRE(tailIt < headIt); // 15 < 20, despite mask(20) = 4
      REQUIRE(iter_t::mask(tailIt.index()) == 5);
      REQUIRE(iter_t::mask(headIt.index()) == 4);
    }
  }

  WHEN("head wraps to 0 and tail is near end")
  {
    auto tailIt = iter_t(buf, max_size); // index_ = 15
    auto headIt = iter_t(buf, buf_size); // index_ = 16
    THEN("tail is less than head despite raw indices")
    {
      REQUIRE(tailIt < headIt);          // 15 < 16, mask(15) = 15, mask(16) = 0
      REQUIRE(*tailIt == *end);          // Valid dereference
    }
  }

  WHEN("iterators span full buffer wraparound")
  {
    auto tailIt = iter_t(buf, 0);        // index_ = 0
    auto headIt = iter_t(buf, max_size); // index_ = 15

    THEN("range covers all slots exactly once")
    {
      int count = 0;
      for (auto it = tailIt; it != headIt; ++it)
      {
        ++count;
      }
      REQUIRE(count == max_size); // Full size - 1
      REQUIRE(tailIt < headIt);   // 0 < 15
    }
  }

  WHEN("iterator increments past multiple wraps")
  {
    auto it = iter_t(buf, 0); // index_ = 0
    it += buf_size * 2;       // index_ = 32 - Two full wraps (16 * 2)

    THEN("it wraps back to original position")
    {
      REQUIRE(it != begin);   // Logically different positions - index(32) != index(0)
      REQUIRE(*it == *begin); // But physically the same task  -  mask(32) == mask(0)
    }
  }
}
