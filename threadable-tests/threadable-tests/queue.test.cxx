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

SCENARIO("queue2: push & claim")
{
  GIVEN("queue with capacity 2")
  {
    auto queue = threadable::queue2<2>{};
    REQUIRE(queue.size() == 0);

    WHEN("push")
    {
      queue.push([]{});
      REQUIRE(queue.size() == 1);

      AND_WHEN("iterate")
      {
        auto nrJobs = 0;
        for(auto& job : queue)
        {
          REQUIRE(job);
          ++nrJobs;
        }
        THEN("1 valid job exists")
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
            THEN("1 valid job still exists")
            {
              REQUIRE(nrJobs == 1);
            }
          }
        }
      }
    }
  }
  GIVEN("queue with capacity 128")
  {
    static constexpr auto queue_capacity = 128;
    auto queue = threadable::queue2<queue_capacity>{};
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

      AND_WHEN("iterate")
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

          AND_WHEN("iterate")
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

SCENARIO("queue2: execution order")
{
  auto queue = threadable::queue2{};

  std::vector<int> order;
  GIVEN("push three jobs")
  {
    queue.push([&order]{ order.push_back(1); });
    queue.push([&order]{ order.push_back(2); });
    queue.push([&order]{ order.push_back(3); });
    WHEN("execute jobs")
    {
      REQUIRE(queue.execute() == 3);
      THEN("jobs are executed FIFO")
      {
        REQUIRE(order.size() == 3);
        REQUIRE(order[0] == 1);
        REQUIRE(order[1] == 2);
        REQUIRE(order[2] == 3);
      }
    }
  }
}

SCENARIO("queue2: completion token")
{
  auto queue = threadable::queue2{};
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
    // WHEN("waiting on token")
    // {
    //   THEN("it releases after job has executed")
    //   {
    //     using clock_t = std::chrono::steady_clock;
    //     const auto start = clock_t::now();

    //     auto waiterDoneTime = clock_t::now();
    //     std::thread waiter([&token, &waiterDoneTime]{ 
    //       token.wait();
    //       waiterDoneTime = clock_t::now();
    //     });

    //     // Give thread some time to start up
    //     std::this_thread::sleep_for(std::chrono::milliseconds{10});

    //     REQUIRE(queue.execute() == 1);
    //     auto jobDoneTime = clock_t::now();
    //     waiter.join();

    //     INFO(
    //       std::chrono::duration_cast<std::chrono::nanoseconds>(jobDoneTime - start).count(),
    //       "us < ", 
    //       std::chrono::duration_cast<std::chrono::nanoseconds>(waiterDoneTime - start).count(),
    //       "us");
    //     REQUIRE(jobDoneTime <= waiterDoneTime);
    //   }
    // }
  }
  GIVEN("stress-test: 1 producer/consumer")
  {
    static constexpr auto nr_of_jobs = queue.max_size() * 2;
    std::size_t jobs_executed = 0;

    for(std::size_t i = 0; i < nr_of_jobs; ++i)
    {
      auto token = queue.push([]{});
      for(auto& job : queue)
      {
        if(job)
        {
          job();
        }
        ++jobs_executed;
      }
      token.wait();
    }

    REQUIRE(jobs_executed == nr_of_jobs);
  }
  GIVEN("stress-test: 1 producer 1 consumer")
  {
    static constexpr auto nr_of_jobs = queue.max_size() * 2;
    std::size_t jobs_executed = 0;
    std::atomic_bool run{true};
    auto worker = std::thread([&]{
      while(run)
      {
        for(auto& job : queue)
        {
          if(job)
          {
            job();
            ++jobs_executed;
          }
        }
      }
    });

    for(std::size_t i = 0; i < nr_of_jobs; ++i)
    {
      auto token = queue.push([]{});
      token.wait();
    }

    run = false;
    worker.join();
    REQUIRE(jobs_executed == nr_of_jobs);
  }
}

SCENARIO("queue2: stress-test")
{
  static constexpr std::size_t queue_capacity = 1 << 18;
  std::atomic_size_t notify_counter = 0;
  auto queue = threadable::queue2<queue_capacity>([&notify_counter](...){ ++notify_counter; });
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

SCENARIO("queue2: standard algorithms")
{
  GIVEN("queue with capacity 1 << 22")
  {
    static constexpr auto queue_capacity = 1 << 22;
    auto queue = threadable::queue2<queue_capacity>{};
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
#if __has_include(<execution>)
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