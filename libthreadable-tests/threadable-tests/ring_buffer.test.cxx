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

SCENARIO("ring_buffer: push & pop")
{
  GIVEN("ring with capacity 2")
  {
    static_assert(fho::ring_buffer<func_t, 2>::max_size() == 2);

    auto ring = fho::ring_buffer<func_t, 2>{};
    REQUIRE(ring.size() == 0);

    WHEN("empty")
    {
      THEN("begin() and end() does not throw/crash")
      {
        REQUIRE_NOTHROW(ring.begin());
        REQUIRE_NOTHROW(ring.end());
      }
      THEN("clear() does not throw/crash")
      {
        REQUIRE_NOTHROW(ring.clear());
      }
      AND_WHEN("moved from")
      {
        auto _ = std::move(ring);
        THEN("members do nothing")
        {
          REQUIRE_NOTHROW(ring.begin());
          REQUIRE_NOTHROW(ring.end());
          REQUIRE(ring.size() == 0);
          REQUIRE(ring.empty());
          REQUIRE_NOTHROW(ring.clear());
        }
      }
    }

    WHEN("emplace 1")
    {
      thread_local int called    = 0;
      thread_local int destroyed = 0;

      struct type
      {
        type()            = default;
        type(type const&) = default;
        type(type&&)      = default;

        ~type()
        {
          ++destroyed;
        }

        auto operator=(type const&) -> type& = delete;
        auto operator=(type&&) -> type&      = delete;

        void
        operator()()
        {
          ++called;
        }
      };

      ring.emplace_back(type{});
      REQUIRE(ring.size() == 1);

      called    = 0;
      destroyed = 0;
      AND_WHEN("cleared")
      {
        ring.clear();
        THEN("it resets & destroys emplaced tasks")
        {
          REQUIRE(ring.empty());
          REQUIRE(called == 0);
          REQUIRE(destroyed == 1);
        }
      }
      AND_WHEN("ring is destroyed")
      {
        {
          auto ring2 = fho::ring_buffer<func_t, 2>{};
          ring2.emplace_back(type{});
          called    = 0;
          destroyed = 0;
        }
        THEN("it resets & destroys emplaced tasks")
        {
          REQUIRE(called == 0);
          REQUIRE(destroyed == 1);
        }
      }
      AND_WHEN("iterate tasks")
      {
        auto nrtasks = 0;
        for (auto const& task : ring)
        {
          REQUIRE(task);
          ++nrtasks;
        }
        THEN("1 valid task existed")
        {
          REQUIRE(nrtasks == 1);
          REQUIRE(called == 0);
        }
      }
      AND_WHEN("pop and execute tasks")
      {
        while (auto task = ring.try_pop_front())
        {
          task();
        }
        THEN("1 valid task executed")
        {
          REQUIRE(called == 1);
        }
      }
    }
    WHEN("emplace")
    {
      THEN("a callable that is larger than buffer size can be emplaced")
      {
        // NOLINTBEGIN
        int called       = 0;
        using big_data_t = std::array<std::byte, fho::details::slot_size * 2>;
        // both capturing big data & passing as argument
        ring.emplace_back(
          [&called, bigData = big_data_t{}](int arg, big_data_t const& data)
          {
            called = arg;
            (void)bigData;
            (void)data;
          },
          16, big_data_t{});
        // NOLINTEND

        REQUIRE(ring.size() == 1);
        while (auto task = ring.try_pop_front())
        {
          task();
        }
        REQUIRE(called == 16);
      }
    }
  }
  GIVEN("ring with capacity 128")
  {
    auto ring = fho::ring_buffer<func_t, 128>{};
    REQUIRE(ring.size() == 0);

    WHEN("emplace all")
    {
      auto executed = std::vector<std::size_t>(ring.max_size(), 0);
      for (std::size_t i = 1; i <= ring.max_size(); ++i)
      {
        ring.emplace_back(
          [i, &executed]
          {
            executed[i - 1] = i;
          });
      }
      REQUIRE(ring.size() == ring.max_size());

      AND_WHEN("pop and execute tasks")
      {
        while (auto task = ring.try_pop_front())
        {
          task();
        }
        THEN("128 tasks were executed")
        {
          REQUIRE(executed.size() == ring.max_size());
          for (std::size_t i = 1; i <= ring.max_size(); ++i)
          {
            REQUIRE(executed[i - 1] == i);
          }

          AND_THEN("ring is empty")
          {
            REQUIRE(ring.size() == 0);
          }

          AND_WHEN("iterate without executing tasks")
          {
            std::ranges::fill(executed, 0);

            for (auto const& task : ring)
            {
              (void)task;
            }
            THEN("0 tasks were executed")
            {
              REQUIRE(executed.size() == ring.max_size());
              for (std::size_t i = 0; i < ring.max_size(); ++i)
              {
                REQUIRE(executed[i] == 0);
              }
            }
          }
        }
      }
    }
  }
}

