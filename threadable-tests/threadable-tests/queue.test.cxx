#include <threadable/queue.hxx>
#include <threadable-tests/doctest_include.hxx>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#if __has_include(<execution>)
#include <execution>
#endif
#include <functional>
#include <type_traits>
#include <thread>

SCENARIO("queue: push, pop, steal")
{
  GIVEN("queue of max 1 job")
  {
    auto queue = threadable::queue<1>{};

    WHEN("push + pop + pop")
    {
      queue.push([]{});
      REQUIRE(queue.size() == 1);
      auto job1 = queue.pop();
      REQUIRE(queue.size() == 0);
      auto job2 = queue.pop();
      REQUIRE(queue.size() == 0);

      THEN("first job is non-empty")
      {
        REQUIRE(job1);
        AND_THEN("last job is empty")
        {
          REQUIRE_FALSE(job2);
        }
      }
    }

    WHEN("push + steal + steal")
    {
      queue.push([]{});
      REQUIRE(queue.size() == 1);
      auto job1 = queue.steal();
      REQUIRE(queue.size() == 0);
      auto job2 = queue.steal();
      REQUIRE(queue.size() == 0);

      THEN("first job is non-empty")
      {
        REQUIRE(job1);
        AND_THEN("last job is empty")
        {
          REQUIRE_FALSE(job2);
        }
      }
    }

    WHEN("push + pop + steal")
    {
      queue.push([]{});
      REQUIRE(queue.size() == 1);
      auto job1 = queue.pop();
      REQUIRE(queue.size() == 0);
      auto job2 = queue.steal();
      REQUIRE(queue.size() == 0);

      THEN("first job is non-empty")
      {
        REQUIRE(job1);
        AND_THEN("last job is empty")
        {
          REQUIRE_FALSE(job2);
        }
      }
    }

    WHEN("push + steal + pop")
    {
      queue.push([]{});
      REQUIRE(queue.size() == 1);
      auto job1 = queue.steal();
      REQUIRE(queue.size() == 0);
      auto job2 = queue.pop();
      REQUIRE(queue.size() == 0);

      THEN("first job is non-empty")
      {
        REQUIRE(job1);
        AND_THEN("last job is empty")
        {
          REQUIRE_FALSE(job2);
        }
      }
    }

    WHEN("push + pop + push + pop")
    {
      queue.push([]{});
      REQUIRE(queue.size() == 1);
      auto job1 = queue.pop();
      REQUIRE(queue.size() == 0);
      queue.push([]{});
      REQUIRE(queue.size() == 1);
      auto job2 = queue.pop();
      REQUIRE(queue.size() == 0);

      THEN("first job is non-empty")
      {
        REQUIRE(job1);
        THEN("last job is non-empty")
        {
          REQUIRE(job2);
        }
      }
    }

    WHEN("push + steal + push + steal")
    {
      queue.push([]{});
      REQUIRE(queue.size() == 1);
      auto job1 = queue.steal();
      REQUIRE(queue.size() == 0);
      queue.push([]{});
      REQUIRE(queue.size() == 1);
      auto job2 = queue.steal();
      REQUIRE(queue.size() == 0);

      THEN("first job is non-empty")
      {
        REQUIRE(job1);
        THEN("last job is non-empty")
        {
          REQUIRE(job2);
        }
      }
    }

    WHEN("push + pop + push + steal")
    {
      queue.push([]{});
      REQUIRE(queue.size() == 1);
      auto job1 = queue.pop();
      REQUIRE(queue.size() == 0);
      queue.push([]{});
      REQUIRE(queue.size() == 1);
      auto job2 = queue.steal();
      REQUIRE(queue.size() == 0);

      THEN("first job is non-empty")
      {
        REQUIRE(job1);
        THEN("last job is non-empty")
        {
          REQUIRE(job2);
        }
      }
    }

    WHEN("push + steal + push + pop")
    {
      queue.push([]{});
      REQUIRE(queue.size() == 1);
      auto job1 = queue.steal();
      REQUIRE(queue.size() == 0);
      queue.push([]{});
      REQUIRE(queue.size() == 1);
      auto job2 = queue.pop();
      REQUIRE(queue.size() == 0);

      THEN("first job is non-empty")
      {
        REQUIRE(job1);
        THEN("last job is non-empty")
        {
          REQUIRE(job2);
        }
      }
    }

    WHEN("push + pop + exec + push + pop")
    {
      queue.push([]{});
      REQUIRE(queue.size() == 1);
      auto job1 = queue.pop();
      REQUIRE(queue.size() == 0);
      job1();
      auto token2 = queue.push([]{});
      REQUIRE(queue.size() == 1);
      auto job2 = queue.pop();
      REQUIRE(queue.size() == 0);
      THEN("last job token is not done")
      {
        REQUIRE_FALSE(token2.done());
      }
    }
  }

  int pushCounter = 0;
  auto queue = threadable::queue<>([&pushCounter](...){ ++pushCounter; });
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
  // NOTE: quit is now only used to signal 'waiters' to bail out, nothing else.
  // GIVEN("queue has quit")
  // {
  //   queue.quit();
  //   WHEN("push job")
  //   {
  //     auto token = queue.push([]{});
  //     THEN("size is 0")
  //     {
  //       REQUIRE(queue.size() == 0);
  //       REQUIRE(queue.empty());
  //     }      
  //     THEN("token is done")
  //     {
  //       REQUIRE(token.done());
  //     }
  //     THEN("wait instantly returns")
  //     {
  //       token.wait();
  //     }
  //   }
  // }
  GIVEN("push two jobs")
  {
    queue.push([]{});
    queue.push([]{});
    THEN("size is 2")
    {
      REQUIRE(queue.size() == 2);
      REQUIRE_FALSE(queue.empty());
    }
    THEN("notifier is invoked twice")
    {
      REQUIRE(pushCounter == 2);
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

SCENARIO("queue (sequential): push, pop, steal")
{
  auto queue = threadable::queue<>(threadable::execution_policy::sequential);

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

SCENARIO("queue (concurrent): execution")
{
  auto queuePtr = std::make_shared<threadable::queue<>>(threadable::execution_policy::concurrent);
  auto& queue = *queuePtr;
  int called = 0;
  GIVEN("push job")
  {
    auto token = queue.push([&called]{ ++called; });
    WHEN("queue is destroyed")
    {
      queuePtr = nullptr;
      THEN("queued job(s) are not executed")
      {
        REQUIRE(called == 0);
      }
    }
    WHEN("popped")
    {
      auto job = queue.pop();
      THEN("job is true before invoked")
      {
        REQUIRE(job);
        AND_THEN("token is not done")
        {
          REQUIRE_FALSE(token.done());
        }
      }
      THEN("job is false after invoked")
      {
        job();
        REQUIRE_FALSE(job);
        AND_THEN("token is done")
        {
          REQUIRE(token.done());
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

SCENARIO("queue (sequential): execution")
{
  auto queue = threadable::queue(threadable::execution_policy::sequential);

  std::vector<int> order;
  GIVEN("push two jobs")
  {
    queue.push([&order]{ order.push_back(1); });
    queue.push([&order]{ order.push_back(2); });
    // DEADLOCK: With a sequential queue jobs must be executed FIFO,
    //           so without a separate thread stealing jobs it would deadlock.

    // WHEN("pop & execute jobs")
    // {
    //   while(!queue.empty())
    //   {
    //     auto job = queue.pop();
    //     job();
    //   }
    //   THEN("jobs are executed LIFO")
    //   {
    //     REQUIRE(order.size() == 2);
    //     REQUIRE(order[0] == 2);
    //     REQUIRE(order[1] == 1);
    //   }
    // }
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
    WHEN("steal & execute one job")
    {
      auto job1 = queue.steal();
      job1();
      AND_WHEN("pop & execute one job")
      {
        auto job2 = queue.pop();
        REQUIRE(job2);
        job2();
        THEN("jobs are executed FIFO")
        {
          REQUIRE(order.size() == 2);
          REQUIRE(order[0] == 1);
          REQUIRE(order[1] == 2);
        }
      }
    }
  }
}

SCENARIO("queue: synchronization")
{
  GIVEN("waiting for job to steal")
  {
    auto queue = threadable::queue();

    WHEN("push job")
    {
      auto waiter = std::thread([&queue]{
        if(auto job = queue.steal_or_wait())
        {
          job();
        }
      });

      std::this_thread::sleep_for(std::chrono::milliseconds{ 2 });

      int called = 0;
      queue.push([&called]{ ++called; });
      waiter.join();

      THEN("job is waited for")
      {
        REQUIRE(called == 1);
      }
    }
    WHEN("queue is quit")
    {
      auto waiter = std::thread([&queue]{
        if(auto job = queue.steal_or_wait())
        {
          job();
        }
      });

      std::this_thread::sleep_for(std::chrono::milliseconds{ 2 });

      queue.quit();
      waiter.join();

      THEN("waiting thread is released")
      {
        REQUIRE(true);
      }
    }
  }
  GIVEN("stolen job is executed")
  {
    using clock_t = std::chrono::steady_clock;
    auto stealerDoneTime = clock_t::now();
    auto destroyerDoneTime = clock_t::now();
    std::atomic_size_t barrier{2};

    auto queuePtr = std::shared_ptr<threadable::queue<2>>(new threadable::queue<2>(threadable::execution_policy::concurrent), [&](auto* ptr){
      --barrier;
      while(barrier != 0);
      delete ptr;
      destroyerDoneTime = clock_t::now();
    });
    auto& queue = *queuePtr;

    // job stolen (steal() == FIFO) and executed by stealer
    queue.push([&]{
      // stealer: wait until destroyer is inside destructor
      --barrier;
      while(barrier != 0);
      stealerDoneTime = clock_t::now(); 
    });
    // job popped (pop() == LIFO) & implicitly executed by destroyer (see queue::dtor)
    queue.push([&]{});

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

      THEN("it waits for stolen job to finish")
      {
        REQUIRE(stealerDoneTime < destroyerDoneTime);
      }
    }
  }
}

SCENARIO("queue: completion token")
{
  auto queue = threadable::queue{};
  REQUIRE(queue.empty());
  GIVEN("push job & store token")
  {
    auto token = queue.push([]{});
    THEN("token is done when job is discarded")
    {
      (void)queue.pop();
      REQUIRE(token.done());
    }
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
        const auto start = clock_t::now();

        auto waiterDoneTime = clock_t::now();
        std::thread waiter([&token, &waiterDoneTime]{ 
          token.wait();
          waiterDoneTime = clock_t::now();
        });

        // Give thread some time to start up
        std::this_thread::sleep_for(std::chrono::milliseconds{100});

        auto job = queue.pop();
        job();
        auto jobDoneTime = clock_t::now();
        waiter.join();

        INFO(
          std::chrono::duration_cast<std::chrono::nanoseconds>(jobDoneTime - start).count(),
          "us < ", 
          std::chrono::duration_cast<std::chrono::nanoseconds>(waiterDoneTime - start).count(),
          "us");
        REQUIRE(jobDoneTime <= waiterDoneTime);
      }
    }
  }
  GIVEN("stress-test")
  {
    std::atomic_bool run{true};
    auto worker = std::thread([&]{
      // std::osyncstream(std::cout) << "consumer: Ready.\n";
      std::size_t i = 0;
      while(run)
      {
        // std::this_thread::sleep_for(std::chrono::milliseconds{500});
        if(auto job = queue.steal())
        {
          // std::osyncstream(std::cout) << "consumer: Running job " << i << "...\n";
          job();
          // std::osyncstream(std::cout) << "consumer: DONE running job " << i << "...\n";
          ++i;
        }
      }
    });

    for(std::size_t i = 0; i < queue.max_size() * 2; ++i)
    {
      // std::osyncstream(std::cout) << "producer: Pushing job " << i << "...\n";
      auto token = queue.push([]{});
      // std::osyncstream(std::cout) << "producer: Waiting for job " << i << "...\n";
      token.wait();
      // std::osyncstream(std::cout) << "producer: DONE waiting for job " << i << "...\n";
    }

    run = false;
    worker.join();
  }
}

SCENARIO("queue: stress-test")
{
  static constexpr std::size_t nr_of_jobs = 1 << 18;
  std::atomic_size_t notify_counter = 0;
  auto queue = threadable::queue<nr_of_jobs>([&notify_counter](...){ ++notify_counter; });
  GIVEN("one producer/consumer & multiple stealers")
  {
    THEN("there are no race conditions")
    {
      std::atomic_size_t jobs_executed{0};
      // pre-fill half
      for(std::size_t i = 0; i < nr_of_jobs/2; ++i)
      {
        queue.push([&jobs_executed]{ ++jobs_executed; });
      }
      {
        // start pusher/popper
        std::thread producer([&queue, &jobs_executed]{
          // push remaining half
          for(std::size_t i = 0; i < nr_of_jobs/2; ++i)
          {
            queue.push([&jobs_executed]{ ++jobs_executed; });
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
          stealers.emplace_back([&queue, &producer]{
            while(producer.joinable() || !queue.empty())
            {
              const auto size = queue.size();
              if(size > nr_of_jobs)
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
      REQUIRE(jobs_executed.load() == nr_of_jobs);
      REQUIRE(notify_counter.load() == nr_of_jobs);
    }
  }
}

SCENARIO("queue: iteration")
{
  static constexpr std::size_t nr_of_jobs = 1 << 18;
  std::atomic_size_t job_counter = 0;
  auto queue = threadable::queue<nr_of_jobs>();
  GIVEN("push multiple jobs")
  {
    for(std::size_t i = 0; i < nr_of_jobs; ++i)
    {
      queue.push([&job_counter](...){ ++job_counter; });
    }

    WHEN("iterating and executing jobs")
    {
      THEN("range-based loop works")
      {
        for(auto& job : queue)
        {
          if(job)
          {
            job();
          }
        }

        REQUIRE(job_counter.load() == nr_of_jobs);
      }
      THEN("std::for_each loop (sequential) works")
      {
        std::for_each(queue.begin(), queue.end(), [](auto& job){
          if(job)
          {
            job();
          }
        });

        REQUIRE(job_counter.load() == nr_of_jobs);
      }
#if defined(__cpp_lib_execution) && defined(__cpp_lib_parallel_algorithm)
      THEN("std::for_each loop (parallel) works")
      {
        std::for_each(std::execution::par, queue.begin(), queue.end(), [](auto& job){
          if(job)
          {
            job();
          }
        });

        REQUIRE(job_counter.load() == nr_of_jobs);
      }
#endif
    }
  }
}
