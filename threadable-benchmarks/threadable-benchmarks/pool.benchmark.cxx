#include <threadable/pool.hxx>
#include <threadable-benchmarks/util.hxx>

#include <nanobench.h>
#include <doctest/doctest.h>

#include <algorithm>
#if __has_include(<execution>)
#include <execution>
#endif
#if __has_include (<pstld/pstld.h>)
  #include <pstld/pstld.h>
#endif
#include <thread>
#include <vector>
#include <queue>

namespace bench = ankerl::nanobench;

namespace
{
  constexpr std::size_t jobs_per_iteration = 1 << 20;
  int val = 1;
}

TEST_CASE("pool: job execution")
{
  const auto thread_count = std::thread::hardware_concurrency();
  auto pool = threadable::pool<jobs_per_iteration>();

  bench::Bench b;
  b.warmup(1)
   .relative(true)
   .batch(jobs_per_iteration)
   .unit("job");

  using queue_t = decltype(pool)::queue_t;
  using job_t = decltype([](){
    bench::doNotOptimizeAway(val = threadable::utils::do_trivial_work(val) );
  });

  b.title("push & wait");
  {
    std::vector<std::queue<job_t>> queues;
    queues.resize(thread_count);
    b.run("std::queue", [&] {
      for(auto& queue : queues)
      {
        for(std::size_t i=0; i<jobs_per_iteration/thread_count; ++i)
        {
          queue.push(job_t{});
        }
      }
      for(auto& q : queues)
      {
        while(!q.empty())
        {
          auto& job = q.back();
          job();
          q.pop();
        }
      }
    });
  }
  {
    std::vector<std::shared_ptr<queue_t>> queues;
    for(std::size_t i=0; i<thread_count; ++i)
    {
      queues.emplace_back(pool.create());
    }
    b.run("threadable::pool", [&] {
      for(auto& queue : queues)
      {
        pool.remove(*queue);
      }
      for_each(std::execution::par, std::begin(queues), std::end(queues), [&](auto& queue){
        for(std::size_t i=0; i<jobs_per_iteration/thread_count; ++i)
        {
          queue->push(job_t{});
        }
      });
      for(auto& queue : queues)
      {
        pool.add(queue);
      }
      pool.wait();
    });
  }
}
