#include <threadable-tests/doctest_include.hxx>
#include <threadable/function.hxx>

#include <format>
#include <memory>
#include <type_traits>

namespace
{
  void
  free_func(int& arg)
  {
    ++arg;
  }
}

SCENARIO("function: print system info")
{
  std::cerr << std::format("hardware_destructive_interference_size: {}\n",
                           fho::details::cache_line_size);
}

SCENARIO("function: type traits")
{
  using namespace fho;

  static_assert(is_function_v<function<>>);
  static_assert(is_function_v<function<> const&>);
  static_assert(!is_function_v<function_dyn>);
  static_assert(!is_function_dyn_v<function<> const&>);
  static_assert(is_function_dyn_v<function_dyn>);
  static_assert(!is_function_v<decltype([] {})>);
  static_assert(required_buffer_size_v<function<56>> == 56);
  constexpr auto lambda = [] {};
  static_assert(required_buffer_size_v<decltype(lambda)> ==
                details::function_buffer_meta_size + sizeof(lambda));
  static_assert(required_buffer_size_v<decltype(lambda), int, int> ==
                details::function_buffer_meta_size + sizeof(lambda) + sizeof(int) + sizeof(int));
}

SCENARIO("function_buffer")
{
  WHEN("constructed with function pointer")
  {
    int  called = 0;
    auto buffer = fho::function_buffer<64>(free_func, std::ref(called));
    THEN("it can be invoked")
    {
      fho::details::invoke(buffer.data());
      REQUIRE(called == 1);
    }
  }
  WHEN("constructed with member function pointer")
  {
    struct type
    {
      void
      func(int& called)
      {
        ++called;
      }
    };

    int  called = 0;
    auto buffer = fho::function_buffer<64>(&type::func, type{}, std::ref(called));
    THEN("it can be invoked")
    {
      fho::details::invoke(buffer.data());
      REQUIRE(called == 1);
    }
  }
  WHEN("constructed with callable")
  {
    int  called = 0;
    auto buffer = fho::function_buffer(
      [&called]
      {
        ++called;
      });
    THEN("it can be invoked")
    {
      fho::details::invoke(buffer.data());
      REQUIRE(called == 1);
    }
  }
  WHEN("constructed with callable + arguments")
  {
    int   passedArg1 = 0;
    float passedArg2 = 0;
    auto  buffer     = fho::function_buffer<64>(
      [&](int arg1, float&& arg2)
      {
        passedArg1 = arg1;
        passedArg2 = std::move(arg2);
      },
      1, 3.4f);

    THEN("it can be invoked & arguments are forwarded")
    {
      fho::details::invoke(buffer.data());
      REQUIRE(passedArg1 == 1);
      REQUIRE(passedArg2 == doctest::Approx(3.4f));
    }
  }
  WHEN("constructed with fho::function")
  {
    thread_local int copyCtor = 0;
    thread_local int moveCtor = 0;

    struct type
    {
      type()  = default;
      ~type() = default;

      type(type const&)
      {
        ++copyCtor;
      }

      type(type&&) noexcept
      {
        ++moveCtor;
      }

      auto operator=(type const&) -> type& = delete;
      auto operator=(type&&) -> type&      = delete;

      void
      operator()()
      {}
    };

    static_assert(std::copy_constructible<type>);

    auto buffer = fho::function_buffer(type{});
    copyCtor    = 0;
    moveCtor    = 0;
    THEN("the callables' copy-ctor is invoked")
    {
      auto copy = buffer;
      REQUIRE(copyCtor == 1);
      REQUIRE_NOTHROW(fho::details::invoke(buffer.data()));
    }
    THEN("the callables' move-ctor is invoked")
    {
      auto moved = std::move(buffer);
      REQUIRE(moveCtor == 1);
      REQUIRE_NOTHROW(fho::details::invoke(buffer.data()));
    }
  }
  WHEN("nested in lambda capture (by value)")
  {
    int  called = 0;
    auto buffer = fho::function_buffer(
      [&called]
      {
        ++called;
      });
    auto lambda = [buffer = buffer]() mutable
    {
      fho::details::invoke(buffer.data());
    };
    THEN("it can be invoked")
    {
      lambda();
      REQUIRE(called == 1);
    }
  }
  WHEN("nested in lambda capture (by r-value)")
  {
    int  called = 0;
    auto buffer = fho::function_buffer(
      [&called]
      {
        ++called;
      });
    auto lambda = [buffer = std::move(buffer)]() mutable
    {
      fho::details::invoke(buffer.data());
    };
    THEN("it can be invoked")
    {
      lambda();
      REQUIRE(called == 1);
    }
  }
  WHEN("assign with callable")
  {
    int  called = 0;
    auto lambda = [&called]
    {
      ++called;
    };
    auto buffer = fho::function_buffer(lambda);
    buffer.reset();
    buffer = lambda;
    THEN("it can be invoked")
    {
      fho::details::invoke(buffer.data());
      REQUIRE(called == 1);
    }
  }
  WHEN("constructed with fho::function")
  {
    int  called = 0;
    auto func   = fho::function(
      [&called]
      {
        ++called;
      });
    AND_WHEN("by value")
    {
      auto buffer = fho::function_buffer(func);
      THEN("it can be invoked")
      {
        fho::details::invoke(buffer.data());
        REQUIRE(called == 1);
      }
    }
    AND_WHEN("by r-value")
    {
      auto buffer = fho::function_buffer(std::move(func));
      THEN("it can be invoked")
      {
        fho::details::invoke(buffer.data());
        REQUIRE(called == 1);
      }
    }
  }
  WHEN("assign with fho::function")
  {
    int  called = 0;
    auto func   = fho::function(
      [&called]
      {
        ++called;
      });
    auto buffer = fho::function_buffer(func);
    buffer.reset();
    AND_WHEN("by value")
    {
      buffer = func;
      THEN("it can be invoked")
      {
        fho::details::invoke(buffer.data());
        REQUIRE(called == 1);
      }
    }
    AND_WHEN("by r-value")
    {
      buffer = std::move(func);
      THEN("it can be invoked")
      {
        fho::details::invoke(buffer.data());
        REQUIRE(called == 1);
      }
    }
  }
}

