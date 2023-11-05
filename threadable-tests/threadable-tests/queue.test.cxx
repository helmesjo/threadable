#include <threadable/queue.hxx>
#include <threadable-tests/doctest_include.hxx>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#if __has_include(<execution>)
#include <execution>
#endif
#include <functional>
#include <type_traits>
#include <thread>
#if __has_include (<pstld/pstld.h>)
    #include <pstld/pstld.h>
#endif

SCENARIO("queue: push & claim")
{
  GIVEN("queue with capacity 2")
  {
    auto queue = threadable::queue<2>{};
    REQUIRE(queue.size() == 0);

    WHEN("push")
    {
      queue.push([]{});
      REQUIRE(queue.size() == 1);

      AND_WHEN("iterate without executing jobs")
      {
        auto nrJobs = 0;
        for(auto& job : queue)
        {
          REQUIRE(job);
          ++nrJobs;
        }
        THEN("1 valid job existed")
        {
          REQUIRE(nrJobs == 1);

          AND_WHEN("iterate without executing jobs")
          {
            nrJobs = 0;
            for(auto& job : queue)
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
        for(auto& job : queue)
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
            for(auto& job : queue)
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
    WHEN("push callable expecting 'const job&' as first parameter")
    {
      bool wasActive = false;
      queue.push([&wasActive](const threadable::job& job){
        wasActive = !job.done();
      });
      REQUIRE(queue.size() == 1);
      THEN("the job will be passed when the job is executed")
      {
        REQUIRE(queue.execute() == 1);
        REQUIRE(wasActive);
      }
    }
    WHEN("push_slow")
    {
      THEN("a callable that is too large can be pushed")
      {
        int called = 0;
        static constexpr auto too_big = threadable::details::cache_line_size * 2;
        // both capturing big data & passing as argument
        queue.push_slow([&called, bigData = std::make_shared<std::uint8_t[]>(too_big)](const threadable::job&, int arg, const std::shared_ptr<std::uint8_t[]>& data){
          called = arg;
          (void)data;
        }, 16, std::make_shared<std::uint8_t[]>(too_big));

        REQUIRE(queue.size() == 1);
        REQUIRE(queue.execute() == 1);
        REQUIRE(called == 16);
      }
    }
  }
  GIVEN("queue with capacity 128")
  {
    static constexpr auto queue_capacity = 128;
    auto queue = threadable::queue<queue_capacity>{};
    REQUIRE(queue.size() == 0);

    std::vector<std::size_t> jobs_executed;
    WHEN("push all")
    {
      for(std::size_t i = 0; i < queue.max_size(); ++i)
      {
        queue.push([i, &jobs_executed]{
          jobs_executed.push_back(i);
        });
      }
      REQUIRE(queue.size() == queue_capacity);

      AND_WHEN("iterate and execute jobs")
      {
        for(auto& job : queue)
        {
          REQUIRE(job);
          job();
        }
        THEN("128 jobs were executed")
        {
          REQUIRE(jobs_executed.size() == queue_capacity);
          for(std::size_t i = 0; i < queue.max_size(); ++i)
          {
            REQUIRE(jobs_executed[i] == i);
          }

          AND_THEN("queue is empty")
          {
            REQUIRE(queue.size() == 0);
          }

          AND_WHEN("iterate without executing jobs")
          {
            jobs_executed.clear();
            for(auto& job : queue)
            {
              (void)job;
              jobs_executed.push_back(0);
            }
            THEN("0 jobs were executed")
            {
              REQUIRE(jobs_executed.size() == 0);
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
    constexpr auto nr_jobs = 32;
    auto queue = threadable::queue<nr_jobs>(threadable::execution_policy::sequential);

    auto order = std::vector<int>{};
    auto m = std::mutex{};
    for(auto i = 0; i < nr_jobs; ++i)
    {
      queue.push([&order, &m, i]{
        auto _ = std::scoped_lock{m};
        order.push_back(i);
      });
    }
    WHEN("queue & execute jobs")
    {
      REQUIRE(queue.execute() == nr_jobs);
      THEN("jobs are executed FIFO")
      {
        REQUIRE(order.size() == nr_jobs);
        for(auto i = 0; i < nr_jobs; ++i)
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
    auto token = queue.push([]{});
    THEN("token is NOT done when job is discarded")
    {
      for(auto& job : queue){ (void)job; /* discard job */}
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
      REQUIRE(token.cancel());
      THEN("it is marked as 'done'")
      {
        REQUIRE(token.done());
      }
      THEN("job is still executed by queue")
      {
        // TODO: Fix so it's not executed. See queue::end().
        REQUIRE(queue.execute() == 1);
      }
    }
  }
}

SCENARIO("queue: stress-test")
{
  static constexpr std::size_t queue_capacity = 1 << 8;
  std::atomic_size_t notify_counter = 0;
  auto queue = threadable::queue<queue_capacity>();
  queue.set_notify([&notify_counter](...){ ++notify_counter; });

  GIVEN("produce & consume enough for wrap-around")
  {
    static constexpr auto nr_of_jobs = queue.max_size() * 2;
    std::size_t jobs_executed = 0;

    for(std::size_t i = 0; i < nr_of_jobs; ++i)
    {
      auto token = queue.push([]{});
      REQUIRE(queue.execute() == 1);
      ++jobs_executed;
      REQUIRE(token.done());
    }

    THEN("all jobs are executed")
    {
      REQUIRE(jobs_executed == nr_of_jobs);
    }
  }
  GIVEN("1 producer & 1 consumer")
  {
    THEN("there are no race conditions")
    {
      std::atomic_size_t jobs_executed{0};
      // pre-fill half
      for(std::size_t i = 0; i < queue_capacity/2; ++i)
      {
        queue.push([&jobs_executed]{ ++jobs_executed; });
      }
      {
        // start producer
        std::thread producer([&queue, &jobs_executed]{
          // push remaining half
          for(std::size_t i = 0; i < queue_capacity/2; ++i)
          {
            queue.push([&jobs_executed]{ ++jobs_executed; });
          }
        });
        // start consumer
        std::thread consumer([&queue, &producer]{
          while(producer.joinable() || !queue.empty())
          {
            queue.execute();
          }
        });

        producer.join();
        consumer.join();
      }
      REQUIRE(jobs_executed.load() == queue_capacity);
      REQUIRE(notify_counter.load() == queue_capacity);
    }
  }
}

SCENARIO("queue: standard algorithms")
{
  GIVEN("queue with capacity of 1M")
  {
    static constexpr auto queue_capacity = 1 << 20;
    auto queue = threadable::queue<queue_capacity>{};
    REQUIRE(queue.size() == 0);

    std::atomic_size_t jobs_executed;
    WHEN("push all")
    {
      while(queue.size() < queue.max_size())
      {
        queue.push([&jobs_executed]{
          ++jobs_executed;
        });
      }
      AND_WHEN("std::for_each")
      {
        std::for_each(queue.begin(), queue.end(), [](auto& job) {
          job();
        });
        THEN("all jobs executed")
        {
          REQUIRE(jobs_executed.load() == queue_capacity);
          REQUIRE(queue.size() == 0);
        }
      }
#ifdef __cpp_lib_execution
      AND_WHEN("std::for_each (parallel)")
      {
        std::for_each(std::execution::par, queue.begin(), queue.end(), [](auto& job) {
          job();
        });
        THEN("all jobs executed")
        {
          REQUIRE(jobs_executed.load() == queue_capacity);
          REQUIRE(queue.size() == 0);
        }
      }
#endif
    }
  }
}
