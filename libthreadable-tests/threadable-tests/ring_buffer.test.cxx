#include <threadable-tests/doctest_include.hxx>
#include <threadable/execution.hxx>
#include <threadable/function.hxx>
#include <threadable/ring_buffer.hxx>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace
{
  template<typename T>
  auto
  is_aligned(T const& ptr, std::size_t alignment) -> bool
  {
    auto addr = reinterpret_cast<std::uintptr_t>(&ptr); // NOLINT
    return (addr % alignment) == 0;
  }

  using func_t = fho::fast_func_t;
}

// SCENARIO("ring_buffer: emplace & pop range")
// {
//   GIVEN("ring with capacity 2 (max size 1)")
//   {
//     static_assert(fho::ring_buffer<func_t, 2>::max_size() == 2);

//     auto ring = fho::ring_buffer<func_t, 2>{};
//     REQUIRE(ring.size() == 0);

//     WHEN("empty")
//     {
//       REQUIRE(ring.begin() == ring.end());
//       THEN("begin() and end() does not throw/crash")
//       {
//         REQUIRE_NOTHROW(ring.begin());
//         REQUIRE_NOTHROW(ring.end());
//         REQUIRE_NOTHROW(ring.try_pop_front(ring.size()));
//         REQUIRE_NOTHROW(ring.try_pop_back(ring.size()));
//       }
//       THEN("clear() does not throw/crash")
//       {
//         REQUIRE_NOTHROW(ring.clear());
//       }
//       AND_WHEN("moved from")
//       {
//         auto _ = std::move(ring);
//         THEN("members do nothing")
//         {
//           REQUIRE_NOTHROW(ring.begin());
//           REQUIRE_NOTHROW(ring.end());
//           REQUIRE_NOTHROW(ring.try_pop_front(ring.size()));
//           REQUIRE_NOTHROW(ring.try_pop_back(ring.size()));
//           REQUIRE(ring.size() == 0);
//           REQUIRE(ring.empty());
//           REQUIRE(ring.try_pop_front(ring.size()).empty());
//           REQUIRE(ring.try_pop_back(ring.size()).empty());
//           REQUIRE_NOTHROW(ring.clear());
//         }
//       }
//     }

//     WHEN("emplace 1")
//     {
//       thread_local int called    = 0;
//       thread_local int destroyed = 0;

//       struct type
//       {
//         type()            = default;
//         type(type const&) = default;
//         type(type&&)      = default;

//         ~type()
//         {
//           ++destroyed;
//         }

//         auto operator=(type const&) -> type& = delete;
//         auto operator=(type&&) -> type&      = delete;

//         void
//         operator()()
//         {
//           ++called;
//         }
//       };

//       ring.emplace_back(type{});
//       REQUIRE(ring.size() == 1);

//       called    = 0;
//       destroyed = 0;
//       AND_WHEN("cleared")
//       {
//         ring.clear();
//         THEN("it resets & destroys emplaced tasks")
//         {
//           REQUIRE(ring.empty());
//           REQUIRE(called == 0);
//           REQUIRE(destroyed == 1);
//         }
//       }
//       AND_WHEN("ring is destroyed")
//       {
//         {
//           auto ring2 = fho::ring_buffer<func_t, 2>{};
//           ring2.emplace_back(type{});
//           called    = 0;
//           destroyed = 0;
//         }
//         THEN("it resets & destroys emplaced tasks")
//         {
//           REQUIRE(called == 0);
//           REQUIRE(destroyed == 1);
//         }
//       }
//       AND_WHEN("iterate tasks")
//       {
//         auto nrtasks = 0;
//         for (auto const& task : ring)
//         {
//           REQUIRE(task);
//           ++nrtasks;
//         }
//         THEN("1 valid task existed")
//         {
//           REQUIRE(nrtasks == 1);
//           REQUIRE(called == 0);
//         }
//       }
//       AND_WHEN("pop range and execute tasks")
//       {
//         auto r = ring.try_pop_front(ring.size());
//         REQUIRE(r.begin() != r.end());
//         REQUIRE(ring.size() == 0);
//         for (auto&& task : r)
//         {
//           task();
//         }
//         THEN("1 valid task executed")
//         {
//           REQUIRE(called == 1);
//         }
//       }
//     }
//     WHEN("emplace")
//     {
//       THEN("a callable that is larger than buffer size can be emplaced")
//       {
//         // NOLINTBEGIN
//         int called       = 0;
//         using big_data_t = std::array<std::byte, fho::details::slot_size * 2>;
//         // both capturing big data & passing as argument
//         ring.emplace_back(
//           [&called, bigData = big_data_t{}](int arg, big_data_t const& data)
//           {
//             called = arg;
//             (void)bigData;
//             (void)data;
//           },
//           16, big_data_t{});
//         // NOLINTEND

