#include <threadable/job_queue.hxx>
#include <threadable/doctest_include.hxx>

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
  void free_func(int& called)
  {
    ++called;
  }
}

SCENARIO("job_queue: execution")
{
  auto queue = job_queue{};
  std::vector<int> order;
  GIVEN("push job")
  {
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
      queue.push(free_func, called);
      THEN("popped job invokes it")
      {
        auto& job = queue.pop();
        job();
        REQUIRE(called == 1);
      }
    }
    WHEN("member function")
    {
      struct type
      {
        void func(int& called)
        {
          ++called;
        }
      } obj;
      queue.push(&type::func, obj, called);
      THEN("popped job invokes it")
      {
        auto& job = queue.pop();
        job();
        REQUIRE(called == 1);
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
