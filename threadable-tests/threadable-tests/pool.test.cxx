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
    auto& queue = pool.create();
    int called = 0;
    WHEN("job is pushed")
    {
      auto token = queue.push([&called]{ ++called; });
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
        REQUIRE(pool.remove(std::move(*queue)));
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

SCENARIO("pool: push jobs")
{
  auto pool = threadable::pool();
  GIVEN("a job is pushed")
  {
    auto token = pool.push([]{});
    THEN("it gets executed")
    {
      token.wait();
      REQUIRE(token.done());
    }
  }
}

SCENARIO("pool: stress-test")
{
  static constexpr std::size_t nr_of_jobs = 1 << 18;
  auto pool = threadable::pool<nr_of_jobs>();
  GIVEN("pool with multiple threads")
  {
    WHEN("single producer pushes a large amount of jobs")
    {
      std::atomic_size_t counter{0};
      std::vector<threadable::job_token> tokens;
      tokens.reserve(nr_of_jobs);

      for(std::size_t i = 0; i < nr_of_jobs; ++i)
      {
        tokens.emplace_back(pool.push([&counter]{ ++counter; }));
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
    WHEN("multiple producers pushes a large amount of jobs")
    {
      const auto nr_producers = std::thread::hardware_concurrency() * 2;
      std::atomic_size_t counter{0};
      std::vector<std::thread> producers;

      for(std::size_t i = 0; i < nr_producers; ++i)
      {
        producers.emplace_back(std::thread([&pool, &counter, &nr_producers]{
          for(std::size_t j = 0; j < nr_of_jobs/nr_producers; ++j)
          {
            (void)pool.push([&counter]{ ++counter; });
          }
        }));
      }

      for(auto& thread : producers)
      {
        thread.join();
      }

      pool.wait();

      THEN("all gets executed")
      {
        REQUIRE(counter.load() == nr_of_jobs);
      }
    }
  }
}
