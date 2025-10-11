#include <threadable-tests/doctest_include.hxx>
#include <threadable/async.hxx>

#include <cstddef>
#include <thread>
#include <vector>

SCENARIO("async: submit tasks")
{
  constexpr auto nr_of_tasks = std::size_t{1024};
  auto           executed    = std::vector<std::size_t>(nr_of_tasks, 0);
  GIVEN("tasks are submitted")
  {
    auto counter = std::atomic_size_t{0};
    auto tokens  = fho::token_group{nr_of_tasks};
    for (std::size_t i = 0; i < nr_of_tasks; ++i)
    {
      tokens += fho::async(
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
    auto counter = std::size_t{0};
    auto token   = fho::slot_token{};
    fho::repeat_async(
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

SCENARIO("execute: execute range of tasks")
{
  constexpr auto nr_of_tasks = std::size_t{1024};

  auto executed = std::vector<std::size_t>(nr_of_tasks, 0);

  auto task = [](decltype(executed)& executed, std::size_t& i, size_t& counter)
  {
    executed[i++] = counter++;
    // simulate interruptions
    if (i % 2 == 0)
    {
      std::this_thread::yield();
    }
  };
  using task_t = decltype(task);

  auto tasks = std::vector<task_t>(nr_of_tasks);

  GIVEN("tasks are executed on the sequential queue")
  {
    std::ranges::fill(executed, 0);
    auto tokens  = fho::token_group{nr_of_tasks};
    auto counter = std::size_t{0};
    auto index   = std::size_t{0};

    fho::execute(fho::execution::seq, tasks, std::ref(executed), std::ref(index),
                 std::ref(counter));

    THEN("all tasks are executed in order")
    {
      REQUIRE(executed.size() == nr_of_tasks);
      for (std::size_t i = 0; i < nr_of_tasks; ++i)
      {
        REQUIRE(executed[i] == i);
      }
    }
  }
  GIVEN("tasks are executed on the parallel queue")
  {
    std::ranges::fill(executed, 0);
    auto tokens  = fho::token_group{nr_of_tasks};
    auto counter = std::size_t{0};
    auto index   = std::size_t{0};

    fho::execute(fho::execution::par, tasks, std::ref(executed), std::ref(index),
                 std::ref(counter));

    THEN("all tasks are executed")
    {
      REQUIRE(executed.size() == nr_of_tasks);
    }
  }
}
