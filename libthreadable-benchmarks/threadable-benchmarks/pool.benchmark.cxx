#include <threadable-benchmarks/util.hxx>
#include <threadable/pool.hxx>

#include <doctest/doctest.h>

#include <nanobench.h>

#if __has_include(<pstld/pstld.h>)
  #include <pstld/pstld.h>
#endif
#include <queue>

namespace bench = ankerl::nanobench;

namespace
{
  constexpr std::size_t jobs_per_iteration = 1 << 12;
  int                   val                = 1; // NOLINT
}

TEST_CASE("pool: job execution")
{
  auto pool = threadable::pool<jobs_per_iteration>();

  bench::Bench b;
  b.warmup(1).relative(true).batch(jobs_per_iteration).unit("job");

  using job_t = decltype([](){
    bench::doNotOptimizeAway(val = threadable::utils::do_non_trivial_work(val) );
  });

  b.title("push & wait");
  {
    std::queue<job_t> queue;
    b.run("std::queue",
          [&]
          {
            for (std::size_t i = 0; i < jobs_per_iteration; ++i)
            {
              queue.emplace();
            }
            while (!queue.empty())
            {
              auto& job = queue.back();
              job();
              queue.pop();
            }
          });
  }
  {
    auto& queue = pool.create(threadable::execution_policy::parallel);
    b.run("threadable::pool",
          [&]
          {
            threadable::token_group group;
            for (std::size_t i = 0; i < queue.max_size(); ++i)
            {
              group += queue.push(job_t{});
            }
            group.wait();
          });
  }
}
