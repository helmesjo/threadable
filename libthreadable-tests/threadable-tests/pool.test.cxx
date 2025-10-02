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

// SCENARIO("pool: execution order")
// {
//   auto pool = fho::pool();
//   GIVEN("a task is submitted to a sequential queue")
//   {
//     auto executed = std::vector<std::size_t>(pool.max_size(), 0);
//     std::ranges::fill(executed, 0);
//     auto tokens  = fho::token_group{};
//     auto counter = std::atomic_size_t{0};
//     for (std::size_t i = 0; i < pool.max_size(); ++i)
//     {
//       tokens += pool.push<fho::execution::seq>(
//         [i, &executed, &counter]()
//         {
//           executed[i] = counter++;
//         });
//       // simulate interruptions
//       if (i % 2 == 0)
//       {
//         std::this_thread::yield();
//       }
//     }
//     REQUIRE(tokens.size() == pool.max_size());
//     tokens.wait();
//     THEN("all tasks are executed in order")
//     {
//       REQUIRE(executed.size() == pool.max_size());
//       for (std::size_t i = 0; i < pool.max_size(); ++i)
//       {
//         REQUIRE(executed[i] == i);
//       }
//     }
//   }
//   GIVEN("a task is submitted to a parallel queue")
//   {
//     auto counter = std::atomic_size_t{0};
//     auto tokens  = fho::token_group{};
//     for (std::size_t i = 0; i < pool.max_size(); ++i)
//     {
//       tokens += pool.push(
//         [&counter]
//         {
//           ++counter;
//         });
//     }
//     tokens.wait();
//     THEN("all tasks are executed")
//     {
//       REQUIRE(counter == pool.max_size());
//     }
//   }
// }

SCENARIO("pool: stress-test")
{
  constexpr auto nr_tasks = std::size_t{1 << 20};

  GIVEN("multiple producers submit a large amount of tasks")
  {
    constexpr auto nr_producers = 4;

    auto pool      = fho::pool(2);
    auto counter   = std::atomic_size_t{0};
    auto producers = std::vector<std::thread>{};
    auto barrier   = std::barrier{nr_producers};

    for (std::size_t i = 0; i < nr_producers; ++i)
    {
      producers.emplace_back(
        [&counter, &pool, &barrier]
        {
          static_assert(nr_tasks % nr_producers == 0, "All tasks must be submitted");
          auto tokens = fho::token_group{};
          barrier.arrive_and_wait();
          for (std::size_t j = 0; j < nr_tasks / nr_producers; ++j)
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
      REQUIRE(counter.load() == nr_tasks);
    }
  }
}
