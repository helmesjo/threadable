#include <threadable-tests/doctest_include.hxx>
#include <threadable/execution.hxx>
#include <threadable/function.hxx>
#include <threadable/ring_buffer.hxx>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace
{
  template<typename T>
  auto
  is_aligned(T const& ptr, std::size_t alignment) -> bool
  {
    auto addr = reinterpret_cast<std::uintptr_t>(&ptr); // NOLINT
    return (addr % alignment) == 0;
  }

  using func_t = fho::fast_func_t;
}

SCENARIO("ring_buffer: push & claim")
{
  GIVEN("ring with capacity 2 (max size 1)")
  {
    static_assert(fho::ring_buffer<func_t, 2>::max_size() == 1);

    auto ring = fho::ring_buffer<func_t, 2>{};
    REQUIRE(ring.size() == 0);

    WHEN("empty")
    {
      THEN("begin() and end() does not throw/crash")
      {
        REQUIRE_NOTHROW(ring.begin());
        REQUIRE_NOTHROW(ring.end());
        REQUIRE_NOTHROW(ring.consume());
      }
      THEN("clear() does not throw/crash")
      {
        REQUIRE_NOTHROW(ring.clear());
      }
      AND_WHEN("moved from")
      {
        auto _ = std::move(ring);
        THEN("members do nothing")
        {
          REQUIRE_NOTHROW(ring.begin());
          REQUIRE_NOTHROW(ring.end());
          REQUIRE_NOTHROW(ring.consume());
          REQUIRE(ring.size() == 0);
          REQUIRE(ring.empty());
          REQUIRE(ring.consume().empty());
          REQUIRE_NOTHROW(ring.clear());
        }
      }
    }

    WHEN("push 1")
    {
      thread_local int called    = 0;
      thread_local int destroyed = 0;

      struct type
      {
        type()            = default;
        type(type const&) = default;
        type(type&&)      = default;

        ~type()
        {
          ++destroyed;
        }

        auto operator=(type const&) -> type& = delete;
        auto operator=(type&&) -> type&      = delete;

        void
        operator()()
        {
          ++called;
        }
      };

      ring.emplace_back(type{});
      REQUIRE(ring.size() == 1);

      called    = 0;
      destroyed = 0;
      AND_WHEN("cleared")
      {
        ring.clear();
        THEN("it resets & destroys pushed jobs")
        {
          REQUIRE(ring.empty());
          REQUIRE(called == 0);
          REQUIRE(destroyed == 1);
        }
      }
      AND_WHEN("ring is destroyed")
      {
        {
          auto ring2 = fho::ring_buffer<func_t, 2>{};
          ring2.emplace_back(type{});
          called    = 0;
          destroyed = 0;
        }
        THEN("it resets & destroys pushed jobs")
        {
          REQUIRE(called == 0);
          REQUIRE(destroyed == 1);
        }
      }
      AND_WHEN("iterate jobs")
      {
        auto nrJobs = 0;
        for (auto const& job : ring)
        {
          REQUIRE(job);
          ++nrJobs;
        }
        THEN("1 valid job existed")
        {
          REQUIRE(nrJobs == 1);
          REQUIRE(called == 0);
        }
      }
      AND_WHEN("consume and execute jobs")
      {
        auto r = ring.consume();
        REQUIRE(r.begin() != r.end());
        REQUIRE(ring.size() == 0);
        for (auto& job : r)
        {
          job();
        }
        THEN("1 valid job executed")
        {
          REQUIRE(called == 1);
        }
      }
    }
    WHEN("push callable with 'slot_token&' as first parameter")
    {
      int  called = 0;
      auto token  = fho::slot_token{};

      token = ring.emplace_back(
        [&called](fho::slot_token& token)
        {
          ++called;
        });
      REQUIRE(ring.size() == 1);
      THEN("the token will be passed when the job is executed")
      {
        token.cancel();
        for (auto& job : ring.consume())
        {
          job();
        }
        REQUIRE(called == 1);
      }
    }
    WHEN("push")
    {
      THEN("a callable that is larger than buffer size can be pushed")
      {
        // NOLINTBEGIN
        int called       = 0;
        using big_data_t = std::array<std::byte, fho::details::slot_size * 2>;
        // both capturing big data & passing as argument
        ring.emplace_back(
          [&called, bigData = big_data_t{}](int arg, big_data_t const& data)
          {
            called = arg;
            (void)bigData;
            (void)data;
          },
          16, big_data_t{});
        // NOLINTEND

        REQUIRE(ring.size() == 1);
        for (auto& job : ring.consume())
        {
          job();
        }
        REQUIRE(called == 16);
      }
    }
  }
  GIVEN("ring with capacity 128")
  {
    auto ring = fho::ring_buffer<func_t, 128>{};
    REQUIRE(ring.size() == 0);

    WHEN("push all")
    {
      auto executed = std::vector<std::size_t>(ring.max_size(), 0);
      for (std::size_t i = 1; i <= ring.max_size(); ++i)
      {
        ring.emplace_back(
          [i, &executed]
          {
            executed[i - 1] = i;
          });
      }
      REQUIRE(ring.size() == ring.max_size());

      AND_WHEN("consume and execute jobs")
      {
        for (auto& job : ring.consume())
        {
          REQUIRE(job);
          job();
        }
        THEN("128 jobs were executed")
        {
          REQUIRE(executed.size() == ring.max_size());
          for (std::size_t i = 1; i <= ring.max_size(); ++i)
          {
            REQUIRE(executed[i - 1] == i);
          }

          AND_THEN("ring is empty")
          {
            REQUIRE(ring.size() == 0);
          }

          AND_WHEN("iterate without executing jobs")
          {
            std::ranges::fill(executed, 0);

            for (auto const& job : ring)
            {
              (void)job;
            }
            THEN("0 jobs were executed")
            {
              REQUIRE(executed.size() == ring.max_size());
              for (std::size_t i = 0; i < ring.max_size(); ++i)
              {
                REQUIRE(executed[i] == 0);
              }
            }
          }
        }
      }
    }
  }
}

