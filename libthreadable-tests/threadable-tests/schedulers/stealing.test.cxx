#include <threadable-tests/doctest_include.hxx>
#include <threadable/ring_buffer.hxx>
#include <threadable/schedulers/stealing.hxx>

namespace stealing = fho::schedulers::stealing;

SCENARIO("schedulers: stealing")
{
  auto activity = stealing::activity_stats{};
  auto exec     = stealing::exec_stats{};

  auto victim = fho::ring_buffer<>{};
  auto self   = fho::ring_buffer<>{};

  auto called = 0;
  auto task   = [&called]
  {
    ++called;
  };

  auto stealer = [&victim]() -> auto
  {
    return victim.try_pop_front();
  };

  GIVEN("worker has task")
  {
    self.push_back(task);
    THEN("next task is from self")
    {
      REQUIRE(self.size() == 1);
      REQUIRE(stealing::exploit_task(self));
      REQUIRE(self.size() == 0);
      REQUIRE(called == 1);
    }
  }
  GIVEN("worker has no task")
  {
    victim.push_back(task);
    THEN("next task is from victim")
    {
      REQUIRE(victim.size() == 1);
      REQUIRE(stealing::explore_task(activity, stealer));
      REQUIRE(victim.size() == 0);
      REQUIRE(called == 1);
    }
  }
  GIVEN("steal-bound is exceeded")
  {
    THEN("next action is to yield")
    {
      exec.steal_bound = 0;
      REQUIRE(stealing::wait_for_task(activity, exec) == stealing::action::yield);
      REQUIRE(exec.yields == 1);
    }
  }
  GIVEN("steal-bound is not exceeded")
  {
    THEN("next action is to retry")
    {
      exec.steal_bound = 1;
      exec.yield_bound = 1;
      REQUIRE(stealing::wait_for_task(activity, exec) == stealing::action::retry);
      REQUIRE(exec.yields == 0);
    }
  }
  GIVEN("yield-bound is exceeded")
  {
    exec.yield_bound = 0;
    WHEN("no workers active (actives == 0)")
    {
      activity.actives = 0;
      THEN("next action is to suspend")
      {
        REQUIRE(stealing::wait_for_task(activity, exec) == stealing::action::suspend);
      }
    }
    WHEN("workers active (actives > 0)")
    {
      activity.actives = 1;
      THEN("next action is to yield")
      {
        REQUIRE(stealing::wait_for_task(activity, exec) == stealing::action::yield);
      }
    }
  }
  GIVEN("yield-bound is not exceeded")
  {
    exec.yield_bound = 1;
    THEN("next action is to retry")
    {
      REQUIRE(stealing::wait_for_task(activity, exec) == stealing::action::retry);
      REQUIRE(exec.yields == 0);
    }
  }
}

SCENARIO("schedulers: stealing (acceptance)")
{
  auto activity = stealing::activity_stats{};
  auto exec     = stealing::exec_stats{};

  auto victim = fho::ring_buffer<>{};
  auto self   = fho::ring_buffer<>{};

  auto order = std::vector<bool>{}; // true == self, false == stolen

  auto selfTask = [&order]
  {
    order.push_back(true);
  };

  auto victimTask = [&order]
  {
    order.push_back(false);
  };

  auto stealer = [&victim]() -> auto
  {
    return victim.try_pop_front();
  };

  // stealer();

  // auto func = stealing::executor_loop;

  // auto worker = std::thread(func, activity, self, stealer);

  self.push_back(selfTask);
  self.push_back(selfTask);
  self.push_back(selfTask);
  victim.push_back(victimTask);
  victim.push_back(victimTask);
  victim.push_back(victimTask);
  victim.push_back(
    [&activity]
    {
      activity.abort = true;
    });

  stealing::executor_loop(activity, self, stealer);

  REQUIRE(order.size() == 6);
  REQUIRE(order[0] == true);
  REQUIRE(order[1] == true);
  REQUIRE(order[2] == true);
  REQUIRE(order[3] == false);
  REQUIRE(order[4] == false);
  REQUIRE(order[5] == false);
  REQUIRE(order[6] == false);
}