//         REQUIRE(ring.size() == 1);
//         for (auto&& task : ring.try_pop_front(ring.size()))
//         {
//           task();
//         }
//         REQUIRE(called == 16);
//       }
//     }
//   }
//   GIVEN("ring with capacity 128")
//   {
//     auto ring = fho::ring_buffer<func_t, 128>{};
//     REQUIRE(ring.size() == 0);

//     WHEN("emplace all")
//     {
//       auto executed = std::vector<std::size_t>(ring.max_size(), 0);
//       for (std::size_t i = 1; i <= ring.max_size(); ++i)
//       {
//         ring.emplace_back(
//           [i, &executed]
//           {
//             executed[i - 1] = i;
//           });
//       }
//       REQUIRE(ring.size() == ring.max_size());

//       AND_WHEN("pop range and execute tasks")
//       {
//         for (auto&& task : ring.try_pop_front(ring.size()))
//         {
//           REQUIRE(task);
//           task();
//         }
//         THEN("128 tasks were executed")
//         {
//           REQUIRE(executed.size() == ring.max_size());
//           for (std::size_t i = 1; i <= ring.max_size(); ++i)
//           {
//             REQUIRE(executed[i - 1] == i);
//           }

//           AND_THEN("ring is empty")
//           {
//             REQUIRE(ring.size() == 0);
//           }

//           AND_WHEN("iterate without executing tasks")
//           {
//             std::ranges::fill(executed, 0);

//             for (auto const& task : ring)
//             {
//               (void)task;
//             }
//             THEN("0 tasks were executed")
//             {
//               REQUIRE(executed.size() == ring.max_size());
//               for (std::size_t i = 0; i < ring.max_size(); ++i)
//               {
//                 REQUIRE(executed[i] == 0);
//               }
//             }
//           }
//         }
//       }
//     }
//   }
// }

// SCENARIO("ring_buffer: pop back")
// {
//   auto ring = fho::ring_buffer<int, 8>{};
//   GIVEN("an empty ring")
//   {
//     auto e = ring.try_pop_back2();
//     REQUIRE_FALSE(e);
//   }
//   GIVEN("a ring with 2 emplaced items")
//   {
//     ring.emplace_back(1);
//     ring.emplace_back(2);
//     WHEN("popping items from back")
//     {
//       THEN("it pops from head")
//       {
//         {
//           auto e = ring.try_pop_back2();
//           REQUIRE(e);
//           REQUIRE(*e == 2);
//           REQUIRE(ring.size() == 1);
//         }
//         {
//           auto e = ring.try_pop_back2();
//           REQUIRE(e);
//           REQUIRE(*e == 1);
//           REQUIRE(ring.size() == 0);
//         }
//         {
//           auto e = ring.try_pop_back2();
//           REQUIRE_FALSE(e);
//         }
//       }
//     }
//   }
//   GIVEN("a ring with 2 sequential-tagged emplaced items")
//   {
//     ring.emplace_back<fho::slot_state::tag_seq>(1);
//     ring.emplace_back<fho::slot_state::tag_seq>(2);
//     WHEN("popping items from back")
//     {
//       THEN("it fails to pop back")
//       {
//         auto e = ring.try_pop_back2();
//         REQUIRE_FALSE(e);
//       }
//     }
//   }
// }

