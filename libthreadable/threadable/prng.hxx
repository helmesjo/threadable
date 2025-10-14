#pragma once

#include <concepts>
#include <cstdint>
#include <limits>
#include <random>
#include <type_traits>

namespace fho
{
  inline auto
  simple_seed() noexcept
  {
    return std::random_device{}();
  }

  /// @brief Engine: prng_engine — UniformRandomBitGenerator (PCG32).
  /// @details Minimal, fast 32-bit PCG XSH RR engine fulfilling the C++ URBG
  ///          concept: exposes result_type, min(), max(), and operator()().
  struct prng_engine
  {
    using result_type = std::uint32_t;

    /// @brief Default-construct with fixed PCG stream/state.
    /// @details Seeds with PCG reference constants; useful for deterministic tests.
    constexpr prng_engine()
      : state_(0x853c49e6748fea9bULL)
      , inc_(0xda3e39cb94b95bdbULL)
    {}

    /// @brief Construct with seed and optional stream selector.
    /// @details Initializes the LCG state and advances twice per PCG seeding recipe.
    explicit prng_engine(std::uint64_t seed, std::uint64_t seq = 54) // NOLINT
      : state_(0)
      , inc_((seq << 1u) | 1u)
    {
      (void)operator()();
      state_ += seed;
      (void)operator()();
    }

    /// @brief Minimum value produced by the engine.
    /// @details Required by UniformRandomBitGenerator; always 0 for 32-bit result.
    static constexpr auto
    min() -> result_type
    {
      return std::numeric_limits<result_type>::min();
    }

    /// @brief Maximum value produced by the engine.
    /// @details Required by UniformRandomBitGenerator; equals UINT32_MAX.
    static constexpr auto
    max() -> result_type
    {
      return std::numeric_limits<result_type>::max();
    }

    /// @brief Generate the next 32-bit random value.
    /// @details One 64-bit LCG step followed by xorshift+rotate output permutation.
    auto
    operator()() -> result_type
    {
      std::uint64_t old = state_;
      state_            = old * 6364136223846793005ULL + inc_;
      auto xorshifted   = static_cast<std::uint32_t>(((old >> 18u) ^ old) >> 27u);
      auto rot          = static_cast<std::uint32_t>(old >> 59u);
      return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
    }

    /// @brief Reseed the engine.
    /// @details Reinitializes using the single-argument constructor semantics.
    void
    seed(std::uint64_t s)
    {
      *this = prng_engine{s};
    }

  private:
    std::uint64_t state_;
    std::uint64_t inc_;
  };

  /// @brief Distribution: prng_dist<Int> — like std::uniform_int_distribution.
  /// @details Inclusive bounds [a, b]; uses Lemire’s multiply-high mapping for
  ///          unbiased, fast reduction with a power-of-two fast path.
  template<std::integral Int = std::uint32_t>
  class prng_dist
  {
  public:
    using result_type = Int;

    /// @brief Parameter bundle for prng_dist.
    /// @details Mirrors std::uniform_int_distribution::param_type (stores a,b).
    struct param_type
    {
      /// @brief Construct parameters with inclusive bounds.
      /// @details No validation beyond type constraints.
      param_type(result_type a, result_type b) // NOLINT
        : a_(a)
        , b_(b)
      {}

      /// @brief Lower bound (inclusive).
      /// @details Returned exactly as provided to the constructor.
      auto
      a() const -> result_type
      {
        return a_;
      }

      /// @brief Upper bound (inclusive).
      /// @details Returned exactly as provided to the constructor.
      auto
      b() const -> result_type
      {
        return b_;
      }

    private:
      result_type a_, b_;
    };

    /// @brief Default-construct to the full range of result_type.
    /// @details Equivalent to prng_dist(0, numeric_limits<result_type>::max()).
    prng_dist()
      : prng_dist(0, std::numeric_limits<result_type>::max())
    {}

    /// @brief Construct with inclusive bounds [a,b].
    /// @details No runtime checks; behavior is undefined if b < a.
    prng_dist(result_type a, result_type b) // NOLINT
      : a_(a)
      , b_(b)
    {}

    /// @brief Construct from parameter bundle.
    /// @details Copies a() and b() from the given param_type.
    explicit prng_dist(param_type const& p)
      : a_(p.a())
      , b_(p.b())
    {}

    /// @brief Reset internal state.
    /// @details Distribution is stateless; this is a no-op for API parity.
    void
    reset()
    {}

    /// @brief Get lower bound (inclusive).
    /// @details Matches current parameterization of the distribution.
    auto
    a() const -> result_type
    {
      return a_;
    }

    /// @brief Get upper bound (inclusive).
    /// @details Matches current parameterization of the distribution.
    auto
    b() const -> result_type
    {
      return b_;
    }

    /// @brief Get the current parameter bundle.
    /// @details Returns a value object containing [a,b].
    auto
    param() const -> param_type
    {
      return param_type{a_, b_};
    }

    /// @brief Set the current parameter bundle.
    /// @details Replaces [a,b] with those from p.
    void
    param(param_type const& p)
    {
      a_ = p.a();
      b_ = p.b();
    }

