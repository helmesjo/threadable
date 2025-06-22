#include <threadable-tests/doctest_include.hxx>
#include <threadable/execution.hxx>
#include <threadable/ring_buffer.hxx>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace
{
  template<typename T>
  auto
  is_aligned(T const* ptr, std::size_t alignment) -> bool
  {
    auto addr = reinterpret_cast<std::uintptr_t>(ptr); // NOLINT
    return (addr % alignment) == 0;
  }
}

SCENARIO("ring_buffer: push & claim")
{
  GIVEN("ring with capacity 2 (max size 1)")
  {
    auto ring = fho::ring_buffer<fho::job, 2>{};
    REQUIRE(ring.size() == 0);
    REQUIRE(ring.max_size() == 1);

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
          REQUIRE(fho::execute(ring.consume()) == 0);
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

      ring.push(type{});
      REQUIRE(ring.size() == 1);

      called    = 0;
      destroyed = 0;
      AND_WHEN("ring is destroyed")
      {
        {
          auto ring2 = fho::ring_buffer<fho::job, 2>{};
          ring2.push(type{});
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
        auto [begin, end] = ring.consume();
        REQUIRE(begin != end);
        REQUIRE(ring.size() == 0);
        std::for_each(begin, end,
                      [](auto& job)
                      {
                        job();
                      });
        THEN("1 valid job executed")
        {
          REQUIRE(called == 1);
        }
      }
    }
    WHEN("push callable with 'job_token&' as first parameter")
    {
      bool wasCancelled = false;
      auto token        = fho::job_token{};

      token = ring.push(
        [&wasCancelled](fho::job_token& token)
        {
          wasCancelled = token.cancelled();
        },
        std::ref(token));
      REQUIRE(ring.size() == 1);
      THEN("the token will be passed when the job is executed")
      {
        token.cancel();
        REQUIRE(fho::execute(ring.consume()) == 1);
        REQUIRE(wasCancelled);
      }
    }
    WHEN("push")
    {
      THEN("a callable that is larger than buffer size can be pushed")
      {
        // NOLINTBEGIN
        int                   called  = 0;
        static constexpr auto too_big = fho::details::job_buffer_size * 2;
        // both capturing big data & passing as argument
        ring.push(
          [&called, bigData = std::make_shared<std::uint8_t[]>(
                      too_big)](int arg, std::shared_ptr<std::uint8_t[]> const& data)
          {
            called = arg;
            (void)data;
          },
          16, std::make_shared<std::uint8_t[]>(too_big));
        // NOLINTEND

        REQUIRE(ring.size() == 1);
        REQUIRE(fho::execute(ring.consume()) == 1);
        REQUIRE(called == 16);
      }
    }
  }
  GIVEN("ring with capacity 128")
  {
    static constexpr auto ring_capacity = 128;
    auto                  ring          = fho::ring_buffer<fho::job, ring_capacity>{};
    REQUIRE(ring.size() == 0);

    WHEN("push all")
    {
      auto executed = std::vector<std::size_t>(ring.max_size(), 0);
      for (std::size_t i = 1; i <= ring.max_size(); ++i)
      {
        ring.push(
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

SCENARIO("ring_buffer: alignment")
{
  static constexpr auto ring_capacity = 128;
  auto                  ring          = fho::ring_buffer<fho::job, ring_capacity>{};
  GIVEN("a ring of 128 items")
  {
    for (std::size_t i = 0; i < ring.max_size(); ++i)
    {
      ring.push([] {});
    }
    THEN("they are alligned to cache line boundaries")
    {
      for (auto const& item : ring)
      {
        REQUIRE(is_aligned(&item, fho::details::cache_line_size));
      }
    }
  }
}

SCENARIO("ring_buffer: completion token")
{
  auto ring = fho::ring_buffer{};
  GIVEN("push job & store token")
  {
    auto token = ring.push([] {});
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
      REQUIRE(fho::execute(ring.consume()) == 1);
      REQUIRE(token.done());
    }
    WHEN("token is cancelled")
    {
      token.cancel();
      THEN("it is marked as 'cancelled'")
      {
        REQUIRE(token.cancelled());
      }
      THEN("job is not executed by ring")
      {
        // TODO: Fix so it's not executed. See ring::end().
        // REQUIRE(ring.execute() == 1);
      }
    }
    // @TODO: Rework this. What to do if job is destroyed before token.wait()?
    // WHEN("related job is destroyed while token is being waited on")
    // {
    //   // Very rudimentary test
    //   auto waiting = std::atomic_bool{false};
    //   auto thread  = std::thread(
    //     [&]
    //     {
    //       while (!waiting)
    //         ;
    //       std::this_thread::sleep_for(std::chrono::milliseconds{10});
    //       ring = fho::ring{};
    //     });

    //   THEN("it does not crash")
    //   {
    //     waiting = true;
    //     token.wait();
    //   }
    //   thread.join();
    // }
  }
}

SCENARIO("ring_buffer: stress-test")
{
  GIVEN("produce & consume enough for wrap-around")
  {
    static constexpr auto ring_capacity = std::size_t{1 << 8};

    auto ring = fho::ring_buffer<fho::job, ring_capacity>();

    static constexpr auto nr_of_jobs   = ring.max_size() * 2;
    std::size_t           jobsExecuted = 0;

    for (std::size_t i = 0; i < nr_of_jobs; ++i)
    {
      auto token = ring.push([] {});
      REQUIRE(fho::execute(ring.consume()) == 1);
      ++jobsExecuted;
      REQUIRE(token.done());
    }

    THEN("all jobs are executed")
    {
      REQUIRE(jobsExecuted == nr_of_jobs);
    }
  }
  GIVEN("1 producer & 1 consumer")
  {
    static constexpr auto ring_capacity = std::size_t{1 << 8};

    auto ring = fho::ring_buffer<fho::job, ring_capacity>();

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
              ring.push(
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
              fho::execute(ring.consume());
            }
          });

        producer.join();
        consumer.join();
      }
      REQUIRE(jobsExecuted.load() == ring.max_size());
    }
  }
  GIVEN("8 producers & 1 consumer")
  {
    static constexpr auto ring_capacity = std::size_t{1 << 20};
    static constexpr auto nr_producers  = std::size_t{5};

    auto ring    = fho::ring_buffer<fho::job, ring_capacity>();
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
            static_assert(ring.max_size() % nr_producers == 0, "All jobs must be pushed");

            auto tokens = fho::token_group{};
            barrier.arrive_and_wait();
            for (std::size_t i = 0; i < ring.max_size() / nr_producers; ++i)
            {
              tokens += ring.push(
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
            fho::execute(ring.consume());
          }
        });

      for (auto& producer : producers)
      {
        producer.join();
      }
      consumer.join();

      REQUIRE(jobsExecuted.load() == ring.max_size());
    }
  }
}

SCENARIO("ring_buffer: standard algorithms")
{
  GIVEN("ring with capacity of 1M")
  {
    static constexpr auto ring_capacity = 1 << 20;
    auto                  ring          = fho::ring_buffer<fho::job, ring_capacity>{};
    REQUIRE(ring.size() == 0);

    auto jobsExecuted = std::atomic_size_t{0};
    WHEN("push all")
    {
      while (ring.size() < ring.max_size())
      {
        ring.push(
          [&jobsExecuted]
          {
            ++jobsExecuted;
          });
      }
      auto [b, e] = ring.consume();
      AND_WHEN("std::for_each")
      {
        std::for_each(b, e,
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
      AND_WHEN("std::for_each (parallel)")
      {
        std::for_each(std::execution::par, b, e,
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
