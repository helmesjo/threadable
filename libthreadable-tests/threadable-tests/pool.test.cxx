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
  auto pool = fho::pool<8>();
  GIVEN("queue is created")
  {
    auto& queue = pool.create();
    static_assert(fho::pool<8>::max_size() == decltype(pool)::queue_t::max_size());

    WHEN("task is emplaced")
    {
      int  called = 0;
      auto token  = queue.emplace_back(
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
  }
  GIVEN("queue is pre-created")
  {
    auto queue = decltype(pool)::queue_t();
    WHEN("added (without tasks) to pool")
    {
      auto& q = pool.add(std::move(queue));
      AND_WHEN("task is emplaced")
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
    WHEN("added (with tasks) to pool")
    {
      int  called = 0;
      auto token  = queue.emplace_back(
        [&called]
        {
          ++called;
        });
      (void)pool.add(std::move(queue));
      THEN("existing tasks are executed")
      {
        token.wait();
        REQUIRE(called == 1);
      }
    }
  }
}

SCENARIO("pool: stress-test")
{
  constexpr auto capacity     = std::size_t{1 << 16};
  constexpr auto nr_producers = std::size_t{4};

  auto pool = fho::pool<capacity>(nr_producers);

  GIVEN("multiple producers submit a large amount of tasks")
  {
    constexpr auto nr_producers = 4;

    auto& queue     = pool.create();
    auto  counter   = std::atomic_size_t{0};
    auto  producers = std::vector<std::thread>{};

    for (std::size_t i = 0; i < nr_producers; ++i)
    {
      producers.emplace_back(
        [&counter, &queue]
        {
          static_assert(decltype(pool)::max_size() % nr_producers == 0,
                        "All tasks must be submitted");
          constexpr auto nr_of_tasks = queue.max_size() / nr_producers;

          auto tokens = fho::token_group{nr_of_tasks};
          for (std::size_t j = 0; j < nr_of_tasks; ++j)
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
  GIVEN("multiple producers submit a large amount of tasks to their own queue and then remove it")
  {
    static constexpr auto nr_producers = 4;

    auto counter   = std::atomic_size_t{0};
    auto producers = std::vector<std::thread>{};
    auto barrier   = std::barrier{nr_producers};

    for (std::size_t i = 0; i < nr_producers; ++i)
    {
      producers.emplace_back(
        [&counter, &pool, &barrier, &queue = pool.create()]
        {
          static_assert(decltype(pool)::max_size() % nr_producers == 0,
                        "All tasks must be submitted");

          constexpr auto nr_of_tasks = queue.max_size() / nr_producers;

          auto tokens = fho::token_group{nr_of_tasks};
          barrier.arrive_and_wait();
          for (std::size_t j = 0; j < nr_of_tasks; ++j)
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
    REQUIRE(counter.load() == decltype(pool)::max_size());
  }
}
