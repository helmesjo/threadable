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
    using job_t = std::function<void()>;
    auto queue = std::vector<job_t>();
    queue.resize(jobs_per_iteration, lambda);

    b.run("std::vector", [&] {
      for(auto& job : queue)
      {
        job();
      }
    });
  }
  {
    using job_t = threadable::job;
    auto queue = threadable::queue2<jobs_per_iteration>();
    std::fill(std::begin(queue), std::end(queue), lambda);

    b.run("threadable::queue", [&] {
      for(auto& job : queue)
      {
        job();
      }
    });
  }

  b.title("iterate - parallel");
  {
    using job_t = std::function<void()>;
    auto queue = std::vector<job_t>();
    queue.resize(jobs_per_iteration, lambda);

    b.run("std::vector", [&] {
      std::for_each(std::execution::par, std::begin(queue), std::end(queue), [](auto& job){
        job();
      });
    });
  }
  {
    using job_t = threadable::job;
    auto queue = threadable::queue2<jobs_per_iteration>();
    std::fill(std::begin(queue), std::end(queue), lambda);

    b.run("threadable::queue", [&] {
      std::for_each(std::execution::par, std::begin(queue), std::end(queue), [](auto& job){
        job();
      });
    });
  }
}
