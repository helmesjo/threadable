#include <threadable-tests/doctest_include.hxx>
#include <threadable/execution.hxx>
#include <threadable/ring_buffer.hxx>
#include <threadable/scheduler/stealing.hxx>

#include <atomic>

SCENARIO("executor: Submit callables")
{
  namespace sched = fho::scheduler::stealing;

  auto activity = sched::activity_stats{};
  auto master   = fho::ring_buffer<>{};
  auto order    = std::vector<bool>{};

  auto stealer = [&master](std::ranges::range auto&& r) -> typename decltype(master)::claimed_type
  {
    using cached_t = std::ranges::range_value_t<decltype(r)>;
    auto cached    = cached_t{nullptr};
    if (auto t = master.try_pop_back())
    {
      if (!cached)
      {
        cached = std::move(t);
      }
      else
      {
        r.emplace_back(std::move(t));
      }
    }
    return cached;
  };

  auto exec1 = fho::executor(activity, {}, stealer);
  auto exec2 = fho::executor(activity, {}, stealer);
  auto exec3 = fho::executor(activity, {}, stealer);
  auto exec4 = fho::executor(activity, {}, stealer);
  GIVEN("a range of callables")
  {
    static constexpr auto size = std::size_t{1024};

    auto executed = std::vector<std::size_t>(size, 0);
    auto tokens   = fho::token_group{size};
    auto counter  = std::atomic_size_t{0};

    for (std::size_t i = 0; i < size; ++i)
    {
      tokens += master.emplace_back(
        [i, &executed, &counter]()
        {
          executed[i] = counter++;
        });
      // simulate interruptions
      activity.notifier.notify_one();
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
  activity.stops = true;
  activity.notifier.notify_all();
}
