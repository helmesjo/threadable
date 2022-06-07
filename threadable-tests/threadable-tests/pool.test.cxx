#include <threadable/pool.hxx>
#include <threadable-tests/doctest_include.hxx>

#include <cstddef>
#include <thread>
#include <vector>

SCENARIO("pool: create/remove queues")
{
  auto pool = threadable::pool(1);
  GIVEN("queue is created")
  {
    auto queue = pool.create();
    int called = 0;
    WHEN("job is pushed")
    {
      auto token = queue->push([&called]{ ++called; });
      THEN("it gets executed")
      {
        token.wait();
        REQUIRE(called == 1);
      }
    }
    THEN("queue can be removed")
    {
      REQUIRE(pool.remove(*queue));
    }
  }
  GIVEN("queue is pre-created")
  {
    auto queue = std::make_shared<decltype(pool)::queue_t>();
    WHEN("added (without jobs) to pool")
    {
      REQUIRE(pool.add(queue));
      AND_WHEN("job is pushed")
      {
        int called = 0;
        auto token2 = queue->push([&called]{ ++called; });
        THEN("it gets executed")
        {
          token2.wait();
          REQUIRE(called == 1);
        }
      }
      AND_WHEN("added to pool again")
      {
        THEN("it's not added")
        {
          REQUIRE_FALSE(pool.add(queue));
        }
      }
      AND_WHEN("removed from pool")
      {
        REQUIRE(pool.remove(*queue));
        THEN("it can be readded")
        {
          REQUIRE(pool.add(queue));
        }
      }
    }
    WHEN("added (with jobs) to pool")
    {
      THEN("existing jobs are executed")
      {
        int called = 0;
        auto token = queue->push([&called]{ ++called; });
        REQUIRE(pool.add(queue));
        token.wait();
        REQUIRE(called == 1);
      }
      AND_WHEN("removed from pool")
      {
        int called = 0;
        std::atomic_int latch{2};
        auto token1 = queue->push([&called, &latch]{ --latch; while(latch != 0); ++called; });
        auto token2 = queue->push([&called]{ ++called; });
        REQUIRE(pool.add(queue));
        while(latch > 1);

        pool.remove(*queue);
        REQUIRE(pool.size() == 0);

        latch = 0;
        token1.wait();
        THEN("pending jobs are not executed")
        {
          std::this_thread::sleep_for(std::chrono::milliseconds{5});
          REQUIRE_FALSE(token2.done());
        }
      }
    }
  }
}

SCENARIO("pool: stress-test")
{
  static constexpr std::size_t nr_of_jobs = 1 << 16;
  static constexpr std::size_t nr_of_threads = 4;
  auto pool = threadable::pool<nr_of_jobs>(nr_of_threads);
  GIVEN("pool with multiple threads")
  {
    WHEN("a large amount of jobs are pushed")
    {
      std::atomic_size_t counter{0};
      std::vector<threadable::job_token> tokens;
      tokens.reserve(nr_of_jobs);

      for(std::size_t i = 0; i < nr_of_jobs; ++i)
      {
        tokens.push_back(pool.push([&counter]{ ++counter; }));
      }

      for(const auto& token : tokens)
      {
        token.wait();
      }

      THEN("all gets executed")
      {
        REQUIRE(counter.load() == nr_of_jobs);
      }
    }
  }
}