SCENARIO("ring_buffer: custom type")
{
  GIVEN("a type that fits within one default slot buffer size")
  {
    struct my_type
    {
      my_type(my_type const&)                    = default;
      my_type(my_type&&)                         = default;
      ~my_type()                                 = default;
      auto operator=(my_type const&) -> my_type& = default;
      auto operator=(my_type&&) -> my_type&      = default;

      my_type(int x, float y) // NOLINT
        : x(x)
        , y(y)
      {}

      int   x;
      float y;

      auto operator<=>(my_type const&) const noexcept = default;
    };

    static_assert(sizeof(my_type) <= fho::details::slot_size);

    THEN("slot size is exactly one cache line bytes")
    {
      auto ring    = fho::ring_buffer<my_type>{};
      using slot_t = std::remove_cvref_t<decltype(*ring.begin().base())>;
      static_assert(sizeof(slot_t) == fho::details::cache_line_size);

      // basic sanity-tests:
      ring.emplace_back(1, 2.5f);
      ring.emplace_back(my_type(3, 4.5f));

      REQUIRE(ring.size() == 2);
      REQUIRE(ring.front() == my_type(1, 2.5f));
      REQUIRE(ring.back() == my_type(3, 4.5f));
      ring.pop();
      REQUIRE(ring.front() == my_type(3, 4.5f));
      ring.pop();
      REQUIRE(ring.size() == 0);
    }
  }
  GIVEN("a type that fits within two default slot buffer size")
  {
    struct my_too_big_type
    {
      std::array<std::byte, fho::details::slot_size * 2> padding = {};
    };

    static_assert(sizeof(my_too_big_type) == fho::details::slot_size * 2);

    THEN("slot size is exactly two cache line bytes")
    {
      auto ring    = fho::ring_buffer<my_too_big_type>{};
      using slot_t = std::remove_cvref_t<decltype(*ring.begin().base())>;
      static_assert(sizeof(slot_t) == fho::details::cache_line_size * 2);

      // basic sanity-tests:
      ring.emplace_back();
      ring.emplace_back(my_too_big_type());

      REQUIRE(ring.size() == 2);
      ring.pop();
      REQUIRE(ring.size() == 1);
      ring.pop();
      REQUIRE(ring.size() == 0);
    }
  }
}

SCENARIO("ring_buffer: alignment")
{
  auto ring = fho::ring_buffer<func_t, 128>{};
  GIVEN("a ring of 128 items")
  {
    for (std::size_t i = 0; i < ring.max_size(); ++i)
    {
      ring.emplace_back([] {});
    }
    THEN("all are aligned to cache line boundaries")
    {
      for (auto itr = ring.begin(); itr != ring.end(); ++itr)
      {
        REQUIRE(is_aligned(*itr.base(), fho::details::cache_line_size));
      }
    }
  }
}

