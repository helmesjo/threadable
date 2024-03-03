#include <threadable-tests/doctest_include.hxx>
#include <threadable/queue.hxx>

#include <algorithm>
#include <atomic>
#include <cstddef>
#if __cpp_lib_execution
  #include <execution>
#endif
#if __has_include(<pstld/pstld.h>)
  #include <pstld/pstld.h>
#endif
#include <functional>
#include <mutex>
#include <thread>

SCENARIO("queue: push & claim")
{
  GIVEN("queue with capacity 2 (max size 1)")
  {
    auto queue = threadable::queue<2>{};
    REQUIRE(queue.size() == 0);

    WHEN("empty")
    {
      THEN("begin() and end() does not throw/crash")
      {
        REQUIRE_NOTHROW(queue.begin());
        REQUIRE_NOTHROW(queue.end<false>());
        REQUIRE_NOTHROW(queue.end<true>());
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
          REQUIRE_NOTHROW(queue.end<false>());
          REQUIRE_NOTHROW(queue.end<true>());
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
          auto queue2 = threadable::queue<2>{};
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
      AND_WHEN("iterate without executing jobs")
      {
        auto nrJobs = 0;
        for (auto& job : queue)
        {
          REQUIRE(job);
          ++nrJobs;
        }
        THEN("1 valid job existed")
        {
          REQUIRE(nrJobs == 1);
          REQUIRE(called == 0);

          AND_WHEN("iterate without executing jobs")
          {
            nrJobs = 0;
            for (auto& job : queue)
            {
              (void)job;
              ++nrJobs;
            }
            THEN("1 valid job still existed")
            {
              REQUIRE(nrJobs == 1);
            }
          }
        }
      }
      AND_WHEN("iterate and execute jobs")
      {
        auto nrJobs = 0;
        for (auto& job : queue)
        {
          job();
          ++nrJobs;
        }
        THEN("1 valid job existed")
        {
          REQUIRE(nrJobs == 1);

          AND_WHEN("iterate")
          {
            nrJobs = 0;
            for (auto& job : queue)
            {
              (void)job;
              ++nrJobs;
            }
            THEN("0 valid jobs still existed")
            {
              REQUIRE(nrJobs == 0);
            }
          }
        }
      }
    }
    WHEN("push callable with 'job_token&' as first parameter")
    {
      bool                  wasCancelled = false;
      threadable::job_token token;
      token = queue.push(
        [&wasCancelled](threadable::job_token& token)
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
        static constexpr auto too_big = threadable::details::job_buffer_size * 2;
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
    auto                  queue          = threadable::queue<queue_capacity>{};
    REQUIRE(queue.size() == 0);

    std::vector<std::size_t> jobsExecuted;
    WHEN("push all")
    {
      for (std::size_t i = 0; i < queue.max_size(); ++i)
      {
        queue.push(
          [i, &jobsExecuted]
          {
            jobsExecuted.push_back(i);
          });
      }
      REQUIRE(queue.size() == queue.max_size());

      AND_WHEN("iterate and execute jobs")
      {
        for (auto& job : queue)
        {
          REQUIRE(job);
          job();
        }
        THEN("128 jobs were executed")
        {
          REQUIRE(jobsExecuted.size() == queue.max_size());
          for (std::size_t i = 0; i < queue.max_size(); ++i)
          {
            REQUIRE(jobsExecuted[i] == i);
          }

          AND_THEN("queue is empty")
          {
            REQUIRE(queue.size() == 0);
          }

          AND_WHEN("iterate without executing jobs")
          {
            jobsExecuted.clear();
            for (auto& job : queue)
            {
              (void)job;
              jobsExecuted.push_back(0);
            }
            THEN("0 jobs were executed")
            {
              REQUIRE(jobsExecuted.size() == 0);
            }
          }
        }
      }
    }
  }
}

SCENARIO("queue: execution order")
{
  GIVEN("a sequential queue")
  {
    auto queue = threadable::queue<32>(threadable::execution_policy::sequential);

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
  auto queue = threadable::queue{};
  GIVEN("push job & store token")
  {
    auto token = queue.push([] {});
    THEN("token is NOT done when job is discarded")
    {
      for (auto& job : queue)
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
    WHEN("related job is destroyed")
    {
      queue = threadable::queue{};
      THEN("it does not throw")
      {
        REQUIRE_NOTHROW(token.cancel());
        REQUIRE_NOTHROW(token.wait());
      }
    }
  }
}

SCENARIO("queue: stress-test")
{
  GIVEN("produce & consume enough for wrap-around")
  {
    static constexpr std::size_t queue_capacity = 1 << 8;
    std::atomic_size_t           notifyCounter  = 0;
    auto                         queue          = threadable::queue<queue_capacity>();
    queue.set_notify(
      [&notifyCounter](...)
      {
        ++notifyCounter;
      });

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
    static constexpr std::size_t queue_capacity = 1 << 8;
    std::atomic_size_t           notifyCounter  = 0;
    auto                         queue          = threadable::queue<queue_capacity>();
    queue.set_notify(
      [&notifyCounter](...)
      {
        ++notifyCounter;
      });

    THEN("there are no race conditions")
    {
      std::atomic_size_t jobsExecuted{0};
      {
        // start producer
        std::thread producer(
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
        std::thread consumer(
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
      REQUIRE(notifyCounter.load() == queue.max_size());
    }
  }
  GIVEN("8 producers & 1 consumer")
  {
    static constexpr std::size_t queue_capacity = 1 << 20;
    static constexpr std::size_t nr_producers   = 5;
    std::atomic_size_t           notifyCounter  = 0;
    auto                         queue          = threadable::queue<queue_capacity>();
    queue.set_notify(
      [&notifyCounter](...)
      {
        ++notifyCounter;
      });

    THEN("there are no race conditions")
    {
      std::atomic_size_t jobsExecuted{0};
      {
        // start producers
        std::vector<std::thread> producers;
        for (std::size_t i = 0; i < nr_producers; ++i)
        {
          producers.emplace_back(
            [&queue, &jobsExecuted]
            {
              static_assert(decltype(queue)::max_size() % nr_producers == 0,
                            "All jobs must be pushed");
              for (std::size_t i = 0; i < queue.max_size() / nr_producers; ++i)
              {
                queue.push(
                  [&jobsExecuted]
                  {
                    ++jobsExecuted;
                  });
              }
            });
        }
        // start consumer
        std::thread consumer(
          [&queue, &producers]
          {
            while (std::any_of(std::begin(producers), std::end(producers),
                               [](const auto& p)
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
      }
      REQUIRE(jobsExecuted.load() == queue.max_size());
      REQUIRE(notifyCounter.load() == queue.max_size());
    }
  }
}

SCENARIO("queue: standard algorithms")
{
  GIVEN("queue with capacity of 1M")
  {
    static constexpr auto queue_capacity = 1 << 20;
    auto                  queue          = threadable::queue<queue_capacity>{};
    REQUIRE(queue.size() == 0);

    std::atomic_size_t jobsExecuted;
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
      AND_WHEN("std::for_each")
      {
        std::for_each(queue.begin(), queue.end(),
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
#ifdef __cpp_lib_execution
      AND_WHEN("std::for_each (parallel)")
      {
        std::for_each(std::execution::par, queue.begin(), queue.end(),
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
#endif
    }
  }
}
