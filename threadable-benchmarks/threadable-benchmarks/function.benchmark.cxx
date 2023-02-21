#include <threadable/function.hxx>
#include <threadable-benchmarks/util.hxx>

#include <nanobench.h>
#include <doctest/doctest.h>
#include <functional>

namespace bench = ankerl::nanobench;

namespace
{
  int val = 1;
}

TEST_CASE("function")
{
  bench::Bench b;
  b.warmup(500).relative(true);

  auto lambda = [](){
    bench::doNotOptimizeAway(val = threadable::utils::do_trivial_work(val) );
  };
  using lambda_t = decltype(lambda);
  auto func = threadable::function<>(lambda);
  auto funcStd = std::function<void()>(lambda);
  b.title("assign")
    .run("lambda", [&] {
      bench::doNotOptimizeAway(lambda = lambda_t{});
  }).run("threadable::function", [&] {
      bench::doNotOptimizeAway(func = lambda);
  }).run("std::function", [&] {
      bench::doNotOptimizeAway(funcStd = lambda);
  });

  b.title("invoke")
    .run("lambda", [&] {
      lambda();
  }).run("threadable::function", [&] {
      func();
  }).run("std::function", [&] {
      funcStd();
  });

  b.title("reset")
    .run("lambda", [&] {
      bench::doNotOptimizeAway(lambda = lambda_t{});
  }).run("threadable::function", [&] {
      bench::doNotOptimizeAway(func = nullptr);
  }).run("std::function", [&] {
      bench::doNotOptimizeAway(funcStd = nullptr);
  });
}
