#include <threadable/queue.hxx>
#include <threadable/doctest_include.hxx>

#include <algorithm>
#include <chrono>
#include <functional>
#include <latch>
#include <type_traits>
#include <thread>

SCENARIO("queue: push, pop, steal")
{
  auto queue = threadable::queue{};

  GIVEN("queue is empty")
  {
    THEN("size is 0")
    {
      REQUIRE(queue.size() == 0);
      REQUIRE(queue.empty());
    }
    WHEN("pop one job")
    {
      auto job = queue.pop();
      THEN("size is 0")
      {
        REQUIRE(queue.size() == 0);
        REQUIRE(queue.empty());
      }
      THEN("no job is returned")
      {
        REQUIRE_FALSE(job);
      }
    }
    WHEN("steal one job")
    {
      auto job = queue.steal();
      THEN("size is 0")
      {
        REQUIRE(queue.size() == 0);
        REQUIRE(queue.empty());
      }
      THEN("no job is returned")
      {
        REQUIRE_FALSE(job);
      }
    }
  }
  GIVEN("push two jobs")
  {
    queue.push([]{});
    queue.push([]{});
    THEN("size is 2")
    {
      REQUIRE(queue.size() == 2);
      REQUIRE_FALSE(queue.empty());
    }
    WHEN("pop two jobs")
    {
      (void)queue.pop();
      (void)queue.pop();
      THEN("size is 0")
      {
        REQUIRE(queue.size() == 0);
      }
    }
    WHEN("pop one job")
    {
      (void)queue.pop();
      THEN("size is 1")
      {
        REQUIRE(queue.size() == 1);
      }
      AND_WHEN("steal one job")
      {
        (void)queue.steal();
        THEN("size is 0")
        {
          REQUIRE(queue.size() == 0);
        }
      }
    }
    WHEN("steal one job")
    {
      (void)queue.steal();
      THEN("size is 1")
      {
        REQUIRE(queue.size() == 1);
      }
      AND_WHEN("pop one job")
      {
        (void)queue.pop();
        THEN("size is 0")
        {
          REQUIRE(queue.size() == 0);
        }
      }
    }
  }
}

namespace
{
  void free_func(int& arg)
  {
    ++arg;
  }
}

SCENARIO("queue: execution")
{
  auto queue = threadable::queue{};
  GIVEN("push job")
  {
    WHEN("popped")
    {
      queue.push([]{});
      auto job = queue.pop();
      THEN("job is true before invoked")
      {
        REQUIRE(job);
      }
      THEN("job is false after invoked")
      {
        job();
        REQUIRE_FALSE(job);
      }
    }
    int called = 0;
    WHEN("lambda")
    {
      queue.push([&called]{ ++called; });
      THEN("popped job invokes it")
      {
        auto job = queue.pop();
        job();
        REQUIRE(called == 1);
      }
    }
    WHEN("free function")
    {
      queue.push(free_func, std::ref(called));
      THEN("popped job invokes it")
      {
        auto job = queue.pop();
        job();
        REQUIRE(called == 1);
      }
    }
    WHEN("member function: trivially copyable type")
    {
      struct type
      {
        void func(int& called)
        {
          ++called;
        }
      };
      static_assert(std::is_trivially_copyable_v<type>);
      static_assert(std::is_trivially_destructible_v<type>);

      queue.push(&type::func, type{}, std::ref(called));
      THEN("popped job invokes it")
      {
        auto job = queue.pop();
        job();
        REQUIRE(called == 1);
      }
    }
    WHEN("member function: non-trivially-copyable type")
    {
      struct type
      {
        ~type()
        {
          ++destroyed;
        }
        void func(int& called)
        {
          ++called;
        }

        int& destroyed;
      };
      static_assert(!std::is_trivially_copyable_v<type>);
      static_assert(std::is_destructible_v<type>);

      int destroyed = 0;
      queue.push(&type::func, type{destroyed}, std::ref(called));
      THEN("popped job invokes it")
      {
        auto job = queue.pop();
        called = 0;
        destroyed = 0;
        job();
        REQUIRE(called == 1);

        AND_THEN("destructor is called")
        {
          REQUIRE(destroyed);
        }
      }
    }
  }

  std::vector<int> order;
  GIVEN("push two jobs")
  {
    queue.push([&order]{ order.push_back(1); });
    queue.push([&order]{ order.push_back(2); });
    WHEN("pop & execute jobs")
    {
      while(!queue.empty())
      {
        auto job = queue.pop();
        job();
      }
      THEN("jobs are executed LIFO")
      {
        REQUIRE(order.size() == 2);
        REQUIRE(order[0] == 2);
        REQUIRE(order[1] == 1);
      }
    }
    WHEN("steal & execute jobs")
    {
      while(!queue.empty())
      {
        auto job = queue.steal();
        REQUIRE(job);
        job();
      }
      THEN("jobs are executed FIFO")
      {
        REQUIRE(order.size() == 2);
        REQUIRE(order[0] == 1);
        REQUIRE(order[1] == 2);
      }
    }
    WHEN("pop & execute one job")
    {
      auto job1 = queue.pop();
      job1();
      AND_WHEN("steal & execute one job")
      {
        auto job2 = queue.steal();
        REQUIRE(job2);
        job2();
        THEN("jobs are executed LIFO")
        {
          REQUIRE(order.size() == 2);
          REQUIRE(order[0] == 2);
          REQUIRE(order[1] == 1);
        }
      }
    }
  }
}

