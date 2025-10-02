// #include <threadable-tests/doctest_include.hxx>
// #include <threadable/ring_buffer.hxx>
// #include <threadable/schedulers/stealing.hxx>

// #include <atomic>
// #include <latch>
// #include <thread>
// #include <vector>

// SCENARIO("schedulers: adaptive stealing (process states)")
// {
//   namespace sched = fho::schedulers::stealing;

//   GIVEN("prev = exploit")
//   {
//     sched::activity_stats activity{};
//     sched::exec_stats     exec{};
//     THEN("→ explore")
//     {
//       REQUIRE(sched::process_state(activity, exec, sched::action::exploit) ==
//               sched::action::explore);
//     }
//   }

//   GIVEN("prev = explore and got_task_last == true")
//   {
//     sched::activity_stats activity{};
//     sched::exec_stats     exec{};
//     exec.got_task_last = true;
//     THEN("→ exploit")
//     {
//       REQUIRE(sched::process_state(activity, exec, sched::action::explore) ==
//               sched::action::exploit);
//     }
//   }

//   GIVEN("prev = explore and ready > 0 (no success yet)")
//   {
//     sched::activity_stats activity{};
//     sched::exec_stats     exec{};
//     activity.ready.store(1, std::memory_order_relaxed);
//     THEN("→ retry (keep exploring, do not sleep)")
//     {
//       REQUIRE(sched::process_state(activity, exec, sched::action::explore) ==
//       sched::action::retry);
//     }
//   }

//   GIVEN("prev = explore, failed_steals >= steal_bound, yields < yield_bound")
//   {
//     sched::activity_stats activity{};
//     sched::exec_stats     exec{};
//     exec.failed_steals = exec.steal_bound; // boundary triggers yield_steal_exceed
//     exec.yields        = 0;
//     THEN("→ yield_steal_exceed")
//     {
//       REQUIRE(sched::process_state(activity, exec, sched::action::explore) ==
//               sched::action::yield_steal_exceed);
//     }
//   }

//   GIVEN("prev = explore, failed_steals >= steal_bound, yields >= yield_bound, actives > 0")
//   {
//     sched::activity_stats activity{};
//     sched::exec_stats     exec{};
//     activity.actives.store(1, std::memory_order_relaxed);
//     exec.failed_steals = exec.steal_bound;
//     exec.yields        = exec.yield_bound; // boundary -> choose keep-one-thief path
//     THEN("→ yield_reset_yields")
//     {
//       REQUIRE(sched::process_state(activity, exec, sched::action::explore) ==
//               sched::action::yield_reset_yields);
//     }
//   }

//   GIVEN("prev = explore, failed_steals >= steal_bound, yields >= yield_bound, actives == 0")
//   {
//     sched::activity_stats activity{};
//     sched::exec_stats     exec{};
//     activity.actives.store(0, std::memory_order_relaxed);
//     exec.failed_steals = exec.steal_bound;
//     exec.yields        = exec.yield_bound;
//     THEN("→ suspend")
//     {
//       REQUIRE(sched::process_state(activity, exec, sched::action::explore) ==
//               sched::action::suspend);
//     }
//   }

//   GIVEN("prev ∈ {retry, yield, yield_steal_exceed, yield_reset_yields, suspend}")
//   {
//     sched::activity_stats activity{};
//     sched::exec_stats     exec{};
//     THEN("→ explore (transient steps resume exploring)")
//     {
//       REQUIRE(sched::process_state(activity, exec, sched::action::retry) ==
//       sched::action::explore); REQUIRE(sched::process_state(activity, exec, sched::action::yield)
//       == sched::action::explore); REQUIRE(sched::process_state(activity, exec,
//       sched::action::yield_steal_exceed) ==
//               sched::action::explore);
//       REQUIRE(sched::process_state(activity, exec, sched::action::yield_reset_yields) ==
//               sched::action::explore);
//       REQUIRE(sched::process_state(activity, exec, sched::action::suspend) ==
//               sched::action::explore);
//     }
//   }

//   GIVEN("abort requested (activity.abort or exec.abort)")
//   {
//     sched::activity_stats activity{};
//     sched::exec_stats     exec{};
//     activity.abort.store(true, std::memory_order_relaxed);
//     THEN("→ abort regardless of prev")
//     {
//       REQUIRE(sched::process_state(activity, exec, sched::action::explore) ==
//       sched::action::abort);
//     }
//   }

