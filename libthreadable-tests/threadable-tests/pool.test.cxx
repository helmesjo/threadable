#include <threadable-tests/doctest_include.hxx>
#include <threadable/pool.hxx>

#include <barrier>
#include <cstddef>
#include <thread>
#include <vector>

SCENARIO("pool: print system info")
{
  std::cerr << "hardware_concurrency: " << std::thread::hardware_concurrency() << std::endl;
}

// SCENARIO("pool: create/remove queues")
// {
//   auto pool = fho::pool<8>();
//   GIVEN("queue is created")
//   {
//     auto& queue = pool.create();
//     static_assert(fho::pool<8>::max_size() == decltype(pool)::queue_t::max_size());

//     WHEN("task is emplaced")
//     {
//       int  called = 0;
//       auto token  = queue.emplace_back(
//         [&called]
//         {
//           ++called;
//         });
//       THEN("it gets executed")
//       {
//         token.wait();
//         REQUIRE(called == 1);
//       }
//     }
//     THEN("queue can be removed")
//     {
//       REQUIRE(pool.remove(std::move(queue)));
//     }
//   }
//   GIVEN("queue is pre-created")
//   {
//     auto queue = decltype(pool)::queue_t();
//     WHEN("added (without tasks) to pool")
//     {
//       auto& q = pool.add(std::move(queue), fho::execution::par);
//       AND_WHEN("task is emplaced")
//       {
//         int  called = 0;
//         auto token2 = q.emplace_back(
//           [&called]
//           {
//             ++called;
//           });
//         THEN("it gets executed")
//         {
//           token2.wait();
//           REQUIRE(called == 1);
//         }
//       }
//       AND_WHEN("removed from pool")
//       {
//         REQUIRE(pool.remove(std::move(q)));
//       }
//     }
//     WHEN("added (with tasks) to pool")
//     {
//       int  called = 0;
//       auto token  = queue.emplace_back(
//         [&called]
//         {
//           ++called;
//         });
//       (void)pool.add(std::move(queue), fho::execution::par);
//       THEN("existing tasks are executed")
//       {
//         token.wait();
//         REQUIRE(called == 1);
//       }
//     }
//   }
// }

// SCENARIO("pool: execution order")
// {
//   auto pool = fho::pool<1024>();
//   GIVEN("a task is submitted to a sequential queue")
//   {
//     auto& queue    = pool.create(fho::execution::seq);
//     auto  executed = std::vector<std::size_t>(queue.max_size(), 0);
//     std::ranges::fill(executed, 0);
//     auto tokens  = fho::token_group{};
//     auto counter = std::atomic_size_t{0};
//     for (std::size_t i = 0; i < queue.max_size(); ++i)
//     {
//       tokens += queue.emplace_back(
//         [i, &executed, &counter]()
//         {
//           executed[i] = counter++;
//         });
//       // simulate interruptions
//       if (i % 2 == 0)
//       {
//         std::this_thread::yield();
//       }
//     }
//     REQUIRE(tokens.size() == queue.max_size());
//     tokens.wait();
//     THEN("all tasks are executed in order")
//     {
//       REQUIRE(executed.size() == queue.max_size());
//       for (std::size_t i = 0; i < queue.max_size(); ++i)
//       {
//         REQUIRE(executed[i] == i);
//       }
//     }
//     (void)pool.remove(std::move(queue));
//   }
//   GIVEN("a task is submitted to a parallel queue")
//   {
//     auto& queue   = pool.create(fho::execution::par);
//     auto  counter = std::atomic_size_t{0};
//     auto  tokens  = fho::token_group{};
//     for (std::size_t i = 0; i < pool.max_size(); ++i)
//     {
//       tokens += queue.emplace_back(
//         [&counter]
//         {
//           ++counter;
//         });
//     }
//     tokens.wait();
//     THEN("all tasks are executed")
//     {
//       REQUIRE(counter == pool.max_size());
//     }
//     (void)pool.remove(std::move(queue));
//   }
// }

// SCENARIO("pool: stress-test")
// {
//   constexpr auto capacity = std::size_t{1 << 16};

