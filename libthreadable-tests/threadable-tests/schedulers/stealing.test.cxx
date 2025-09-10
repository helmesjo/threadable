#include <threadable-tests/doctest_include.hxx>
#include <threadable/ring_buffer.hxx>
#include <threadable/schedulers/stealing.hxx>

#include <latch>
#include <thread>

// namespace stealing = fho::schedulers::stealing;

// SCENARIO("schedulers: stealing")
// {
//   auto activity = stealing::activity_stats{};
//   auto exec     = stealing::exec_stats{};

//   auto victim = fho::ring_buffer<>{};
//   auto self   = fho::ring_buffer<>{};

//   auto called = 0;
//   auto task   = [&called]
//   {
//     ++called;
//   };

//   auto stealer = [&victim]() -> auto
//   {
//     return victim.try_pop_front();
//   };

//   GIVEN("worker has task")
//   {
//     self.push_back(task);
//     THEN("next task is from self")
//     {
//       REQUIRE(self.size() == 1);
//       REQUIRE(stealing::exploit_task(self));
//       REQUIRE(self.size() == 0);
//       REQUIRE(called == 1);
//     }
//   }
//   GIVEN("worker has no task")
//   {
//     victim.push_back(task);
//     THEN("next task is from victim")
//     {
//       REQUIRE(victim.size() == 1);
//       REQUIRE(stealing::explore_task(stealer));
//       REQUIRE(victim.size() == 0);
//       REQUIRE(called == 1);
//     }
//   }
//   GIVEN("steal-bound is exceeded")
//   {
//     THEN("next action is to yield")
//     {
//       exec.steal_bound = 0;
//       REQUIRE(stealing::wait_for_task(activity, exec) == stealing::action::yield);
//       REQUIRE(exec.yields == 1);
//     }
//   }
//   GIVEN("steal-bound is not exceeded")
//   {
//     THEN("next action is to retry")
//     {
//       exec.steal_bound = 1;
//       exec.yield_bound = 1;
//       REQUIRE(stealing::wait_for_task(activity, exec) == stealing::action::retry);
//       REQUIRE(exec.yields == 0);
//     }
//   }
//   GIVEN("yield-bound is exceeded")
//   {
//     exec.yield_bound = 0;
//     WHEN("no workers active (actives == 0)")
//     {
//       activity.actives = 0;
//       THEN("next action is to suspend")
//       {
//         REQUIRE(stealing::wait_for_task(activity, exec) == stealing::action::suspend);
//       }
//     }
//     WHEN("workers active (actives > 0)")
//     {
//       activity.actives = 1;
//       THEN("next action is to yield")
//       {
//         REQUIRE(stealing::wait_for_task(activity, exec) == stealing::action::yield);
//       }
//     }
//   }
//   GIVEN("yield-bound is not exceeded")
//   {
//     exec.yield_bound = 1;
//     THEN("next action is to retry")
//     {
//       REQUIRE(stealing::wait_for_task(activity, exec) == stealing::action::retry);
//       REQUIRE(exec.yields == 0);
//     }
//   }
// }

// SCENARIO("schedulers: stealing (acceptance)")
// {
//   auto activity = stealing::activity_stats{};
//   auto exec     = stealing::exec_stats{};

//   auto victim = fho::ring_buffer<>{};
//   auto self   = fho::ring_buffer<>{};

//   auto order = std::vector<bool>{}; // true == self, false == stolen

//   auto selfTask = [&order]
//   {
//     order.push_back(true);
//   };

//   auto victimTask = [&order]
//   {
//     order.push_back(false);
//   };

//   auto stealer = [&victim]() -> auto
//   {
//     return victim.try_pop_front();
//   };

//   // stealer();

//   // auto func = stealing::executor_loop;

//   // auto worker = std::thread(func, activity, self, stealer);

//   self.push_back(selfTask);
//   self.push_back(selfTask);
//   self.push_back(selfTask);
//   victim.push_back(victimTask);
//   victim.push_back(victimTask);
//   victim.push_back(victimTask);
//   victim.push_back(
//     [&activity]
//     {
//       activity.abort = true;
//     });

