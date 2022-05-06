#include <threadable/queue.hxx>
#include <threadable/doctest_include.hxx>

#include <type_traits>

SCENARIO("function: set/reset")
{
  auto func = threadable::function{};
  GIVEN("is empty")
  {
    THEN("function is unset")
    {
      REQUIRE_FALSE(func);
    }
    WHEN("callable is set")
    {
      AND_WHEN("using set()")
      {
        func.set([]{});
        REQUIRE(func);

        AND_WHEN("function is reset")
        {
          func.reset();
          THEN("function is unset")
          {
            REQUIRE_FALSE(func);
          }
        }
        AND_WHEN("function was invoked")
        {
          func();
          THEN("function is unset")
          {
            REQUIRE_FALSE(func);
          }
        }
      }
      AND_WHEN("using assign operator")
      {
        func = []{};
        REQUIRE(func);

        AND_WHEN("function is reset")
        {
          func = nullptr;
          THEN("function is unset")
          {
            REQUIRE_FALSE(func);
          }
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

SCENARIO("function: execution")
{
  auto func = threadable::function{};
  GIVEN("callable is set")
  {
    WHEN("callable with value member")
    {
      struct callable_type
      {
        void operator()()
        {
          ++val;
        }
        int val = 0;
      } callable;

      func.set(callable);
      WHEN("it is invoked")
      {
        func();
        THEN("internal copy of callable with different value instance is invoked")
        {
          REQUIRE(callable.val == 0);
        }
      }
    }
    WHEN("callable with reference member")
    {
      int val = 0;
      struct callable_type
      {
        void operator()()
        {
          ++val;
        }
        int& val;
      } callable{val};

      func.set(callable);
      WHEN("it is invoked")
      {
        func();
        THEN("an internal copy of callable with same reference is invoked")
        {
          REQUIRE(callable.val == 1);
        }
      }
    }
    int called = 0;
    WHEN("lambda")
    {
      func.set([&called]{ ++called; });
      THEN("it's invoked")
      {
        func();
        REQUIRE(called == 1);
      }
    }
    WHEN("free function")
    {
      func.set(free_func, std::ref(called));
      THEN("it's invoked")
      {
        func();
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

      func.set(&type::func, type{}, std::ref(called));
      THEN("it's invoked")
      {
        func();
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
      func.set(&type::func, type{destroyed}, std::ref(called));
      THEN("it's invoked")
      {
        called = 0;
        destroyed = 0;
        func();
        REQUIRE(called == 1);

        AND_THEN("destructor is called")
        {
          REQUIRE(destroyed);
        }
      }
    }
  }
}