//   GIVEN("prev = abort")
//   {
//     sched::activity_stats activity{};
//     sched::exec_stats     exec{};
//     THEN("→ abort")
//     {
//       REQUIRE(sched::process_state(activity, exec, sched::action::abort) ==
//       sched::action::abort);
//     }
//   }
// }

// SCENARIO("schedulers: adaptive stealing (process actions)")
// {
//   namespace sched = fho::schedulers::stealing;

//   // test fixtures
//   auto             victim = fho::ring_buffer<>{};
//   auto             self   = fho::ring_buffer<decltype(victim)::claimed_type>{};
//   std::vector<int> executed; // capture order: +1 from selfTask, +2 from victimTask

//   auto selfTask = [&]
//   {
//     executed.push_back(1);
//   };
//   auto victimTask = [&]
//   {
//     executed.push_back(2);
//   };

//   // A stealer that ONLY moves tasks; never touches activity.ready.
//   auto stealer = [&](std::ranges::range auto&& r) -> std::size_t
//   {
//     if (auto t = victim.try_pop_back())
//     {
//       r.emplace_back(std::move(t));
//       return 1;
//     }
//     return 0;
//   };

//   GIVEN("action::exploit with empty self")
//   {
//     sched::activity_stats activity{};
//     sched::exec_stats     exec{};
//     // ready==0 and no tasks
//     THEN("actives toggles and returns; nothing executed")
//     {
//       sched::process_action(sched::action::exploit, activity, exec, self, stealer);
//       REQUIRE(activity.actives.load(std::memory_order_acquire) == 0);
//       REQUIRE(self.empty());
//       REQUIRE(executed.empty());
//     }
//   }

//   GIVEN("action::exploit with three tasks in self, ready=3")
//   {
//     sched::activity_stats activity{};
//     sched::exec_stats     exec{};
//     // Seed victim with 3 tasks and move them to self (preserve simple FIFO via front->self).
//     victim.push_back(selfTask);
//     victim.push_back(selfTask);
//     victim.push_back(selfTask);
//     self.push_back(victim.try_pop_front());
//     self.push_back(victim.try_pop_front());
//     self.push_back(victim.try_pop_front());
//     activity.ready.store(3, std::memory_order_release);

//     THEN("drains all tasks, executes all, leaves actives=0 and self empty")
//     {
//       sched::process_action(sched::action::exploit, activity, exec, self, stealer);
//       REQUIRE(activity.actives.load(std::memory_order_acquire) == 0);
//       REQUIRE(self.empty());
//       REQUIRE(executed == std::vector<int>{1, 1, 1});
//       REQUIRE(activity.ready.load(std::memory_order_acquire) == 0);
//       REQUIRE(exec.failed_steals == 0);
//       REQUIRE(exec.yields == 0);
//     }
//   }

//   GIVEN("action::explore with no work to steal (first explore episode)")
//   {
//     sched::activity_stats activity{};
//     sched::exec_stats     exec{}; // failed_steals=0, yields=0 -> first-episode increments
//     thieves THEN("thieves increments, failed_steals++ (no execution)")
//     {
//       sched::process_action(sched::action::explore, activity, exec, self, stealer);
//       REQUIRE(activity.thieves.load(std::memory_order_acquire) == 1);
//       REQUIRE(exec.failed_steals == 1);
//       REQUIRE(exec.yields == 0);
//       REQUIRE(victim.empty());
//       REQUIRE(self.empty());
//       REQUIRE(executed.empty());
//     }
//   }

//   GIVEN("action::explore stealing one task from victim (first explore episode)")
//   {
//     sched::activity_stats activity{};
//     sched::exec_stats     exec{}; // first entry -> thieves++
//     victim.push_back(victimTask);

//     THEN("moves task to self, sets got_task_last, and restores thieves to 0")
//     {
//       sched::process_action(sched::action::explore, activity, exec, self, stealer);
//       REQUIRE(exec.got_task_last == true);
//       REQUIRE(activity.thieves.load(std::memory_order_acquire) ==
//               0); // incremented then decremented
//       REQUIRE(victim.empty());
//       REQUIRE(self.size() == 1);
//       REQUIRE(executed.empty());