//   stealing::executor_loop(activity, self, stealer);

//   REQUIRE(order.size() == 6);
//   REQUIRE(order[0] == true);
//   REQUIRE(order[1] == true);
//   REQUIRE(order[2] == true);
//   REQUIRE(order[3] == false);
//   REQUIRE(order[4] == false);
//   REQUIRE(order[5] == false);
//   REQUIRE(order[6] == false);
// }

SCENARIO("schedulers: adaptive stealing (process states)")
{
  namespace sched = fho::schedulers::stealing::v2;
  GIVEN("process_state with previous exploit")
  {
    sched::activity_stats activity{};
    sched::exec_stats     exec{};
    THEN("returns explore (maps to failed owner_pop leading to steal_random)")
    {
      REQUIRE(sched::process_state(activity, exec, sched::action::exploit) ==
              sched::action::explore);
    }
  }
  GIVEN(
    "process_state with previous explore and failed_steals <= steal_bound, yields <= yield_bound")
  {
    sched::activity_stats activity{};
    sched::exec_stats     exec{.failed_steals = 1024,
                               .yields        = 16}; // At bounds, but > checks strict exceedance.
    THEN("returns yield (maps to else branch for simple yield on non-exceeded bounds)")
    {
      REQUIRE(sched::process_state(activity, exec, sched::action::explore) == sched::action::yield);
    }
  }
  GIVEN("process_state with previous explore and failed_steals > steal_bound")
  {
    sched::activity_stats activity{};
    sched::exec_stats     exec{.failed_steals = 1025, .yields = 0}; // Exceed steal_bound.
    THEN("returns yield_steal_exceed (maps to first if for special yield on steal exceedance)")
    {
      REQUIRE(sched::process_state(activity, exec, sched::action::explore) ==
              sched::action::yield_steal_exceed);
    }
  }
  GIVEN("process_state with previous explore and yields > yield_bound, actives > 0")
  {
    sched::activity_stats activity{.actives = 1}; // actives > 0.
    sched::exec_stats     exec{.yields = 17};     // Exceed yield_bound.
    THEN("returns yield_reset_yields (maps to second if with actives > 0 for yield post-reset)")
    {
      REQUIRE(sched::process_state(activity, exec, sched::action::explore) ==
              sched::action::yield_reset_yields);
    }
  }
  GIVEN("process_state with previous explore and yields > yield_bound, actives == 0")
  {
    sched::activity_stats activity{.actives = 0}; // actives == 0.
    sched::exec_stats     exec{.yields = 17};     // Exceed yield_bound.
    THEN(
      "returns suspend_reset_yields (maps to second if with actives == 0 for suspend post-reset)")
    {
      REQUIRE(sched::process_state(activity, exec, sched::action::explore) ==
              sched::action::suspend);
    }
  }
  GIVEN("process_state with previous yield_steal_exceed and yields <= yield_bound")
  {
    sched::activity_stats activity{};
    sched::exec_stats     exec{.yields = 16}; // Updated post-increment <= bound.
    THEN("returns yield (maps to else after first if, for another yield on non-yield-exceed)")
    {
      REQUIRE(sched::process_state(activity, exec, sched::action::yield_steal_exceed) ==
              sched::action::yield);
    }
  }
  GIVEN("process_state with previous yield_steal_exceed and yields > yield_bound, actives > 0")
  {
    sched::activity_stats activity{.actives = 1}; // actives > 0.
    sched::exec_stats     exec{.yields = 17};     // Exceed yield_bound post-increment.
    THEN("returns yield_reset_yields (maps to second if from yield_steal_exceed path)")
    {
      REQUIRE(sched::process_state(activity, exec, sched::action::yield_steal_exceed) ==
              sched::action::yield_reset_yields);
    }
  }
  GIVEN("process_state with previous yield_steal_exceed and yields > yield_bound, actives == 0")
  {
    sched::activity_stats activity{.actives = 0}; // actives == 0.
    sched::exec_stats     exec{.yields = 17};     // Exceed yield_bound post-increment.
    THEN("returns suspend_reset_yields (maps to second if from yield_steal_exceed path)")
    {
      REQUIRE(sched::process_state(activity, exec, sched::action::yield_steal_exceed) ==
              sched::action::suspend);
    }
  }
  GIVEN("process_state with previous yield")
  {
    sched::activity_stats activity{};
    sched::exec_stats     exec{};
    THEN("returns exploit (maps to loop continuation post-yield)")
    {
      REQUIRE(sched::process_state(activity, exec, sched::action::yield) == sched::action::exploit);
    }
  }
  GIVEN("process_state with previous yield_reset_yields")
  {
    sched::activity_stats activity{};
    sched::exec_stats     exec{};
    THEN("returns exploit (maps to loop continuation post-yield with reset)")
    {
      REQUIRE(sched::process_state(activity, exec, sched::action::yield_reset_yields) ==
              sched::action::exploit);
    }
  }
  GIVEN("process_state with previous suspend_reset_yields")
  {
    sched::activity_stats activity{};
    sched::exec_stats     exec{};
    THEN("returns exploit (maps to loop continuation post-suspend with reset)")
    {
      REQUIRE(sched::process_state(activity, exec, sched::action::suspend) ==
              sched::action::exploit);
    }
  }
  GIVEN("process_state with previous suspend")
  {
    sched::activity_stats activity{};
    sched::exec_stats     exec{};
    THEN("returns exploit (maps to loop continuation post-suspend; extend if distinct from reset "
         "variant)")
    {
      REQUIRE(sched::process_state(activity, exec, sched::action::suspend) ==
              sched::action::exploit);
    }
  }
  GIVEN("process_state with previous abort")
  {
    sched::activity_stats activity{};
    sched::exec_stats     exec{};
    THEN("returns abort (no other action to perform)")
    {
      REQUIRE(sched::process_state(activity, exec, sched::action::abort) == sched::action::abort);
    }
  }
}

