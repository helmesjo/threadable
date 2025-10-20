#include <threadable-tests/doctest_include.hxx>
#include <threadable/pool.hxx>

#include <cstddef>
#include <latch>
#include <thread>
#include <vector>

#include "threadable/execution.hxx"

SCENARIO("pool: print system info")
{
  std::cerr << "hardware_concurrency: " << std::thread::hardware_concurrency() << std::endl;
}

SCENARIO("pool: create/remove queues")
{
  auto pool = fho::pool(4);
  GIVEN("queue is created")
  {
    auto queue = pool.make();

    WHEN("task is pushed to queue")
    {
      int  called = 0;
      auto token  = queue.push(
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
    WHEN("sequential tasks are pushed to queue")
    {
      constexpr auto nr_of_tasks = std::size_t{1024};

      auto executed = std::vector<std::size_t>(nr_of_tasks, 0);

      auto tokens  = fho::token_group{nr_of_tasks};
      auto counter = std::size_t{0};

      for (std::size_t i = 0; i < nr_of_tasks; ++i)
      {
        tokens += queue.push(fho::execution::seq,
                             [&executed, &counter, i]()
                             {
                               executed[i] = counter++;
                               // simulate interruptions
                               if (i % 2 == 0)
                               {
                                 std::this_thread::yield();
                               }
                             });
      }
      tokens.wait();

      THEN("all tasks are executed in order")
      {
        REQUIRE(executed.size() == nr_of_tasks);
        for (std::size_t i = 0; i < nr_of_tasks; ++i)
        {
          REQUIRE(executed[i] == i);
        }
      }
    }
  }
}

SCENARIO("pool: stress-test")
{
  constexpr auto nr_producers = std::size_t{4};

  auto pool = fho::pool(nr_producers);
  REQUIRE(pool.thread_count() == nr_producers);

  GIVEN("multiple producers submit a large amount of tasks")
  {
    auto counter   = std::atomic_size_t{0};
    auto producers = std::vector<std::thread>{};
    auto latch     = std::latch{nr_producers};

    for (std::size_t i = 0; i < nr_producers; ++i)
    {
      producers.emplace_back(
        [&counter, &pool, &latch]
        {
          static_assert(decltype(pool)::max_size() % nr_producers == 0,
                        "All tasks must be submitted");
          constexpr auto nr_of_tasks = decltype(pool)::max_size() / nr_producers;

          auto tokens = fho::token_group{nr_of_tasks};
          latch.arrive_and_wait();
          for (std::size_t j = 0; j < nr_of_tasks; ++j)
          {
            tokens += pool.push(
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
      REQUIRE(counter.load() == pool.max_size());
    }
  }
  GIVEN("multiple producers submit a large amount of tasks to their own queue and then remove it")
  {
    static constexpr auto nr_producers = 4;

    auto counter   = std::atomic_size_t{0};
    auto producers = std::vector<std::thread>{};
    auto latch     = std::latch{nr_producers};

    for (std::size_t i = 0; i < nr_producers; ++i)
    {
      producers.emplace_back(
        [&counter, &latch, queue = pool.make()]() mutable
        {
          static_assert(decltype(pool)::max_size() % nr_producers == 0,
                        "All tasks must be submitted");

          auto tokens = fho::token_group{};
          latch.arrive_and_wait();
          for (std::size_t j = 0; j < queue.max_size() / nr_producers; ++j)
          {
            tokens += queue.push(
              [&counter]
              {
                ++counter;
              });
          }
          tokens.wait();
          queue = {}; // release/remove
        });
    }

    for (auto& thread : producers)
    {
      thread.join();
    }

    THEN("all gets executed")
    {
      REQUIRE(counter.load() == pool.max_size());
    }
  }
}