// SCENARIO("ring_buffer: pop back & front")
// {
//   auto ring = fho::ring_buffer<int, 8>{};
//   // GIVEN("a ring with 2 emplaced items")
//   // {
//   //   ring.emplace_back(1);
//   //   ring.emplace_back(2);
//   //   WHEN("popping once from back & front")
//   //   {
//   //     THEN("it pops respective item")
//   //     {
//   //       {
//   //         auto e = ring.try_pop_back2();
//   //         REQUIRE(*e == 2);
//   //         REQUIRE(ring.size() == 1);
//   //       }
//   //       {
//   //         auto e = ring.try_pop_back2();
//   //         REQUIRE(e);
//   //         REQUIRE(*e == 1);
//   //         REQUIRE(ring.size() == 0);
//   //       }
//   //       REQUIRE_FALSE(ring.try_pop_back());
//   //       REQUIRE_FALSE(ring.try_pop_front());
//   //     }
//   //   }
//   // }
//   GIVEN("a ring with gaps (empty) slots")
//   {
//     ring.emplace_back(1);
//     ring.emplace_back(2);
//     ring.emplace_back(3);
//     auto e = ring.try_pop_back2();
//     REQUIRE(e);
//     REQUIRE(*e == 3);
//     ring.emplace_back(4);
//     e = ring.try_pop_back2();
//     REQUIRE(e);
//     REQUIRE(*e == 4);
//     e = ring.try_pop_back2();
//     REQUIRE(e);
//     REQUIRE(*e == 2);
//     ring.emplace_back(5);
//     e = ring.try_pop_back2();
//     REQUIRE(e);
//     REQUIRE(*e == 5);
//     e = ring.try_pop_back2();
//     REQUIRE(e);
//     REQUIRE(*e == 1);

//     // auto itr = ring.begin();
//     // REQUIRE(*itr == 1); // slot: 0
//     // ++itr;
//     // REQUIRE(*itr == 2); // slot: 1
//     // ++itr;
//     // ++itr;
//     // ++itr;
//     // REQUIRE(*itr == 5); // slot: 4
//     // head_: 5, headPop_: 5
//   }
// }

// SCENARIO("ring_buffer: push/pop linearization")
// {
//   auto ring = fho::ring_buffer<int>();

//   ring.emplace_back(2);
//   ring.emplace_back(3);
//   int val = ring.try_pop_back();
//   REQUIRE(val == 3);
//   ring.emplace_back(4);
//   val = ring.try_pop_back();
//   REQUIRE(val == 4);
//   val = ring.try_pop_back();
//   REQUIRE(val == 2);
//   REQUIRE_FALSE(ring.try_pop_back());
//   ring.emplace_back(5);
//   val = ring.try_pop_back();
//   REQUIRE(val == 5);
// }

// SCENARIO("ring_buffer: custom type")
// {
//   GIVEN("a type that fits within one default slot buffer size")
//   {
//     struct my_type
//     {
//       my_type(my_type const&)                    = default;
//       my_type(my_type&&)                         = default;
//       ~my_type()                                 = default;
//       auto operator=(my_type const&) -> my_type& = default;
//       auto operator=(my_type&&) -> my_type&      = default;

//       my_type(int x, float y) // NOLINT
//         : x(x)
//         , y(y)
//       {}

//       int   x;
//       float y;

//       auto operator<=>(my_type const&) const noexcept = default;
//     };

//     static_assert(sizeof(my_type) <= fho::details::slot_size);

//     THEN("slot size is exactly one cache line bytes")
//     {
//       auto ring    = fho::ring_buffer<my_type>{};
//       using slot_t = std::remove_cvref_t<decltype(*ring.begin().base())>;
//       static_assert(sizeof(slot_t) == fho::details::cache_line_size);

//       // basic sanity-tests:
//       ring.emplace_back(1, 2.5f);
//       ring.emplace_back(my_type(3, 4.5f));

//       REQUIRE(ring.size() == 2);
//       REQUIRE(ring.front() == my_type(1, 2.5f));
//       REQUIRE(ring.back() == my_type(3, 4.5f));
//       ring.pop_front();
//       REQUIRE(ring.front() == my_type(3, 4.5f));
//       ring.pop_front();
//       REQUIRE(ring.size() == 0);
//     }
//   }
//   GIVEN("a type that fits within two default slot buffer size")
//   {
//     struct my_too_big_type
//     {
//       std::array<std::byte, fho::details::slot_size * 2> padding = {};
//     };

//     static_assert(sizeof(my_too_big_type) == fho::details::slot_size * 2);

