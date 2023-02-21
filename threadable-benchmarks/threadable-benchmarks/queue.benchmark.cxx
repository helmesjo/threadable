#include <threadable/queue.hxx>
#include <threadable-benchmarks/util.hxx>

#include <nanobench.h>
#include <doctest/doctest.h>

#include <algorithm>
#if __has_include(<execution>)
#include <execution>
#endif
#include <functional>
#if __has_include (<pstld/pstld.h>)
    #include <pstld/pstld.h>
#endif
#include <vector>

namespace bench = ankerl::nanobench;

namespace
{
  constexpr std::size_t jobs_per_iteration = 1 << 16;
  int val = 1;
}

TEST_CASE("queue")
{
  bench::Bench b;
  b.warmup(500).relative(true);
  b.title("iterate - sequential");

  auto lambda = [](){
    bench::doNotOptimizeAway(val = threadable::utils::do_trivial_work(val) );
  };

  {
    auto queue = std::vector<std::function<void()>>();
    queue.resize(jobs_per_iteration, lambda);

    b.run("std::vector", [&] {
      for(auto& job : queue)
      {
        job();
      }
    });
  }
  {
    auto queue = threadable::queue2<jobs_per_iteration>();
    for(std::size_t i=0; i<jobs_per_iteration; ++i)
    {
      queue.push(lambda);
    }

    b.run("threadable::queue", [&] {
      for(auto& job : queue)
      {
        job();
      }
    });
  }

  b.title("iterate - parallel");
  {
    auto queue = std::vector<std::function<void()>>();
    queue.resize(jobs_per_iteration, lambda);

    b.run("std::vector", [&] {
      std::for_each(std::execution::par, std::begin(queue), std::end(queue), [](auto& job){
        job();
      });
    });
  }
  {
    auto queue = threadable::queue2<jobs_per_iteration>();
    for(std::size_t i=0; i<jobs_per_iteration; ++i)
    {
      queue.push(lambda);
    }

    b.run("threadable::queue", [&] {
      std::for_each(std::execution::par, std::begin(queue), std::end(queue), [](auto& job){
        job();
      });
    });
  }
}
