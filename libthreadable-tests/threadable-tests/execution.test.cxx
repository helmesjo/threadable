#include <threadable-tests/doctest_include.hxx>
#include <threadable/execution.hxx>
#include <threadable/ring_buffer.hxx>

#include <atomic>

#include "threadable/schedulers/stealing.hxx"

SCENARIO("executor: Submit callables")
{
  auto exec = fho::executor();
  GIVEN("a range of callables")
  {
    static constexpr auto size = std::size_t{1024};

    auto items    = std::vector<std::function<void()>>{};
    auto executed = std::vector<std::size_t>(size, 0);
    auto tokens   = fho::token_group{};
    auto counter  = std::atomic_size_t{0};

    for (std::size_t i = 0; i < size; ++i)
    {
      items.emplace_back(
        [i, &executed, &counter]()
        {
          executed[i] = counter++;
        });
      // simulate interruptions
      if (i % 2 == 0)
      {
        tokens += exec.submit(items, fho::execution::seq);
        items.clear();
        std::this_thread::yield();
      }
    }
    WHEN("submitted to the executor")
    {
      tokens.wait();
      THEN("all tasks are executed in order")
      {
        REQUIRE(executed.size() == size);
        for (std::size_t i = 0; i < items.size(); ++i)
        {
          REQUIRE(executed[i] == i);
        }
      }
    }
  }
}

SCENARIO("executor v2: Submit callables")
{
  namespace sched = fho::schedulers::stealing;

  auto activity = sched::activity_stats{};
  auto master   = fho::ring_buffer<>{};
  auto order    = std::vector<bool>{};

  auto stealer = [&master]() -> auto
  {
    return master.try_pop_back();
  };

  auto exec1 = fho::v2::executor(activity, stealer);
  auto exec2 = fho::v2::executor(activity, stealer);
  auto exec3 = fho::v2::executor(activity, stealer);
  auto exec4 = fho::v2::executor(activity, stealer);
  GIVEN("a range of callables")
  {
    static constexpr auto size = std::size_t{1024};

    auto executed = std::vector<std::size_t>(size, 0);
    auto tokens   = fho::token_group{};
    auto counter  = std::atomic_size_t{0};

    for (std::size_t i = 0; i < size; ++i)
    {
      tokens += master.emplace_back(
        [i, &executed, &counter]()
        {
          executed[i] = counter++;
        });
      // simulate interruptions
      activity.ready.store(true, std::memory_order_release);
      activity.ready.notify_one();
      if (i % 2 == 0)
      {
        std::this_thread::yield();
      }
    }

    WHEN("submitted to the executor")
    {
      tokens.wait();
      THEN("all tasks are executed")
      {
        REQUIRE(executed.size() == size);
      }
    }
  }
}