SCENARIO("ring_buffer: completion token")
{
  auto ring = fho::ring_buffer{};
  GIVEN("push job & store token")
  {
    int  called = 0;
    auto token  = ring.emplace_back(
      [&called]
      {
        ++called;
      });
    THEN("token is NOT done when job is discarded")
    {
      for (auto const& job : ring)
      {
        (void)job; /* discard job */
      }
      REQUIRE_FALSE(token.done());
    }
    THEN("token is NOT done before job is invoked")
    {
      REQUIRE_FALSE(token.done());
    }
    THEN("token is done after job was invoked")
    {
      for (auto& job : ring.consume())
      {
        job();
      }
      REQUIRE(token.done());
    }
    WHEN("token is cancelled")
    {
      token.cancel();
      THEN("it is marked as 'cancelled'")
      {
        REQUIRE(token.cancelled());
      }
      THEN("job is still executed by ring")
      {
        // "cancel" only signals through the token,
        // meant to be reacted upon by used code -
        // the job will still be executed if
        // processed by a background thread.
        for (auto& job : ring.consume())
        {
          job();
        }
        REQUIRE(called == 1);
      }
    }
  }
}

SCENARIO("ring_buffer: stress-test")
{
  GIVEN("produce & consume enough for wrap-around")
  {
    static constexpr auto capacity = std::size_t{1 << 8};

    auto ring = fho::ring_buffer<func_t, capacity>();

    static constexpr auto nr_of_jobs   = ring.max_size() * 2;
    std::size_t           jobsExecuted = 0;

    for (std::size_t i = 0; i < nr_of_jobs; ++i)
    {
      auto token = ring.emplace_back(
        [&jobsExecuted]
        {
          ++jobsExecuted;
        });
      for (auto& job : ring.consume())
      {
        job();
      }
      REQUIRE(token.done());
    }

    THEN("all jobs are executed")
    {
      REQUIRE(jobsExecuted == nr_of_jobs);
    }
  }
  GIVEN("1 producer & 1 consumer")
  {
    static constexpr auto capacity = std::size_t{1 << 8};

    auto ring = fho::ring_buffer<func_t, capacity>();

    THEN("there are no race conditions")
    {
      auto jobsExecuted = std::atomic_size_t{0};
      {
        // start producer
        auto producer = std::thread(
          [&ring, &jobsExecuted]
          {
            for (std::size_t i = 0; i < ring.max_size(); ++i)
            {
              ring.emplace_back(
                [&jobsExecuted]
                {
                  ++jobsExecuted;
                });
            }
          });
        // start consumer
        auto consumer = std::thread(
          [&ring, &producer]
          {
            while (producer.joinable() || !ring.empty())
            {
              for (auto& job : ring.consume())
              {
                job();
              }
            }
          });

        producer.join();
        consumer.join();
      }
      REQUIRE(jobsExecuted.load() == ring.max_size());
    }
  }
  GIVEN("5 producers & 1 consumer")
  {
    static constexpr auto capacity     = std::size_t{1 << 16};
    static constexpr auto nr_producers = std::size_t{5};

    auto ring    = fho::ring_buffer<func_t, capacity>();
    auto barrier = std::barrier{nr_producers};

    THEN("there are no race conditions")
    {
      std::atomic_size_t jobsExecuted{0};

      // start producers
      std::vector<std::thread> producers;
      for (std::size_t i = 0; i < nr_producers; ++i)
      {
        producers.emplace_back(
          [&barrier, &ring, &jobsExecuted]
          {
            auto tokens = fho::token_group{};
            barrier.arrive_and_wait();
            for (std::size_t i = 0; i < ring.max_size(); ++i)
            {
              tokens += ring.emplace_back(
                [&jobsExecuted]
                {
                  ++jobsExecuted;
                });
            }
            tokens.wait();
          });
      }
      // start consumer
      std::thread consumer(
        [&ring, &producers]
        {
          while (std::ranges::any_of(producers,
                                     [](auto const& p)
                                     {
                                       return p.joinable();
                                     }) ||
                 !ring.empty())
          {
            for (auto& job : ring.consume())
            {
              job();
            }
          }
        });

      for (auto& producer : producers)
      {
        producer.join();
      }
      consumer.join();

      REQUIRE(jobsExecuted.load() == ring.max_size() * nr_producers);
    }
  }
}

SCENARIO("ring_buffer: standard algorithms")
{
  GIVEN("ring with capacity of 65K")
  {
    static constexpr auto capacity = 1 << 16;
    auto                  ring     = fho::ring_buffer<func_t, capacity>{};
    REQUIRE(ring.size() == 0);

    auto jobsExecuted = std::atomic_size_t{0};
    WHEN("push all")
    {
      while (ring.size() < ring.max_size())
      {
        ring.emplace_back(
          [&jobsExecuted]
          {
            ++jobsExecuted;
          });
      }
      auto r = ring.consume();
      AND_WHEN("std::for_each")
      {
        std::ranges::for_each(r,
                              [](auto& job)
                              {
                                job();
                              });
        THEN("all jobs executed")
        {
          REQUIRE(jobsExecuted.load() == ring.max_size());
          REQUIRE(ring.size() == 0);
        }
      }
    }
  }
}