SCENARIO("ring_buffer: try pop")
{
  auto ring = fho::ring_buffer<int, 8>{};
  static_assert(ring.max_size() == 8);
  GIVEN("ring with capacity 6")
  {
    WHEN("push back 3 items")
    {
      ring.emplace_back(1);
      ring.emplace_back(2);
      ring.emplace_back(3);
      THEN("3 items can be popped from front")
      {
        auto e = ring.try_pop_front();
        REQUIRE(e);
        REQUIRE(*e == 1);
        e = ring.try_pop_front();
        REQUIRE(e);
        REQUIRE(*e == 2);
        e = ring.try_pop_front();
        REQUIRE(e);
        REQUIRE(*e == 3);
        REQUIRE_FALSE(ring.try_pop_front());
      }
      THEN("3 items can be popped from back")
      {
        // NOTE: Can only pop from back once last pop is
        //       reset to 'empty', else it's considered
        //       unavailable. So reclaim slot each time.
        {
          auto e = ring.try_pop_back();
          REQUIRE(e);
          REQUIRE(*e == 3);
        }
        {
          auto e = ring.try_pop_back();
          REQUIRE(e);
          REQUIRE(*e == 2);
        }
        {
          auto e = ring.try_pop_back();
          REQUIRE(e);
          REQUIRE(*e == 1);
        }
        REQUIRE_FALSE(ring.try_pop_back());
      }
      THEN("1 item is popped from front, 2 from back")
      {
        // NOTE: Can only pop from back once last pop is
        //       reset to 'empty', else it's considered
        //       unavailable.
        {
          auto e = ring.try_pop_front();
          REQUIRE(e);
          REQUIRE(*e == 1);
        }
        {
          auto e = ring.try_pop_back();
          REQUIRE(e);
          REQUIRE(*e == 3);
        }
        {
          auto e = ring.try_pop_back();
          REQUIRE(e);
          REQUIRE(*e == 2);
        }
        REQUIRE_FALSE(ring.try_pop_front());
        REQUIRE_FALSE(ring.try_pop_back());
      }
    }
    WHEN("push back 2 sequentially tagged items")
    {
      ring.emplace_back<fho::slot_state::tag_seq>(1);
      ring.emplace_back<fho::slot_state::tag_seq>(2);
      THEN("item can't be popped from front until previous is processed")
      {
        {
          auto e = ring.try_pop_front();
          REQUIRE(e);
          REQUIRE(*e == 1);
          REQUIRE_FALSE(ring.try_pop_front());
        }
        auto e = ring.try_pop_front();
        REQUIRE(e);
        REQUIRE(*e == 2);
      }
    }
  }
}

