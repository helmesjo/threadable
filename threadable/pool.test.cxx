#include <threadable/pool.hxx>
#include <threadable/doctest_include.hxx>

#include <cstddef>
#include <thread>
#include <vector>

SCENARIO("pool: create/remove queues")
{
  auto threadPool = threadable::pool(1);
  GIVEN("queue is created")
  {
    auto queue = threadPool.create();
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
    WHEN("queue is removed")
    {
      REQUIRE(threadPool.remove(*queue));
      AND_WHEN("job is pushed")
      {
        (void)queue->push([&called]{ ++called; });
        THEN("it is not executed")
        {
          REQUIRE(threadPool.size() == 0);
          REQUIRE(called == 0);
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