SCENARIO("queue: synchronization")
{
  GIVEN("stolen job is executed")
  {
    using clock_t = std::chrono::steady_clock;
    auto stealerDoneTime = clock_t::now();
    auto destroyerDoneTime = clock_t::now();
    std::latch barrier{2};

    auto queuePtr = std::shared_ptr<threadable::queue<2>>(new threadable::queue<2>(), [&](auto* ptr){
      delete ptr;
      destroyerDoneTime = clock_t::now();
    });
    auto& queue = *queuePtr;

    // job stolen (steal() == FIFO) and executed by stealer
    queue.push([&]{
      // stealer: wait until destroyer is inside destructor
      barrier.arrive_and_wait();
      stealerDoneTime = clock_t::now(); 
    });
    // job popped (pop() == LIFO) & implicitly executed by destroyer (see queue::dtor)
    queue.push([&]{
      // queue::dtor invokes this and releases stealer job
      barrier.arrive_and_wait();
    });

    auto job = queue.steal();
    WHEN("queue is simultaneously being destroyed")
    {
      auto stealer = std::thread([&]{
        job();
      });
      auto destroyer = std::thread([&]{
        queuePtr = nullptr;
      });

      stealer.join();
      destroyer.join();

      THEN("it waits for job to finish")
      {
        REQUIRE(stealerDoneTime < destroyerDoneTime);
      }
    }
  }
}

SCENARIO("queue: completion token")
{
  auto queue = threadable::queue{};
  GIVEN("push job & store token")
  {
    auto token = queue.push([]{});
    THEN("token is not done before job is invoked")
    {
      REQUIRE_FALSE(token.done());
    }
    THEN("token is done after job was invoked")
    {
      auto job = queue.pop();
      job();
      REQUIRE(token.done());
    }
    WHEN("waiting on token")
    {
      THEN("it releases after job has executed")
      {
        using clock_t = std::chrono::steady_clock;

        auto waiterDoneTime = clock_t::now();
        std::thread waiter([&token, &waiterDoneTime]{ 
          token.wait();
          waiterDoneTime = clock_t::now();
        });

        auto job = queue.pop();
        job();
        auto jobDoneTime = clock_t::now();
        waiter.join();

        REQUIRE(jobDoneTime < waiterDoneTime);
      }
    }
  }
}

SCENARIO("queue: stress-test")
{
  static constexpr std::size_t nr_of_jobs = 1 << 16;
  // allocate on heap to not overflow stack
  auto queuePtr = std::make_shared<threadable::queue<nr_of_jobs>>();
  auto& queue = *queuePtr;
  GIVEN("one producer/consumer & multiple stealers")
  {
    THEN("there are no race conditions")
    {
      std::atomic_size_t counter{0};
      // pre-fill half
      for(std::size_t i = 0; i < nr_of_jobs/2; ++i)
      {
        queue.push([&counter]{ ++counter; });
      }
      {
        // start pusher/popper
        std::thread producer([&queue, &counter]{
          // push remaining half
          for(std::size_t i = 0; i < nr_of_jobs/2; ++i)
          {
            queue.push([&counter]{ ++counter; });
          }
          while(!queue.empty())
          {
            if(auto job = queue.pop(); job)
            {
              job();
            }
          }
        });
        // start stealers
        std::vector<std::thread> stealers;
        for(std::size_t i = 0; i < std::min(10u, std::thread::hardware_concurrency()); ++i)
        {
          stealers.emplace_back([&queue]{
            while(!queue.empty())
            {
              if(auto job = queue.steal(); job)
              {
                job();
              }
            }
          });
        }

        producer.join();
        std::for_each(stealers.begin(), stealers.end(), [](auto& thread) { thread.join(); });
      }
      REQUIRE(counter.load() == nr_of_jobs);
    }
  }
}