//     THEN("slot size is exactly two cache line bytes")
//     {
//       auto ring    = fho::ring_buffer<my_too_big_type>{};
//       using slot_t = std::remove_cvref_t<decltype(*ring.begin().base())>;
//       static_assert(sizeof(slot_t) == fho::details::cache_line_size * 2);

//       // basic sanity-tests:
//       ring.emplace_back();
//       ring.emplace_back(my_too_big_type());

//       REQUIRE(ring.size() == 2);
//       ring.pop_front();
//       REQUIRE(ring.size() == 1);
//       ring.pop_front();
//       REQUIRE(ring.size() == 0);
//     }
//   }
// }

// SCENARIO("ring_buffer: alignment")
// {
//   auto ring = fho::ring_buffer<func_t, 128>{};
//   GIVEN("a ring of 128 items")
//   {
//     for (std::size_t i = 0; i < ring.max_size(); ++i)
//     {
//       ring.emplace_back([] {});
//     }
//     THEN("all are aligned to cache line boundaries")
//     {
//       for (auto itr = ring.begin(); itr != ring.end(); ++itr)
//       {
//         REQUIRE(is_aligned(*itr.base(), fho::details::cache_line_size));
//       }
//     }
//   }
// }

// SCENARIO("ring_buffer: completion token")
// {
//   auto ring = fho::ring_buffer{};
//   GIVEN("emplace task & store token")
//   {
//     int  called = 0;
//     auto token  = ring.emplace_back(
//       [&called]
//       {
//         ++called;
//       });
//     THEN("token is NOT done when task is discarded")
//     {
//       for (auto const& task : ring)
//       {
//         (void)task; /* discard task */
//       }
//       REQUIRE_FALSE(token.done());
//     }
//     THEN("token is NOT done before task is invoked")
//     {
//       REQUIRE_FALSE(token.done());
//     }
//     THEN("token is done after task was invoked")
//     {
//       for (auto&& task : ring.try_pop_front(ring.size()))
//       {
//         task();
//       }
//       REQUIRE(token.done());
//     }
//     WHEN("token is cancelled")
//     {
//       token.cancel();
//       THEN("it is marked as 'cancelled'")
//       {
//         REQUIRE(token.cancelled());
//       }
//       THEN("task is still executed by ring")
//       {
//         // "cancel" only signals through the token,
//         // meant to be reacted upon by used code -
//         // the task will still be executed if
//         // processed by a background thread.
//         for (auto&& task : ring.try_pop_front(ring.size()))
//         {
//           task();
//         }
//         REQUIRE(called == 1);
//       }
//     }
//   }
// }

