#include <threadable/pool.hxx>
#include <threadable-tests/doctest_include.hxx>

#include <cstddef>
#include <thread>
#include <vector>

SCENARIO("pool: create/remove queues")
{
  auto pool = threadable::pool();
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
      int called = 0;
      auto token = queue->push([&called]{ ++called; });
      REQUIRE(pool.add(queue));
      THEN("existing jobs are executed")
      {
        token.wait();
        REQUIRE(called == 1);
      }
    }
  }
}

SCENARIO("pool: stress-test")
{
  static constexpr std::size_t nr_of_jobs = 1 << 13;
  auto pool = threadable::pool<nr_of_jobs>();
  GIVEN("pool with multiple threads")
  {
    WHEN("a large amount of jobs are pushed")
    {
      std::atomic_size_t counter{0};
      std::vector<threadable::job_token> tokens;
      tokens.reserve(nr_of_jobs);

      auto queue = pool.create();
      for(std::size_t i = 0; i < nr_of_jobs; ++i)
      {
        tokens.emplace_back(queue->push([&counter]{ ++counter; }));
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
