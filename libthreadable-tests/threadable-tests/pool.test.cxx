#include <threadable-tests/doctest_include.hxx>
#include <threadable/pool.hxx>

#include <cstddef>
#include <latch>
#include <thread>
#include <vector>

SCENARIO("pool: print system info")
{
  std::cerr << "hardware_concurrency: " << std::thread::hardware_concurrency() << std::endl;
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
}