SCENARIO("ring_buffer: stress-test")
{
#ifdef NDEBUG
  static constexpr auto capacity = std::size_t{1 << 18};
#else
  static constexpr auto capacity = std::size_t{1 << 12};
#endif

  static constexpr auto keep_consuming = [](auto& ring, auto& producers)
  {
    if constexpr (std::ranges::range<decltype(producers)>)
    {
      return !ring.empty() || std::ranges::any_of(producers,
                                                  [](auto const& p)
                                                  {
                                                    return p.joinable();
                                                  });
    }
    else
    {
      return !ring.empty() || producers.joinable();
    }
  };
  // GIVEN("produce & consume enough for wrap-around")
  // {
  //   auto ring = fho::ring_buffer<func_t, capacity>();

  //   static constexpr auto nr_of_tasks = ring.max_size() * 2;

  //   auto executed = std::atomic_size_t{0};

  //   for (std::size_t i = 0; i < nr_of_tasks; ++i)
  //   {
  //     auto token = ring.emplace_back(
  //       [&executed]
  //       {
  //         ++executed;
  //       });
  //     if (auto e = ring.try_pop_front(); e)
  //     {
  //       e();
  //     }
  //     REQUIRE(token.done());
  //   }

  //   THEN("all tasks are executed")
  //   {
  //     REQUIRE(executed.load() == nr_of_tasks);
  //     for (auto d = ring.data(); d < ring.data() + capacity; ++d)
  //     {
  //       REQUIRE(d->load(std::memory_order_acquire) == fho::slot_state::empty);
  //     }
  //   }
  // }
  // GIVEN("1 producer (front) & 1 consumer (front)")
  // {
  //   static constexpr auto nr_producers  = std::size_t{1};
  //   static constexpr auto nr_consumers  = std::size_t{1};
  //   static constexpr auto nr_to_process = std::size_t{capacity * nr_producers};

  //   auto ring    = fho::ring_buffer<func_t, capacity>();
  //   auto barrier = std::barrier{nr_consumers + nr_producers};

  //   THEN("there are no race conditions")
  //   {
  //     auto k = 0u;
  //     while (true)
  //     {
  //       auto executed = std::atomic_size_t{0};

  //       // start producers
  //       std::vector<std::thread> producers;
  //       for (std::size_t i = 0; i < nr_producers; ++i)
  //       {
  //         producers.emplace_back(
  //           [&barrier, &ring, &executed]
  //           {
  //             auto tokens = fho::token_group{};
  //             barrier.arrive_and_wait();
  //             for (std::size_t i = 0; i < ring.max_size(); ++i)
  //             {
  //               tokens += ring.emplace_front(
  //                 [&executed]
  //                 {
  //                   ++executed;
  //                 });
  //             }
  //             tokens.wait();
  //           });
  //       }
  //       // start consumers
  //       std::vector<std::thread> consumers;
  //       for (std::size_t i = 0; i < nr_consumers; ++i)
  //       {
  //         consumers.emplace_back(
  //           [&barrier, &ring, &producers]
  //           {
  //             barrier.arrive_and_wait();
  //             while (keep_consuming(ring, producers))
  //             {
  //               if (auto e = ring.try_pop_front2(); e)
  //               {
  //                 e();
  //               }
  //             }
  //           });
  //       }

  //       for (auto& producer : producers)
  //       {
  //         producer.join();
  //       }

  //       for (auto& consumer : consumers)
  //       {
  //         consumer.join();
  //       }

  //       auto i = std::uint64_t{0u};
  //       for (auto d = ring.data(); d < ring.data() + capacity; ++d, ++i)
  //       {
  //         INFO("ring-size: " << ring.size() << " (empty: " << ring.empty() << ")"
  //                            << ", slot: " << decltype(ring)::mask(i)
  //                            << " state: " << d->load(std::memory_order_acquire));
  //         REQUIRE(d->template test<fho::slot_state::empty>());
  //       }
  //       REQUIRE(executed.load() == nr_to_process);
  //       REQUIRE(ring.size() == 0);
  //       if (++k % 100 == 0)
  //       {
  //         std::cout << "Gucci after: " << k << "\n";
  //       }
  //     }
  //   }
  // }
  GIVEN("1 producer (back) & 1 consumer (back)")
  {
    static constexpr auto nr_producers  = std::size_t{1};
    static constexpr auto nr_consumers  = std::size_t{2};
    static constexpr auto nr_to_process = std::size_t{capacity * nr_producers};

    auto ring    = fho::ring_buffer<func_t, capacity>();
    auto barrier = std::barrier{nr_consumers + nr_producers};

    THEN("there are no race conditions")
    {
      auto k = 0u;
      while (true)
      {
        auto executed = std::atomic_size_t{0};

        // start producers
        std::vector<std::thread> producers;
        for (std::size_t i = 0; i < nr_producers; ++i)
        {
          producers.emplace_back(
            [&barrier, &ring, &executed]
            {
              auto tokens = fho::token_group{};
              barrier.arrive_and_wait();
              for (std::size_t i = 0; i < ring.max_size(); ++i)
              {
                tokens += ring.emplace_back(
                  [&executed]
                  {
                    ++executed;
                  });
              }
              tokens.wait();
            });
        }
        // start consumers
        std::vector<std::thread> consumers;
        for (std::size_t i = 0; i < nr_consumers; ++i)
        {
          consumers.emplace_back(
            [&barrier, &ring, &producers]
            {
              barrier.arrive_and_wait();
              while (keep_consuming(ring, producers))
              {
                if (auto e = ring.try_pop_back2(); e)
                {
                  e();
                }
              }
            });
        }

        for (auto& producer : producers)
        {
          producer.join();
        }

        for (auto& consumer : consumers)
        {
          consumer.join();
        }

        auto i = std::uint64_t{0u};
        for (auto d = ring.data(); d < ring.data() + capacity; ++d, ++i)
        {
          INFO("ring-size: " << ring.size() << " (empty: " << ring.empty() << ")"
                             << ", slot: " << decltype(ring)::mask(i)
                             << " state: " << d->load(std::memory_order_acquire));
          REQUIRE(d->template test<fho::slot_state::empty>());
        }
        REQUIRE(executed.load() == nr_to_process);
        REQUIRE(ring.size() == 0);
        if (++k % 100 == 0)
        {
          std::cout << "Gucci after: " << k << "\n";
        }
      }
    }
  }
  // GIVEN("5 producers (back) & 1 consumer (front)")
  // {
  //   static constexpr auto nr_producers  = std::size_t{5};
  //   static constexpr auto nr_consumers  = std::size_t{1};
  //   static constexpr auto nr_to_process = std::size_t{capacity * nr_producers};

  //   auto ring    = fho::ring_buffer<func_t, capacity>();
  //   auto barrier = std::barrier{nr_consumers + nr_producers};

  //   THEN("there are no race conditions")
  //   {
  //     auto executed = std::atomic_size_t{0};

  //     // start producers
  //     std::vector<std::thread> producers;
  //     for (std::size_t i = 0; i < nr_producers; ++i)
  //     {
  //       producers.emplace_back(
  //         [&barrier, &ring, &executed]
  //         {
  //           barrier.arrive_and_wait();
  //           for (std::size_t i = 0; i < ring.max_size(); ++i)
  //           {
  //             ring.emplace_back(
  //               [&executed]
  //               {
  //                 ++executed;
  //               });
  //           }
  //         });
  //     }
  //     // start consumers
  //     std::vector<std::thread> consumers;
  //     for (std::size_t i = 0; i < nr_consumers; ++i)
  //     {
  //       consumers.emplace_back(
  //         [&barrier, &ring, &producers]
  //         {
  //           barrier.arrive_and_wait();
  //           while (keep_consuming(ring, producers))
  //           {
  //             if (auto e = ring.try_pop_front(); e)
  //             {
  //               e();
  //             }
  //           }
  //         });
  //     }

  //     for (auto& producer : producers)
  //     {
  //       producer.join();
  //     }

  //     for (auto& consumer : consumers)
  //     {
  //       consumer.join();
  //     }

  //     auto i = std::uint64_t{0u};
  //     for (auto d = ring.data(); d < ring.data() + capacity; ++d, ++i)
  //     {
  //       INFO("ring-size: " << ring.size() << " (empty: " << ring.empty() << ")"
  //                          << ", slot: " << decltype(ring)::mask(i)
  //                          << " state: " << d->load(std::memory_order_acquire));
  //       REQUIRE(d->template test<fho::slot_state::empty>());
  //     }
  //     REQUIRE(executed.load() == nr_to_process);
  //     REQUIRE(ring.size() == 0);
  //   }
  // }
  // GIVEN("1 producer (back) & 5 consumers (front)")
  // {
  //   static constexpr auto nr_producers  = std::size_t{1};
  //   static constexpr auto nr_consumers  = std::size_t{5};
  //   static constexpr auto nr_to_process = std::size_t{capacity * nr_producers};

  //   auto ring    = fho::ring_buffer<func_t, capacity>();
  //   auto barrier = std::barrier{nr_consumers + nr_producers};

  //   THEN("there are no race conditions")
  //   {
  //     auto executed = std::atomic_size_t{0};

  //     // start producers
  //     std::vector<std::thread> producers;
  //     for (std::size_t i = 0; i < nr_producers; ++i)
  //     {
  //       producers.emplace_back(
  //         [&barrier, &ring, &executed]
  //         {
  //           barrier.arrive_and_wait();
  //           for (std::size_t i = 0; i < ring.max_size(); ++i)
  //           {
  //             ring.emplace_back(
  //               [&executed]
  //               {
  //                 ++executed;
  //               });
  //           }
  //         });
  //     }
  //     // start consumers
  //     std::vector<std::thread> consumers;
  //     for (std::size_t i = 0; i < nr_consumers; ++i)
  //     {
  //       consumers.emplace_back(
  //         [&barrier, &ring, &producers, i]
  //         {
  //           barrier.arrive_and_wait();
  //           while (keep_consuming(ring, producers))
  //           {
  //             if (i % 2 == 0)
  //             {
  //               if (auto e = ring.try_pop_front(); e)
  //               {
  //                 e();
  //               }
  //             }
  //             else
  //             {
  //               for (auto e : ring.try_pop_front(3))
  //               {
  //                 e();
  //               }
  //             }
  //           }
  //         });
  //     }

  //     for (auto& producer : producers)
  //     {
  //       producer.join();
  //     }

  //     for (auto& consumer : consumers)
  //     {
  //       consumer.join();
  //     }

  //     auto i = std::uint64_t{0u};
  //     for (auto d = ring.data(); d < ring.data() + capacity; ++d, ++i)
  //     {
  //       INFO("ring-size: " << ring.size() << " (empty: " << ring.empty() << ")"
  //                          << ", slot: " << decltype(ring)::mask(i)
  //                          << " state: " << d->load(std::memory_order_acquire));
  //       REQUIRE(d->template test<fho::slot_state::empty>());
  //     }
  //     REQUIRE(executed.load() == nr_to_process);
  //     REQUIRE(ring.size() == 0);
  //   }
  // }
  // GIVEN("4 producers (back) & 4 consumers (front)")
  // {
  //   static constexpr auto nr_producers  = std::size_t{4};
  //   static constexpr auto nr_consumers  = std::size_t{4};
  //   static constexpr auto nr_to_process = std::size_t{capacity * nr_producers};

  //   auto ring    = fho::ring_buffer<func_t, capacity>();
  //   auto barrier = std::barrier{nr_consumers + nr_producers};

  //   THEN("there are no race conditions")
  //   {
  //     auto executed = std::atomic_size_t{0};

  //     // start producers
  //     std::vector<std::thread> producers;
  //     for (std::size_t i = 0; i < nr_producers; ++i)
  //     {
  //       producers.emplace_back(
  //         [&barrier, &ring, &executed]
  //         {
  //           barrier.arrive_and_wait();
  //           for (std::size_t i = 0; i < ring.max_size(); ++i)
  //           {
  //             ring.emplace_back(
  //               [&executed]
  //               {
  //                 ++executed;
  //               });
  //           }
  //         });
  //     }
  //     // start consumers
  //     std::vector<std::thread> consumers;
  //     for (std::size_t i = 0; i < nr_consumers; ++i)
  //     {
  //       consumers.emplace_back(
  //         [&barrier, &ring, &producers, i]
  //         {
  //           barrier.arrive_and_wait();
  //           while (keep_consuming(ring, producers))
  //           {
  //             if (i % 2 == 0)
  //             {
  //               if (auto e = ring.try_pop_front(); e)
  //               {
  //                 e();
  //               }
  //             }
  //             else
  //             {
  //               for (auto e : ring.try_pop_front(3))
  //               {
  //                 e();
  //               }
  //             }
  //           }
  //         });
  //     }

  //     for (auto& producer : producers)
  //     {
  //       producer.join();
  //     }

  //     for (auto& consumer : consumers)
  //     {
  //       consumer.join();
  //     }

  //     auto i = std::uint64_t{0u};
  //     for (auto d = ring.data(); d < ring.data() + capacity; ++d, ++i)
  //     {
  //       INFO("ring-size: " << ring.size() << " (empty: " << ring.empty() << ")"
  //                          << ", slot: " << decltype(ring)::mask(i)
  //                          << " state: " << d->load(std::memory_order_acquire));
  //       REQUIRE(d->template test<fho::slot_state::empty>());
  //     }
  //     REQUIRE(executed.load() == nr_to_process);
  //     REQUIRE(ring.size() == 0);
  //   }
  // }
  // GIVEN("3 producers & 3 consumers (front) & 3 consumers (back)")
  // {
  //   static constexpr auto nr_producers  = std::size_t{3};
  //   static constexpr auto nr_consumersf = std::size_t{3};
  //   static constexpr auto nr_consumersb = std::size_t{3};
  //   static constexpr auto nr_to_process = std::size_t{capacity * (nr_consumersf +
  //   nr_consumersb)};

  //   auto ring    = fho::ring_buffer<func_t, capacity>();
  //   auto barrier = std::barrier{nr_producers + nr_consumersf + nr_consumersb};

  //   static_assert(nr_to_process % nr_producers == 0, "All tasks must be submitted");

  //   THEN("there are no race conditions")
  //   {
  //     auto executed = std::atomic_size_t{0};

  //     // start producers
  //     std::vector<std::thread> producers;
  //     for (std::size_t i = 0; i < nr_producers; ++i)
  //     {
  //       producers.emplace_back(
  //         [&barrier, &ring, &executed]
  //         {
  //           barrier.arrive_and_wait();
  //           for (std::size_t i = 0; i < nr_to_process / nr_producers; ++i)
  //           {
  //             ring.emplace_back(
  //               [&executed]
  //               {
  //                 ++executed;
  //               });
  //           }
  //         });
  //     }
  //     // start consumers (front)
  //     std::vector<std::thread> consumersf;
  //     for (std::size_t i = 0; i < nr_consumersf; ++i)
  //     {
  //       consumersf.emplace_back(
  //         [&barrier, &ring, &producers, i]
  //         {
  //           barrier.arrive_and_wait();
  //           while (keep_consuming(ring, producers))
  //           {
  //             if (i % 2 == 0)
  //             {
  //               if (auto e = ring.try_pop_front(); e)
  //               {
  //                 e();
  //               }
  //             }
  //             else
  //             {
  //               for (auto e : ring.try_pop_front(3))
  //               {
  //                 e();
  //               }
  //             }
  //           }
  //         });
  //     }
  //     // start consumers (back)
  //     std::vector<std::thread> consumersb;
  //     for (std::size_t i = 0; i < nr_consumersb; ++i)
  //     {
  //       consumersb.emplace_back(
  //         [&barrier, &ring, &producers, i]
  //         {
  //           barrier.arrive_and_wait();
  //           while (keep_consuming(ring, producers))
  //           {
  //             if (i % 2 == 0)
  //             {
  //               if (auto e = ring.try_pop_back(); e)
  //               {
  //                 e();
  //               }
  //             }
  //             else
  //             {
  //               for (auto e : ring.try_pop_back(3))
  //               {
  //                 e();
  //               }
  //             }
  //           }
  //         });
  //     }

  //     for (auto& p : producers)
  //     {
  //       p.join();
  //     }

  //     for (auto& c : consumersf)
  //     {
  //       c.join();
  //     }

  //     for (auto& c : consumersb)
  //     {
  //       c.join();
  //     }

  //     auto i = std::uint64_t{0u};
  //     for (auto d = ring.data(); d < ring.data() + capacity; ++d, ++i)
  //     {
  //       INFO("ring-size: " << ring.size() << " (empty: " << ring.empty() << ")"
  //                          << ", slot: " << decltype(ring)::mask(i)
  //                          << " state: " << d->load(std::memory_order_acquire));
  //       REQUIRE(d->template test<fho::slot_state::empty>());
  //     }
  //     REQUIRE(executed.load() == nr_to_process);
  //     REQUIRE(ring.size() == 0);
  //   }
  // }
}

// SCENARIO("ring_buffer: standard algorithms")
// {
//   GIVEN("ring with capacity of 65K")
//   {
//     static constexpr auto capacity = 1 << 16;
//     auto                  ring     = fho::ring_buffer<func_t, capacity>{};
//     REQUIRE(ring.size() == 0);

//     auto executed = std::atomic_size_t{0};

//     WHEN("emplace all")
//     {
//       while (ring.size() < ring.max_size())
//       {
//         ring.emplace_back(
//           [&executed]
//           {
//             ++executed;
//           });
//       }
//       auto r = ring.try_pop_front(ring.size());
//       AND_WHEN("std::for_each")
//       {
//         std::ranges::for_each(r,
//                               [](auto&& task)
//                               {
//                                 task();
//                               });
//         THEN("all tasks executed")
//         {
//           REQUIRE(executed.load() == ring.max_size());
//           REQUIRE(ring.size() == 0);
//           for (auto d = ring.data(); d < ring.data() + capacity; ++d)
//           {
//             REQUIRE(d->load(std::memory_order_acquire) == fho::slot_state::empty);
//           }
//         }
//       }
//     }
//   }
// }