//       AND_THEN("action::exploit runs the stolen task, ready=1 → 0")
//       {
//         activity.ready.store(1, std::memory_order_release);
//         sched::process_action(sched::action::exploit, activity, exec, self, stealer);
//         REQUIRE(executed == std::vector<int>{2});
//         REQUIRE(activity.ready.load(std::memory_order_acquire) == 0);
//         REQUIRE(self.empty());
//       }
//     }
//   }

//   GIVEN("action::yield_steal_exceed adjusts counters only")
//   {
//     sched::activity_stats activity{};
//     sched::exec_stats     exec{};
//     exec.failed_steals = 1025;
//     exec.yields        = 5;
//     THEN("resets failed_steals, increments yields; no global changes")
//     {
//       sched::process_action(sched::action::yield_steal_exceed, activity, exec, self, stealer);
//       REQUIRE(exec.failed_steals == 0);
//       REQUIRE(exec.yields == 6);
//       REQUIRE(activity.thieves.load(std::memory_order_acquire) == 0);
//       REQUIRE(activity.actives.load(std::memory_order_acquire) == 0);
//     }
//   }

//   GIVEN("action::yield_reset_yields zeros yields only")
//   {
//     sched::activity_stats activity{};
//     sched::exec_stats     exec{};
//     exec.yields = 17;
//     THEN("yields reset to 0; no other changes")
//     {
//       sched::process_action(sched::action::yield_reset_yields, activity, exec, self, stealer);
//       REQUIRE(exec.yields == 0);
//       REQUIRE(exec.failed_steals == 0);
//       REQUIRE(activity.thieves.load(std::memory_order_acquire) == 0);
//     }
//   }

//   GIVEN("action::suspend waits on bell, with thieves pre-counted (episode semantics)")
//   {
//     sched::activity_stats activity{};
//     sched::exec_stats     exec{};
//     activity.thieves.store(1, std::memory_order_release); // enter suspend from an explore
//     episode

//     THEN("blocks until bell notified; post-wake counters reset; thieves becomes 0")
//     {
//       std::latch  go{2};
//       std::thread th(
//         [&]
//         {
//           go.arrive_and_wait();
//           sched::process_action(sched::action::suspend, activity, exec, self, stealer);
//         });
//       go.arrive_and_wait();
//       std::this_thread::sleep_for(std::chrono::milliseconds(1));
//       // wake the sleeper
//       activity.bell.fetch_add(1, std::memory_order_release);
//       activity.bell.notify_one();
//       th.join();

//       REQUIRE(activity.thieves.load(std::memory_order_acquire) == 0);
//       REQUIRE(exec.yields == 0);
//       REQUIRE(exec.failed_steals == 0);
//       REQUIRE(self.empty());
//     }
//   }

//   GIVEN("action::retry and action::yield are no-ops on state")
//   {
//     sched::activity_stats activity{};
//     sched::exec_stats     exec{};
//     THEN("no counters or queues change")
//     {
//       sched::process_action(sched::action::retry, activity, exec, self, stealer);
//       sched::process_action(sched::action::yield, activity, exec, self, stealer);
//       REQUIRE(activity.actives.load(std::memory_order_acquire) == 0);
//       REQUIRE(activity.thieves.load(std::memory_order_acquire) == 0);
//       REQUIRE(exec.yields == 0);
//       REQUIRE(exec.failed_steals == 0);
//       REQUIRE(victim.empty());
//       REQUIRE(self.empty());
//       REQUIRE(executed.empty());
//     }
//   }

//   GIVEN("action::abort notifies but leaves counters as-is")
//   {
//     sched::activity_stats activity{};
//     sched::exec_stats     exec{};
//     THEN("no local counters change")
//     {
//       sched::process_action(sched::action::abort, activity, exec, self, stealer);
//       REQUIRE(activity.actives.load(std::memory_order_acquire) == 0);
//       REQUIRE(activity.thieves.load(std::memory_order_acquire) == 0);
//       REQUIRE(exec.yields == 0);
//       REQUIRE(exec.failed_steals == 0);
//     }
//   }
// }
