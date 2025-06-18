#include <threadable-tests/doctest_include.hxx>
#include <threadable/queue.hxx>

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

SCENARIO("queue: push & claim")
{
  GIVEN("queue with capacity 2 (max size 1)")
  {
    auto queue = fho::queue<2>{};
    REQUIRE(queue.size() == 0);
    REQUIRE(queue.max_size() == 1);

    WHEN("empty")
    {
      THEN("begin() and end() does not throw/crash")
      {
        REQUIRE_NOTHROW(queue.begin());
        REQUIRE_NOTHROW(queue.end());
        REQUIRE_NOTHROW(queue.consume());
      }
      THEN("clear() does not throw/crash")
      {
        REQUIRE_NOTHROW(queue.clear());
      }
      AND_WHEN("moved from")
      {
        auto _ = std::move(queue);
        THEN("members do nothing")
        {
          REQUIRE_NOTHROW(queue.begin());
          REQUIRE_NOTHROW(queue.end());
          REQUIRE_NOTHROW(queue.consume());
          REQUIRE(queue.size() == 0);
          REQUIRE(queue.empty());
          REQUIRE(queue.execute() == 0);
          REQUIRE(queue.execute() == 0);
          REQUIRE_NOTHROW(queue.clear());
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

      queue.push(type{});
      REQUIRE(queue.size() == 1);

      called    = 0;
      destroyed = 0;
      AND_WHEN("queue is destroyed")
      {
        {
          auto queue2 = fho::queue<2>{};
          queue2.push(type{});
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
        for (auto const& job : queue)
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
        auto [begin, end] = queue.consume();
        REQUIRE(begin != end);
        REQUIRE(queue.size() == 0);
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

      token = queue.push(
        [&wasCancelled](fho::job_token& token)
        {
          wasCancelled = token.cancelled();
        },
        std::ref(token));
      REQUIRE(queue.size() == 1);
      THEN("the token will be passed when the job is executed")
      {
        token.cancel();
        REQUIRE(queue.execute() == 1);
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
        queue.push(
          [&called, bigData = std::make_shared<std::uint8_t[]>(
                      too_big)](int arg, std::shared_ptr<std::uint8_t[]> const& data)
          {
            called = arg;
            (void)data;
          },
          16, std::make_shared<std::uint8_t[]>(too_big));
        // NOLINTEND

        REQUIRE(queue.size() == 1);
        REQUIRE(queue.execute() == 1);
        REQUIRE(called == 16);
      }
    }
  }
  GIVEN("queue with capacity 128")
  {
    static constexpr auto queue_capacity = 128;
    auto                  queue          = fho::queue<queue_capacity>{};
    REQUIRE(queue.size() == 0);

    WHEN("push all")
    {
      auto executed = std::vector<std::size_t>(queue.max_size(), 0);
      for (std::size_t i = 1; i <= queue.max_size(); ++i)
      {
        queue.push(
          [i, &executed]
          {
            executed[i - 1] = i;
          });
      }
      REQUIRE(queue.size() == queue.max_size());

      AND_WHEN("consume and execute jobs")
      {
        for (auto& job : queue.consume())
        {
          REQUIRE(job);
          job();
        }
        THEN("128 jobs were executed")
        {
          REQUIRE(executed.size() == queue.max_size());
          for (std::size_t i = 1; i <= queue.max_size(); ++i)
          {
            REQUIRE(executed[i - 1] == i);
          }

          AND_THEN("queue is empty")
          {
            REQUIRE(queue.size() == 0);
          }

          AND_WHEN("iterate without executing jobs")
          {
            std::fill(executed.begin(), executed.end(), 0);

            for (auto const& job : queue)
            {
              (void)job;
            }
            THEN("0 jobs were executed")
            {
              REQUIRE(executed.size() == queue.max_size());
              for (std::size_t i = 0; i < queue.max_size(); ++i)
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

SCENARIO("queue: alignment")
{
  static constexpr auto queue_capacity = 128;
  auto                  queue          = fho::queue<queue_capacity>{};
  GIVEN("a queue of 128 items")
  {
    for (std::size_t i = 0; i < queue.max_size(); ++i)
    {
      queue.push([] {});
    }
    THEN("they are alligned to cache line boundaries")
    {
      for (auto const& item : queue)
      {
        REQUIRE(is_aligned(&item, fho::details::cache_line_size));
      }
    }
  }
}

SCENARIO("queue: execution order")
{
  GIVEN("a sequential queue")
  {
    auto queue = fho::queue<32>(fho::execution_policy::sequential);

    auto order = std::vector<std::size_t>{};
    auto m     = std::mutex{};
    for (std::size_t i = 0; i < queue.max_size(); ++i)
    {
      queue.push(
        [&order, &m, i]
        {
          auto _ = std::scoped_lock{m};
          order.push_back(i);
        });
    }
    WHEN("queue & execute jobs")
    {
      REQUIRE(queue.execute() == queue.max_size());
      THEN("jobs are executed FIFO")
      {
        REQUIRE(order.size() == queue.max_size());
        for (std::size_t i = 0; i < queue.max_size(); ++i)
        {
          REQUIRE(order[i] == i);
        }
      }
    }
  }
}

SCENARIO("queue: completion token")
{
  auto queue = fho::queue{};
  GIVEN("push job & store token")
  {
    auto token = queue.push([] {});
    THEN("token is NOT done when job is discarded")
    {
      for (auto const& job : queue)
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
      REQUIRE(queue.execute() == 1);
      REQUIRE(token.done());
    }
    WHEN("token is cancelled")
    {
      token.cancel();
      THEN("it is marked as 'cancelled'")
      {
        REQUIRE(token.cancelled());
      }
      THEN("job is not executed by queue")
      {
        // TODO: Fix so it's not executed. See queue::end().
        // REQUIRE(queue.execute() == 1);
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
    //       queue = fho::queue{};
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

SCENARIO("queue: stress-test")
{
  GIVEN("produce & consume enough for wrap-around")
  {
    static constexpr auto queue_capacity = std::size_t{1 << 8};

    auto queue = fho::queue<queue_capacity>();

    static constexpr auto nr_of_jobs   = queue.max_size() * 2;
    std::size_t           jobsExecuted = 0;

    for (std::size_t i = 0; i < nr_of_jobs; ++i)
    {
      auto token = queue.push([] {});
      REQUIRE(queue.execute() == 1);
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
    static constexpr auto queue_capacity = std::size_t{1 << 8};

    auto queue = fho::queue<queue_capacity>();

    THEN("there are no race conditions")
    {
      auto jobsExecuted = std::atomic_size_t{0};
      {
        // start producer
        auto producer = std::thread(
          [&queue, &jobsExecuted]
          {
            for (std::size_t i = 0; i < queue.max_size(); ++i)
            {
              queue.push(
                [&jobsExecuted]
                {
                  ++jobsExecuted;
                });
            }
          });
        // start consumer
        auto consumer = std::thread(
          [&queue, &producer]
          {
            while (producer.joinable() || !queue.empty())
            {
              queue.execute();
            }
          });

        producer.join();
        consumer.join();
      }
      REQUIRE(jobsExecuted.load() == queue.max_size());
    }
  }
  GIVEN("8 producers & 1 consumer")
  {
    static constexpr auto queue_capacity = std::size_t{1 << 20};
    static constexpr auto nr_producers   = std::size_t{5};

    auto queue   = fho::queue<queue_capacity>();
    auto barrier = std::barrier{nr_producers};

    THEN("there are no race conditions")
    {
      std::atomic_size_t jobsExecuted{0};

      // start producers
      std::vector<std::thread> producers;
      for (std::size_t i = 0; i < nr_producers; ++i)
      {
        producers.emplace_back(
          [&barrier, &queue, &jobsExecuted]
          {
            static_assert(queue.max_size() % nr_producers == 0, "All jobs must be pushed");

            auto tokens = fho::token_group{};
            barrier.arrive_and_wait();
            for (std::size_t i = 0; i < queue.max_size() / nr_producers; ++i)
            {
              tokens += queue.push(
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
        [&queue, &producers]
        {
          while (std::ranges::any_of(producers,
                                     [](auto const& p)
                                     {
                                       return p.joinable();
                                     }) ||
                 !queue.empty())
          {
            queue.execute();
          }
        });

      for (auto& producer : producers)
      {
        producer.join();
      }
      consumer.join();

      REQUIRE(jobsExecuted.load() == queue.max_size());
    }
  }
}

SCENARIO("queue: standard algorithms")
{
  GIVEN("queue with capacity of 1M")
  {
    static constexpr auto queue_capacity = 1 << 20;
    auto                  queue          = fho::queue<queue_capacity>{};
    REQUIRE(queue.size() == 0);

    auto jobsExecuted = std::atomic_size_t{0};
    WHEN("push all")
    {
      while (queue.size() < queue.max_size())
      {
        queue.push(
          [&jobsExecuted]
          {
            ++jobsExecuted;
          });
      }
      auto [b, e] = queue.consume();
      AND_WHEN("std::for_each")
      {
        std::for_each(b, e,
                      [](auto& job)
                      {
                        job();
                      });
        THEN("all jobs executed")
        {
          REQUIRE(jobsExecuted.load() == queue.max_size());
          REQUIRE(queue.size() == 0);
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
          REQUIRE(jobsExecuted.load() == queue.max_size());
          REQUIRE(queue.size() == 0);
        }
      }
    }
  }
}
