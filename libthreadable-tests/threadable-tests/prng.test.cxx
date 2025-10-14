#include <threadable-tests/doctest_include.hxx>
#include <threadable/prng.hxx>

#include <array>
#include <cstdint>
#include <limits>
#include <random>

namespace
{
  /// @brief Deterministic 32-bit URBG to exercise distribution paths without probability.
  /// @details Produces 0,1,2,...,255,0,1,... (wraps). Satisfies URBG requirements.
  struct word_counter_urbg
  {
    using result_type = std::uint32_t;

    result_type v = 0;

    static constexpr auto
    min() -> result_type
    {
      return std::numeric_limits<result_type>::min();
    }

    static constexpr auto
    max() -> result_type
    {
      return std::numeric_limits<result_type>::max();
    }

    auto
    operator()() -> result_type
    {
      return v++;
    }
  };
}

SCENARIO("prng: generate random numbers")
{
  // Basic compatibility check
  static_assert(std::constructible_from<std::mt19937, fho::prng_dist<int>&>);

  // Engine seeded once; used only for non-deterministic sanity checks below.
  auto rng = fho::prng_engine{std::random_device{}()};

  WHEN("bounds: low=0, high=256 (inclusive)")
  {
    auto dist = fho::prng_dist<std::size_t>(0u, 256u);
    THEN("values are within [0,256]")
    {
      for (auto i = 0u; i < 1024u; ++i)
      {
        auto r = dist(rng);
        REQUIRE(r >= 0u);
        REQUIRE(r <= 256u);
      }
    }
  }

  WHEN("bounds: low=-128, high=128 (inclusive)")
  {
    auto dist = fho::prng_dist<int>(-128, 128);
    THEN("values are within [-128,128]")
    {
      for (auto i = 0u; i < 1024u; ++i)
      {
        auto r = dist(rng);
        REQUIRE(r >= -128);
        REQUIRE(r <= 128);
      }
    }
  }

  WHEN("reproducibility: same seed -> identical engine sequence")
  {
    auto a = fho::prng_engine{42u};
    auto b = fho::prng_engine{42u};
    THEN("first 1k samples match exactly")
    {
      for (int i = 0; i < 1000; ++i)
      {
        REQUIRE(a() == b());
      }
    }
  }

  WHEN("param round-trip for distribution")
  {
    fho::prng_dist<unsigned> d0(37u, 413u);
    auto                     p = d0.param();
    fho::prng_dist<unsigned> d1(p);
    THEN("copied params produce in-range values")
    {
      for (auto i = 0u; i < 512u; ++i)
      {
        auto r0 = d0(rng);
        auto r1 = d1(rng);
        REQUIRE(r0 >= 37u);
        REQUIRE(r0 <= 413u);
        REQUIRE(r1 >= 37u);
        REQUIRE(r1 <= 413u);
      }
    }
  }

  // ---------------------------------------------------------------------------
  // Deterministic coverage tests using the 32-bit URBG.
  // These do not rely on chance and will always pass if the mapping is correct.
  // ---------------------------------------------------------------------------

  WHEN("power-of-two range: [0,255] uses fast mask path")
  {
    // With our byte_counter_urbg and prng_dist<uint32_t>(0,255), the result
    // must equal the next byte (exactly one of each 0..255 per 256 outputs).
    auto urbg = word_counter_urbg{};
    auto dist = fho::prng_dist<std::uint32_t>(0u, 255u);

    THEN("exactly one of each value 0..255 per cycle")
    {
      auto counts = std::array<unsigned, 256>{};
      for (int i = 0; i < 256; ++i)
      {
        auto r = dist(urbg);
        REQUIRE(r <= 255u);
        counts[static_cast<std::size_t>(r)]++; // NOLINT
      }
      for (auto c : counts)
      {
        REQUIRE(c == 1u);
      }
    }

    THEN("sequence repeats deterministically every 256 draws")
    {
      // Collect two cycles and compare
      auto a = std::array<std::uint32_t, 256>{};
      auto b = std::array<std::uint32_t, 256>{};
      for (int i = 0; i < 256; ++i)
      {
        a[i] = dist(urbg); // NOLINT
      }
      for (int i = 0; i < 256; ++i)
      {
        b[i] = dist(urbg); // NOLINT
      }
      REQUIRE(a == b);
    }
  }

  WHEN("non power-of-two range: [0,250] exercises rejection path")
  {
    // We don’t use probability; we only assert strict bounds and determinism.
    auto urbg = word_counter_urbg{};
    auto dist = fho::prng_dist<std::uint32_t>(0u, 250u);

    THEN("all values are within [0,250]")
    {
      for (int i = 0; i < 2048; ++i)
      {
        auto r = dist(urbg);
        REQUIRE(r <= 250u);
      }
    }

    THEN("deterministic sequence for a deterministic URBG")
    {
      // Two fresh identical URBGs must yield identical sequences
      auto u1 = word_counter_urbg{};
      auto u2 = word_counter_urbg{};
      for (int i = 0; i < 1024; ++i)
      {
        REQUIRE(dist(u1) == dist(u2));
      }
    }
  }

  WHEN("single-value range [a,a] always returns a")
  {
    auto deq = fho::prng_dist<std::uint32_t>(7u, 7u);
    THEN("returns the bound exactly")
    {
      for (auto i = 0u; i < 64u; ++i)
      {
        REQUIRE(deq(rng) == 7u);
      }
    }
  }

  WHEN("[a,a+1] only returns the two endpoints")
  {
    auto d2 = fho::prng_dist<std::uint32_t>(9u, 10u);
    THEN("values are either 9 or 10")
    {
      for (auto i = 0u; i < 512u; ++i)
      {
        auto r = d2(rng);
        INFO("r: " << r);
        REQUIRE((r == 9u || r == 10u));
      }
    }
  }
}