//   GIVEN("multiple producers submit a large amount of tasks")
//   {
//     constexpr auto nr_producers = 4;

//     auto  pool      = fho::pool<capacity>(nr_producers);
//     auto& queue     = pool.create(fho::execution::par);
//     auto  counter   = std::atomic_size_t{0};
//     auto  producers = std::vector<std::thread>{};

//     for (std::size_t i = 0; i < nr_producers; ++i)
//     {
//       producers.emplace_back(
//         [&counter, &queue]
//         {
//           static_assert(decltype(pool)::max_size() % nr_producers == 0,
//                         "All tasks must be submitted");
//           auto tokens = fho::token_group{};
//           for (std::size_t j = 0; j < queue.max_size() / nr_producers; ++j)
//           {
//             tokens += queue.emplace_back(
//               [&counter]
//               {
//                 ++counter;
//               });
//           }
//           tokens.wait();
//         });
//     }

//     for (auto& thread : producers)
//     {
//       thread.join();
//     }

//     THEN("all gets executed")
//     {
//       REQUIRE(counter.load() == queue.max_size());
//     }
//   }
//   GIVEN("multiple producers submit a large amount of tasks to their own queue and then remove
//   it")
//   {
//     static constexpr auto nr_producers = 4;

//     auto pool      = fho::pool<capacity>(nr_producers);
//     auto counter   = std::atomic_size_t{0};
//     auto producers = std::vector<std::thread>{};
//     auto barrier   = std::barrier{nr_producers};

//     for (std::size_t i = 0; i < nr_producers; ++i)
//     {
//       producers.emplace_back(
//         [&counter, &pool, &barrier, &queue = pool.create(fho::execution::par)]
//         {
//           static_assert(decltype(pool)::max_size() % nr_producers == 0,
//                         "All tasks must be submitted");

//           auto tokens = fho::token_group{};
//           barrier.arrive_and_wait();
//           for (std::size_t j = 0; j < queue.max_size() / nr_producers; ++j)
//           {
//             tokens += queue.emplace_back(
//               [&counter]
//               {
//                 ++counter;
//               });
//           }
//           tokens.wait();
//           REQUIRE(pool.remove(std::move(queue)));
//         });
//     }

//     for (auto& thread : producers)
//     {
//       thread.join();
//     }
//     REQUIRE(counter.load() == decltype(pool)::max_size());
//   }
// }

SCENARIO("pool (v2): create/remove queues")
{
  auto pool = fho::v2::pool();
  GIVEN("queue is created")
  {
    auto& queue = pool.create();
    static_assert(fho::v2::pool::max_size() == decltype(pool)::queue_t::max_size());

    WHEN("task is emplaced")
    {
      int  called = 0;
      auto token  = queue.push(
        [&called]
        {
          ++called;
        });
      THEN("it gets executed")
      {
        token.wait();
        REQUIRE(called == 1);
      }
    }
    THEN("queue can be removed")
    {
      REQUIRE(pool.remove(std::move(queue)));
    }
  }
  // GIVEN("queue is pre-created")
  // {
  //   auto queue = decltype(pool)::queue_t();
  //   WHEN("added (without tasks) to pool")
  //   {
  //     auto& q = pool.add(std::move(queue), fho::execution::par);
  //     AND_WHEN("task is emplaced")
  //     {
  //       int  called = 0;
  //       auto token2 = q.emplace_back(
  //         [&called]
  //         {
  //           ++called;
  //         });
  //       THEN("it gets executed")
  //       {
  //         token2.wait();
  //         REQUIRE(called == 1);
  //       }
  //     }
  //     AND_WHEN("removed from pool")
  //     {
  //       REQUIRE(pool.remove(std::move(q)));
  //     }
  //   }
  //   WHEN("added (with tasks) to pool")
  //   {
  //     int  called = 0;
  //     auto token  = queue.emplace_back(
  //       [&called]
  //       {
  //         ++called;
  //       });
  //     (void)pool.add(std::move(queue), fho::execution::par);
  //     THEN("existing tasks are executed")
  //     {
  //       token.wait();
  //       REQUIRE(called == 1);
  //     }
  //   }
  // }
}
