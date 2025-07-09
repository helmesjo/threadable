#include <threadable-tests/doctest_include.hxx>
#include <threadable/pool.hxx>

#include <barrier>
#include <cstddef>
#include <thread>
#include <vector>

SCENARIO("pool: print system info")
{
  std::cerr << "hardware_concurrency: " << std::thread::hardware_concurrency() << std::endl;
}

SCENARIO("pool: create/remove queues")
{
  auto pool = fho::pool();
  GIVEN("queue is created")
  {
    auto& queue  = pool.create();
    int   called = 0;
    WHEN("job is pushed")
    {
      auto token = queue.emplace_back(
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
    //   auto token = queue.emplace_back(
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
      auto& q = pool.add(std::move(queue), fho::execution::par);
      AND_WHEN("job is pushed")
      {
        int  called = 0;
        auto token2 = q.emplace_back(
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
      auto token  = queue.emplace_back(
        [&called]
        {
          ++called;
        });
      (void)pool.add(std::move(queue), fho::execution::par);
      THEN("existing jobs are executed")
      {
        token.wait();
        REQUIRE(called == 1);
      }
    }
  }
}

SCENARIO("pool: submit jobs to global pool")
{
  constexpr auto nr_of_jobs = std::size_t{1024};
  auto           executed   = std::vector<std::size_t>(nr_of_jobs, 0);
  GIVEN("a job is submitted to sequential queue")
  {
    std::ranges::fill(executed, 0);
    auto tokens  = fho::token_group{};
    auto counter = std::atomic_size_t{0};
    for (std::size_t i = 0; i < nr_of_jobs; ++i)
    {
      tokens += fho::async<fho::execution::seq>(
        [i, &executed, &counter]
        {
          executed[i] = counter++;
        });
      // simulate interruptions
      if (i % 2 == 0)
      {
        std::this_thread::yield();
      }
    }
    tokens.wait();
    THEN("all jobs are executed in order")
    {
      REQUIRE(executed.size() == nr_of_jobs);
      for (std::size_t i = 0; i < nr_of_jobs; ++i)
      {
        REQUIRE(executed[i] == i);
      }
    }
  }
  GIVEN("a job is submitted to parallel queue")
  {
    auto counter = std::atomic_size_t{0};
    auto tokens  = fho::token_group{};
    for (std::size_t i = 0; i < nr_of_jobs; ++i)
    {
      tokens += fho::async<fho::execution::par>(
        [&counter]
        {
          ++counter;
        });
    }
    tokens.wait();
    THEN("all jobs are executed")
    {
      REQUIRE(counter == nr_of_jobs);
    }
  }
}

SCENARIO("pool: execution order")
{
  constexpr auto capacity = std::size_t{4};
  auto           pool     = fho::pool<capacity>();
  GIVEN("a job is submitted to sequential queue")
  {
    auto& queue    = pool.create(fho::execution::seq);
    auto  executed = std::vector<std::size_t>(queue.max_size(), 0);
    std::ranges::fill(executed, 0);
    auto tokens  = fho::token_group{};
    auto counter = std::atomic_size_t{0};
    for (std::size_t i = 0; i < queue.max_size(); ++i)
    {
      tokens += queue.emplace_back(
        [i, &executed, &counter]()
        {
          executed[i] = counter++;
        });
      // simulate interruptions
      if (i % 2 == 0)
      {
        std::this_thread::yield();
      }
    }
    REQUIRE(tokens.size() == queue.max_size());
    tokens.wait();
    THEN("all jobs are executed in order")
    {
      REQUIRE(executed.size() == queue.max_size());
      for (std::size_t i = 0; i < queue.max_size(); ++i)
      {
        REQUIRE(executed[i] == i);
      }
    }
    (void)pool.remove(std::move(queue));
  }
  GIVEN("a job is submitted to parallel queue")
  {
    auto counter = std::atomic_size_t{0};
    auto tokens  = fho::token_group{};
    for (std::size_t i = 0; i < capacity; ++i)
    {
      tokens += fho::async<fho::execution::par>(
        [&counter]
        {
          ++counter;
        });
    }
    tokens.wait();
    THEN("all jobs are executed")
    {
      REQUIRE(counter == capacity);
    }
  }
}

SCENARIO("pool: stress-test")
{
  constexpr auto capacity = std::size_t{1 << 16};
  auto           pool     = fho::pool<capacity>();
  GIVEN("multiple producers submit a large amount of jobs")
  {
    constexpr auto nr_producers = 5;

    auto& queue     = pool.create(fho::execution::par);
    auto  counter   = std::atomic_size_t{0};
    auto  producers = std::vector<std::thread>{};

    for (std::size_t i = 0; i < nr_producers; ++i)
    {
      producers.emplace_back(
        [&counter, &queue]
        {
          static_assert(decltype(pool)::queue_t::max_size() % nr_producers == 0,
                        "All jobs must be submitted");
          auto tokens = fho::token_group{};
          for (std::size_t j = 0; j < queue.max_size() / nr_producers; ++j)
          {
            tokens += queue.emplace_back(
              [&counter]
              {
                ++counter;
              });
          }
          tokens.wait();
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
  GIVEN("multiple producers submit a large amount of jobs to their own queue and then remove it")
  {
    static constexpr auto nr_producers = 3;

    auto counter   = std::atomic_size_t{0};
    auto producers = std::vector<std::thread>{};
    auto barrier   = std::barrier{nr_producers};

    for (std::size_t i = 0; i < nr_producers; ++i)
    {
      producers.emplace_back(
        [&counter, &pool, &barrier, &queue = pool.create(fho::execution::par)]
        {
          static_assert(decltype(pool)::queue_t::max_size() % nr_producers == 0,
                        "All jobs must be submitted");

          auto tokens = fho::token_group{};
          barrier.arrive_and_wait();
          for (std::size_t j = 0; j < queue.max_size() / nr_producers; ++j)
          {
            tokens += queue.emplace_back(
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
