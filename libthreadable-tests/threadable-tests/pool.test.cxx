#include <threadable-tests/doctest_include.hxx>
#include <threadable/pool.hxx>

#include <chrono>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

SCENARIO("pool: create/remove queues")
{
  auto pool = threadable::pool();
  GIVEN("queue is created")
  {
    auto& queue  = pool.create();
    int   called = 0;
    WHEN("job is pushed")
    {
      auto token = queue.push(
        [&called]
        {
          ++called;
        });
      THEN("it gets executed")
      {
        token.wait();
        REQUIRE(called == 1);
      }
    }
    THEN("queue can be removed")
    {
      REQUIRE(pool.remove(std::move(queue)));
    }
    WHEN("queue is removed inside a job")
    {
      auto token = queue.push(
        [&pool, &queue]
        {
          pool.remove(std::move(queue));
        });
      THEN("it gets removed")
      {
        token.wait();
        REQUIRE(pool.queues() == 0);
      }
    }
  }
  GIVEN("queue is pre-created")
  {
    auto queue = std::make_unique<decltype(pool)::queue_t>();
    WHEN("added (without jobs) to pool")
    {
      auto& q = pool.add(std::move(queue));
      AND_WHEN("job is pushed")
      {
        int  called = 0;
        auto token2 = q.push(
          [&called]
          {
            ++called;
          });
        THEN("it gets executed")
        {
          token2.wait();
          REQUIRE(called == 1);
        }
      }
      AND_WHEN("removed from pool")
      {
        REQUIRE(pool.remove(std::move(q)));
      }
    }
    WHEN("added (with jobs) to pool")
    {
      int  called = 0;
      auto token  = queue->push(
        [&called]
        {
          ++called;
        });
      (void)pool.add(std::move(queue));
      THEN("existing jobs are executed")
      {
        token.wait();
        REQUIRE(called == 1);
      }
    }
  }
}

SCENARIO("pool: push jobs to global pool")
{
  constexpr std::size_t    nr_of_jobs = 1024;
  std::mutex               mutex;
  std::vector<std::size_t> results;
  GIVEN("a job is pushed to sequential queue")
  {
    threadable::job_token token;
    for (std::size_t i = 0; i < nr_of_jobs; ++i)
    {
      token = threadable::push<threadable::execution_policy::sequential>(
        [i, &results, &mutex]
        {
          std::scoped_lock _{mutex};
          results.push_back(i);
        });
    }
    token.wait();
    THEN("all jobs are executed in order")
    {
      REQUIRE(results.size() == nr_of_jobs);
      for (std::size_t i = 0; i < nr_of_jobs; ++i)
      {
        REQUIRE(results[i] == i);
      }
    }
  }
  GIVEN("a job is pushed to parallel queue")
  {
    std::atomic_size_t      counter = 0;
    threadable::token_group group;
    for (std::size_t i = 0; i < nr_of_jobs; ++i)
    {
      group += threadable::push<threadable::execution_policy::parallel>(
        [&counter]
        {
          ++counter;
        });
    }
    group.wait();
    THEN("all jobs are executed")
    {
      REQUIRE(counter == nr_of_jobs);
    }
  }
}

SCENARIO("pool: stress-test")
{
  constexpr auto capacity = std::size_t{1 << 18};
  auto           pool     = threadable::pool<capacity>();
  GIVEN("single producer pushes a large amount of jobs")
  {
    auto& queue   = pool.create(threadable::execution_policy::parallel);
    auto  counter = std::atomic_size_t{0};

    auto group = threadable::token_group{};
    for (std::size_t i = 0; i < queue.max_size(); ++i)
    {
      group += queue.push(
        [&counter]
        {
          ++counter;
        });
    }

    group.wait();

    THEN("all gets executed")
    {
      REQUIRE(counter.load() == queue.max_size());
    }
  }
  GIVEN("multiple producers pushes a large amount of jobs")
  {
    constexpr auto nr_producers = 3;

    auto& queue     = pool.create(threadable::execution_policy::parallel);
    auto  counter   = std::atomic_size_t{0};
    auto  producers = std::vector<std::thread>{};

    for (std::size_t i = 0; i < nr_producers; ++i)
    {
      producers.emplace_back(
        [&counter, &queue]
        {
          static_assert(decltype(pool)::queue_t::max_size() % nr_producers == 0,
                        "All jobs must be pushed");
          threadable::token_group group;
          for (std::size_t j = 0; j < queue.max_size() / nr_producers; ++j)
          {
            group += queue.push(
              [&counter]
              {
                ++counter;
              });
          }
          group.wait();
        });
    }

    for (auto& thread : producers)
    {
      thread.join();
    }

    THEN("all gets executed")
    {
      REQUIRE(counter.load() == queue.max_size());
    }
  }
  GIVEN("multiple producers pushes a large amount of jobs to their own queue and then remove it")
  {
    static constexpr auto    nr_producers = 3;
    std::atomic_size_t       counter{0};
    std::vector<std::thread> producers;

    for (std::size_t i = 0; i < nr_producers; ++i)
    {
      producers.emplace_back(
        [&counter, &pool, &queue = pool.create(threadable::execution_policy::parallel)]
        {
          static_assert(decltype(pool)::queue_t::max_size() % nr_producers == 0,
                        "All jobs must be pushed");
          for (std::size_t j = 0; j < queue.max_size() / nr_producers; ++j)
          {
            (void)queue.push(
              [&counter]
              {
                ++counter;
              });
          }
          pool.remove(std::move(queue));
        });
    }

    for (auto& thread : producers)
    {
      thread.join();
    }
  }
  GIVEN("multiple queues with varying-length jobs are pushed")
  {
    using namespace std::chrono;
    using clk_t = high_resolution_clock;

    auto  manualPool = threadable::pool<capacity>(false);
    auto& queue1     = manualPool.create(threadable::execution_policy::sequential);
    auto& queue2     = manualPool.create(threadable::execution_policy::sequential);

    static constexpr auto spin_wait = [](nanoseconds const& dur)
    {
      const auto start = clk_t::now();
      while (dur > clk_t::now() - start)
        ;
    };

    THEN("they are load balanced")
    {
      for (std::size_t i = 0; i < 2; ++i)
      {
        queue1.push(
          []
          {
            spin_wait(10ms);
          });
      }
      for (std::size_t i = 0; i < 4; ++i)
      {
        queue2.push(
          []
          {
            spin_wait(4ms);
          });
      }

      manualPool.execute();
      REQUIRE(queue1.size() == 0);
      REQUIRE(queue2.size() == 2);

      for (std::size_t i = 0; i < 2; ++i)
      {
        queue1.push(
          []
          {
            spin_wait(10ms);
          });
      }
      for (std::size_t i = 0; i < 2; ++i)
      {
        queue2.push(
          []
          {
            spin_wait(4ms);
          });
      }

      manualPool.execute();
      REQUIRE(queue1.size() == 1);
      REQUIRE(queue2.size() == 0);

      for (std::size_t i = 0; i < 1; ++i)
      {
        queue1.push(
          []
          {
            spin_wait(10ms);
          });
      }

      manualPool.execute();
      REQUIRE(queue1.size() == 1);
      REQUIRE(queue2.size() == 0);
    }
  }
}