SCENARIO("schedulers: adaptive stealing (process actions)")
{
  namespace sched = fho::schedulers::stealing::v2;
  auto activity   = sched::activity_stats{};
  auto exec       = sched::exec_stats{};
  auto victim     = fho::ring_buffer<>{};
  auto self       = fho::ring_buffer<>{};
  auto order      = std::vector<bool>{};
  auto selfTask   = [&order]
  {
    order.push_back(true);
  };
  auto victimTask = [&order]
  {
    order.push_back(false);
  };
  auto stealer = [&victim]() -> auto
  {
    return victim.try_pop_back();
  };

  /// @brief Tests exploit action with empty self queue.
  /// @pre Self queue is empty; activity and exec are default-initialized.
  /// @details Verifies actives toggles without lingering inc, no resets occur, self remains empty,
  /// and no tasks execute.
  GIVEN("action::exploit with empty self queue")
  {
    auto activity = sched::activity_stats{};
    auto exec     = sched::exec_stats{};
    auto action   = sched::action::exploit;
    THEN("actives toggles but no execution, no resets, self remains empty")
    {
      sched::process_action(action, activity, exec, self, stealer);
      REQUIRE(activity.actives.load(std::memory_order_acquire) == 0);
      REQUIRE(exec.yields == 0);
      REQUIRE(exec.failed_steals == 0);
      REQUIRE(self.empty());
      REQUIRE(order.empty());
    }
  }

  /// @brief Tests exploit action with multiple tasks in self queue.
  /// @pre Self queue has three tasks pushed; activity and exec are default-initialized.
  /// @details Verifies full drain under single actives toggle, resets counters, self empties, and
  /// tasks execute in FIFO order.
  GIVEN("action::exploit with multiple tasks in self")
  {
    auto activity = sched::activity_stats{};
    auto exec     = sched::exec_stats{};
    auto action   = sched::action::exploit;
    self.push_back(selfTask);
    self.push_back(selfTask);
    self.push_back(selfTask);
    THEN("drains all, actives toggles once, resets counters, executes in order")
    {
      sched::process_action(action, activity, exec, self, stealer);
      REQUIRE(activity.actives.load(std::memory_order_acquire) == 0);
      REQUIRE(exec.yields == 0);
      REQUIRE(exec.failed_steals == 0);
      REQUIRE(self.empty());
      REQUIRE(order == std::vector<bool>{true, true, true});
    }
  }

  /// @brief Tests explore action with no task in victim.
  /// @pre Victim queue is empty; activity and exec are default-initialized.
  /// @details Verifies no execution, thieves unchanged, inc failed_steals, victim remains empty,
  /// and no tasks run.
  GIVEN("action::explore with no task in victim")
  {
    auto activity = sched::activity_stats{};
    auto exec     = sched::exec_stats{.steal_bound = 8};
    auto action   = sched::action::explore;
    THEN("no execution, thieves unchanged (no inc/dec), no resets, failed steals reached bounds")
    {
      sched::process_action(action, activity, exec, self, stealer);
      REQUIRE(activity.thieves.load(std::memory_order_acquire) == 0);
      REQUIRE(exec.yields == 1);
      REQUIRE(exec.failed_steals == exec.steal_bound);
      REQUIRE(victim.empty());
      REQUIRE(order.empty());
    }
  }

  /// @brief Tests explore action with task in victim.
  /// @pre Victim queue has one task pushed; activity and exec are default-initialized.
  /// @details Verifies no execution (task only stolen), thieves unchanged, resets counters, victim
  /// empty, self holds task.
  GIVEN("action::explore with task in victim={t}")
  {
    auto activity = sched::activity_stats{};
    auto exec     = sched::exec_stats{};
    auto action   = sched::action::explore;
    victim.push_back(victimTask);
    THEN("executes, thieves unchanged, resets counters, victim={}, self={t}")
    {
      sched::process_action(action, activity, exec, self, stealer);
      REQUIRE(activity.thieves.load(std::memory_order_acquire) == 0);
      REQUIRE(exec.yields == 0);
      REQUIRE(exec.failed_steals == 0);
      REQUIRE(victim.empty());
      REQUIRE(self.size() == 1);
    }
  }

  /// @brief Tests abort action.
  /// @pre Activity and exec are default-initialized.
  /// @details Verifies no-op with no changes to states or queues.
  GIVEN("action::abort")
  {
    auto activity = sched::activity_stats{};
    auto exec     = sched::exec_stats{};
    auto action   = sched::action::abort;
    THEN("no-op, no changes to states or queues")
    {
      sched::process_action(action, activity, exec, self, stealer);
      REQUIRE(activity.actives.load(std::memory_order_acquire) == 0);
      REQUIRE(activity.thieves.load(std::memory_order_acquire) == 0);
      REQUIRE(exec.yields == 0);
      REQUIRE(exec.failed_steals == 0);
      REQUIRE(self.empty());
      REQUIRE(victim.empty());
      REQUIRE(order.empty());
    }
  }

  /// @brief Tests retry action.
  /// @pre Activity and exec are default-initialized.
  /// @details Verifies no-op with no changes to states or queues.
  GIVEN("action::retry")
  {
    auto activity = sched::activity_stats{};
    auto exec     = sched::exec_stats{};
    auto action   = sched::action::retry;
    THEN("no-op, no changes")
    {
      sched::process_action(action, activity, exec, self, stealer);
      REQUIRE(activity.actives.load(std::memory_order_acquire) == 0);
      REQUIRE(activity.thieves.load(std::memory_order_acquire) == 0);
      REQUIRE(exec.yields == 0);
      REQUIRE(exec.failed_steals == 0);
      REQUIRE(self.empty());
      REQUIRE(victim.empty());
      REQUIRE(order.empty());
    }
  }

  /// @brief Tests yield action.
  /// @pre Activity and exec are default-initialized.
  /// @details Verifies yield occurs with no state changes.
  GIVEN("action::yield")
  {
    auto activity = sched::activity_stats{};
    auto exec     = sched::exec_stats{};
    auto action   = sched::action::yield;
    THEN("yields but no state changes")
    {
      sched::process_action(action, activity, exec, self, stealer);
      REQUIRE(activity.actives.load(std::memory_order_acquire) == 0);
      REQUIRE(activity.thieves.load(std::memory_order_acquire) == 0);
      REQUIRE(exec.yields == 0);
      REQUIRE(exec.failed_steals == 0);
      REQUIRE(self.empty());
      REQUIRE(victim.empty());
      REQUIRE(order.empty());
    }
  }

  /// @brief Tests yield_steal_exceed action with pre-set counters.
  /// @pre Exec has failed_steals=1025, yields=5; activity default-initialized.
  /// @details Verifies yield, resets failed_steals, increments yields, no global changes.
  GIVEN("action::yield_steal_exceed")
  {
    auto activity = sched::activity_stats{};
    auto exec     = sched::exec_stats{.failed_steals = 1025, .yields = 5};
    auto action   = sched::action::yield_steal_exceed;
    THEN("yields, resets failed_steals, inc yields, no global changes")
    {
      sched::process_action(action, activity, exec, self, stealer);
      REQUIRE(activity.actives.load(std::memory_order_acquire) == 0);
      REQUIRE(activity.thieves.load(std::memory_order_acquire) == 0);
      REQUIRE(exec.yields == 6);
      REQUIRE(exec.failed_steals == 0);
      REQUIRE(self.empty());
      REQUIRE(victim.empty());
      REQUIRE(order.empty());
    }
  }

  /// @brief Tests yield_reset_yields action with pre-set yields.
  /// @pre Exec has yields=17; activity default-initialized.
  /// @details Verifies yield, resets yields, no other changes.
  GIVEN("action::yield_reset_yields")
  {
    auto activity = sched::activity_stats{};
    auto exec     = sched::exec_stats{.yields = 17};
    auto action   = sched::action::yield_reset_yields;
    THEN("yields, resets yields, no other changes")
    {
      sched::process_action(action, activity, exec, self, stealer);
      REQUIRE(activity.actives.load(std::memory_order_acquire) == 0);
      REQUIRE(activity.thieves.load(std::memory_order_acquire) == 0);
      REQUIRE(exec.yields == 0);
      REQUIRE(exec.failed_steals == 0);
      REQUIRE(self.empty());
      REQUIRE(victim.empty());
      REQUIRE(order.empty());
    }
  }

  /// @brief Tests suspend action with ready initially false and pre-set counters.
  /// @pre Activity ready=false, thieves=1; exec has yields=17, failed_steals=1025.
  /// @details Verifies blocks until notify, decrements thieves, resets counters post-wake; uses
  /// thread/latch for simulation.
  GIVEN("action::suspend with ready initially false")
  {
    auto activity = sched::activity_stats{.ready = false};
    auto exec     = sched::exec_stats{.failed_steals = 1025, .yields = 17};
    auto action   = sched::action::suspend;
    THEN("waits (blocks), dec thieves, resets yields/failed_steals post-wake")
    {
      activity.thieves = 1;
      auto l           = std::latch{2};
      auto th          = std::thread(
        [&]
        {
          l.arrive_and_wait();
          sched::process_action(action, activity, exec, self, stealer);
        });
      l.arrive_and_wait();
      std::this_thread::yield();
      activity.ready.store(true, std::memory_order_release);
      activity.ready.notify_one();
      th.join();

      REQUIRE(activity.thieves.load(std::memory_order_acquire) == 0);
      REQUIRE(exec.yields == 0);
      REQUIRE(exec.failed_steals == 0);
      REQUIRE(self.empty());
      REQUIRE(victim.empty());
      REQUIRE(order.empty());
    }
  }
}
