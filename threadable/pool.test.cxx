#include <threadable/pool.hxx>
#include <threadable/doctest_include.hxx>

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
    WHEN("added to pool")
    {
      int called = 0;
      auto token = queue->push([&called]{ ++called; });
      pool.add(queue);
      token.wait();
      THEN("existing jobs are executed")
      {
        REQUIRE(called == 1);
      }
      called = 0; 
      AND_WHEN("job is pushed")
      {
        auto token2 = queue->push([&called]{ ++called; });
        THEN("it gets executed")
        {
          token2.wait();
          REQUIRE(called == 1);
        }
      }
      THEN("queue can be removed")
      {
        REQUIRE(pool.remove(*queue));
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