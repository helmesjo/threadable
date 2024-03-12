#include <threadable-benchmarks/util.hxx>
#include <threadable/pool.hxx>

#include <queue>

#include <doctest/doctest.h>

#include <nanobench.h>

namespace bench = ankerl::nanobench;

namespace
{
  constexpr auto jobs_per_iteration = 1 << 20;
  auto           val                = 1; // NOLINT
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
