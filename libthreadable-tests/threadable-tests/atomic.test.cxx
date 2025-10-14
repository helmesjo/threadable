#include <threadable-tests/doctest_include.hxx>
#include <threadable/atomic.hxx>

#include <bitset>
#include <cstdint>
#include <thread>

namespace
{
  using u8_t       = std::uint8_t;
  using bitfield_t = fho::atomic_bitfield<u8_t>;
}

namespace doctest
{
  template<>
  struct StringMaker<u8_t>
  {
    static auto
    convert(u8_t val) -> String
    {
      thread_local auto tmp = std::string{};
      tmp                   = std::bitset<sizeof(val) * 8>{val}.to_string();
      return tmp.c_str();
    }
  };

  template<>
  struct StringMaker<bitfield_t>
  {
    static auto
    convert(bitfield_t const& v) -> String
    {
      thread_local auto tmp = std::string{};
      auto              val = v.load();
      tmp                   = std::bitset<sizeof(val) * 8>{val}.to_string();
      return tmp.c_str();
    }
  };
}

SCENARIO("atomic_bitfield")
{
  GIVEN("a bitfield set to 0")
  {
    auto field = bitfield_t{0b00000000};
    INFO("field=" << field);
    THEN("test<0x00> is false")
    {
      REQUIRE_FALSE(field.test<0b00000000>());
    }
    WHEN("test_and_set bit 6 to true")
    {
      THEN("new value is assigned and old is returned")
      {
        REQUIRE_FALSE(field.test_and_set<0b00001000, true>());
        REQUIRE(field.test<0b00001000>());
        REQUIRE(field.load() == 0b00001000);

        field.wait<0b00001000>(false);
      }
    }
  }
  GIVEN("a bitfield set to all 1")
  {
    auto field = bitfield_t{0b11111111};
    INFO("field=" << field);
    THEN("test<0x00> is false")
    {
      REQUIRE_FALSE(field.test<0b00000000>());
    }
    WHEN("test_and_set bit 3 to false")
    {
      THEN("new value is assigned and old is returned")
      {
        REQUIRE(field.test_and_set<0b00000010, false>());
        REQUIRE(field.load() == 0b11111101);
        field.wait<0b00000010>(true);
      }
    }
  }
  GIVEN("CAS fails")
  {
    auto field = bitfield_t{0b00010000};
    INFO("field=" << field);
    THEN("'expected' is updated")
    {
      auto expected = u8_t{0b11111111};
      REQUIRE_FALSE(field.compare_exchange_weak<0b11111111>(expected, 0b00000010));
      REQUIRE(expected == 0b00010000);
      expected = 0b11111111;
      REQUIRE_FALSE(field.compare_exchange_strong<0b11111111>(expected, 0b00000010));
      REQUIRE(expected == 0b00010000);
    }
  }
}

