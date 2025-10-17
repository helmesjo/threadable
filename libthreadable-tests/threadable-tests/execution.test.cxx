#include <threadable-tests/doctest_include.hxx>
#include <threadable/execution.hxx>
#include <threadable/ring_buffer.hxx>
#include <threadable/scheduler/stealing.hxx>

#include <atomic>

namespace
{
  using ring_t    = fho::ring_buffer<fho::fast_func_t>;
  using claimed_t = typename ring_t::claimed_type;

  struct master_queue
  {
    ring_t queue;

    auto
    steal(std::ranges::range auto&& r, bool) -> claimed_t
    {
      using cached_t = std::ranges::range_value_t<decltype(r)>;
      auto cached    = cached_t{nullptr};
      if (auto t = queue.try_pop_back())
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
    }

    auto
    empty() const -> bool
    {
      return queue.empty(true);
    }
  };
}

SCENARIO("executor: Submit callables")
{
  namespace sched = fho::scheduler::stealing;

  auto activity = sched::activity_stats{};
  auto master   = master_queue{};
  auto order    = std::vector<bool>{};

  GIVEN("multiple executors")
  {
    static constexpr auto size = std::size_t{1024};

    auto executors    = std::vector<std::unique_ptr<fho::executor>>();
    auto defaultStats = fho::scheduler::stealing::exec_stats{};
    for (auto i = 0u; i < 4u; ++i)
    {
      auto& exec = executors.emplace_back(std::make_unique<fho::executor>(activity.notifier));
      exec->start(activity, defaultStats, master);
    }

    auto executed = std::vector<std::size_t>(size, 0);
    auto tokens   = fho::token_group{size};
    auto counter  = std::atomic_size_t{0};

    WHEN("tasks submitted to the master queue")
    {
      for (std::size_t i = 0; i < size; ++i)
      {
        tokens += master.queue.emplace_back(
          [i, &executed, &counter]()
          {
            executed[i] = counter++;
          });
        activity.notifier.notify_one();
        // simulate interruptions
        if (i % 2 == 0)
        {
          std::this_thread::yield();
        }
      }
      tokens.wait();
      THEN("all tasks are executed")
      {
        REQUIRE(executed.size() == size);
      }
    }
    activity.stops = true;
    activity.notifier.notify_all();
  }
}