SCENARIO("ring_buffer: custom type")
{
  GIVEN("a type that fits within one default slot buffer size")
  {
    struct my_type
    {
      my_type(my_type const&)                    = default;
      my_type(my_type&&)                         = default;
      ~my_type()                                 = default;
      auto operator=(my_type const&) -> my_type& = default;
      auto operator=(my_type&&) -> my_type&      = default;

      my_type(int x, float y) // NOLINT
        : x(x)
        , y(y)
      {}

      int   x;
      float y;

      auto operator<=>(my_type const&) const noexcept = default;
    };

    static_assert(sizeof(my_type) <= fho::details::slot_size);

    THEN("slot size is exactly one cache line bytes")
    {
      auto ring    = fho::ring_buffer<my_type>{};
      using slot_t = std::remove_cvref_t<decltype(*ring.begin().base())>;
      static_assert(sizeof(slot_t) == fho::details::cache_line_size);

      // basic sanity-tests:
      ring.emplace_back(1, 2.5f);
      ring.emplace_back(my_type(3, 4.5f));

      REQUIRE(ring.size() == 2);
      REQUIRE(ring.front() == my_type(1, 2.5f));
      REQUIRE(ring.back() == my_type(3, 4.5f));
      ring.pop();
      REQUIRE(ring.front() == my_type(3, 4.5f));
      ring.pop();
      REQUIRE(ring.size() == 0);
    }
  }
  GIVEN("a type that fits within two default slot buffer size")
  {
    struct my_too_big_type
    {
      std::array<std::byte, fho::details::slot_size * 2> padding = {};
    };

    static_assert(sizeof(my_too_big_type) == fho::details::slot_size * 2);

    THEN("slot size is exactly two cache line bytes")
    {
      auto ring    = fho::ring_buffer<my_too_big_type>{};
      using slot_t = std::remove_cvref_t<decltype(*ring.begin().base())>;
      static_assert(sizeof(slot_t) == fho::details::cache_line_size * 2);

      // basic sanity-tests:
      ring.emplace_back();
      ring.emplace_back(my_too_big_type());

      REQUIRE(ring.size() == 2);
      ring.pop();
      REQUIRE(ring.size() == 1);
      ring.pop();
      REQUIRE(ring.size() == 0);
    }
  }
}

SCENARIO("ring_buffer: alignment")
{
  auto ring = fho::ring_buffer<func_t, 128>{};
  GIVEN("a ring of 128 items")
  {
    for (std::size_t i = 0; i < ring.max_size(); ++i)
    {
      ring.emplace_back([] {});
    }
    THEN("all are aligned to cache line boundaries")
    {
      for (auto itr = ring.begin(); itr != ring.end(); ++itr)
      {
        REQUIRE(is_aligned(*itr.base(), fho::details::cache_line_size));
      }
    }
  }
}

SCENARIO("ring_buffer: completion token")
{
  auto ring = fho::ring_buffer{};
  GIVEN("emplace task & store token")
  {
    int  called = 0;
    auto token  = ring.emplace_back(
      [&called]
      {
        ++called;
      });
    THEN("token is NOT done when task is discarded")
    {
      for (auto const& task : ring)
      {
        (void)task; /* discard task */
      }
      REQUIRE_FALSE(token.done());
    }
    THEN("token is NOT done before task is invoked")
    {
      REQUIRE_FALSE(token.done());
    }
    THEN("token is done after task was invoked")
    {
      while (auto task = ring.try_pop_front())
      {
        task();
      }
      REQUIRE(token.done());
    }
    WHEN("token is cancelled")
    {
      token.cancel();
      THEN("it is marked as 'cancelled'")
      {
        REQUIRE(token.cancelled());
      }
      THEN("task is still executed by ring")
      {
        // "cancel" only signals through the token,
        // meant to be reacted upon by used code -
        // the task will still be executed if
        // processed by a background thread.
        while (auto task = ring.try_pop_front())
        {
          task();
        }
        REQUIRE(called == 1);
      }
    }
  }
}

