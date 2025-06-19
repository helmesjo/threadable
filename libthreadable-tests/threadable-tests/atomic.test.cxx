#include <threadable-tests/doctest_include.hxx>
#include <threadable/atomic.hxx>

#include <bitset>

SCENARIO("atomic_bitfield")
{
  using bitfield_t = fho::details::atomic_bitfield_t<std::size_t>;
  GIVEN("a bitfield set to 0")
  {
    auto field = bitfield_t{0};
    WHEN("test_and_set bit 6 to true")
    {
      THEN("new value is assigned and old is returned")
      {
        REQUIRE_FALSE(fho::details::test_and_set<1 << 5, true>(field));
        REQUIRE(fho::details::test<1 << 5>(field));
        REQUIRE(field.load() == std::bitset<8>("00100000").to_ulong());
        fho::details::wait<1 << 5, false>(field);
      }
    }
  }
  GIVEN("a bitfield set to all 1")
  {
    auto field = bitfield_t{static_cast<std::uint8_t>(-1)};
    WHEN("test_and_set bit 3 to false")
    {
      THEN("new value is assigned and old is returned")
      {
        REQUIRE(fho::details::test_and_set<1 << 2, false>(field));
        REQUIRE(field.load() == std::bitset<8>("11111011").to_ulong());
        fho::details::wait<1 << 2, true>(field);
      }
    }
  }
}