    /// @brief Generate a value using the stored parameters.
    /// @details Consumes bits from URBG and maps uniformly into [a,b].
    template<class URBG>
    auto
    operator()(URBG& g) -> result_type
    {
      return (*this)(g, param());
    }

    /// @brief Generate a value using explicit parameters.
    /// @details Uses Lemire mapping to obtain an unbiased result in [p.a(),p.b()].
    template<class URBG>
    auto
    operator()(URBG& g, param_type const& p) -> result_type
    {
      using U = std::make_unsigned_t<result_type>;
      U lo = static_cast<U>(p.a()), hi = static_cast<U>(p.b());
      U n = hi - lo + U{1}; // width in [1, 2^bits]

      // Draw unbiased u in [0, n)
      U u = map_to_range<U>(draw_bits<U>(g), n, g);
      if constexpr (std::is_signed_v<Int>)
      {
        using S = std::make_signed_t<Int>;
        // u ∈ [0, b-a], so a + u ∈ [a,b] — no overflow in S.
        return static_cast<Int>(static_cast<S>(p.a()) + static_cast<S>(u));
      }
      else
      {
        return static_cast<Int>(static_cast<U>(p.a()) + u);
      }
    }

  private:
    /// @brief POD pair for wide multiplication halves.
    /// @details Holds low and high halves of a*b in native unsigned width.
    template<class U>
    struct hi_lo
    {
      U lo; ///< @brief Low half of the product. @details Equal to (a*b) mod 2^bits.
      U hi; ///< @brief High half of the product. @details Floor((a*b)/2^bits).
    };

    /// @brief Draw an unsigned value covering all bits of U.
    /// @details Uses one or more URBG calls depending on URBG::result_type width.
    template<class U, class URBG>
    static auto
    draw_bits(URBG& g) -> U
    {
      using G             = typename URBG::result_type;
      constexpr int ubits = int(sizeof(U) * 8);
      constexpr int gbits = int(sizeof(G) * 8);
      if constexpr (ubits <= gbits)
      {
        return static_cast<U>(g());
      }
      else
      {
        // build U from multiple calls
        U   v      = 0;
        int filled = 0;
        while (filled < ubits)
        {
          v |= U(static_cast<U>(g())) << filled;
          filled += gbits;
        }
        return v;
      }
    }

    /// @brief Wide multiply returning both halves of a*b.
    /// @details Provides 32×32→64 via 64-bit arithmetic and 64×64→128 via
    ///          __int128 when available; otherwise uses a portable 64×64
    ///          decomposition. Returns hi_lo<U>{lo, hi}.
    template<class U>
    inline static auto
    mul_wide(U a, U b) -> hi_lo<U>
    {
      if constexpr (sizeof(U) <= 4)
      {
        std::uint64_t m = std::uint64_t(a) * std::uint64_t(b);
        return hi_lo<U>{U(m), U(m >> 32)};
      }
      else
      {
#if defined(__SIZEOF_INT128__)
        __uint128_t m = (__uint128_t)a * (__uint128_t)b;
        return hi_lo<U>{U(m), U(m >> 64)};
#else
        // Portable 64×64 -> 128 via decomposition into 32-bit limbs.
        std::uint64_t const a0 = std::uint64_t(a & U(0xffffffff));
        std::uint64_t const a1 = std::uint64_t(a >> 32);
        std::uint64_t const b0 = std::uint64_t(b & U(0xffffffff));
        std::uint64_t const b1 = std::uint64_t(b >> 32);

        std::uint64_t const p00 = a0 * b0;
        std::uint64_t const p01 = a0 * b1;
        std::uint64_t const p10 = a1 * b0;
        std::uint64_t const p11 = a1 * b1;

        std::uint64_t mid_lo = (p00 >> 32) + (p01 & 0xffffffff) + (p10 & 0xffffffff);
        std::uint64_t lo     = (p00 & 0xffffffff) | (mid_lo << 32);
        std::uint64_t hi     = p11 + (p01 >> 32) + (p10 >> 32) + (mid_lo >> 32);
        return hi_lo<U>{U(lo), U(hi)};
#endif
      }
    }

    /// @brief Unbiased map of x to [0, n).
    /// @details Power-of-two fast path; otherwise Lemire’s multiply-high with
    ///          rejection sampling using mul_wide for portability.
    template<class U, class URBG>
    static auto
    map_to_range(U x, U n, URBG& g) -> U
    {
      if (n == 0)
      {
        return x; // full range (degenerate)
      }
      if ((n & (n - 1)) == 0)
      {
        return x & (n - 1);                   // power-of-two fast path
      }

      U const t = U((U{0} - n) % n);          // threshold from Lemire 2019
      while (true)
      {
        auto const parts = mul_wide<U>(x, n); // parts.lo = low, parts.hi = high
        if (parts.lo >= t)
        {
          return parts.hi;
        }
        x = draw_bits<U>(g); // resample
      }
    }

    result_type a_, b_;
  };
}
