#include <threadable-tests/doctest_include.hxx>
#include <threadable/pool.hxx>

#include <barrier>
#include <cstddef>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

SCENARIO("pool: create/remove queues")
{
  auto pool = fho::pool();
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
    // @TODO: Figure out how to test this (token.wait() on potentially removed job)
    // WHEN("queue is removed inside a job")
    // {
    //   auto token = queue.push(
    //     [&pool, &queue]
    //     {
    //       REQUIRE(pool.remove(std::move(queue)));
    //     });
    //   THEN("it gets removed")
    //   {
    //     token.wait();
    //     REQUIRE(pool.queues() == 0);
    //   }
    // }
  }
  GIVEN("queue is pre-created")
  {
    auto queue = decltype(pool)::queue_t();
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
      auto token  = queue.push(
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
  constexpr auto nr_of_jobs = std::size_t{1024};
  auto           mutex      = std::mutex{};
  auto           results    = std::vector<std::size_t>{};
  GIVEN("a job is pushed to sequential queue")
  {
    auto token = fho::job_token{};
    for (std::size_t i = 0; i < nr_of_jobs; ++i)
    {
      token = fho::push<fho::execution_policy::sequential>(
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
    auto counter = std::atomic_size_t{0};
    auto group   = fho::token_group{};
    for (std::size_t i = 0; i < nr_of_jobs; ++i)
    {
      group += fho::push<fho::execution_policy::parallel>(
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
  auto           pool     = fho::pool<capacity>();
  GIVEN("single producer pushes a large amount of jobs")
  {
    auto& queue   = pool.create(fho::execution_policy::parallel);
    auto  counter = std::atomic_size_t{0};

    auto group = fho::token_group{};
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

    auto& queue     = pool.create(fho::execution_policy::parallel);
    auto  counter   = std::atomic_size_t{0};
    auto  producers = std::vector<std::thread>{};

    for (std::size_t i = 0; i < nr_producers; ++i)
    {
      producers.emplace_back(
        [&counter, &queue]
        {
          static_assert(decltype(pool)::queue_t::max_size() % nr_producers == 0,
                        "All jobs must be pushed");
          fho::token_group group;
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
    static constexpr auto nr_producers = 3;

    auto counter   = std::atomic_size_t{0};
    auto producers = std::vector<std::thread>{};
    auto barrier   = std::barrier{nr_producers};

    for (std::size_t i = 0; i < nr_producers; ++i)
    {
      producers.emplace_back(
        [&counter, &pool, &barrier, &queue = pool.create(fho::execution_policy::parallel)]
        {
          static_assert(decltype(pool)::queue_t::max_size() % nr_producers == 0,
                        "All jobs must be pushed");

          auto tokens = fho::token_group{};
          barrier.arrive_and_wait();
          for (std::size_t j = 0; j < queue.max_size() / nr_producers; ++j)
          {
            tokens += queue.push(
              [&counter]
              {
                ++counter;
              });
          }
          tokens.wait();
          REQUIRE(pool.remove(std::move(queue)));
        });
    }

    for (auto& thread : producers)
    {
      thread.join();
    }
    REQUIRE(counter.load() == decltype(pool)::queue_t::max_size());
  }
}