SCENARIO("ring_buffer: stress-test")
{
  GIVEN("push & pop enough for wrap-around")
  {
    static constexpr auto capacity = std::size_t{1 << 8};

    auto ring = fho::ring_buffer<func_t, capacity>();

    static constexpr auto nr_of_tasks   = ring.max_size() * 2;
    std::size_t           tasksExecuted = 0;

    for (std::size_t i = 0; i < nr_of_tasks; ++i)
    {
      auto token = ring.emplace_back(
        [&tasksExecuted]
        {
          ++tasksExecuted;
        });
      while (auto task = ring.try_pop_front())
      {
        task();
      }
      REQUIRE(token.done());
    }

    THEN("all tasks are executed")
    {
      REQUIRE(tasksExecuted == nr_of_tasks);
    }
  }

  GIVEN("1 producer & 1 consumer (front)")
  {
    static constexpr auto capacity = std::size_t{1 << 8};

    auto ring = fho::ring_buffer<func_t, capacity>();

    THEN("there are no race conditions")
    {
      auto tasksExecuted = std::atomic_size_t{0};
      {
        // start producer
        auto producer = std::thread(
          [&ring, &tasksExecuted]
          {
            for (std::size_t i = 0; i < ring.max_size(); ++i)
            {
              ring.emplace_back(
                [&tasksExecuted]
                {
                  ++tasksExecuted;
                });
            }
          });
        // start consumer
        auto consumer = std::thread(
          [&ring, &producer]
          {
            while (producer.joinable() || !ring.empty())
            {
              while (auto task = ring.try_pop_front())
              {
                task();
              }
            }
          });

        producer.join();
        consumer.join();
      }
      REQUIRE(tasksExecuted.load() == ring.max_size());
    }
  }

  GIVEN("1 producers & 1 consumers (back)")
  {
    static constexpr auto capacity      = std::size_t{1 << 16};
    static constexpr auto nr_producers  = std::size_t{1};
    static constexpr auto nr_consumers  = std::size_t{1};
    static constexpr auto nr_to_process = std::size_t{capacity * nr_producers};

    auto ring    = fho::ring_buffer<func_t, capacity>();
    auto barrier = std::barrier{nr_producers + nr_consumers};

    THEN("there are no race conditions")
    {
      auto processed = std::atomic_size_t{0};

      // start producers
      auto producers = std::vector<std::thread>{};
      for (std::size_t i = 0; i < nr_producers; ++i)
      {
        producers.emplace_back(
          [&barrier, &ring, &processed]
          {
            barrier.arrive_and_wait();
            static_assert(nr_to_process % nr_producers == 0, "All tasks must be submitted");
            for (std::size_t i = 0; i < nr_to_process / nr_producers; ++i)
            {
              ring.emplace_back(
                [&processed]
                {
                  ++processed;
                });
            }
          });
      }
      // start consumers
      auto consumers = std::vector<std::thread>{};
      for (std::size_t i = 0; i < nr_consumers; ++i)
      {
        consumers.emplace_back(
          [&barrier, &ring, &processed]
          {
            barrier.arrive_and_wait();
            while (processed < nr_to_process)
            {
              if (auto t = ring.try_pop_back())
              {
                t();
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
                           << ", index: " << i << ", slot: " << ring.mask(i)
                           << " state: " << d->load(std::memory_order_acquire));
        REQUIRE(d->template test<fho::slot_state::empty>());
      }
      REQUIRE(processed.load() == nr_to_process);
      REQUIRE(ring.size() == 0);
    }
  }

  GIVEN("4 producers & 1 consumers (front)")
  {
    static constexpr auto capacity      = std::size_t{1 << 16};
    static constexpr auto nr_producers  = std::size_t{4};
    static constexpr auto nr_consumers  = std::size_t{1};
    static constexpr auto nr_to_process = std::size_t{capacity * nr_producers};

    auto ring    = fho::ring_buffer<func_t, capacity>();
    auto barrier = std::barrier{nr_producers + nr_consumers};

    THEN("there are no race conditions")
    {
      auto processed = std::atomic_size_t{0};

      // start producers
      auto producers = std::vector<std::thread>{};
      for (std::size_t i = 0; i < nr_producers; ++i)
      {
        producers.emplace_back(
          [&barrier, &ring, &processed]
          {
            barrier.arrive_and_wait();
            static_assert(nr_to_process % nr_producers == 0, "All tasks must be submitted");
            for (std::size_t i = 0; i < nr_to_process / nr_producers; ++i)
            {
              ring.emplace_back(
                [&processed]
                {
                  ++processed;
                });
            }
          });
      }
      // start consumers
      auto consumers = std::vector<std::thread>{};
      for (std::size_t i = 0; i < nr_consumers; ++i)
      {
        consumers.emplace_back(
          [&barrier, &ring, &processed]
          {
            barrier.arrive_and_wait();
            while (processed < nr_to_process)
            {
              if (auto t = ring.try_pop_front())
              {
                t();
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
                           << ", index: " << i << ", slot: " << ring.mask(i)
                           << " state: " << d->load(std::memory_order_acquire));
        REQUIRE(d->template test<fho::slot_state::empty>());
      }
      REQUIRE(processed.load() == nr_to_process);
      REQUIRE(ring.size() == 0);
    }
  }

  GIVEN("2 producers & 1 consumers (back)")
  {
    // This means a highly contended path, so limit stress-test
    // to only TWO PRODUCERS and ONE CONSUMER.
    static constexpr auto capacity      = std::size_t{1 << 16};
    static constexpr auto nr_producers  = std::size_t{2};
    static constexpr auto nr_consumers  = std::size_t{1};
    static constexpr auto nr_to_process = std::size_t{capacity * nr_producers};

    auto ring    = fho::ring_buffer<func_t, capacity>();
    auto barrier = std::barrier{nr_producers + nr_consumers};

    THEN("there are no race conditions")
    {
      auto processed = std::atomic_size_t{0};

      // start producers
      auto producers = std::vector<std::thread>{};
      for (std::size_t i = 0; i < nr_producers; ++i)
      {
        producers.emplace_back(
          [&barrier, &ring, &processed]
          {
            barrier.arrive_and_wait();
            static_assert(nr_to_process % nr_producers == 0, "All tasks must be submitted");
            for (std::size_t i = 0; i < nr_to_process / nr_producers; ++i)
            {
              ring.emplace_back(
                [&processed]
                {
                  ++processed;
                });
            }
          });
      }
      // start consumers
      auto consumers = std::vector<std::thread>{};
      for (std::size_t i = 0; i < nr_consumers; ++i)
      {
        consumers.emplace_back(
          [&barrier, &ring, &processed]
          {
            barrier.arrive_and_wait();
            while (processed < nr_to_process)
            {
              if (auto t = ring.try_pop_back())
              {
                t();
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
                           << ", index: " << i << ", slot: " << ring.mask(i)
                           << " state: " << d->load(std::memory_order_acquire));
        REQUIRE(d->template test<fho::slot_state::empty>());
      }
      REQUIRE(processed.load() == nr_to_process);
      REQUIRE(ring.size() == 0);
    }
  }

  GIVEN("1 producers & 4 consumers (front)")
  {
    static constexpr auto capacity      = std::size_t{1 << 16};
    static constexpr auto nr_producers  = std::size_t{1};
    static constexpr auto nr_consumers  = std::size_t{4};
    static constexpr auto nr_to_process = std::size_t{capacity * nr_producers};

    auto ring    = fho::ring_buffer<func_t, capacity>();
    auto barrier = std::barrier{nr_producers + nr_consumers};

    THEN("there are no race conditions")
    {
      auto processed = std::atomic_size_t{0};

      // start producers
      auto producers = std::vector<std::thread>{};
      for (std::size_t i = 0; i < nr_producers; ++i)
      {
        producers.emplace_back(
          [&barrier, &ring, &processed]
          {
            barrier.arrive_and_wait();
            static_assert(nr_to_process % nr_producers == 0, "All tasks must be submitted");
            for (std::size_t i = 0; i < nr_to_process / nr_producers; ++i)
            {
              ring.emplace_back(
                [&processed]
                {
                  ++processed;
                });
            }
          });
      }
      // start consumers
      auto consumers = std::vector<std::thread>{};
      for (std::size_t i = 0; i < nr_consumers; ++i)
      {
        consumers.emplace_back(
          [&barrier, &ring, &processed]
          {
            barrier.arrive_and_wait();
            while (processed < nr_to_process)
            {
              if (auto t = ring.try_pop_front())
              {
                t();
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
                           << ", index: " << i << ", slot: " << ring.mask(i)
                           << " state: " << d->load(std::memory_order_acquire));
        REQUIRE(d->template test<fho::slot_state::empty>());
      }
      REQUIRE(processed.load() == nr_to_process);
      REQUIRE(ring.size() == 0);
    }
  }

  GIVEN("1 producers & 2 consumers (back)")
  {
    // This means a highly contended path, so limit stress-test
    // to only ONE PRODUCER and TWO CONSUMERS.
    static constexpr auto capacity      = std::size_t{1 << 16};
    static constexpr auto nr_producers  = std::size_t{1};
    static constexpr auto nr_consumers  = std::size_t{2};
    static constexpr auto nr_to_process = std::size_t{capacity * nr_producers};

    auto ring    = fho::ring_buffer<func_t, capacity>();
    auto barrier = std::barrier{nr_producers + nr_consumers};

    THEN("there are no race conditions")
    {
      auto processed = std::atomic_size_t{0};

      // start producers
      auto producers = std::vector<std::thread>{};
      for (std::size_t i = 0; i < nr_producers; ++i)
      {
        producers.emplace_back(
          [&barrier, &ring, &processed]
          {
            barrier.arrive_and_wait();
            static_assert(nr_to_process % nr_producers == 0, "All tasks must be submitted");
            for (std::size_t i = 0; i < nr_to_process / nr_producers; ++i)
            {
              ring.emplace_back(
                [&processed]
                {
                  ++processed;
                });
            }
          });
      }
      // start consumers
      auto consumers = std::vector<std::thread>{};
      for (std::size_t i = 0; i < nr_consumers; ++i)
      {
        consumers.emplace_back(
          [&barrier, &ring, &processed]
          {
            barrier.arrive_and_wait();
            while (processed < nr_to_process)
            {
              if (auto t = ring.try_pop_back())
              {
                t();
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
                           << ", index: " << i << ", slot: " << ring.mask(i)
                           << " state: " << d->load(std::memory_order_acquire));
        REQUIRE(d->template test<fho::slot_state::empty>());
      }
      REQUIRE(processed.load() == nr_to_process);
      REQUIRE(ring.size() == 0);
    }
  }
}

SCENARIO("ring_buffer: standard algorithms")
{
  GIVEN("ring with capacity of 65K")
  {
    static constexpr auto capacity = 1 << 16;
    auto                  ring     = fho::ring_buffer<func_t, capacity>{};
    REQUIRE(ring.size() == 0);

    auto iterated = std::atomic_size_t{0};
    WHEN("emplace all")
    {
      for (auto i = std::size_t{0}; i < capacity; ++i)
      {
        ring.emplace_back();
      }
      AND_WHEN("std::for_each")
      {
        std::ranges::for_each(ring,
                              [&iterated](auto const& task)
                              {
                                ++iterated;
                              });
        THEN("all tasks executed")
        {
          REQUIRE(iterated.load() == ring.max_size());
        }
      }
    }
  }
}
