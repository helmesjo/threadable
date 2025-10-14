#include <threadable-tests/doctest_include.hxx>
#include <threadable/ring_buffer.hxx>
#include <threadable/scheduler/stealing.hxx>

#include <atomic>

SCENARIO("scheduler: adaptive stealing")
{
  namespace sched = fho::scheduler::stealing;
  using ring_t    = fho::ring_buffer<fho::fast_func_t>;
  using claimed_t = fho::ring_buffer<fho::fast_func_t>::claimed_type;

  auto activity = sched::activity_stats{};
  auto master   = fho::ring_buffer<fho::fast_func_t>{};

  auto stealer = [&master](std::ranges::range auto&&)
  {
    return master.try_pop_back();
  };

  GIVEN("empty queue for exploit_task")
  {
    THEN("no tasks executed, actives unchanged")
    {
      auto queue  = ring_t{};
      auto stolen = claimed_t{nullptr};

      sched::exploit_task(stolen, activity, queue);

      REQUIRE(activity.actives.load() == 0);
      REQUIRE(!stolen);
    }
  }

  GIVEN("single task for exploit_task")
  {
    THEN("task executed, actives incremented then decremented")
    {
      auto queue    = fho::ring_buffer<fho::fast_func_t>{};
      auto executed = std::atomic_size_t{0};

      auto stolen = fho::fast_func_t{[&]
                                     {
                                       ++executed;
                                     }};
      sched::exploit_task(stolen, activity, queue);

      REQUIRE(executed);
      REQUIRE(activity.actives.load() == 0);
      REQUIRE(!stolen);
    }
  }

  GIVEN("multiple tasks in worker queue for exploit_task")
  {
    THEN("all tasks executed, actives incremented then decremented")
    {
      auto queue    = fho::ring_buffer<fho::fast_func_t>{};
      auto executed = std::atomic_size_t{0};
      auto stolen   = claimed_t{nullptr};

      queue.emplace_back(
        [&]
        {
          ++executed;
        });
      queue.emplace_back(
        [&]
        {
          ++executed;
        });
      stolen = queue.try_pop_back();
      sched::exploit_task(stolen, activity, queue);

      REQUIRE(executed.load() == 2);
      REQUIRE(activity.actives.load() == 0);
      REQUIRE(queue.empty());
    }
  }

  GIVEN("tasks in master queue for explore_task")
  {
    THEN("task stolen, stats updated")
    {
      auto queue    = fho::ring_buffer<fho::fast_func_t>{};
      auto executed = std::atomic_size_t{0};
      auto stolen   = claimed_t{nullptr};

      master.emplace_back(
        [&]
        {
          ++executed;
        });
      auto stats = sched::exec_stats{.steal_bound = 2, .yield_bound = 2};
      REQUIRE(sched::explore_task(stolen, stats, queue, stealer));
      REQUIRE(stolen);
      stolen(); // execute task
      REQUIRE(executed.load() == 1);
      REQUIRE(stats.failed_steals == 0);
      REQUIRE(stats.yields == 0);
    }
  }

  GIVEN("empty master queue for explore_task")
  {
    THEN("no task stolen, bounds reached")
    {
      auto queue  = fho::ring_buffer<fho::fast_func_t>{};
      auto stolen = claimed_t{nullptr};

      auto stats = sched::exec_stats{.steal_bound = 2, .yield_bound = 2};
      REQUIRE(!sched::explore_task(stolen, stats, queue, stealer));
      REQUIRE(!stolen);
      REQUIRE(stats.failed_steals >= stats.steal_bound);
      REQUIRE(stats.yields >= stats.yield_bound);
    }
  }

  GIVEN("task in master queue for wait_for_task")
  {
    THEN("task stolen, thieves adjusted")
    {
      auto queue    = fho::ring_buffer<fho::fast_func_t>{};
      auto executed = std::atomic_size_t{0};
      master.emplace_back(
        [&]
        {
          ++executed;
        });
      auto stolen = claimed_t{nullptr};

      auto stats = sched::exec_stats{.steal_bound = 2, .yield_bound = 2};
      REQUIRE(sched::wait_for_task(stolen, activity, stats, queue, stealer));
      REQUIRE(stolen);
      stolen();
      REQUIRE(executed.load() == 1);
      REQUIRE(activity.thieves.load() == 0);
    }
  }

  GIVEN("scheduler stopped for wait_for_task")
  {
    THEN("no task stolen, returns false")
    {
      auto queue  = fho::ring_buffer<fho::fast_func_t>{};
      auto stolen = claimed_t{nullptr};
      auto stats  = sched::exec_stats{.steal_bound = 2, .yield_bound = 2};

      activity.stops = true;
      REQUIRE(!sched::wait_for_task(stolen, activity, stats, queue, stealer));
      REQUIRE(!stolen);
      REQUIRE(activity.thieves.load() == 0);
    }
  }
}