SCENARIO("atomic_bitfield: CAS")
{
  GIVEN("a bitfield set to 0")
  {
    bitfield_t field{0b00000000};
    INFO("field=" << field);
    WHEN("CAS bit 0 from 0 to 1 with MaskExp=0x1, MaskDes=0x1")
    {
      auto expected = u8_t{0b00000000};
      REQUIRE(field.compare_exchange_strong<0b00000001, 0b00000001>(expected, 0b00000001));
      THEN("bit 0 is set, others unchanged")
      {
        REQUIRE(field.test<0b00000001>());
        REQUIRE(field.load() == 0b00000001);
      }
    }
    WHEN("CAS bit 0 from 1 to 0 with MaskExp=0x1, MaskDes=0x1 (expected mismatch)")
    {
      auto expected = u8_t{0b00000001};
      REQUIRE_FALSE(field.compare_exchange_strong<0b00000001, 0b00000001>(expected, 0b00000000));
      THEN("bit 0 is unchanged, expected updated")
      {
        REQUIRE_FALSE(field.test<0b00000001>());
        REQUIRE(expected == 0b00000000);
      }
    }
    WHEN("weak CAS bit 1 from 0 to 1 with MaskExp=0x2, MaskDes=0x2")
    {
      auto expected = u8_t{0b00000000};
      REQUIRE(field.compare_exchange_weak<0b00000010, 0b00000010>(expected, 0b00000010));
      THEN("bit 1 is set if successful")
      {
        REQUIRE(field.test<0b00000010>());
        REQUIRE(field.load() == 0b00000010);
      }
    }
    WHEN("CAS bits 0-1 from 0x0 to 0x3 with MaskExp=0x3, MaskDes=0x3")
    {
      auto expected = u8_t{0b00000000};
      REQUIRE(field.compare_exchange_strong<0b00000100, 0b00000100>(expected, 0b00000100));
      THEN("bits 0-1 are set, others unchanged")
      {
        REQUIRE(field.test<0b00000100>());
        REQUIRE(field.load() == 0b00000100);
      }
    }
  }

  GIVEN("a bitfield set to 0xFF")
  {
    bitfield_t field{0b11111111};
    INFO("field=" << field);
    WHEN("CAS bits 0-1 from 0x3 to 0x0 with MaskExp=0x3, MaskDes=0x3")
    {
      auto expected = u8_t{0b00000100};
      REQUIRE(field.compare_exchange_strong<0b00000100, 0b00000100>(expected, 0b00000000));
      THEN("bits 0-1 are cleared, others preserved")
      {
        REQUIRE_FALSE(field.test<0b00000100>());
        REQUIRE(field.load() == (0b11111111 & ~0b00000100)); // 0xFF & ~0x3 = 0xFC
      }
    }
    WHEN("CAS all bits to 0x0 with MaskExp=0x3, MaskDes=0xFF")
    {
      auto expected = u8_t{0b00000100};
      REQUIRE(field.compare_exchange_strong<0b00000100, 0b11111111>(expected, 0b00000000));
      THEN("all bits are cleared")
      {
        REQUIRE(field.load() == 0b00000000);
      }
    }
    WHEN("CAS fails due to MaskExp mismatch")
    {
      auto expected = u8_t{0b00000000};
      REQUIRE_FALSE(field.compare_exchange_strong<0b00000100, 0b11111111>(expected, 0b00000000));
      THEN("state unchanged, expected updated")
      {
        REQUIRE(field.load() == 0b11111111);
        REQUIRE(expected == 0b11111111);
      }
    }
  }
}

SCENARIO("atomic_bitfield: stress-test multiple bits with CAS")
{
  // NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays,cppcoreguidelines-pro-bounds-constant-array-index)
  GIVEN("5 threads manipulating distinct bits")
  {
    auto field = bitfield_t{0b00000000}; // Initialize with all bits unset
    INFO("field=" << field);

    constexpr auto iterations = std::size_t{100000};
    auto           threads    = std::vector<std::thread>{};

    threads.emplace_back(
      [&field]
      {
        constexpr auto mask = u8_t{0b00000001};
        for (std::size_t j = 0; j < iterations; ++j)
        {
          auto expected = field.load();
          auto desired  = u8_t{0b00000000};

          expected = field.load();
          desired  = expected | mask; // Set the thread's bit
          while (!field.compare_exchange_strong<mask>(expected, desired))
            ;
          desired = expected & ~mask; // Clear the thread's bit
          while (!field.compare_exchange_strong<mask>(expected, desired))
            ;
        }
      });

    threads.emplace_back(
      [&field]
      {
        constexpr auto mask = u8_t{0b00000010};
        for (std::size_t j = 0; j < iterations; ++j)
        {
          auto expected = field.load();
          expected      = field.load();
          while (!field.compare_exchange_strong<mask>(expected, 1))              // set bit to 1
            ;
          while (!field.compare_exchange_strong<mask>(expected,
                                                      static_cast<u8_t>(~mask))) // set bit to 0
            ;
        }
      });

    threads.emplace_back(
      [&field]
      {
        constexpr auto mask = u8_t{0b00000100};
        for (std::size_t j = 0; j < iterations; ++j)
        {
          auto expected = field.load();
          expected      = field.load();
          while (!field.compare_exchange_strong<mask>(expected, 1))              // set bit to 1
            ;
          while (!field.compare_exchange_strong<mask>(expected,
                                                      static_cast<u8_t>(~mask))) // set bit to 0
            ;
        }
      });

    for (auto& t : threads)
    {
      t.join();
    }

    THEN("all operations succeeded and final bitfield state is correct")
    {
      // Verify final bitfield state is 0 (all bits unset, as each thread sets and clears)
      REQUIRE(field.load() == 0);
    }
  }
  // NOLINTEND(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays,cppcoreguidelines-pro-bounds-constant-array-index)
}
