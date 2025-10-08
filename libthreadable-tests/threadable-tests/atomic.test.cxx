#include <threadable-tests/doctest_include.hxx>
#include <threadable/atomic.hxx>

#include <thread>

SCENARIO("atomic_bitfield")
{
  using bitfield_t = fho::atomic_bitfield<std::uint8_t>;
  GIVEN("a bitfield set to 0")
  {
    auto field = bitfield_t{0};
    THEN("test<0x00> is false")
    {
      REQUIRE_FALSE(field.test<0x00>());
    }
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
    THEN("test<0x00> is false")
    {
      REQUIRE_FALSE(field.test<0x00>());
    }
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
  GIVEN("CAS fails")
  {
    auto field = bitfield_t{0};
    THEN("'expected' is updated")
    {
      auto expected = static_cast<std::uint8_t>(-1);
      REQUIRE_FALSE(field.compare_exchange_weak(expected, 0xbb));
      REQUIRE(expected == 0);
      expected = static_cast<std::uint8_t>(-1);
      REQUIRE_FALSE(field.compare_exchange_strong(expected, 0xbb));
      REQUIRE(expected == 0);
    }
  }
}

SCENARIO("atomic_bitfield: CAS")
{
  using bitfield_t = fho::atomic_bitfield<std::uint8_t>;

  GIVEN("a bitfield set to 0")
  {
    bitfield_t field{0};
    WHEN("CAS bit 0 from 0 to 1 with MaskExp=0x1, MaskDes=0x1")
    {
      auto expected = std::uint8_t{0};
      REQUIRE(field.compare_exchange_strong<0x1, 0x1>(expected, 1));
      THEN("bit 0 is set, others unchanged")
      {
        REQUIRE(field.test<0x1>());
        REQUIRE(field.load() == 0x1);
      }
    }
    WHEN("CAS bit 0 from 1 to 0 with MaskExp=0x1, MaskDes=0x1 (expected mismatch)")
    {
      auto expected = std::uint8_t{1};
      REQUIRE_FALSE(field.compare_exchange_strong<0x1, 0x1>(expected, 0));
      THEN("bit 0 is unchanged, expected updated")
      {
        REQUIRE_FALSE(field.test<0x1>());
        REQUIRE(expected == 0);
      }
    }
    WHEN("weak CAS bit 1 from 0 to 1 with MaskExp=0x2, MaskDes=0x2")
    {
      auto expected = std::uint8_t{0};
      bool result   = field.compare_exchange_weak<0x2, 0x2>(expected, 2);
      THEN("bit 1 is set if successful")
      {
        if (result)
        {
          REQUIRE(field.test<0x2>());
          REQUIRE(field.load() == 2);
        }
        else
        {
          REQUIRE_FALSE(field.test<0x2>());
          REQUIRE(expected == field.load());
        }
      }
    }
    WHEN("CAS bits 0-1 from 0x0 to 0x3 with MaskExp=0x3, MaskDes=0x3")
    {
      auto expected = std::uint8_t{0};
      REQUIRE(field.compare_exchange_strong<0x3, 0x3>(expected, 0x3));
      THEN("bits 0-1 are set, others unchanged")
      {
        REQUIRE(field.test<0x3>());
        REQUIRE(field.load() == 0x3);
      }
    }
  }

  GIVEN("a bitfield set to 0xFF")
  {
    bitfield_t field{0xFF};
    WHEN("CAS bits 0-1 from 0x3 to 0x0 with MaskExp=0x3, MaskDes=0x3")
    {
      auto expected = std::uint8_t{0x3};
      REQUIRE(field.compare_exchange_strong<0x3, 0x3>(expected, 0));
      THEN("bits 0-1 are cleared, others preserved")
      {
        REQUIRE_FALSE(field.test<0x3>());
        REQUIRE(field.load() == 0xFC); // 0xFF & ~0x3 = 0xFC
      }
    }
    WHEN("CAS all bits to 0x0 with MaskExp=0x3, MaskDes=0xFF")
    {
      auto expected = std::uint8_t{0x3};
      REQUIRE(field.compare_exchange_strong<0x3, 0xFF>(expected, 0));
      THEN("all bits are cleared")
      {
        REQUIRE(field.load() == 0);
      }
    }
    WHEN("CAS fails due to MaskExp mismatch")
    {
      auto expected = std::uint8_t{0};
      REQUIRE_FALSE(field.compare_exchange_strong<0x3, 0xFF>(expected, 0));
      THEN("state unchanged, expected updated")
      {
        REQUIRE(field.load() == 0xFF);
        REQUIRE(expected == 0xFF);
      }
    }
  }
}

SCENARIO("atomic_bitfield: stress-test multiple bits with CAS")
{
  // NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays,cppcoreguidelines-pro-bounds-constant-array-index)
  GIVEN("5 threads manipulating distinct bits")
  {
    using bitfield_t                              = fho::atomic_bitfield<std::uint8_t>;
    auto                   bitfield               = bitfield_t{0}; // Initialize with all bits unset
    constexpr auto         num_threads            = std::size_t{5};
    constexpr auto         iterations             = std::size_t{100000};
    constexpr std::uint8_t bit_masks[num_threads] = {
      0b00000001, 0b00000010, 0b00000100, 0b00001000, 0b00010000,
    }; // Distinct bits for each thread

    auto threads         = std::vector<std::thread>{};
    auto successCounters = std::array<std::atomic<std::size_t>, num_threads>{};

    for (std::size_t i = 0; i < num_threads; ++i)
    {
      threads.emplace_back(
        [&, mask = bit_masks[i], threadId = i]
        {
          for (std::size_t j = 0; j < iterations; ++j)
          {
            auto expected = std::uint8_t{bitfield.load(std::memory_order_acquire)};
            auto desired  = std::uint8_t{0};
            auto success  = false;

            // Try compare_exchange_strong to set the thread's bit
            do
            {
              expected = bitfield.load(std::memory_order_acquire);
              desired  = expected | mask; // Set the thread's bit
              success =
                bitfield.compare_exchange_strong(expected, desired, std::memory_order_acq_rel,
                                                 std::memory_order_acquire);
            }
            while (!success);

            successCounters[threadId].fetch_add(1, std::memory_order_relaxed);

            // Try compare_exchange_weak to clear the thread's bit
            do
            {
              expected = bitfield.load(std::memory_order_acquire);
              desired  = expected & ~mask; // Clear the thread's bit
              success = bitfield.compare_exchange_weak(expected, desired, std::memory_order_acq_rel,
                                                       std::memory_order_acquire);
              // No yield here; weak CAS may fail spuriously, so we retry
            }
            while (!success);
          }
        });
    }

    for (auto& t : threads)
    {
      t.join();
    }

    THEN("all operations succeeded and final bitfield state is correct")
    {
      // Verify each thread completed the expected number of successful operations
      for (std::size_t i = 0; i < num_threads; ++i)
      {
        REQUIRE(successCounters[i].load(std::memory_order_relaxed) == iterations);
      }

      // Verify final bitfield state is 0 (all bits unset, as each thread sets and clears)
      REQUIRE(bitfield.load(std::memory_order_seq_cst) == 0);
    }
  }
  // NOLINTEND(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays,cppcoreguidelines-pro-bounds-constant-array-index)
}
