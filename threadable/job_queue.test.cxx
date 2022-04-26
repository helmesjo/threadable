#include <threadable/job_queue.hxx>
#include <threadable/doctest_include.hxx>
#include <type_traits>

using namespace threadable;

SCENARIO("job_queue: push, pop, steal")
{
  auto queue = job_queue{};

  GIVEN("queue is empty")
  {
    WHEN("steal one job")
    {
      auto* job = queue.steal();
      THEN("no job is returned")
      {
        REQUIRE(!job);
      }
    }
  }
  GIVEN("push two jobs")
  {
    queue.push([]{});
    queue.push([]{});
    WHEN("pop two jobs")
    {
      auto& job1 = queue.pop();
      auto& job2 = queue.pop();
      THEN("queue is empty")
      {
        REQUIRE(queue.size() == 0);
      }
    }
    WHEN("pop one job")
    {
      auto& job1 = queue.pop();
      AND_WHEN("steal one job")
      {
        auto* job2 = queue.steal();
        THEN("queue is empty")
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

SCENARIO("job_queue: execution")
{
  auto queue = job_queue{};
  std::vector<int> order;
  GIVEN("push job")
  {
    WHEN("popped")
    {
      queue.push([]{});
      auto& job = queue.pop();
      THEN("job is true before invoked")
      {
        REQUIRE(job == true);
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
        auto& job = queue.pop();
        job();
        REQUIRE(called == 1);
      }
    }
    WHEN("free function")
    {
      queue.push(free_func, std::ref(called));
      THEN("popped job invokes it")
      {
        auto& job = queue.pop();
        job();
        REQUIRE(called == 1);
      }
    }
    WHEN("member: trivially copyable type")
    {
      struct type
      {
        void func(int& called)
        {
          ++called;
        }
      };
      static_assert(std::is_trivially_copyable_v<type>);

      queue.push(&type::func, type{}, std::ref(called));
      THEN("popped job invokes it")
      {
        auto& job = queue.pop();
        job();
        REQUIRE(called == 1);
      }
    }
    WHEN("member: non-trivially-copyable type")
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

      int destroyed = 0;
      queue.push(&type::func, type{destroyed}, std::ref(called));
      THEN("popped job invokes it")
      {
        auto& job = queue.pop();
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
  GIVEN("push two jobs")
  {
    queue.push([&order]{ order.push_back(1); });
    queue.push([&order]{ order.push_back(2); });
    WHEN("pop two jobs")
    {
      auto& job1 = queue.pop();
      auto& job2 = queue.pop();
      THEN("jobs are ordered LIFO")
      {
        REQUIRE(job1);
        job1();
        job2();
        REQUIRE(order.size() == 2);
        REQUIRE(order[0] == 2);
        REQUIRE(order[1] == 1);
      }
    }
    WHEN("steal two jobs")
    {
      auto* job1 = queue.steal();
      auto* job2 = queue.steal();
      THEN("jobs are ordered FIFO")
      {
        (*job1)();
        (*job2)();
        REQUIRE(order.size() == 2);
        REQUIRE(order[0] == 1);
        REQUIRE(order[1] == 2);
      }
    }
    WHEN("pop one job")
    {
      auto& job1 = queue.pop();
      AND_WHEN("steal one job")
      {
        auto* job2 = queue.steal();
        THEN("jobs are ordered LIFO")
        {
          job1();
          (*job2)();
          REQUIRE(order.size() == 2);
          REQUIRE(order[0] == 2);
          REQUIRE(order[1] == 1);
        }
      }
    }
  }
}
