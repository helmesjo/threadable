#include <threadable-tests/doctest_include.hxx>
#include <threadable/async.hxx>

#include <cstddef>
#include <thread>
#include <vector>

SCENARIO("async: submit tasks")
{
  constexpr auto nr_of_tasks = std::size_t{1024};
  auto           executed    = std::vector<std::size_t>(nr_of_tasks, 0);
  GIVEN("tasks are submitted to the sequential queue")
  {
    std::ranges::fill(executed, 0);
    auto tokens  = fho::token_group{};
    auto counter = 0;
    for (std::size_t i = 0; i < nr_of_tasks; ++i)
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
    THEN("all tasks are executed in order")
    {
      REQUIRE(executed.size() == nr_of_tasks);
      for (std::size_t i = 0; i < nr_of_tasks; ++i)
      {
        REQUIRE(executed[i] == i);
      }
    }
  }
  GIVEN("tasks are submitted to the parallel queue")
  {
    auto counter = std::atomic_size_t{0};
    auto tokens  = fho::token_group{};
    for (std::size_t i = 0; i < nr_of_tasks; ++i)
    {
      tokens += fho::async<fho::execution::par>(
        [&counter]
        {
          ++counter;
        });
    }
    tokens.wait();
    THEN("all tasks are executed")
    {
      REQUIRE(counter.load() == nr_of_tasks);
    }
  }
  GIVEN("a repeated task is submitted")
  {
    auto counter = 0;
    auto token   = fho::slot_token{};
    fho::repeat_async<fho::execution::seq>(
      token,
      [&counter](fho::slot_token& token) mutable
      {
        if (++counter >= nr_of_tasks)
        {
          token.cancel();
        }
        // simulate interruptions
        if (counter % 2 == 0)
        {
          std::this_thread::yield();
        }
      },
      std::ref(token));

    token.wait();

    REQUIRE(token.done());
    REQUIRE(token.cancelled());
    THEN("it re-submits automatically")
    {
      REQUIRE(counter == nr_of_tasks);
    }
  }
}
