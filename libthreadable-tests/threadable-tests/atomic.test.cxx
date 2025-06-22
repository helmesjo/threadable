#include <threadable-tests/doctest_include.hxx>
#include <threadable/atomic.hxx>

SCENARIO("atomic_bitfield")
{
  using bitfield_t = fho::atomic_bitfield<std::uint8_t>;
  GIVEN("a bitfield set to 0")
  {
    auto field = bitfield_t{0};
    WHEN("test_and_set bit 6 to true")
    {
      THEN("new value is assigned and old is returned")
      {
        REQUIRE_FALSE(field.test_and_set<(1 << 5), true>());
        REQUIRE(field.test<(1 << 5)>());
        REQUIRE(field.load() == 0b00100000);
        field.wait<(1 << 5), false>();
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
        REQUIRE(field.test_and_set<(1 << 2), false>());
        REQUIRE(field.load() == 0b11111011);
        field.wait<(1 << 2), true>();
      }
    }
  }
}