SCENARIO("function_dyn")
{
  WHEN("constructed with callable")
  {
    int  called = 0;
    auto func   = fho::function_dyn(
      [&called]
      {
        ++called;
      });
    REQUIRE(func);
    THEN("it can be invoked")
    {
      func();
      REQUIRE(called == 1);
    }
  }
  WHEN("constructed with fho::function_buffer")
  {
    int  called = 0;
    auto func   = fho::function_dyn(fho::function_buffer(
      [&called]
      {
        ++called;
      }));
    REQUIRE(func);
    THEN("it can be invoked")
    {
      func();
      REQUIRE(called == 1);
    }
  }
  WHEN("constructed with fho::function")
  {
    int  called = 0;
    auto func   = fho::function_dyn(fho::function(
      [&called]
      {
        ++called;
      }));
    REQUIRE(func);
    THEN("it can be invoked")
    {
      func();
      REQUIRE(called == 1);
    }
  }
  WHEN("copy-constructed")
  {
    int  called = 0;
    auto func1  = fho::function_dyn(
      [&called]
      {
        ++called;
      });
    auto func2 = fho::function_dyn(func1);
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
    int  called = 0;
    auto func1  = fho::function_dyn(
      [&called]
      {
        ++called;
      });
    auto func2 = fho::function_dyn(std::move(func1));
    REQUIRE(func2);
    THEN("it can be invoked")
    {
      func2();
      REQUIRE(called == 1);
    }
  }
  WHEN("deep-copy constructed")
  {
    struct type
    {
      type(type&&) = delete;

      type(int v)
        : val(new int{})
      {
        *val = v;
      }

      type(type const& that)
        : val(new int(*that.val))
      {}

      ~type()
      {
        // make sure we modify the memory address
        // so the "right" values doesn't stick around.
        *val = 8;
        delete val;
        val = nullptr;
      }

      auto operator=(type const&) -> type& = delete;
      auto operator=(type&&) -> type&      = delete;

      int* val = nullptr;
    };

    // this order and combination of things is
    // important to trigger the obscure case,
    // so leave as-is.
    auto func = fho::function();
    func      = [obj = type{5}]
    {
      REQUIRE(*obj.val == 5);
    };
    auto funcDyn = fho::function_dyn(func);
    func         = [func = funcDyn]() mutable
    {
      func();
    };
    THEN("it's invocable after source object has been destroyed")
    {
      funcDyn.reset();
      func();
    }
  }
  WHEN("constructed with callable capturing function_dyn")
  {
    int  called = 0;
    auto func1  = fho::function_dyn(
      [&called]
      {
        ++called;
      });
    auto func2 = fho::function(
      [func1]() mutable
      {
        func1();
      });
    THEN("it can be invoked")
    {
      func2();
      REQUIRE(called == 1);
    }
    AND_WHEN("a copy is constructed")
    {
      auto func2Copy = func2;
      THEN("it can be invoked")
      {
        func2Copy();
        REQUIRE(called == 1);
      }
      THEN("both can be cleared")
      {
        func2     = {};
        func2Copy = {};
      }
    }
  }
}

