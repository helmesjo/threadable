#include <threadable-tests/doctest_include.hxx>
#include <threadable/execution.hxx>
#include <threadable/ring_buffer.hxx>
#include <threadable/schedulers/stealing.hxx>

#include <atomic>

SCENARIO("executor: Submit callables")
{
  namespace sched = fho::schedulers::stealing;

  auto activity = sched::activity_stats{};
  auto master   = fho::ring_buffer<>{};
  auto order    = std::vector<bool>{};

  auto stealer = [&master](std::ranges::range auto&& r) -> std::size_t
  {
    if (auto t = master.try_pop_back())
    {
      r.emplace_back(std::move(t));
      return 1;
    }
    return 0;
  };

  auto exec1 = fho::executor(activity, stealer);
  auto exec2 = fho::executor(activity, stealer);
  auto exec3 = fho::executor(activity, stealer);
  auto exec4 = fho::executor(activity, stealer);
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
      activity.ready.fetch_add(1, std::memory_order_release);
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
  activity.abort = true;
  activity.ready = 1;
  activity.ready.notify_all();
}
