#include <threadable/pool.hxx>
#include <threadable-benchmarks/util.hxx>

#include <nanobench.h>
#include <doctest/doctest.h>

#include <algorithm>
#if __has_include(<execution>)
  #include <execution>
#endif
#if __has_include (<pstld/pstld.h>)
  #include <atomic> // missing include
  #include <pstld/pstld.h>
#endif
#include <thread>
#include <vector>
#include <queue>

namespace bench = ankerl::nanobench;

namespace
{
  constexpr std::size_t jobs_per_iteration = 1 << 12;
  int val = 1;
}

TEST_CASE("pool: job execution")
{
  auto pool = threadable::pool<jobs_per_iteration>();

  bench::Bench b;
  b.warmup(1)
   .relative(true)
   .batch(jobs_per_iteration)
   .unit("job");

  using job_t = decltype([](){
    bench::doNotOptimizeAway(val = threadable::utils::do_non_trivial_work(val) );
  });

  b.title("push & wait");
  {
    std::queue<job_t> queue;
    b.run("std::queue", [&] {
      for(std::size_t i = 0; i < jobs_per_iteration; ++i)
      {
        queue.push(job_t{});
      }
      while(!queue.empty())
      {
        auto& job = queue.back();
        job();
        queue.pop();
      }
    });
  }
  {
    auto& queue = pool.create(threadable::execution_policy::parallel);
    b.run("threadable::pool", [&] {
      for(std::size_t i = 0; i < queue.max_size(); ++i)
      {
        queue.push(job_t{});
      }
      pool.wait();
    });
  }
}