SCENARIO("function: assign/reset")
{
  auto func = fho::function{};
  GIVEN("is empty")
  {
    THEN("function is unassigned")
    {
      REQUIRE_FALSE(func);
    }
    WHEN("callable is assigned")
    {
      AND_WHEN("using assign()")
      {
        func = [] {};
        REQUIRE(func);

        AND_WHEN("function is reset")
        {
          func.reset();
          THEN("function is unassigned")
          {
            REQUIRE_FALSE(func);
          }
        }
        AND_WHEN("function was invoked")
        {
          func();
          THEN("function is assigned")
          {
            REQUIRE(func);
          }
        }
      }
      AND_WHEN("using assign operator")
      {
        func = [] {};
        REQUIRE(func);

        AND_WHEN("function is reset")
        {
          func = nullptr;
          THEN("function is unassigned")
          {
            REQUIRE_FALSE(func);
          }
        }
      }
    }
    WHEN("callable with argument is assigned")
    {
      int argReceived = 0;
      func            = fho::function(
        [&argReceived](int arg)
        {
          argReceived = arg;
        },
        5);
      THEN("it can be invoked")
      {
        func();
        REQUIRE(argReceived == 5);
      }
    }
    WHEN("recursive callable is assigned")
    {
      int  val      = 0;
      auto callable = [&val](auto self) -> void
      {
        (void)val;
        static_assert(sizeof(self) == 8);
      };
      static_assert(sizeof(callable) == 8);
      func = fho::function(callable, callable);
    }
    WHEN("non-trivially-copyable callable is assigned")
    {
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
        {}
      };

      // add test to verify func = std::move(func) doesn't first reset "self"
      // dito for func = func which should be no-op.A
      // dito for function_dyn

      static_assert(!std::is_trivially_copyable_v<type>);
      static_assert(std::copy_constructible<type>);
      static_assert(std::is_destructible_v<type>);

      func = type{};
      AND_WHEN("function is reset")
      {
        destroyed = 0;
        func.reset();

        THEN("destructor is invoked")
        {
          REQUIRE(destroyed == 1);
        }
      }
      AND_WHEN("a new callable is assigned")
      {
        destroyed = 0;
        func      = [] {};

        THEN("destructor is invoked on previous callable")
        {
          REQUIRE(destroyed == 1);
        }
      }
    }
  }
}

SCENARIO("function: execution")
{
  auto func = fho::function{};
  GIVEN("callable is assigned")
  {
    WHEN("callable with value member")
    {
      struct callable_type
      {
        void
        operator()()
        {
          ++val;
        }

        int val = 0;
      } callable;

      func = callable;
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
        void
        operator()()
        {
          ++val;
        }

        int& val; // NOLINT
      } callable{val};

      func = callable;
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
      int constVal    = 0;

      struct callable_type
      {
        void
        operator()()
        {
          ++nonconst_val;
        }

        void
        operator()() const
        {
          ++const_val;
        }

        int& nonconst_val; // NOLINT
        int& const_val;    // NOLINT
      } const callable{.nonconst_val = nonconstVal, .const_val = constVal};

      func = callable;
      WHEN("it is invoked")
      {
        func();
        THEN("const overload is invoked")
        {
          REQUIRE(callable.nonconst_val == 0);
          REQUIRE(callable.const_val == 1);
        }
      }
    }
    int called = 0;
    WHEN("lambda")
    {
      func = [&called]
      {
        ++called;
      };
      THEN("it's invoked")
      {
        func();
        REQUIRE(called == 1);
      }
    }
    WHEN("free function")
    {
      func = fho::function(free_func, std::ref(called));
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
        void
        func(int& called)
        {
          ++called;
        }
      };

      func = fho::function(&type::func, type{}, std::ref(called));
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
  auto                  func      = fho::function<func_size>{};
  GIVEN("callable is assigned")
  {
    thread_local int called = 0;

    struct type
    {
      type()            = default;
      type(type const&) = default;
      type(type&&)      = default;
      ~type()           = default;

      auto operator=(type const&) -> type& = delete;
      auto operator=(type&&) -> type&      = delete;

      void
      operator()()
      {
        ++called;
      }
    };

    func = type{};
    WHEN("function is converted to std::function")
    {
      std::function<void()> stdFunc = func;

      WHEN("it is invoked")
      {
        called = 0;
        stdFunc();
        THEN("callable is invoked")
        {
          REQUIRE(called == 1);
        }
      }
    }
    WHEN("function is converted to function_dyn")
    {
      fho::function_dyn funcDyn = func;

      static_assert(sizeof(funcDyn) == sizeof(std::unique_ptr<std::byte*>));
      REQUIRE(funcDyn);
      REQUIRE(funcDyn.size() == func.size());

      WHEN("it is invoked")
      {
        called = 0;
        funcDyn();
        THEN("callable is invoked")
        {
          REQUIRE(called == 1);
        }
      }
    }
  }
}

SCENARIO("function_dyn")
{
  GIVEN("callable is assigned")
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

    auto funcDyn = fho::function_dyn(type{});

    WHEN("it is invoked")
    {
      funcDyn();
      THEN("callable is invoked")
      {
        REQUIRE(called == 1);
      }
    }
    WHEN("function is reset")
    {
      destroyed = 0;
      funcDyn.reset();

      THEN("destructor is invoked")
      {
        REQUIRE(destroyed == 1);
      }
    }
    called    = 0;
    destroyed = 0;
  }
}
