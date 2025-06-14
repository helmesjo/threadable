#include <threadable-benchmarks/util.hxx>
#include <threadable/function.hxx>

#include <functional>

#include <doctest/doctest.h>

#include <nanobench.h>

namespace bench = ankerl::nanobench;

namespace
{
  auto val = 1; // NOLINT
}

TEST_CASE("function")
{
  bench::Bench b;
  b.warmup(1).relative(true).batch(1);

  auto lambda = []()
  {
    bench::doNotOptimizeAway(val = fho::utils::do_trivial_work(val));
  };
  using lambda_t = decltype(lambda);
  auto func      = fho::function<>(lambda);
  auto funcStd   = std::function<void()>(lambda);

  b.title("function: assign")
    .run("lambda",
         [&]
         {
           bench::doNotOptimizeAway(lambda = lambda_t{});
         })
    .run("std::function",
         [&]
         {
           bench::doNotOptimizeAway(funcStd = lambda);
         })
    .run("fho::function",
         [&]
         {
           bench::doNotOptimizeAway(func = lambda);
         });

  b.title("function: invoke")
    .run("lambda",
         [&]
         {
           lambda();
         })
    .run("std::function",
         [&]
         {
           funcStd();
         })
    .run("fho::function",
         [&]
         {
           func();
         });

  b.title("function: reset")
    .run("lambda",
         [&]
         {
           bench::doNotOptimizeAway(lambda = lambda_t{});
         })
    .run("std::function",
         [&]
         {
           bench::doNotOptimizeAway(funcStd = nullptr);
         })
    .run("fho::function",
         [&]
         {
           bench::doNotOptimizeAway(func = nullptr);
         });
}
