#include "threadable/atomic.hxx"

#include <threadable-tests/doctest_include.hxx>
#include <threadable/queue.hxx>

#include <bitset>

SCENARIO("atomic_bitfield")
{
  GIVEN("a bitfield set to 0")
  {
    threadable::details::atomic_bitfield field{0};
    WHEN("test_and_set bit 6 to true")
    {
      THEN("new value is assigned and old is returned")
      {
        REQUIRE_FALSE(threadable::details::test_and_set<5, true>(field));
        REQUIRE(std::bitset<8>(field).to_string() == std::bitset<8>(1 << 5).to_string());
        threadable::details::wait<5, false>(field);
      }
    }
  }
  GIVEN("a bitfield set to all 1")
  {
    threadable::details::atomic_bitfield field{static_cast<std::uint8_t>(-1)};
    WHEN("test_and_set bit 3 to false")
    {
      THEN("new value is assigned and old is returned")
      {
        REQUIRE(threadable::details::test_and_set<2, false>(field));
        REQUIRE(std::bitset<8>(field).to_string() ==
                std::bitset<8>(static_cast<unsigned int>(~(1 << 2))).to_string());
        threadable::details::wait<2, true>(field);
      }
    }
  }
}
