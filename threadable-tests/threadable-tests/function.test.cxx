#include <threadable/function.hxx>
#include <threadable-tests/doctest_include.hxx>

#include <type_traits>
#include <memory>

SCENARIO("function: type traits")
{
  using namespace threadable;

  static_assert(is_function_v<function<>>);
  static_assert(is_function_v<const function<>&>);
  static_assert(is_function_v<function_dyn>);
  static_assert(!is_function_v<decltype([]{})>);
  static_assert(required_buffer_size_v<function<56>> == 56);
  constexpr auto lambda = []{};
  static_assert(required_buffer_size_v<decltype(lambda)> == details::header_size + (details::func_ptr_size * 2) + sizeof(lambda));
}

SCENARIO("function_buffer")
{
  WHEN("constructed with callable")
  {
    int called = 0;
    auto buffer = threadable::function_buffer([&called]{ ++called; });
    THEN("it can be invoked")
    {
      threadable::details::invoke(buffer.data());
      REQUIRE(called == 1);
    }
  }
  WHEN("set with callable")
  {
    int called = 0;
    auto lambda = [&called]{ ++called; };
    auto buffer = threadable::function_buffer(lambda);
    buffer.reset();
    buffer.set(lambda);
    THEN("it can be invoked")
    {
      threadable::details::invoke(buffer.data());
      REQUIRE(called == 1);
    }
  }
  WHEN("constructed with threadable::function")
  {
    int called = 0;
    auto func = threadable::function([&called]{ ++called; });
    AND_WHEN("by value")
    {
      auto buffer = threadable::function_buffer(func);
      THEN("it can be invoked")
      {
        threadable::details::invoke(buffer.data());
        REQUIRE(called == 1);
      }
    }
    AND_WHEN("by r-value")
    {
      auto buffer = threadable::function_buffer(std::move(func));
      THEN("it can be invoked")
      {
        threadable::details::invoke(buffer.data());
        REQUIRE(called == 1);
      }
    }
  }
  WHEN("set with threadable::function")
  {
    int called = 0;
    auto func = threadable::function([&called]{ ++called; });
    auto buffer = threadable::function_buffer(func);
    buffer.reset();
    AND_WHEN("by value")
    {
      buffer.set(func);
      THEN("it can be invoked")
      {
        threadable::details::invoke(buffer.data());
        REQUIRE(called == 1);
      }
    }
    AND_WHEN("by r-value")
    {
      buffer.set(std::move(func));
      THEN("it can be invoked")
      {
        threadable::details::invoke(buffer.data());
        REQUIRE(called == 1);
      }
    }
  }
}

SCENARIO("function_dyn")
{
  WHEN("constructed with callable")
  {
    int called = 0;
    auto func = threadable::function_dyn([&called]{ ++called; });
    REQUIRE(func);
    THEN("it can be invoked")
    {
      func();
      REQUIRE(called == 1);
    }
  }
  WHEN("constructed with function_buffer")
  {
    int called = 0;
    auto func = threadable::function_dyn(threadable::function_buffer([&called]{ ++called; }));
    REQUIRE(func);
    THEN("it can be invoked")
    {
      func();
      REQUIRE(called == 1);
    }
  }
  WHEN("constructed with function")
  {
    int called = 0;
    auto func = threadable::function_dyn(threadable::function([&called]{ ++called; }));
    REQUIRE(func);
    THEN("it can be invoked")
    {
      func();
      REQUIRE(called == 1);
    }
  }
  WHEN("copy-constructed")
  {
    int called = 0;
    auto func1 = threadable::function_dyn([&called]{ ++called; });
    auto func2 = threadable::function_dyn(func1);
    REQUIRE(func1);
    REQUIRE(func2);
    THEN("copy can be invoked")
    {
      func2();
      REQUIRE(called == 1);
    }
    THEN("original can be invoked")
    {
      func1();
      REQUIRE(called == 1);
    }
  }
  WHEN("move-constructed")
  {
    int called = 0;
    auto copy = threadable::function_dyn([&called]{ ++called; });
    auto func = threadable::function_dyn(std::move(copy));
    REQUIRE(func);
    THEN("it can be invoked")
    {
      func();
      REQUIRE(called == 1);
    }
  }
  // WHEN("constructed with callable capturing fynction_dyn")
  // {
  //   int called = 0;
  //   auto func1 = threadable::function_dyn([&called]{ ++called; });
  //   threadable::function_dyn func2([func1] mutable {
  //     func1();
  //   });
  //   THEN("it can be invoked")
  //   {
  //     func2();
  //     REQUIRE(called == 1);
  //   }
  // }
}

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
          THEN("function is set")
          {
            REQUIRE(func);
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
    WHEN("non-trivially-copyable callable is set")
    {
      thread_local int destroyed = 0;
      struct type
      {
        type() = default;
        type(type&&) = default;
        ~type()
        {
          ++destroyed;
        }
        void operator()(){}
        std::unique_ptr<std::uint8_t[]> member;
      };
      static_assert(!std::is_trivially_copyable_v<type>);
      static_assert(std::is_destructible_v<type>);

      func.set(type{});
      AND_WHEN("function is reset")
      {
        destroyed = 0;
        func.set([]{});

        AND_THEN("destructor is invoked")
        {
          REQUIRE(destroyed == 1);
        }
      }
      AND_WHEN("a new callable is set")
      {
        destroyed = 0;
        func.set([]{});

        AND_THEN("destructor is invoked on previous callable")
        {
          REQUIRE(destroyed == 1);
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
    WHEN("callable is const")
    {
      int nonconstVal = 0;
      int constVal = 0;
      struct callable_type
      {
        void operator()()
        {
          ++nonconstVal;
        }
        void operator()() const
        {
          ++constVal;
        }
      int& nonconstVal;
      int& constVal;
      } const callable{nonconstVal, constVal};

      func.set(callable);
      WHEN("it is invoked")
      {
        func();
        THEN("const overload is invoked")
        {
          REQUIRE(callable.nonconstVal == 0);
          REQUIRE(callable.constVal == 1);
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
    WHEN("member function")
    {
      struct type
      {
        void func(int& called)
        {
          ++called;
        }
      };

      func.set(&type::func, type{}, std::ref(called));
      THEN("it's invoked")
      {
        func();
        REQUIRE(called == 1);
      }
    }
  }
}

SCENARIO("function: Conversion")
{
  static constexpr auto func_size = 64;
  auto func = threadable::function<func_size>{};
  GIVEN("callable is set")
  {
    int called = 0;

    func.set([&called]() mutable { ++called; });
    WHEN("function is converted to std::function")
    {
      std::function<void()> stdFunc = func;

      WHEN("it is invoked")
      {
        stdFunc();
        THEN("callable is invoked")
        {
          REQUIRE(called == 1);
        }
      }
    }
    WHEN("function is converted to function_dyn")
    {
      threadable::function_dyn funcDyn = func;

      static_assert(sizeof(funcDyn) == sizeof(std::unique_ptr<std::uint8_t[]>));
      REQUIRE(funcDyn);
      REQUIRE(funcDyn.size() == func.size());

      WHEN("it is invoked")
      {
        funcDyn();
        THEN("callable is invoked")
        {
          REQUIRE(called == 1);
        }
      }
    }
  }
}